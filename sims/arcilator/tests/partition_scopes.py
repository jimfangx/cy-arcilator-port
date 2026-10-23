#!/usr/bin/env python3
"""Verify formatter operands remain typed correctly across sibling regions."""
import argparse
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
SIM = HERE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    args = parser.parse_args()
    work = SIM / 'generated-src/partition-scope-test'
    work.mkdir(parents=True, exist_ok=True)
    subprocess.run(['python3', str(SIM / 'partition.py'), '--input',
                    str(HERE / 'partition_scopes.mlir'), '--output', str(work),
                    '--top', 'ScopeHarness', '--circt-opt', str(args.circt_opt)],
                   check=True)
    # Parsing the transformed IR catches a formatter value substituted for
    # the integer with the same printed SSA ID in a sibling process.
    subprocess.run([str(args.circt_opt), str(work / 'partitioned.mlir'),
                    '-o', str(work / 'verified.mlir')], check=True)
    print('PASS: formatter types and operands across sibling procedural scopes',
          flush=True)


if __name__ == '__main__':
    main()
