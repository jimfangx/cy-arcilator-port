#!/usr/bin/env python3
"""Resolve observed array lanes without hiding real or undefined feedback."""
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
                    default=sim / 'generated-src/array-feedback-guards')
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
library = plugin(args.circt_opt, 'c++', sim / 'generated-src/compiler')

body = '''
%i0 = hw.constant 0 : i2
%i1 = hw.constant 1 : i2
%i2 = hw.constant 2 : i2
%i3 = hw.constant 3 : i2
%one = hw.constant 1 : i2
%inverse = comb.xor %data, %i3 : i2
%increment = comb.add %data, %one : i2
%root = comb.mux %select, %fallback, %filled : !hw.array<4xi2>
%fill0 = hw.array_inject %root[%i0], %data : !hw.array<4xi2>, i2
%fill1 = hw.array_inject %fill0[%i1], %inverse : !hw.array<4xi2>, i2
%filled = hw.array_inject %fill1[%i2], %increment : !hw.array<4xi2>, i2
'''


def source(body, read, output='i2', output_value='%value', extra=''):
    return f'''module {{
      {extra}
      hw.module @ArcModel(in %select: i1, in %data: i2, in %index: i2, in %fallback: !hw.array<4xi2>, out result: {output}) {{
        {body}
        {read}
        hw.output {output_value} : {output}
      }}
    }}'''


def lower(name, text):
    path, output = args.work / (name + '.mlir'), args.work / (name +
                                                              '-lowered.mlir')
    path.write_text(text)
    result = subprocess.run([
        args.circt_opt, path, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-partition{max-ops=0})', '-o',
        output
    ],
                            capture_output=True,
                            text=True)
    assert result.returncode == 0, result.stderr
    return output, result.stderr


def convert(path, success):
    result = subprocess.run([
        args.circt_opt, path, '--canonicalize', '--convert-to-arcs', '-o',
        path.with_suffix('.arc.mlir')
    ],
                            capture_output=True,
                            text=True)
    assert (result.returncode == 0) == success, result.stderr
    if not success:
        assert 'combinational loop detected' in result.stderr, result.stderr


read = '%value = hw.array_get %root[%i0] : !hw.array<4xi2>, i2'
path, log = lower('element', source(body, read))
assert 'Resolved 1 cyclic aggregate element reads' in log
assert 'hw.array_inject' not in path.read_text()
convert(path, True)
# Stock conversion rejects the original false aggregate cycle.
convert(args.work / 'element.mlir', False)
slice_read = '%value = hw.array_slice %root[%i1] : (!hw.array<4xi2>) -> !hw.array<2xi2>'
path, log = lower('slice', source(body, slice_read, '!hw.array<2xi2>'))
assert 'Resolved 1 cyclic aggregate element reads' in log
assert 'hw.array_inject' not in path.read_text()
convert(path, True)

alias = '!hw.typealias<@types::@slice_t, !hw.array<2xi2>>'
alias_source = source(body, slice_read,
                      '!hw.array<2xi2>').replace('!hw.array<2xi2>', alias)
alias_source = alias_source.replace(
    'module {',
    'module {\nhw.type_scope @types { hw.typedecl @slice_t : !hw.array<2xi2> }',
    1)
path, log = lower('alias_slice', alias_source)
assert 'Resolved 1 cyclic aggregate element reads' in log
assert '-> ' + alias in path.read_text()
convert(path, True)

for selected, expected in ((False, 10), (True, 4)):
    # These constants independently check lane order and a nonzero slice offset.
    constant_body = body.replace('%select', '%choice').replace(
        '%data', '%digit').replace('%fallback', '%known')
    constants = f'''%choice = hw.constant {str(selected).lower()}
      %digit = hw.constant 1 : i2
      %k0 = hw.constant 2 : i2
      %k1 = hw.constant 0 : i2
      %k2 = hw.constant 1 : i2
      %k3 = hw.constant 3 : i2
      %known = hw.array_create %k3, %k2, %k1, %k0 : i2
    '''
    # data=1: lane1=1 xor 3=2, lane2=1+1=2, hence 0b1010.
    path, _ = lower(
        'constant_' + str(int(selected)),
        source(
            constants + constant_body, slice_read +
            '\n%packed = hw.bitcast %value : (!hw.array<2xi2>) -> i4', 'i4',
            '%packed'))
    folded = path.with_suffix('.folded.mlir')
    subprocess.run([args.circt_opt, path, '--canonicalize', '-o', folded],
                   check=True)
    match = re.search(r'hw.constant (-?\d+) : i4', folded.read_text())
    assert match and int(match[1]) % 16 == expected, folded.read_text()

guards = [
    ('unwritten', body, read.replace('%i0]', '%i3]')),
    ('partial_branch', body.replace('%fallback, %filled', '%fill0, %root'),
     read.replace('%i0]', '%i1]')),
    ('dynamic_read', body, read.replace('%i0]', '%index]')),
    ('dynamic_write', body.replace('%root[%i0]', '%root[%index]'), read),
    ('scalar_feedback',
     body.replace('%data : !hw.array<4xi2>', '%feedback : !hw.array<4xi2>') +
     '\n%feedback = comb.xor %value, %data : i2', read),
]
for name, tree, access in guards:
    path, _ = lower(name, source(tree, access))
    convert(path, False)

# A whole-array observer or side-effecting call keeps the original aggregate
# cone live even when another individual read can be resolved.
whole = source(body,
               read).replace('out result: i2',
                             'out result: i2, out observed: !hw.array<4xi2>')
whole = whole.replace('hw.output %value : i2',
                      'hw.output %value, %root : i2, !hw.array<4xi2>')
path, log = lower('whole_array', whole)
assert 'Resolved 1 cyclic aggregate element reads' in log
convert(path, False)
effect = read + '\n%unwritten = hw.array_get %root[%i3] : !hw.array<4xi2>, i2\nsim.func.dpi.call @touch(%unwritten) : (i2) -> ()'
path, _ = lower(
    'effect',
    source(body, effect, extra='sim.func.dpi private @touch(in %x: i2)'))
assert 'sim.func.dpi.call @touch(' in path.read_text()
convert(path, False)

# OOB accesses are undefined. The custom pass must leave them alone rather than
# inventing a lane value; checking before canonicalization preserves the guard.
oob = body.replace('4xi2', '3xi2')
oob_read = read.replace('%i0]', '%i3]').replace('4xi2', '3xi2')
for name, tree, access in (('oob_read', oob, oob_read),
                           ('oob_write', oob.replace('%root[%i0]',
                                                     '%root[%i3]'),
                            read.replace('4xi2', '3xi2'))):
    text = source(tree, access).replace('in %fallback: !hw.array<4xi2>',
                                        'in %fallback: !hw.array<3xi2>')
    path, log = lower(name, text)
    assert 'Resolved ' not in log and 'hw.array_get' in path.read_text()
path, log = lower('acyclic',
                  source(body.replace('%root[%i0]', '%fallback[%i0]'), read))
assert 'Resolved ' not in log and 'hw.array_get' in path.read_text()
convert(path, True)
print(
    'PASS: cyclic array element/slice resolution, declared alias types, nonzero slice offset and lane order; '
    'real scalar/array feedback, partial/dynamic/OOB accesses, whole-array observers and DPI effects retained',
    flush=True)
