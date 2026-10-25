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

# Declaration DPI effects have a distinct startup frame. Runtime inputs,
# unbound context and escaping dependencies must not be frozen into it.
# Mutable declarations also require the native-storage pipeline.
def declaration_probe(name, body, *, context=False, task=False, metadata=True,
                      success=False, native_storage=False):
    attrs = (' attributes {chipyard.dpi_decl = {context = ' + str(context).lower() +
             ', task = ' + str(task).lower() + '}}') if metadata else ''
    source = args.work / ('declaration-' + name + '.mlir')
    source.write_text('module {\n sim.func.dpi private @seed(in %tag: i32, return return: i32)' +
        attrs + '\n hw.module @Startup(in %data: i32, out q: i32) {\n' +
        ' %tag = hw.constant 17 : i32\n %time = llhd.constant_time <0ns, 0d, 1e>\n' + body +
        '\n }\n}\n')
    plugins = ['--load-pass-plugin=' + str(library)]
    pipeline = 'hw.module(chipyard-snapshot-signal-initializers)'
    if native_storage:
        storage = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                         sim / 'generated-src/compiler', sim / 'compiler/SignalMemory.cpp')
        plugins.append('--load-pass-plugin=' + str(storage))
        pipeline = ('chipyard-prepare-declaration-initializers,'
                    'hw.module(chipyard-snapshot-signal-initializers{native-storage=true})')
    result = subprocess.run([str(args.circt_opt), str(source), *plugins,
        '--pass-pipeline=builtin.module(' + pipeline + ')'],
        capture_output=True, text=True)
    (args.work / ('declaration-' + name + '.log')).write_text(result.stdout + result.stderr)
    if success:
        assert result.returncode == 0, result.stderr
        assert result.stdout.count('seq.initial(') == 1, result.stdout
        assert result.stdout.count('sim.func.dpi.call @seed(') == 1, result.stdout
        if native_storage:
            assert 'ArcSignalMemory' in result.stdout and 'chipyard_arc_signal_write' in result.stdout
        assert result.stdout.index('seq.initial(') < result.stdout.index('sim.func.dpi.call')
    else:
        assert result.returncode != 0 and ('native signal initialization requires a pure startup value'
                                           in result.stderr), result.stderr

base = (' %v = sim.func.dpi.call @seed(%tag) : (i32) -> i32\n'
        ' %a = llhd.sig %v : <i32>\n %q = llhd.prb %a : i32\n hw.output %q : i32')
declaration_probe('closed', base, success=True)
declaration_probe('shared-call', base.replace(' hw.output',
    ' %b = llhd.sig %v : <i32>\n hw.output'), success=True)
unused = (' %v = sim.func.dpi.call @seed(%tag) '
          '{chipyard.declaration_initializer} : (i32) -> i32\n hw.output %tag : i32')
declaration_probe('unused-tagged', unused, success=True)
declaration_probe('unused-live-input', unused.replace('@seed(%tag)', '@seed(%data)'))
declaration_probe('input', base.replace('@seed(%tag)', '@seed(%data)'))
declaration_probe('context', base, context=True)
declaration_probe('task', base, task=True)
declaration_probe('unknown-ABI', base, metadata=False)
declaration_probe('write', base.replace(' hw.output', ' llhd.drv %a, %data after %time : i32\n hw.output'))
declaration_probe('escape', base.replace('hw.output %q', 'hw.output %v'))
declaration_probe('cycle', base.replace('@seed(%tag)', '@seed(%q)'))
declaration_probe('mutable-derived', base.replace(' hw.output %q : i32',
    ' %b = llhd.sig %q : <i32>\n %pb = llhd.prb %b : i32\n'
    ' llhd.drv %b, %data after %time : i32\n hw.output %pb : i32'))
declaration_probe('effect-order',
    ' %first = sim.func.dpi.call @seed(%later_value) : (i32) -> i32\n'
    ' %later = sim.func.dpi.call @seed(%tag) : (i32) -> i32\n'
    ' %a = llhd.sig %first : <i32>\n %b = llhd.sig %later : <i32>\n'
    ' %later_value = llhd.prb %b : i32\n %q = llhd.prb %a : i32\n hw.output %q : i32')
declaration_probe('constant-folded-writable',
    ' %v = sim.func.dpi.call @seed(%tag) : (i32) -> i32\n'
    ' %a = llhd.sig %v : <i32>\n %q = llhd.prb %a : i32\n'
    ' %zero = hw.constant 0 : i32\n %z = comb.and %q, %zero : i32\n'
    ' %b = llhd.sig %z : <i32>\n llhd.drv %b, %data after %time : i32\n'
    ' hw.output %q : i32', success=True)
