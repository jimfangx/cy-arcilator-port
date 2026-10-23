#!/usr/bin/env python3
"""Preserve live concat bits when the SDK removes zero-width operands."""
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
    parser.add_argument('--circt-bin', required=True, type=Path)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/zero-width-test')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    sdk = args.circt_bin.resolve()
    library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/Model.cpp')

    def run(*command):
        subprocess.run([str(value) for value in command], check=True)

    i0_library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                        SIM / 'generated-src/compiler',
                        SIM / 'compiler/RemoveI0Types.cpp')
    lowered = args.work / 'llvm.mlir'
    run(
        sdk / 'circt-opt', HERE / 'zero_width.mlir',
        '--load-pass-plugin=' + str(library),
        '--load-pass-plugin=' + str(i0_library),
        '--pass-pipeline=builtin.module(chipyard-normalize-arrays,'
        'chipyard-remove-i0-types,lower-arc-to-llvm,reconcile-unrealized-casts)',
        '-o', lowered)
    assert 'i0' not in lowered.read_text(
    ), 'zero-width types survived lowering'
    llvm = args.work / 'model.ll'
    run(sdk / 'mlir-translate', '--mlir-to-llvmir', lowered, '-o', llvm)
    for level in (0, 2):
        obj, executable = (args.work / f'model-O{level}.o',
                           args.work / f'test-O{level}')
        run(sdk / 'llc', f'-O{level}', '-relocation-model=pic',
            '-filetype=obj', llvm, '-o', obj)
        run(*shlex.split(os.environ.get('CXX', 'c++')), f'-O{level}',
            HERE / 'zero_width.cpp', obj, '-o', executable)
        run(executable)


if __name__ == '__main__':
    main()
