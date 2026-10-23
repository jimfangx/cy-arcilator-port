#!/usr/bin/env python3
"""Execute array-valued control flow through the native compiler's lowering."""
import argparse
from pathlib import Path
import subprocess
import re
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(SIM / 'compiler'))
from partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path,
                        default=SIM / 'generated-src/compiler-array-test')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    circt = args.circt_bin.resolve()
    library = plugin(circt / 'circt-opt', 'c++', SIM / 'generated-src/compiler',
                     SIM / 'compiler/LowerArrays.cpp')

    def run(*command):
        subprocess.run([str(value) for value in command], check=True)

    run(circt / 'circt-opt', HERE / 'compiler_arrays.mlir',
        '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(hw-convert-bitcasts{allow-partial-conversion=false},'
        'chipyard-lower-arrays,lower-arc-to-llvm,reconcile-unrealized-casts)',
        '-o', args.work / 'llvm.mlir')
    run(circt / 'mlir-translate', '--mlir-to-llvmir', args.work / 'llvm.mlir',
        '-o', args.work / 'model.ll')
    assert re.search(r'define\b[^\n]*@mixed_forward\(ptr\b[^,]*,\s*i32 zeroext\b',
                     (args.work / 'model.ll').read_text()), 'sret insertion lost or shifted argument attributes'
    run(circt / 'llc', '-O2', '-relocation-model=pic', '-filetype=obj',
        args.work / 'model.ll', '-o', args.work / 'model.o')
    run('c++', '-O2', HERE / 'compiler_arrays.cpp', args.work / 'model.o',
        '-o', args.work / 'test')
    run(args.work / 'test')


if __name__ == '__main__':
    main()
