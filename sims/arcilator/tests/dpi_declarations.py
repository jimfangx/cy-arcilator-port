#!/usr/bin/env python3
"""Unify compatible DPI imports without changing call effects or C prototypes."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim))
from compiler.partition import plugin

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-opt', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=sim / 'generated-src/dpi-declaration-tests')
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
library = plugin(args.circt_opt, 'c++', sim / 'generated-src/compiler',
                 sim / 'compiler/Model.cpp')


def lower(name, text, success=True):
    source, output = args.work / (name + '.mlir'), args.work / (
        name + '-lowered.mlir')
    source.write_text(text)
    result = subprocess.run([
        args.circt_opt, source, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-coalesce-dpi)', '-o', output
    ],
                            capture_output=True,
                            text=True)
    if success:
        assert result.returncode == 0, result.stderr
        return output.read_text()
    assert result.returncode != 0 and "incompatible DPI declarations for C symbol 'shared'" in result.stderr, result.stderr
    assert 'previous declaration is here' in result.stderr


signature = 'in %x: i32, inout %a: i64, out y: i1, ref %pointer: !llvm.ptr, return result: i64'
renamed = signature.replace('%x', '%renamed_x').replace(
    '%a', '%renamed_a').replace('out y', 'out renamed_y').replace(
        '%pointer', '%renamed_pointer').replace('return result',
                                                'return renamed_result')
text = f'''module {{
  sim.func.dpi private @shared({signature})
  sim.func.dpi @left_dpi({renamed}) attributes {{verilogName = "shared"}}
  sim.func.dpi private @right_dpi({signature}) attributes {{verilogName = "shared"}}
  sim.func.dpi private @other({signature}) attributes {{verilogName = "other_c"}}
  func.func @calls(%x: i32, %a: i64, %pointer: !llvm.ptr) -> i64 {{
    %first:3 = sim.func.dpi.call @shared(%x, %a, %pointer) : (i32, i64, !llvm.ptr) -> (i64, i1, i64)
    %left:3 = sim.func.dpi.call @left_dpi(%x, %first#0, %pointer) : (i32, i64, !llvm.ptr) -> (i64, i1, i64)
    %right:3 = sim.func.dpi.call @right_dpi(%x, %left#0, %pointer) : (i32, i64, !llvm.ptr) -> (i64, i1, i64)
    %other:3 = sim.func.dpi.call @other(%x, %right#0, %pointer) : (i32, i64, !llvm.ptr) -> (i64, i1, i64)
    func.return %other#2 : i64
  }}
  module @nested {{
    sim.func.dpi private @left_dpi(in %x: i8, return result: i8) attributes {{verilogName = "nested_c"}}
    func.func @call(%x: i8) -> i8 {{
      %value = sim.func.dpi.call @left_dpi(%x) : (i8) -> i8
      func.return %value : i8
    }}
  }}
}}
'''
lowered = lower('compatible', text)
assert len(re.findall(r'sim.func.dpi (?:private )?@', lowered)) == 3
assert len(re.findall(r'sim.func.dpi.call @shared\(', lowered)) == 3
assert len(re.findall(r'sim.func.dpi.call @other\(', lowered)) == 1
assert len(re.findall(r'sim.func.dpi.call @left_dpi\(', lowered)) == 1
assert 'verilogName = "nested_c"' in lowered
assert lower('idempotent', lowered) == lowered

conflicts = [('width', 'in %x: i32', 'in %x: i64'),
             ('input_output', 'in %x: i32', 'out x: i32'),
             ('out_return', 'out x: i32', 'return x: i32'),
             ('input_ref', 'in %x: !llvm.ptr', 'ref %x: !llvm.ptr'),
             ('order', 'in %x: i32, out y: i64', 'out x: i32, in %y: i64'),
             ('arity', 'in %x: i32', 'in %x: i32, in %y: i32'),
             ('aggregate', 'in %x: !hw.array<2xi8>', 'in %x: i16')]
for name, left, right in conflicts:
    lower(name,
          f'''module {{
      sim.func.dpi private @first({left}) attributes {{verilogName = "shared"}}
      sim.func.dpi private @second({right}) attributes {{verilogName = "shared"}}
    }}''',
          success=False)
lower('attributes',
      '''module {
  sim.func.dpi private @first(in %x: i32) attributes {verilogName = "shared", chipyard.semantic = true}
  sim.func.dpi private @second(in %x: i32) attributes {verilogName = "shared", chipyard.semantic = false}
}''',
      success=False)
print(
    'PASS: compatible DPI aliases, renamed formals, implicit C names, call effects/order, '
    'independent symbols and nested scopes; eight incompatible C-prototype/metadata guards',
    flush=True)
