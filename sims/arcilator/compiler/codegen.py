#!/usr/bin/env python3
"""Compile native LLVM models with bounded vectorization and DAG sizes."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from partition import plugin


def compile_model(source, output, llvm_opt, llc, cache, cxx='c++',
                  opt_level='3', llc_opt_level='3', bitcode=None):
    bitcode = bitcode or output.with_suffix('.bc')
    paths = [path.resolve() for path in (source, output, bitcode)]
    if len(set(paths)) != 3:
        raise ValueError('input, object, and bitcode paths must be distinct')
    output.parent.mkdir(parents=True, exist_ok=True)
    bitcode.parent.mkdir(parents=True, exist_ok=True)
    library = plugin(llvm_opt, cxx, cache,
                     Path(__file__).with_name('Codegen.cpp'))
    # O3 argument promotion can recreate giant scalar machine arguments from
    # the private buffers introduced before optimization. Lower those ABIs
    # again before pinning instruction-selection block boundaries; blockaddress
    # uses intentionally make the later private-ABI rewrite ineligible.
    abi_libraries = [plugin(llvm_opt, cxx, cache, Path(__file__).with_name(name))
                     for name in ('LargeReturns.cpp', 'LargeArguments.cpp')]
    # Work beside each destination so final renames do not copy large models.
    work = Path(tempfile.mkdtemp(prefix='native-codegen-', dir=output.parent))
    fd, name = tempfile.mkstemp(prefix=bitcode.name + '.', suffix='.tmp',
                                dir=bitcode.parent)
    os.close(fd)
    temporary_bitcode = Path(name)
    temporary_object = work / 'model.o'
    pipeline = ('chipyard-guard-wide-vectors,default<O' + opt_level + '>,'
                'chipyard-restore-vector-flags,'
                'chipyard-buffer-large-returns,chipyard-buffer-large-arguments,'
                'chipyard-bound-codegen-blocks,verify')
    try:
        subprocess.run([str(llvm_opt), '-load-pass-plugin=' + str(library),
                        *['-load-pass-plugin=' + str(p) for p in abi_libraries],
                        '-passes=' + pipeline, str(source), '-o',
                        str(temporary_bitcode)], check=True)
        subprocess.run([str(llc), '-O' + llc_opt_level,
                        '-relocation-model=pic', '-filetype=obj',
                        str(temporary_bitcode), '-o', str(temporary_object)],
                       check=True)
    except BaseException:
        # A failed compiler must preserve the last complete model and retain
        # its failing IR/object for diagnosis; another build gets unique paths.
        print('Native LLVM compiler artifacts retained: ' +
              str(temporary_bitcode) + ' and ' + str(work), flush=True)
        raise
    os.replace(temporary_bitcode, bitcode)
    os.replace(temporary_object, output)
    shutil.rmtree(work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bitcode', type=Path)
    parser.add_argument('--llvm-opt', type=Path, required=True)
    parser.add_argument('--llc', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--opt-level', choices=('0', '1', '2', '3', 's', 'z'),
                        default='3')
    parser.add_argument('--llc-opt-level', choices=('0', '1', '2', '3'),
                        default='3')
    args = parser.parse_args()
    try:
        compile_model(args.input, args.output, args.llvm_opt, args.llc,
                      args.cache, args.cxx, args.opt_level, args.llc_opt_level,
                      args.bitcode)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Native LLVM code generation: {error}\n')


if __name__ == '__main__':
    main()
