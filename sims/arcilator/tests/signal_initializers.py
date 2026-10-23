#!/usr/bin/env python3
"""Check that static initializers cannot become live signal aliases."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim))
from compiler.partition import plugin

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-opt', type=Path, required=True)
parser.add_argument('--work', type=Path, required=True)
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
library = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                 sim / 'generated-src/compiler', sim / 'compiler/SignalState.cpp')
memory = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                sim / 'generated-src/compiler', sim / 'compiler/NativeMem2Reg.cpp')
positive = '''module {
  hw.module @Snapshot(in %data: i8, out initial: i8, out live: i8) {
    %seven = hw.constant 7 : i8
    %five = hw.constant 5 : i8
    %time = llhd.constant_time <0ns, 0d, 1e>
    %a = llhd.sig %seven : <i8>
    %pa = llhd.prb %a : i8
    %sum = comb.add %pa, %five : i8
    %b = llhd.sig %sum : <i8>
    %pb = llhd.prb %b : i8
    %c = llhd.sig %pb : <i8>
    %pc = llhd.prb %c : i8
    llhd.drv %a, %data after %time : i8
    hw.output %pc, %pa : i8, i8
  }
}
'''
source = args.work / 'positive.mlir'
output = args.work / 'positive-output.mlir'
source.write_text(positive)
command = [str(args.circt_opt), str(source), '--load-pass-plugin=' + str(library),
           '--load-pass-plugin=' + str(memory),
           '--pass-pipeline=builtin.module(chipyard-prepare-native-llhd)',
           '-o', str(output)]
subprocess.run(command, check=True)
text = output.read_text()
constant = re.search(r'(%\w+) = hw.constant 12 : i8', text)
assert constant and re.search(r'hw.output ' + re.escape(constant[1]) + r', %data : i8, i8', text), text

cases = {
    'runtime_input': '%a = llhd.sig %data : <i8>\n %q = llhd.prb %a : i8',
    'cyclic_initial': '%a = llhd.sig %pb : <i8>\n %b = llhd.sig %pa : <i8>\n %pa = llhd.prb %a : i8\n %pb = llhd.prb %b : i8\n %q = llhd.prb %a : i8',
    'effectful_initial': '%v = func.call @effect() : () -> i8\n %a = llhd.sig %v : <i8>\n %q = llhd.prb %a : i8',
}
for name, body in cases.items():
    source = args.work / (name + '.mlir')
    source.write_text('module {\n func.func private @effect() -> i8\n hw.module @Test(in %data: i8, out q: i8) {\n' + body + '\n hw.output %q : i8\n }\n}\n')
    result = subprocess.run([str(args.circt_opt), str(source),
                             '--load-pass-plugin=' + str(library),
                             '--pass-pipeline=builtin.module(hw.module(chipyard-snapshot-signal-initializers))'],
                            capture_output=True, text=True)
    (args.work / (name + '.log')).write_text(result.stdout + result.stderr)
    assert result.returncode != 0 and 'native signal initialization requires a pure startup value' in result.stderr, result.stderr
print('PASS: chained nonzero startup values and live drives remain distinct; runtime, cyclic and effectful initializers diagnosed')

global_source = '''module {
  llhd.global_signal @seed : i8 init {
    %v = hw.constant 17 : i8
    llhd.yield %v : i8
  }
  llhd.global_signal @default_zero : i8
  llhd.global_signal @derived : i8 init {
    %r = llhd.get_global_signal @seed : <i8>
    %s = llhd.prb %r : i8
    %z = llhd.get_global_signal @default_zero : <i8>
    %d = llhd.prb %z : i8
    %five = hw.constant 5 : i8
    %v = comb.add %s, %d, %five : i8
    llhd.yield %v : i8
  }
  hw.module @Reader(out q: i8) {
    %r = llhd.get_global_signal @derived : <i8>
    %q = llhd.prb %r : i8
    hw.output %q : i8
  }
}
'''
source = args.work / 'global-positive.mlir'
output = args.work / 'global-positive-output.mlir'
source.write_text(global_source)
subprocess.run([str(args.circt_opt), str(source), '--load-pass-plugin=' + str(library),
                '--pass-pipeline=builtin.module(chipyard-snapshot-global-signal-initializers)',
                '-o', str(output)], check=True)
text = output.read_text()
body = text.split('llhd.global_signal @derived', 1)[1].split('hw.module @Reader', 1)[0]
assert 'hw.constant 22 : i8' in body and 'llhd.prb' not in body, text
for name, initializer in {
    'global_effect': '%unused = func.call @effect() : () -> i8\n %v = hw.constant 0 : i8',
    'global_runtime': '%v = func.call @effect() : () -> i8',
    'global_cycle': '%r = llhd.get_global_signal @shared : <i8>\n %v = llhd.prb %r : i8',
}.items():
    source = args.work / (name + '.mlir')
    source.write_text('module {\n func.func private @effect() -> i8\n llhd.global_signal @shared : i8 init {\n' + initializer + '\n llhd.yield %v : i8\n }\n}\n')
    result = subprocess.run([str(args.circt_opt), str(source), '--load-pass-plugin=' + str(library),
                             '--pass-pipeline=builtin.module(chipyard-snapshot-global-signal-initializers)'],
                            text=True, capture_output=True)
    (args.work / (name + '.log')).write_text(result.stdout + result.stderr)
    assert result.returncode != 0 and 'native global signal initialization requires a pure constant startup value' in result.stderr, result.stderr
print('PASS: global nonzero/default startup dependencies; unused initializer effects and global cycles diagnosed')

# A module static can snapshot shared storage without retaining a live global
# read. Global declarations remain shared; only the initializer is detached.
source = args.work / 'module-from-global.mlir'
output = args.work / 'module-from-global-output.mlir'
source.write_text(global_source.replace('%q = llhd.prb %r : i8', '%initial = llhd.prb %r : i8\n %local = llhd.sig %initial : <i8>\n %q = llhd.prb %local : i8'))
subprocess.run([str(args.circt_opt), str(source), '--load-pass-plugin=' + str(library),
                '--load-pass-plugin=' + str(memory),
                '--pass-pipeline=builtin.module(chipyard-prepare-native-llhd)',
                '-o', str(output)], check=True)
body = output.read_text().split('hw.module @Reader', 1)[1]
assert 'hw.constant 22 : i8' in body and 'llhd.prb' not in body, body
print('PASS: module-local initialization snapshots shared nonzero startup state')
