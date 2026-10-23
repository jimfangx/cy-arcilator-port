#!/usr/bin/env python3
"""Numerically check aggregate array constants through native lowering."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(SIM / 'compiler'))
from partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    args = parser.parse_args()
    sdk = args.circt_bin.resolve()
    work = SIM / 'generated-src/aggregate-array-test'
    work.mkdir(parents=True, exist_ok=True)
    library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/LowerArrays.cpp')

    def run(*command):
        subprocess.run([str(value) for value in command], check=True)

    run(
        sdk / 'circt-opt', HERE / 'aggregate_arrays.mlir',
        '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-lower-arrays,lower-arc-to-llvm,reconcile-unrealized-casts)',
        '-o', work / 'llvm.mlir')
    run(sdk / 'mlir-translate', '--mlir-to-llvmir', work / 'llvm.mlir', '-o',
        work / 'model.ll')
    for level in (0, 2):
        run(sdk / 'opt', f'-O{level}', work / 'model.ll', '-o',
            work / f'model-O{level}.bc')
        run(sdk / 'llc', f'-O{level}', '-relocation-model=pic',
            '-filetype=obj', work / f'model-O{level}.bc', '-o',
            work / f'model-O{level}.o')
        run(*shlex.split(os.environ.get('CXX', 'c++')), f'-O{level}',
            HERE / 'aggregate_arrays.cpp', work / f'model-O{level}.o', '-o',
            work / f'test-O{level}')
        run(work / f'test-O{level}')


if __name__ == '__main__':
    main()
