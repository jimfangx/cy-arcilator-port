#!/usr/bin/env python3
"""Keep immutable literals local to hardware partitions and root outputs valid."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/partition-literal-tests')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    library = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler')
    source = args.work / 'input.mlir'
    source.write_text('''module {
      hw.module @ArcModel(in %x: i17, in %choose: i1, out y: i17, out complement: i1,
          out literal: i17, out array: !hw.array<2xi17>, out truth: i1) {
        %true = hw.constant true
        %one = hw.constant 1 : i1
        %literal = hw.constant 12345 : i17
        %array = hw.aggregate_constant [17 : i17, 31 : i17] : !hw.array<2xi17>
        %lane = hw.array_get %array[%one] : !hw.array<2xi17>, i1
        %sum = comb.add %x, %literal : i17
        %xor = comb.xor %sum, %lane : i17
        %not = comb.xor %choose, %true : i1
        hw.output %xor, %not, %literal, %array, %true : i17, i1, i17, !hw.array<2xi17>, i1
      }
    }''')
    for size in [1, 2, 3]:
        target = args.work / f'parts-{size}.mlir'
        subprocess.run([
            args.circt_opt, source, '--load-pass-plugin=' + str(library),
            f'--pass-pipeline=builtin.module(chipyard-partition{{max-ops={size}}})',
            '-o', target
        ],
                       check=True)
        text = target.read_text()
        # Successful verification checks that constant root outputs were not
        # moved into a private partition while their root uses stayed behind.
        root = text[text.index('  hw.module @ArcModel'):]
        assert 'hw.constant 12345 : i17' in root
        assert 'hw.aggregate_constant [17 : i17, 31 : i17]' in root
        assert 'hw.constant true' in root
        # Literal-only roots need no foreign dynamic input ports. Every
        # private partition consumes only an original input or a computed
        # intermediate, and constant expressions remain visible locally.
        parts = re.findall(r'  hw.module private @ArcModel_part_\d+.*?\n  }',
                           text, re.S)
        assert parts
        assert any('hw.constant true' in part and 'comb.xor' in part
                   for part in parts)
        assert any('hw.constant 12345 : i17' in part and 'comb.add' in part
                   for part in parts)
        assert any('hw.aggregate_constant' in part and 'hw.array_get' in part
                   for part in parts)
        literals = set(
            re.findall(
                r'^\s*(%[\w.-]+) = hw\.(?:constant|aggregate_constant) ', root,
                re.M))
        for line in root.splitlines():
            if 'hw.instance ' in line:
                assert not literals.intersection(re.findall(r'%[\w.-]+',
                                                            line)), line
    print(
        'PASS: scalar/Boolean/aggregate literal rematerialization at three partition sizes; direct constant root outputs retain valid ownership',
        flush=True)


if __name__ == '__main__':
    main()
