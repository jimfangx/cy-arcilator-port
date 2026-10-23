#!/usr/bin/env python3
"""Check source-level SVA next-cycle operators against finite trace semantics."""
import argparse
from itertools import product
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--circt-verilog', type=Path, required=True)
p.add_argument('--circt-bin', type=Path)
p.add_argument('--work', type=Path, required=True)
p.add_argument('--expect-bug', action='store_true')
a = p.parse_args()
a.work.mkdir(parents=True, exist_ok=True)
source = a.work / 'operators.sv'
source.write_text('''module Operators(input logic clk, a, b, c);
assert property (@(posedge clk) a |=> b);
assert property (@(posedge clk) a |-> b);
assert property (@(posedge clk) (a ##2 b) |=> c);
assert property (@(posedge clk) a #-# b);
assert property (@(posedge clk) a #=# b);
endmodule
''')
output = a.work / 'operators.mlir'
subprocess.run([
    a.circt_verilog, '--ir-hw', '--infer-clock-ports=false', '--top=Operators',
    source, '-o', output
],
               check=True)
defs, roots = {}, []
for line in output.read_text().splitlines():
    m = re.match(r'\s*(%[\w.]+) = (.*)', line)
    if m:
        defs[m[1]] = m[2]
    m = re.search(r'verif.clocked_assert (%[\w.]+)', line)
    if m:
        roots.append(m[1])
assert len(roots) == 5
checks = differences = 0
for samples in product(range(8), repeat=4):
    trace = [{
        n: bool(bits & (1 << i))
        for i, n in enumerate('abc')
    } for bits in samples]

    def scalar(v, t):
        if v[1:] in 'abc':
            return trace[t][v[1:]]
        op = defs[v]
        if op.startswith('hw.constant '):
            return op.split()[1] == 'true'
        operands = re.findall(r'%[\w.]+', op)
        values = [scalar(x, t) for x in operands]
        if op.startswith('comb.xor '):
            return sum(values) % 2 != 0
        raise AssertionError(op)

    def sequence(v, start):
        if start >= len(trace):
            return set()
        op = defs.get(v, '')
        operands = re.findall(r'%[\w.]+', op)
        if op.startswith('ltl.delay '):
            lo, length = map(int, re.search(r', (\d+), (\d+)', op).groups())
            return set().union(*(sequence(operands[0], start + shift)
                                 for shift in range(lo, lo + length + 1)))
        if op.startswith('ltl.concat '):
            ends = {start}
            for operand in operands:
                ends = set().union(*(sequence(operand, t) for t in ends))
            return ends
        return {start} if scalar(v, start) else set()

    def property_value(v, start):
        op = defs.get(v, '')
        operands = re.findall(r'%[\w.]+', op)
        if op.startswith('ltl.implication '):
            return all(
                property_value(operands[1], t)
                for t in sequence(operands[0], start))
        if op.startswith('ltl.not '):
            return not property_value(operands[0], start)
        return scalar(v, start)

    # These reference formulas follow the SV source directly, with all values
    # indexed forward in time. They share no production monitor implementation.
    expected = [
        not trace[0]['a'] or trace[1]['b'], not trace[0]['a'] or trace[0]['b'],
        not (trace[0]['a'] and trace[2]['b']) or trace[3]['c'], trace[0]['a']
        and trace[0]['b'], trace[0]['a'] and trace[1]['b']
    ]
    actual = [property_value(root, 0) for root in roots]
    differences += sum(x != y for x, y in zip(actual, expected))
    checks += len(roots)
if a.expect_bug:
    assert differences, 'reference compiler no longer reproduces the delay-placement bug'
    print(
        f'PASS: reproduced {differences} incorrect results in pinned frontend')
else:
    assert differences == 0, f'{differences} SVA operator results differ'
    print(f'PASS: {checks} source-level SVA operator truth checks')
