#!/usr/bin/env python3
"""Check known runtime values and literal X/Z case comparisons before Arc."""
import argparse
from pathlib import Path
import random
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-verilog', type=Path, required=True)
parser.add_argument('--circt-bin', type=Path)
parser.add_argument('--work', type=Path, required=True)
parser.add_argument('--expect-bug', action='store_true')
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
checks, failures = 0, []
random = random.Random(9371)
for width in [1, 3, 7, 13, 65, 129]:
    source, output = args.work / f'bits{width}.sv', args.work / f'bits{width}.mlir'
    source.write_text(f'''module KnownBits(input logic [{width - 1}:0] value,
  output logic hot0, hot, unknown_bit, eq_x, ne_z, x_eq, z_ne,
  output logic literal_xx, literal_xz);
assign hot0 = $onehot0(value);
assign hot = $onehot(value);
assign unknown_bit = $isunknown(value);
assign eq_x = value === {{{width}{{1'bx}}}};
assign ne_z = value !== {{{width}{{1'bz}}}};
assign x_eq = {{{width}{{1'bx}}}} === value;
assign z_ne = {{{width}{{1'bz}}}} !== value;
assign literal_xx = 7'b10x01z0 === 7'b10x01z0;
assign literal_xz = 7'bxxxxxxx === 7'bzzzzzzz;
endmodule
''')
    subprocess.run([
        args.circt_verilog, '--ir-hw', '--infer-clock-ports=false',
        '--top=KnownBits', source, '-o', output
    ],
                   check=True)
    text = output.read_text()
    definitions = re.findall(r'^\s*(%[\w-]+) = (.*)$', text, re.M)
    outputs = re.search(r'hw.output (.*?) :', text)[1].split(', ')
    samples = list(range(1 << width)) if width <= 13 else [
        0, (1 << width) - 1, *[1 << bit for bit in range(width)],
        *[random.getrandbits(width) for _ in range(512)]
    ]
    for value in samples:
        values = {'%value': value}
        for name, operation in definitions:
            operands = [values[n] for n in re.findall(r'%[\w-]+', operation)]
            typed = re.search(r': i(\d+)$', operation)
            bits = int(typed[1]) if typed else 1
            if operation.startswith('hw.constant '):
                token = operation.split()[1]
                result = {'true': 1, 'false': 0}.get(token)
                if result is None:
                    result = int(token)
            elif operation.startswith('comb.add '):
                result = sum(operands)
            elif operation.startswith('comb.and '):
                result = operands[0]
                for operand in operands[1:]:
                    result &= operand
            elif operation.startswith('comb.xor '):
                result = operands[0]
                for operand in operands[1:]:
                    result ^= operand
            elif operation.startswith('comb.parity '):
                result, bits = operands[0].bit_count() & 1, 1
            elif operation.startswith(('comb.icmp eq ', 'comb.icmp ceq ')):
                result, bits = int(operands[0] == operands[1]), 1
            elif operation.startswith(('comb.icmp ne ', 'comb.icmp cne ')):
                result, bits = int(operands[0] != operands[1]), 1
            else:
                raise ValueError(operation)
            values[name] = result & ((1 << bits) - 1)
        expected = [
            value.bit_count() <= 1,
            value.bit_count() == 1, False, False, True, False, True, True,
            False
        ]
        for name, wanted in zip(outputs, expected, strict=True):
            checks += 1
            if values[name] != wanted:
                failures.append((width, value, name, values[name], wanted))
if args.expect_bug:
    assert failures, 'old frontend unexpectedly passes the negative control'
    print(
        f'PASS: reproduced {len(failures)} old-frontend errors in {checks} checks'
    )
else:
    assert not failures, failures[:8]
    print(
        f'PASS: {checks} onehot0/onehot/isunknown and case-comparison checks; '
        'known widths 1/3/7/13/65/129 and preserved literal X/Z comparisons')
