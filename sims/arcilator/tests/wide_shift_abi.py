#!/usr/bin/env python3
"""Check scalar shift-amount lowering against independent Python integers."""
import argparse
import ctypes
import hashlib
import json
import mmap
import os
from pathlib import Path
import random
import re
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM / 'compiler'))
from codegen import compile_model
from llvm import BYTE_DECLARATION, lower, wrapper

WIDTHS = [1025, 1027, 1088, 2048, 2051, 4096, 4099, 8192, 16399]


def source(widths=WIDTHS):
    lines = []
    for width in widths:
        rounded = (width + 7) // 8 * 8
        for mode in range(3):
            name = f'chipyard_arc_wide_shift_{mode}_{width}'
            lines.append(f'declare i{width} @{name}(i{width}, i{width})')
            for small in (False, True):
                prefix = 'small' if small else 'full'
                argument = 'i64 %n' if small else 'ptr %ap'
                lines += [
                    f'define void @{prefix}_{width}_{mode}(ptr %out, ptr %data, {argument}) {{',
                    f'  %d = load i{width}, ptr %data, align 1'
                ]
                lines += [
                    f'  %a = zext i64 %n to i{width}'
                    if small else f'  %a = load i{width}, ptr %ap, align 1',
                    f'  %v = call i{width} @{name}(i{width} %d, i{width} %a)'
                ]
                value = '%v'
                if rounded != width:
                    lines.append(f'  %clean = zext i{width} %v to i{rounded}')
                    value = '%clean'
                lines += [
                    f'  store i{rounded} {value}, ptr %out, align 1',
                    '  ret void', '}'
                ]
    return '\n'.join(lines) + '\n' + unused_source(widths)


def unused_source(widths):
    # These results are unused; the separately named ordinary effect must run.
    lines = [
        'define void @ordinary_tick(ptr %counter) noinline {',
        '  %old = load i64, ptr %counter, align 1',
        '  %next = add i64 %old, 1',
        '  store i64 %next, ptr %counter, align 1', '  ret void', '}'
    ]
    for width in widths:
        for mode in range(3):
            for small in ((False, True) if mode == 0 else (False,)):
                prefix = 'unused_small' if small else 'unused_full'
                lines += [
                    f'define void @{prefix}_{width}_{mode}(ptr %counter, ptr %data, ptr %ap) {{'
                ]
                if small:
                    lines += ['  %low = load i64, ptr %data, align 1',
                              f'  %d = zext i64 %low to i{width}']
                else:
                    lines.append(f'  %d = load i{width}, ptr %data, align 1')
                lines += [
                    f'  %a = load i{width}, ptr %ap, align 1',
                    f'  %unused = call i{width} @chipyard_arc_wide_shift_{mode}_{width}(i{width} %d, i{width} %a)',
                    '  call void @ordinary_tick(ptr %counter)', '  ret void', '}'
                ]
    return '\n'.join(lines) + '\n'


def exercise_unused(library):
    count = 0
    for width in WIDTHS:
        size = (width + 7) // 8
        for mode in range(3):
            for small in ((False, True) if mode == 0 else (False,)):
                prefix = 'unused_small' if small else 'unused_full'
                fn = getattr(library, f'{prefix}_{width}_{mode}')
                fn.argtypes = [ctypes.c_void_p] * 3
                fn.restype = None
                for alias in (False, True):
                    for value in (0, 1, (1 << 64) - 1):
                        data = ctypes.create_string_buffer(bytes([0xa5]) * size)
                        amount = ctypes.create_string_buffer((1 << 64).to_bytes(size, 'little'))
                        state = ctypes.c_uint64(value)
                        pointer = ctypes.addressof(data) if alias else ctypes.addressof(state)
                        if alias:
                            ctypes.memmove(pointer, value.to_bytes(8, 'little'), 8)
                        before, before_amount = data.raw, amount.raw
                        fn(pointer, ctypes.addressof(data), ctypes.addressof(amount))
                        expected = (value + 1) & ((1 << 64) - 1)
                        assert int.from_bytes(ctypes.string_at(pointer, 8), 'little') == expected
                        assert data.raw == (expected.to_bytes(8, 'little') + before[8:] if alias else before)
                        assert amount.raw == before_amount
                        count += 1
    return count