print('PASS: closed declaration DPI startup frame; ten unsafe dependency/effect cases diagnosed')

# Exercise the production storage preparation, including its deferred frontier,
# rather than relying only on rejection by the standalone snapshot pass.
for name, body, options in [
    ('write', base.replace(' hw.output', ' llhd.drv %a, %data after %time : i32\n hw.output'), {'success': True}),
    ('derived-write', base.replace(' hw.output %q : i32',
        ' %b = llhd.sig %q : <i32>\n %pb = llhd.prb %b : i32\n'
        ' llhd.drv %b, %data after %time : i32\n hw.output %pb : i32'), {'success': True}),
    ('input', base.replace('@seed(%tag)', '@seed(%data)'), {}),
    ('unused-input', unused.replace('@seed(%tag)', '@seed(%data)'), {}),
    ('context', base, {'context': True}),
    ('task', base, {'task': True}),
    ('unknown-ABI', base, {'metadata': False}),
    ('escape', base.replace('hw.output %q', 'hw.output %v'), {}),
    ('cycle', base.replace('@seed(%tag)', '@seed(%q)'), {}),
    ('effect-order',
        ' %first = sim.func.dpi.call @seed(%later_value) : (i32) -> i32\n'
        ' %later = sim.func.dpi.call @seed(%tag) : (i32) -> i32\n'
        ' %a = llhd.sig %first : <i32>\n %b = llhd.sig %later : <i32>\n'
        ' %later_value = llhd.prb %b : i32\n %q = llhd.prb %a : i32\n hw.output %q : i32', {}),
]:
    declaration_probe('native-' + name, body, native_storage=True, **options)
print('PASS: native storage preparation accepts writable roots and retains eight unsafe initializer diagnostics')

# Native scope outputs have a provider contract and are available before the
# model initial function. Other model inputs cannot be asserted immutable by
# an unvalidated conversion cast.
startup = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                 sim / 'generated-src/compiler', sim / 'compiler/StartupState.cpp')
capture = """module {
 func.func private @seed(i64) -> i64
 hw.module @Capture(in %scope: i64, out value: i64) {
  %cap = builtin.unrealized_conversion_cast %scope : i64 to !seq.immutable<i64>
     {chipyard.native_startup_scope}
  %imm = seq.initial(%cap) {
   ^bb0(%arg: i64):
    %v = func.call @seed(%arg) : (i64) -> i64
    seq.yield %v : i64
  } : (!seq.immutable<i64>) -> !seq.immutable<i64>
  %v = seq.from_immutable %imm : (!seq.immutable<i64>) -> i64
  hw.output %v : i64
 }
}
"""

def capture_probe(name, text, allowed='scope', success=False):
    source = args.work / ('startup-capture-' + name + '.mlir')
    source.write_text(text)
    result = subprocess.run([str(args.circt_opt), str(source),
        '--load-pass-plugin=' + str(startup),
        '--pass-pipeline=builtin.module(chipyard-lower-native-startup-state{scope-inputs=' + allowed + '})'],
        capture_output=True, text=True)
    (args.work / ('startup-capture-' + name + '.log')).write_text(result.stdout + result.stderr)
    if success:
        assert result.returncode == 0, result.stderr
        assert 'arc.initial' in result.stdout and 'call @seed' in result.stdout
        assert 'seq.initial' not in result.stdout and 'unrealized_conversion_cast' not in result.stdout
    else:
        assert result.returncode != 0 and ('native startup capture requires a verified initialized binding input'
                                           in result.stderr), result.stderr

capture_probe('scope', capture, success=True)
capture_probe('no-contract', capture, allowed='')
capture_probe('ordinary-input', capture.replace('%scope', '%data'))
capture_probe('wrong-width', capture.replace('i64', 'i32'))
capture_probe('multiple-operands', capture.replace('%scope : i64 to', '%scope, %scope : i64, i64 to'))
capture_probe('escape', capture.replace('  %v = seq.from_immutable',
    '  %escape = builtin.unrealized_conversion_cast %cap : !seq.immutable<i64> to i64\n'
    '  %v = seq.from_immutable'))
capture_probe('arithmetic', capture.replace('  %cap =',
    '  %sum = comb.add %scope, %scope : i64\n  %cap =').replace('cast %scope :', 'cast %sum :'))
print('PASS: native startup scope capture and verified SDK state lowering; six unsafe captures diagnosed')
