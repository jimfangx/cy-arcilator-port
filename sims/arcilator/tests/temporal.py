#!/usr/bin/env python3
"""Ensure unsupported temporal operators cannot become partial native monitors."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--circt-opt', type=Path, required=True)
p.add_argument('--work', type=Path, required=True)
a = p.parse_args()
a.work.mkdir(parents=True, exist_ok=True)
library = plugin(a.circt_opt, 'c++', SIM / 'generated-src/compiler',
                 SIM / 'compiler/TemporalProperties.cpp')
guards = [
    '%s = ltl.delay %a, 1 : i1',
    '%s = ltl.delay %a, 4097, 0 : i1',
    '%s = ltl.repeat %a, 0, 0 : i1',
    '%s = ltl.repeat %a, 1 : i1',
    '%s = ltl.clock %a, posedge %other : i1',
    '%s = ltl.clocked_atom %a, negedge %other : i1',
    '%s = ltl.clocked_delay %a, posedge %other, 1, 0 : i1',
    '%u = ltl.delay %a, 1 : i1\n%bnd = ltl.delay %b, 1, 0 : i1\n'
    '%s = ltl.or %bnd, %u : !ltl.sequence, !ltl.sequence',
    '%u = ltl.delay %a, 1 : i1\n%bnd = ltl.delay %b, 1, 0 : i1\n'
    '%s = ltl.and %bnd, %u : !ltl.sequence, !ltl.sequence',
    '%s = ltl.repeat %a, 4097, 0 : i1',
]
lines = [
    'module { hw.module @Guards(in %clock: i1, in %other: i1, '
    'in %enable: i1, in %a: i1, in %b: i1) {'
]
for i, body in enumerate(guards):
    body = re.sub(r'%(s|u|bnd)\b', lambda m: '%' + m[1] + str(i), body)
    lines += [
        body, f'%p{i} = ltl.implication %s{i}, %b : !ltl.sequence, i1',
        f'verif.clocked_assert %p{i} if %enable, posedge %clock label "guard_{i}" : !ltl.property'
    ]
lines += [
    '%yes = hw.constant true', '%delay = ltl.delay %yes, 1, 0 : i1',
    '%seq = ltl.concat %a, %delay : i1, !ltl.sequence',
    '%temporal = ltl.implication %seq, %b : !ltl.sequence, i1',
    '%not = ltl.not %temporal : !ltl.property',
    'verif.clocked_assert %not if %enable, posedge %clock label "not" : !ltl.property',
    '%or = ltl.or %temporal, %b : !ltl.property, i1',
    'verif.clocked_assert %or if %enable, posedge %clock label "or" : !ltl.property',
    'verif.clocked_assert %temporal if %enable, edge %clock label "both" : !ltl.property',
    'hw.output', '}', '}'
]
source, output = a.work / 'guards.mlir', a.work / 'lowered.mlir'
source.write_text('\n'.join(lines) + '\n')
subprocess.run([
    a.circt_opt, source, '--load-pass-plugin=' + str(library),
    '--pass-pipeline=builtin.module(chipyard-lower-temporal-properties)', '-o',
    output
],
               check=True)
text = output.read_text()
for label in [
        *(f'guard_{i}' for i in range(len(guards))), 'not', 'or', 'both'
]:
    assert re.search(r'label "' + label + r'" : !ltl.property', text), label
assert 'arc_property_' not in text, 'failed admission left a partial monitor'
print(
    f'PASS: {len(guards)+3} temporal admission guards retain unsupported semantics'
)