def exercise(library, width, mode, data, amount, small=False, offset=0):
    size, mask = (width + 7) // 8, (1 << width) - 1
    amount &= mask
    if small:
        amount &= (1 << 64) - 1
    signed = data - (1 << width) if data >> (width - 1) else data
    expected = ((data << amount if amount < width else 0) if mode == 0 else
                (data >> amount if amount < width else 0)
                if mode == 1 else signed >> min(amount, width)) & mask
    before, after = b'\x93' * (16 + offset), b'\x5a' * 16

    def allocate(value):
        raw = value.to_bytes(size, 'little')
        if width % 8:
            raw = raw[:-1] + bytes(
                [raw[-1] | (255 ^ ((1 << (width % 8)) - 1))])
        return ctypes.create_string_buffer(before + raw + after)

    output = allocate(0)
    operands = [allocate(data), allocate(amount)]
    snapshots = [value.raw for value in operands]
    pointers = [ctypes.addressof(value) + len(before) for value in operands]
    function = getattr(library,
                       ('small' if small else 'full') + f'_{width}_{mode}')
    function.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_uint64 if small else ctypes.c_void_p
    ]
    function.restype = None
    function(
        ctypes.addressof(output) + len(before), pointers[0],
        amount if small else pointers[1])
    raw = output.raw
    actual = int.from_bytes(raw[len(before):len(before) + size], 'little')
    assert raw[:len(before)] == before and raw[len(before) + size:-1] == after
    assert all(value.raw == saved for value, saved in zip(operands, snapshots))
    if actual != expected:
        raise AssertionError(
            f'width={width} mode={mode} small={small} amount={amount}: '
            f'{actual:x} != {expected:x}')


