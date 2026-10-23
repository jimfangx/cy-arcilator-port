#!/usr/bin/env python3
"""Check constant string comparisons without changing dynamic operations."""
import argparse
from pathlib import Path
import re
import sys

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim))
from compiler.partition import plugin
from native_hw import prepare_native_signals

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-opt', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=sim / 'generated-src/constant-string-tests')
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
library = plugin(args.circt_opt, 'c++', sim / 'generated-src/compiler',
                 sim / 'compiler/SignalState.cpp')
strings = [b'', b'a', b'aa', b'b', b'\x7f', b'\x80', 'é'.encode(), b'a\0b']
predicates = [('eq', lambda a, b: a == b), ('ne', lambda a, b: a != b),
              ('lt', lambda a, b: a < b), ('le', lambda a, b: a <= b),
              ('gt', lambda a, b: a > b), ('ge', lambda a, b: a >= b)]
count = len(strings)**2 * len(predicates)
lines = [f'module {{ hw.module @Literals(out result: i{count}) {{']
for index, value in enumerate(strings):
    spelling = ''.join(f'\\{byte:02X}' for byte in value)
    lines.append(f'%s{index} = sim.string.literal "{spelling}"')
expected = 0
values = []
for left, a in enumerate(strings):
    for right, b in enumerate(strings):
        for predicate, reference in predicates:
            index = len(values)
            name = '%v' + str(index)
            lines.append(
                f'{name} = sim.string.cmp {predicate} %s{left}, %s{right} : !sim.dstring'
            )
            values.append(name)
            expected |= int(reference(a, b)) << index
lines += [
    '%packed = comb.concat ' + ', '.join(reversed(values)) + ' : ' +
    ', '.join(['i1'] * count), f'hw.output %packed : i{count}', '}',
    'func.func @dynamic(%left: !sim.dstring, %right: !sim.dstring) -> i1 {',
    '%value = sim.string.cmp eq %left, %right : !sim.dstring',
    'func.return %value : i1', '}', '}'
]
source, output = args.work / 'input.mlir', args.work / 'output.mlir'
source.write_text('\n'.join(lines) + '\n')
prepare_native_signals(source, output, args.circt_opt, library)
text = output.read_text()
match = re.search(r'hw.constant (-?\d+) : i' + str(count) + r'\b', text)
assert match and int(match[1]) % (1 << count) == expected
assert text.count('sim.string.cmp') == 1 and 'sim.string.literal' not in text
print(
    'PASS: 384 constant string predicates, prefixes, UTF-8/unsigned bytes and embedded NUL; dynamic comparison preserved'
)
