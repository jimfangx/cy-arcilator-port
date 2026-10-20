#!/usr/bin/env python3
"""Lower generated wide arithmetic calls to a byte-oriented native C ABI."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

from partition import plugin

DECLARATION = re.compile(
    r'^declare i(\d+) @(chipyard_arc_wide_shift_([012])_(\d+))\(i\1, i\1\)\s*$'
)
BYTE_DECLARATION = (
    'declare void @chipyard_arc_wide_shift_bytes('
    'ptr nocapture writeonly, ptr nocapture readonly, ptr nocapture readonly, '
    'i64, i32) nounwind memory(argmem: readwrite)\n')


def wrapper(bits, name, mode):
    """Keep LLVM integers inside LLVM; pass only byte pointers to C++."""
    rounded = (bits + 7) // 8 * 8
    lines = [
        f'define internal i{bits} @{name}(i{bits} %data, i{bits} %amount) alwaysinline {{',
        f'  %input = alloca i{rounded}, align 8',
        f'  %shift = alloca i{rounded}, align 8',
        f'  %output = alloca i{rounded}, align 8'
    ]
    data, amount = '%data', '%amount'
    if rounded != bits:
        # Fully initialize the bytes passed across the ABI. The C++ helper also
        # masks padding, including the shift amount's unused high bits.
        lines += [
            f'  %d = zext i{bits} %data to i{rounded}',
            f'  %a = zext i{bits} %amount to i{rounded}'
        ]
        data, amount = '%d', '%a'
    lines += [
        f'  store i{rounded} {data}, ptr %input, align 8',
        f'  store i{rounded} {amount}, ptr %shift, align 8',
        f'  call void @chipyard_arc_wide_shift_bytes(ptr %output, ptr %input, ptr %shift, i64 {bits}, i32 {mode})',
        f'  %value = load i{rounded}, ptr %output, align 8'
    ]
    value = '%value'
    if rounded != bits:
        lines.append(f'  %result = trunc i{rounded} %value to i{bits}')
        value = '%result'
    lines += [f'  ret i{bits} {value}', '}']
    return '\n'.join(lines) + '\n'


def lower(path):
    count, byte_declaration = 0, False
    temporary = None
    try:
        # Large models have hundreds of megabytes of LLVM IR. Stream it instead
        # of retaining multiple complete copies during compilation.
        with path.open() as source, tempfile.NamedTemporaryFile(
                mode='w', dir=path.parent, prefix=path.name + '.',
                delete=False) as target:
            temporary = Path(target.name)
            for line in source:
                match = DECLARATION.fullmatch(line)
                if match:
                    bits, name, mode, suffix = match.groups()
                    if bits != suffix or int(bits) <= 1024:
                        raise ValueError(
                            'invalid generated wide arithmetic declaration: ' +
                            line.strip())
                    target.write(wrapper(int(bits), name, int(mode)))
                    count += 1
                else:
                    if line.startswith(
                            'declare') and '@chipyard_arc_wide_shift_' in line:
                        if '@chipyard_arc_wide_shift_bytes(' not in line:
                            raise ValueError(
                                'unsupported generated wide arithmetic declaration: '
                                + line.strip())
                        if line != BYTE_DECLARATION:
                            raise ValueError(
                                'incompatible native wide arithmetic byte ABI')
                        byte_declaration = True
                    target.write(line)
            if count and not byte_declaration:
                target.write('\n' + BYTE_DECLARATION)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--llvm-opt', type=Path)
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    if args.llvm_opt and not args.cache:
        parser.error('--cache is required with --llvm-opt')
    temporary = None
    try:
        if args.llvm_opt:
            libraries = [
                plugin(args.llvm_opt, args.cxx, args.cache,
                       Path(__file__).with_name(name))
                for name in ('LargeReturns.cpp', 'LargeArguments.cpp')
            ]
            # Keep all transformations in one transaction. An unsupported
            # arithmetic declaration or LLVM failure preserves the input.
            with tempfile.NamedTemporaryFile(dir=args.input.parent,
                                             prefix=args.input.name +
                                             '.buffered.',
                                             delete=False) as stream:
                temporary = Path(stream.name)
            subprocess.run([
                str(args.llvm_opt), *[
                    '-load-pass-plugin=' + str(library)
                    for library in libraries
                ],
                '-passes=chipyard-buffer-large-returns,chipyard-buffer-large-arguments,verify',
                '-S',
                str(args.input), '-o',
                str(temporary)
            ],
                           check=True)
            count = lower(temporary)
            os.replace(temporary, args.input)
            temporary = None
        else:
            count = lower(args.input)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Native LLVM arithmetic lowering: {error}\n')
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f'Lowered {count} wide arithmetic helpers to the native byte ABI',
          flush=True)


if __name__ == '__main__':
    main()