def guard_pages(library):
    """Place every loaded/stored byte range immediately before PROT_NONE."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    libc.mprotect.restype = ctypes.c_int
    count = 0
    for width in (1025, 1027, 2051, 16399):
        size, mask = (width + 7) // 8, (1 << width) - 1
        length = (
            (size + mmap.PAGESIZE - 1) // mmap.PAGESIZE + 1) * mmap.PAGESIZE
        mappings = [
            mmap.mmap(-1, length, prot=mmap.PROT_READ | mmap.PROT_WRITE)
            for _ in range(3)
        ]
        bases = [
            ctypes.addressof(ctypes.c_char.from_buffer(value))
            for value in mappings
        ]
        pointers = [base + length - mmap.PAGESIZE - size for base in bases]
        try:
            for base in bases:
                assert libc.mprotect(base + length - mmap.PAGESIZE,
                                     mmap.PAGESIZE,
                                     0) == 0, ctypes.get_errno()
            for data in (0, 1, mask, 1 << (width - 1)):
                for amount in (0, 1, 63, 64, width, width + 1, 1 << 64):
                    for pointer, value in zip(pointers[1:], (data, amount)):
                        raw = value.to_bytes(size, 'little')
                        if width % 8:
                            raw = raw[:-1] + bytes(
                                [raw[-1] | (255 ^ ((1 << (width % 8)) - 1))])
                        ctypes.memmove(pointer, raw, size)
                    signed = data - (1 << width) if data >> (width -
                                                             1) else data
                    refs = [
                        data << amount if amount < width else 0,
                        data >> amount if amount < width else 0,
                        signed >> min(amount, width)
                    ]
                    for mode, reference in enumerate(refs):
                        function = getattr(library, f'full_{width}_{mode}')
                        function.argtypes = [ctypes.c_void_p] * 3
                        function.restype = None
                        function(*pointers)
                        actual = int.from_bytes(
                            ctypes.string_at(pointers[0], size), 'little')
                        assert actual == reference & mask, (width, mode,
                                                            amount)
                        count += 1
        finally:
            for value in mappings:
                value.close()
    return count


def check(circt, cxx, work):
    work.mkdir(parents=True, exist_ok=True)
    raw = work / 'source.ll'
    raw.write_text(source())
    expected_count = len(WIDTHS) * 3
    assert len(wrapper((1 << 20) + 3, 'large_probe', 0)) < 2048
    assert lower(raw) == expected_count
    lowered = raw.read_bytes()
    assert lower(raw) == 0 and raw.read_bytes() == lowered
    assert b'%shift = alloca' not in lowered and b'ptr %shift' not in lowered
    assert lowered.count(
        b'@chipyard_arc_wide_shift_words(') == expected_count + 1
    # Both ABI declarations may coexist with newly generated wrappers.
    mixed = work / 'mixed.ll'
    mixed.write_text(BYTE_DECLARATION + source([1025]))
    assert lower(mixed) == 3 and BYTE_DECLARATION in mixed.read_text()
    assert lower(mixed) == 0
    subprocess.run(
        [str(circt / 'opt'), '-passes=verify',
         str(mixed), '-disable-output'],
        check=True)
    invalid = [
        'declare i1025 @chipyard_arc_wide_shift_0_1026(i1025,i1025)\n',
        'declare i64 @chipyard_arc_wide_shift_0_64(i64,i64)\n',
        'declare i1025 @chipyard_arc_wide_shift_9_1025(i1025,i1025)\n',
        'declare void @chipyard_arc_wide_shift_words(ptr,i64,i64,i32)\n',
        'declare void @chipyard_arc_wide_shift_bytes(ptr,ptr,ptr,i64,i64)\n'
    ]
    for index, text in enumerate(invalid):
        path = work / f'invalid-{index}.ll'
        path.write_text('define void @valid() { ret void }\n' + text)
        before = path.read_bytes()
        try:
            lower(path)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid wide helper ABI accepted')
        assert path.read_bytes() == before
        assert not list(work.glob(path.name + '.*'))
    results, count = [], 0
    for level in ('0', '3'):
        obj = work / f'wrapper-O{level}.o'
        compile_model(raw, obj, circt / 'opt', circt / 'llc',
                      SIM / 'generated-src/compiler', cxx, level, level)
        library = work / f'wrapper-O{level}.so'
        subprocess.run([
            *shlex.split(cxx), '-O' + level, '-std=c++17', '-fPIC', '-shared',
            str(SIM / 'native/wide.cpp'),
            str(obj), '-o',
            str(library)
        ],
                       check=True)
        native = ctypes.CDLL(str(library.resolve()))
        protected_checks = guard_pages(native)
        unused_checks = exercise_unused(native)
        rng = random.Random(194)
        for width in WIDTHS:
            mask = (1 << width) - 1
            amounts = [
                0, 1, 7, 8, 9, 63, 64, 65, width - 1, width, width + 1,
                1 << 64, (1 << 64) | 1, (1 << 64) - 1, 1 << (width - 1),
                (1 << (width - 1)) | 1
            ]
            for sample in range(80):
                data = [
                    0, mask, 1 << (width - 1), mask >> 1,
                    rng.getrandbits(width)
                ][sample % 5]
                amount = amounts[sample % len(amounts)]
                for mode in range(3):
                    for small in (False, True):
                        exercise(native, width, mode, data, amount, small,
                                 (sample + mode) % 8)
                        count += 1
        if level == '3':
            ir = subprocess.check_output([
                str(circt / 'llvm-dis'),
                str(obj.with_suffix('.bc')), '-o', '-'
            ],
                                         text=True)
            for width in WIDTHS:
                for mode in range(3):
                    for prefix in (['unused_full', 'unused_small'] if mode == 0 else ['unused_full']):
                        dead = re.search(r'^define[^\n]*@' + prefix + '_' + str(width) + '_' + str(mode) + r'\([^\n]*\)[^{]*\{(.*?)^}', ir, re.M | re.S)
                        assert dead and '@chipyard_arc_' not in dead[1]
                        assert '@ordinary_tick(' in dead[1]
            for width in WIDTHS:
                for mode in range(3):
                    body = re.search(
                        r'^define[^\n]*@small_' + str(width) + '_' +
                        str(mode) + r'\([^\n]*\)[^{]*\{(.*?)^}', ir,
                        re.M | re.S)
                    assert body, (width, mode)
                    assert all(
                        int(bits) <= 64 for bits in re.findall(
                            r'\bicmp\s+\w+\s+i(\d+)\b', body[1]))
                    assert f'i64 0, i64 0, i64 {width}, i32 {mode}' in body[1]
        # Prove the oracle rejects discarding high bits instead of bounding them.
        fault = work / f'fault-O{level}.ll'
        fault.write_text(source([1025]))
        lower(fault)
        text, replacements = re.subn(
            r'(call void @chipyard_arc_wide_shift_words\([^\n]*i64 %low), i64 %high, i64 %tail, i64 1025',
            r'\1, i64 0, i64 0, i64 1025', fault.read_text())
        assert replacements == 3
        fault.write_text(text)
        fault_obj = work / f'fault-O{level}.o'
        compile_model(fault, fault_obj, circt / 'opt', circt / 'llc',
                      SIM / 'generated-src/compiler', cxx, level, level)
        fault_library = work / f'fault-O{level}.so'
        subprocess.run([
            *shlex.split(cxx), '-O' + level, '-std=c++17', '-fPIC', '-shared',
            str(SIM / 'native/wide.cpp'),
            str(fault_obj), '-o',
            str(fault_library)
        ],
                       check=True)
        try:
            exercise(ctypes.CDLL(str(fault_library.resolve())), 1025, 0, 1,
                     1 << 64)
        except AssertionError as error:
            assert 'amount=18446744073709551616' in str(error)
        else:
            raise AssertionError(
                'high-amount truncation fault was not rejected')
        results.append(
            dict(level=level,
                 vectors=len(WIDTHS) * 80 * 3 * 2,
                 unused_results_preserve_ordinary_effect_checks=unused_checks,
                 high_bit_truncation_fault_rejected=True,
                 protected_page_checks=protected_checks,
                 object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest()))
    report = dict(status='passed',
                  checks=count,
                  levels=results,
                  no_wide_amount_buffer=True,
                  zero_extended_amounts_reduce_to_scalar=True,
                  malformed_ABIs_preserve_source=True,
                  lowering_idempotent=True,
                  legacy_byte_ABI_declaration_preserved=True)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(
        f'PASS: {count} independent generated-shift ABI vectors at O0/O3, '
        'unknown high bits, scalar sources, dirty padding, guards, transactional '
        'lowering, idempotence and detected high-bit truncation faults',
        flush=True)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin',
                        type=Path,
                        default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/wide-shift-abi')
    args = parser.parse_args()
    if args.circt_bin is None:
        parser.error('set CIRCT_BIN or supply --circt-bin')
    check(args.circt_bin, args.cxx, args.work)
