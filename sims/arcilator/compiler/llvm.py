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

WORD_DECLARATION = (
    'declare void @chipyard_arc_wide_shift_words('
    'ptr nocapture writeonly, ptr nocapture readonly, i64, i64, i64, i64, i32) '
    'nounwind memory(argmem: readwrite)\n')


def wrapper(bits, name, mode):
    """Pass a low word, reduced full high words and a masked partial tail."""
    # Integer arguments/results expose bounded deterministic arithmetic. The
    # C helper accesses only this wrapper's nonescaping scratch allocations;
    # LLVM memory(none) permits these local reads/writes. It invokes no driver
    # or SDK callbacks and always returns without throwing. The pointer ABI
    # declarations retain their argument-memory effects.
    rounded = (bits + 7) // 8 * 8
    lines = [
        f'define internal i{bits} @{name}(i{bits} %data, i{bits} %amount) alwaysinline nounwind willreturn memory(none) {{',
        f'  %input = alloca i{rounded}, align 8',
        f'  %output = alloca i{rounded}, align 8',
        f'  %low = trunc i{bits} %amount to i64'
    ]
    # Compare only complete upper words. The remaining partial word crosses
    # the ABI separately and is masked in C++, where padding is explicit.
    # Keeping the reduction in one operation also avoids one wide APInt shift
    # constant per word for large hardware values.
    upper_bits = (bits // 64 - 1) * 64
    lines += [
        f'  %upper_slice = lshr i{bits} %amount, 64',
        f'  %upper = trunc i{bits} %upper_slice to i{upper_bits}',
        f'  %nonzero = icmp ne i{upper_bits} %upper, 0',
        '  %high = zext i1 %nonzero to i64'
    ]
    high = '%high'
    tail = '0'
    if bits % 64:
        lines += [
            f'  %tail_slice = lshr i{bits} %amount, {bits // 64 * 64}',
            f'  %tail = trunc i{bits} %tail_slice to i64'
        ]
        tail = '%tail'
    data = '%data'
    if rounded != bits:
        lines.append(f'  %d = zext i{bits} %data to i{rounded}')
        data = '%d'
    lines += [
        f'  store i{rounded} {data}, ptr %input, align 8',
        f'  call void @chipyard_arc_wide_shift_words(ptr %output, ptr %input, i64 %low, i64 {high}, i64 {tail}, i64 {bits}, i32 {mode})',
        f'  %value = load i{rounded}, ptr %output, align 8'
    ]
    value = '%value'
    if rounded != bits:
        lines.append(f'  %result = trunc i{rounded} %value to i{bits}')
        value = '%result'
    lines += [f'  ret i{bits} {value}', '}']
    return '\n'.join(lines) + '\n'


SCALAR_DECLARATION = (
    'declare void @chipyard_arc_private_shift_scalar('
    'ptr nocapture writeonly, i64, i64, i64, i64, i64) '
    'nounwind memory(argmem: readwrite)\n')

SCALAR_CALL = re.compile(
    r'^(\s*%[-a-zA-Z$._0-9]+ = )call i([0-9]+) '
    r'@chipyard_arc_wide_shift_0_\2\(i\2 ([^,]+), i\2 ([^)]+)\)([^\n]*)$')
SCALAR_ZEXT = re.compile(
    r'^\s*(%[-a-zA-Z$._0-9]+) = zext i([0-9]+) '
    r'(%[-a-zA-Z$._0-9]+|-?[0-9]+) to i([0-9]+)\s*$')
# Most model lines contain no helper ABI. Check required literals before
# invoking the exact recognizers while retaining their validation rules.
SCALAR_SYMBOL = re.compile(r'@(chipyard_arc_private_scalar_[0-9]+_[0-9]+)\(')


def scalar_wrapper(bits, small):
    """Keep the exact full-width amount contract without materializing data."""
    name = f'chipyard_arc_private_scalar_{bits}_{small}'
    text = wrapper(bits, name, 0).replace(
        f'(i{bits} %data,', f'(i{small} %small,')
    lines = []
    for line in text.splitlines():
        if ('%input = alloca' in line or line.startswith('  %d = zext ')
                or line.startswith('  store ')):
            continue
        if 'call void @chipyard_arc_wide_shift_words' in line:
            data = '%small'
            if small < 64:
                lines.append(f'  %scalar = zext i{small} %small to i64')
                data = '%scalar'
            line = line.replace(
                '@chipyard_arc_wide_shift_words(ptr %output, ptr %input,',
                '@chipyard_arc_private_shift_scalar(ptr %output, i64 ' + data + ',')
            line = line.replace(', i32 0)', ')')
        lines.append(line)
        if line.startswith('  %output = alloca '):
            # Test a frozen value so an otherwise unused poison operand does
            # not introduce a new branch UB. The original value still goes to
            # the nonzero helper path; zero is an identity for every amount.
            lines += [
                f'  %zero_probe = freeze i{small} %small',
                f'  %is_zero = icmp eq i{small} %zero_probe, 0',
                '  br i1 %is_zero, label %zero_result, label %shift_result',
                'zero_result:', f'  ret i{bits} 0', 'shift_result:'
            ]
    return '\n'.join(lines) + '\n'


def lower(path):
    count, word_declaration, scalar_declaration = 0, False, False
    temporary = None
    # Only declarations owned by the generated pure arithmetic ABI qualify.
    # A first streaming pass lets uses precede declarations without retaining
    # complete large LLVM modules or arbitrarily inferring foreign-call purity.
    known, occupied = set(), set()
    with path.open() as source:
        for line in source:
            match = (DECLARATION.fullmatch(line)
                     if line.startswith('declare i') else None)
            if match:
                bits, name, mode, suffix = match.groups()
                if bits != suffix or int(bits) <= 1024:
                    raise ValueError(
                        'invalid generated wide arithmetic declaration: ' +
                        line.strip())
                if mode == '0':
                    known.add(name)
            if '@chipyard_arc_private_scalar_' in line:
                occupied.update(SCALAR_SYMBOL.findall(line))
    values, pairs = {}, set()
    try:
        with path.open() as source, tempfile.NamedTemporaryFile(
                mode='w', dir=path.parent, prefix=path.name + '.',
                delete=False) as target:
            temporary = Path(target.name)
            for line in source:
                if line.startswith('define '):
                    values.clear()
                if known:
                    zext = (SCALAR_ZEXT.fullmatch(line)
                            if ' = zext i' in line else None)
                    if zext:
                        value, small, operand, width = zext.groups()
                        if 1 <= int(small) <= 64 and int(width) > 1024:
                            values[value] = (int(width), int(small), operand)
                    call = (SCALAR_CALL.fullmatch(line.rstrip('\n'))
                            if '@chipyard_arc_wide_shift_0_' in line else None)
                    if call:
                        prefix, width, data, amount, tail = call.groups()
                        width = int(width)
                        name = f'chipyard_arc_wide_shift_0_{width}'
                        small = None
                        if name in known:
                            if (data.isdecimal() and len(data) <= 20 and
                                    int(data).bit_length() <= 64):
                                small = max(1, int(data).bit_length())
                            elif data in values and values[data][0] == width:
                                _, small, data = values[data]
                        narrow = f'chipyard_arc_private_scalar_{width}_{small}'
                        if small is not None and narrow not in occupied:
                            pairs.add((width, small))
                            line = (prefix + f'call i{width} @{narrow}('
                                    f'i{small} {data}, i{width} {amount})' + tail + '\n')
                match = (DECLARATION.fullmatch(line)
                         if line.startswith('declare i') else None)
                if match:
                    bits, name, mode, suffix = match.groups()
                    # The first pass already validated the complete signature.
                    target.write(wrapper(int(bits), name, int(mode)))
                    count += 1
                else:
                    if (line.startswith('declare') and
                            ('@chipyard_arc_wide_shift_' in line or
                             '@chipyard_arc_private_shift_scalar(' in line)):
                        if not any(name in line for name in (
                                '@chipyard_arc_wide_shift_bytes(',
                                '@chipyard_arc_wide_shift_words(',
                                '@chipyard_arc_private_shift_scalar(')):
                            raise ValueError(
                                'unsupported generated wide arithmetic declaration: ' +
                                line.strip())
                        if line not in (BYTE_DECLARATION, WORD_DECLARATION,
                                        SCALAR_DECLARATION):
                            raise ValueError('incompatible native wide arithmetic helper ABI')
                        word_declaration |= line == WORD_DECLARATION
                        scalar_declaration |= line == SCALAR_DECLARATION
                    target.write(line)
            if count and not word_declaration:
                target.write('\n' + WORD_DECLARATION)
            for bits, small in sorted(pairs):
                target.write('\n' + scalar_wrapper(bits, small))
            if pairs and not scalar_declaration:
                target.write('\n' + SCALAR_DECLARATION)
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
    parser.add_argument('--no-abi-buffers',
                        dest='abi_buffers',
                        action='store_false',
                        help='retain private LLVM argument and return ABIs')
    parser.add_argument(
        '--no-early-inline', dest='early_inline', action='store_false',
        help='retain private forwarding calls until the final LLVM pipeline')
    sharing = parser.add_mutually_exclusive_group()
    sharing.add_argument(
        '--share-procedures',
        action='store_true',
        default=None,
        help='share private procedure values (default with --llvm-opt)')
    sharing.add_argument('--no-share-procedures',
                         dest='share_procedures',
                         action='store_false',
                         help='retain separate private procedure buffers')
    args = parser.parse_args()
    if args.share_procedures is None:
        args.share_procedures = bool(args.llvm_opt)
    if args.llvm_opt and not args.cache:
        parser.error('--cache is required with --llvm-opt')
    if args.share_procedures and not args.llvm_opt:
        parser.error('--share-procedures requires --llvm-opt and --cache')
    temporary = None
    try:
        if args.llvm_opt:
            names = (['LargeReturns.cpp', 'LargeArguments.cpp']
                     if args.abi_buffers else [])
            if args.share_procedures:
                names.insert(0, 'SharedProcedures.cpp')
            libraries = [
                plugin(args.llvm_opt, args.cxx, args.cache,
                       Path(__file__).with_name(name)) for name in names
            ]
            # Keep all transformations in one transaction. An unsupported
            # arithmetic declaration or LLVM failure preserves the input.
            with tempfile.NamedTemporaryFile(dir=args.input.parent,
                                             prefix=args.input.name +
                                             '.buffered.',
                                             delete=False) as stream:
                temporary = Path(stream.name)
            # Expose ordinary forwarding expressions before arithmetic calls
            # become opaque C ABIs. LLVM retains noinline and effect ordering;
            # shared storage and private-ABI passes still own their transforms.
            passes = []
            if args.early_inline:
                passes += ['require<profile-summary>', 'cgscc(inline)']
            if args.share_procedures:
                passes.append('chipyard-share-procedure-values')
            if args.abi_buffers:
                passes += ['chipyard-buffer-large-returns',
                           'chipyard-buffer-large-arguments']
            passes.append('verify')
            subprocess.run([
                str(args.llvm_opt), *[
                    '-load-pass-plugin=' + str(library)
                    for library in libraries
                ], '-passes=' + ','.join(passes), '-S',
                str(args.input), '-o', str(temporary)
            ], check=True)
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
