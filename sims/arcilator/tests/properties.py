#!/usr/bin/env python3
"""Check pointwise property truth tables and preserve temporal semantics."""
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
                    default=sim / 'generated-src/property-tests')
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
library = plugin(args.circt_opt, 'c++', sim / 'generated-src/compiler',
                 sim / 'compiler/Properties.cpp')


def lower(name, text):
    source, output = args.work / (name + '.mlir'), args.work / (
        name + '-lowered.mlir')
    source.write_text(text)
    subprocess.run([
        args.circt_opt, source, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-lower-pointwise-properties)',
        '-o', output
    ],
                   check=True)
    return output.read_text()


lines = [
    'module { hw.module @Truth(in %clock: i1, in %enable: i1, out result: i1) {',
    '%false = hw.constant false'
]
expected, count = 0, 0
for case in range(16):
    a, b, c, d = [bool(case & (1 << i)) for i in range(4)]
    prefix = f'%v{case}_'
    for name, value in zip('abcd', (a, b, c, d)):
        lines.append(prefix + name + ' = hw.constant ' + str(value).lower())
    # Each root has an independent reference expression below. No production
    # lowering or textual Boolean evaluator computes the expected truth table.
    body = '''
%yes = ltl.boolean_constant true
%no = ltl.boolean_constant false
%not_a = ltl.not %a : i1
%imp = ltl.implication %a, %b : i1, i1
%not_c = ltl.not %c : i1
%joined = ltl.and %imp, %not_c : !ltl.property, !ltl.property
%nested = ltl.implication %d, %joined : i1, !ltl.property
%or = ltl.or %imp, %not_c : !ltl.property, !ltl.property
%and = ltl.and %imp, %not_c : !ltl.property, !ltl.property
%concat = ltl.concat %a, %b, %c : i1, i1, i1
%delay = ltl.delay %concat, 0, 0 : !ltl.sequence
%intersect = ltl.intersect %delay, %b : !ltl.sequence, i1
%repeat = ltl.repeat %delay, 1, 0 : !ltl.sequence
'''
    lines.append(re.sub(r'%([\w]+)', lambda m: prefix + m[1], body))
    references = [('yes', True, '!ltl.property'),
                  ('no', False, '!ltl.property'),
                  ('not_a', not a, '!ltl.property'),
                  ('imp', not a or b, '!ltl.property'),
                  ('nested', not d
                   or ((not a or b) and not c), '!ltl.property'),
                  ('or', not a or b or not c, '!ltl.property'),
                  ('and', (not a or b) and not c, '!ltl.property'),
                  ('concat', a and b and c, '!ltl.sequence'),
                  ('intersect', a and b and c, '!ltl.sequence'),
                  ('repeat', a and b and c, '!ltl.sequence')]
    for name, value, ty in references:
        edge = ('posedge', 'negedge', 'edge')[count % 3]
        lines.append(
            f'verif.clocked_assert {prefix}{name} if %enable, {edge} %clock label "check_{count}" : {ty}'
        )
        expected = (expected << 1) | int(value)
        count += 1
lines += ['hw.output %false : i1', '}', '}']
lowered = lower('truth', '\n'.join(lines) + '\n')
assertions = [
    line.strip() for line in lowered.splitlines()
    if 'verif.clocked_assert' in line
]
assert len(assertions) == count
roots = []
for index, assertion in enumerate(assertions):
    edge = ('posedge', 'negedge', 'edge')[index % 3]
    match = re.fullmatch(
        r'verif.clocked_assert (%[\w]+) if %enable, ' + edge +
        r' %clock label "check_' + str(index) + r'" : i1', assertion)
    assert match, assertion
    roots.append(match[1])
# Reuse the converted assertion operands as outputs, then let CIRCT's Boolean
# folder evaluate them independently of the property converter.
evaluated = re.sub(r'^\s*verif.clocked_assert .*\n', '', lowered, flags=re.M)
evaluated = evaluated.replace('out result : i1', f'out result : i{count}')
evaluated = re.sub(
    r'hw.output %\w+ : i1', '%packed = comb.concat ' + ', '.join(roots) +
    ' : ' + ', '.join(['i1'] * count) + f'\n    hw.output %packed : i{count}',
    evaluated)
source, output = args.work / 'evaluated.mlir', args.work / 'folded.mlir'
source.write_text(evaluated)
subprocess.run([args.circt_opt, source, '--canonicalize', '-o', output],
               check=True)
folded = output.read_text()
match = re.search(r'hw.constant (-?\d+) : i' + str(count) + r'\b', folded)
assert match and int(match[1]) % (1 << count) == expected, folded

guards = [('%delayed = ltl.delay %a, 1, 0 : i1', '%delayed', '!ltl.sequence'),
          ('%unbounded = ltl.delay %a, 0 : i1', '%unbounded', '!ltl.sequence'),
          ('%empty = ltl.repeat %a, 0, 0 : i1', '%empty', '!ltl.sequence'),
          ('%two = ltl.repeat %a, 2, 0 : i1', '%two', '!ltl.sequence'),
          ('%many = ltl.repeat %a, 1 : i1', '%many', '!ltl.sequence'),
          ('%clocked = ltl.clock %a, posedge %other : i1', '%clocked',
           '!ltl.sequence'),
          ('%atom = ltl.clocked_atom %a, negedge %other : i1', '%atom',
           '!ltl.sequence'),
          ('%cdelay = ltl.clocked_delay %a, posedge %other, 0, 0 : i1',
           '%cdelay', '!ltl.sequence')]
lines = [
    'module { hw.module @Temporal(in %clock: i1, in %other: i1, in %enable: i1, in %a: i1, in %b: i1) {'
]
for index, (body, name, ty) in enumerate(guards):
    lines += [
        body, f'%p{index} = ltl.implication %b, {name} : i1, {ty}',
        f'verif.clocked_assert %p{index} if %enable, posedge %clock label "guard_{index}" : !ltl.property'
    ]
# A convertible producer is still needed by a genuinely temporal consumer.
lines += [
    '%shared = ltl.not %a : i1',
    '%temporal = ltl.clock %shared, posedge %other : !ltl.property',
    'verif.clocked_assert %shared if %enable, posedge %clock label "shared" : !ltl.property',
    'verif.clocked_assert %temporal if %enable, posedge %clock label "temporal" : !ltl.property',
    'hw.output', '}', '}'
]
guarded = lower('temporal', '\n'.join(lines) + '\n')
for index in range(len(guards)):
    assert re.search(r'label "guard_' + str(index) + r'" : !ltl.property',
                     guarded)
assert re.search(r'label "shared" : i1', guarded)
assert re.search(r'label "temporal" : !ltl.property', guarded)
assert 'ltl.not' in guarded and 'ltl.clock ' in guarded
rejected = subprocess.run([
    sys.executable, sim / 'partition.py', '--input',
    args.work / 'temporal.mlir', '--output', args.work / 'temporal-partition',
    '--top', 'Temporal', '--circt-opt', args.circt_opt
],
                          capture_output=True,
                          text=True)
assert rejected.returncode != 0 and 'unrecognized clocked assertion' in rejected.stderr, rejected.stderr
print(
    f'PASS: {count} pointwise property truth checks; edge, enable and labels preserved; '
    f'{len(guards)} temporal cones and shared temporal users retained',
    flush=True)
