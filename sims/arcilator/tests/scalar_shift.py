#!/usr/bin/env python3
"""Check scalar-input wide shifts against independent Python integers."""
import argparse
import ctypes
import json
import mmap
import os
from pathlib import Path
import random
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM / 'compiler'))
from codegen import compile_model
from llvm import lower, scalar_wrapper

libc = ctypes.CDLL(None, use_errno=True)
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.mprotect.restype = ctypes.c_int
word = (1 << 64) - 1
rng = random.Random(220)

def args(width, amount):
    high = 0
    for i in range(1, width // 64):
        high |= amount >> i * 64 & word
    tail = amount >> width // 64 * 64 if width % 64 else rng.getrandbits(64)
    low = amount & word
    if width > 64 and width % 64:
        tail |= word ^ (1 << width % 64) - 1
    if width < 64:
        low |= word ^ (1 << width) - 1
    return (low, high, tail)

def guard(library):
    fn = library.chipyard_arc_private_shift_scalar
    fn.argtypes = [ctypes.c_void_p] + [ctypes.c_uint64] * 5
    fn.restype = None
    count = 0
    fn(None, 0, 0, 0, 0, 0)
    for width in [1, 7, 8, 9, 63, 64, 65, 1025, 1027, 2048, 2051, 4099, 16399, 65537]:
        size = (width + 7) // 8
        mask = (1 << width) - 1
        pages = (size + mmap.PAGESIZE - 1) // mmap.PAGESIZE
        length = (pages + 2) * mmap.PAGESIZE
        for end in [False, True]:
            area = mmap.mmap(-1, length, prot=mmap.PROT_READ | mmap.PROT_WRITE)
            base = ctypes.addressof(ctypes.c_ubyte.from_buffer(area))
            assert libc.mprotect(base, mmap.PAGESIZE, 0) == 0 and libc.mprotect(base + (pages + 1) * mmap.PAGESIZE, mmap.PAGESIZE, 0) == 0
            pointer = base + mmap.PAGESIZE + (pages * mmap.PAGESIZE - size if end else 0)
            try:
                for data in [0, 1, word, 1 << 63, rng.getrandbits(64)]:
                    for raw in [0, 1, 7, 8, 9, 63, 64, 65, width - 1, width, width + 1, 1 << 64, 1 << width - 1]:
                        amount = raw & mask
                        fn(pointer, data, *args(width, amount), width)
                        actual = int.from_bytes(ctypes.string_at(pointer, size), 'little')
                        expected = data << amount & mask if amount < width else 0
                        assert actual == expected, (width, data, amount)
                        count += 1
            finally:
                area.close()
    return count

def direct(library):
    fn = library.chipyard_arc_private_shift_scalar
    fn.argtypes = [ctypes.c_void_p] + [ctypes.c_uint64] * 5
    fn.restype = None
    count = 0
    for width in [*range(1, 81), 127, 128, 129, 511, 512, 513, 1024, 1025, 1027, 1039, 1088, 2048, 2051, 4099, 8193, 16399, 32769, 65537]:
        size = (width + 7) // 8
        mask = (1 << width) - 1
        for sample in range(120):
            data = [0, 1, word, 1 << 63, rng.getrandbits(64)][sample % 5]
            amount = rng.choice([0, 1, 7, 8, 9, 31, 32, 63, 64, 65, width - 1, width, width + 1, rng.randrange(2 * width), 1 << 64, 1 << width - 1]) & mask
            prefix = b'\x93' * (16 + sample % 8)
            suffix = b'Z' * 16
            output = ctypes.create_string_buffer(prefix + b'\xcc' * size + suffix)
            fn(ctypes.addressof(output) + len(prefix), data, *args(width, amount), width)
            raw = output.raw
            assert raw[:len(prefix)] == prefix and raw[len(prefix) + size:-1] == suffix
            actual = int.from_bytes(raw[len(prefix):len(prefix) + size], 'little')
            expected = data << amount & mask if amount < width else 0
            assert actual == expected, (width, data, amount)
            count += 1
    return count

def toy_source():
    lines = []
    for width in [1025, 1027, 1088, 2051, 4099, 16399]:
        lines.append(f'declare i{width} @chipyard_arc_wide_shift_0_{width}(i{width}, i{width})')
        for small in [1, 4, 8, 16, 32, 63, 64]:
            for amount_kind in ['full', 'small']:
                arg = 'ptr %ap' if amount_kind == 'full' else 'i64 %n'
                lines.append(f'define void @{amount_kind}_{width}_{small}(ptr %out, i64 %data, {arg}) {{')
                value = '%data'
                if small < 64:
                    lines.append(f'  %s = trunc i64 %data to i{small}')
                    value = '%s'
                lines.append(f'  %d = zext i{small} {value} to i{width}')
                lines.append(f'  %a = load i{width}, ptr %ap, align 1' if amount_kind == 'full' else f'  %a = zext i64 %n to i{width}')
                lines += [f'  %v = call i{width} @chipyard_arc_wide_shift_0_{width}(i{width} %d, i{width} %a)']
                rounded = (width + 7) // 8 * 8
                value = '%v'
                if rounded != width:
                    lines.append(f'  %clean = zext i{width} %v to i{rounded}')
                    value = '%clean'
                lines += [f'  store i{rounded} {value}, ptr %out, align 1', '  ret void', '}']
    lines.append('declare void @chipyard_arc_wide_shift_words(ptr nocapture writeonly, ptr nocapture readonly, i64, i64, i64, i64, i32) nounwind memory(argmem: readwrite)')
    return '\n'.join(lines) + '\n'

def exercise(library):
    count = 0
    for width in [1025, 1027, 1088, 2051, 4099, 16399]:
        size = (width + 7) // 8
        mask = (1 << width) - 1
        for small in [1, 4, 8, 16, 32, 63, 64]:
            for amount_kind in ['full', 'small']:
                fn = getattr(library, f'{amount_kind}_{width}_{small}')
                fn.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p if amount_kind == 'full' else ctypes.c_uint64]
                fn.restype = None
                for sample in range(100):
                    data = [0, 1, word, 1 << 63, rng.getrandbits(64)][sample % 5]
                    amount = rng.choice([0, 1, 7, 8, 9, 31, 32, 63, 64, 65, width - 1, width, width + 1, 1 << 64, 1 << width - 1, rng.randrange(2 * width)])
                    amount &= word if amount_kind == 'small' else mask
                    prefix = b'\x93' * (16 + sample % 8)
                    suffix = b'Z' * 16
                    raw = amount.to_bytes(size, 'little')
                    if width % 8:
                        raw = raw[:-1] + bytes([raw[-1] | 255 ^ (1 << width % 8) - 1])
                    a = ctypes.create_string_buffer(prefix + raw + suffix)
                    old = a.raw
                    output = ctypes.create_string_buffer(prefix + b'\xcc' * size + suffix)
                    fn(ctypes.addressof(output) + len(prefix), data, ctypes.addressof(a) + len(prefix) if amount_kind == 'full' else amount)
                    raw = output.raw
                    assert raw[:len(prefix)] == prefix and raw[len(prefix) + size:-1] == suffix and (a.raw == old)
                    actual = int.from_bytes(raw[len(prefix):len(prefix) + size], 'little')
                    data &= (1 << small) - 1
                    expected = data << amount & mask if amount < width else 0
                    assert actual == expected, (width, small, amount_kind, data, amount)
                    count += 1
    return count


def contracts(work):
    checks = []
    for data, setup in (
            ('15', ''),
            ('%d', '  %d = zext i4 %small to i1025\n'),
            ('-1', ''),
            ('18446744073709551616', ''),
            ('%d', '  %d = sext i4 %small to i1025\n'),
            ('%d', '  %d = zext i65 %wide to i1025\n')):
        source = ('define i1025 @probe(i4 %small, i65 %wide, i1025 %amount) {\n' +
                  setup + '  %r = call i1025 @chipyard_arc_wide_shift_0_1025('
                  f'i1025 {data}, i1025 %amount)\n  ret i1025 %r\n}}\n'
                  'declare i1025 @chipyard_arc_wide_shift_0_1025(i1025, i1025)\n')
        path = work / ('contract-' + str(len(checks)) + '.ll')
        path.write_text(source)
        assert lower(path) == 1
        chosen = '@chipyard_arc_private_shift_scalar(' in path.read_text()
        assert chosen == (len(checks) < 2)
        lowered = path.read_bytes()
        assert lower(path) == 0 and path.read_bytes() == lowered
        checks.append(str(path))
    # Very large APInts must stay on the generic path without Python integer
    # parsing limits. This decimal literal fits the declared 32768-bit type.
    path = work / 'large-constant.ll'
    value = '1' + '0' * 5000
    path.write_text('define i32768 @large(i32768 %n) {\n'
                    '  %r = call i32768 @chipyard_arc_wide_shift_0_32768('
                    f'i32768 {value}, i32768 %n)\n  ret i32768 %r\n}}\n'
                    'declare i32768 @chipyard_arc_wide_shift_0_32768(i32768, i32768)\n')
    assert lower(path) == 1
    assert '@chipyard_arc_private_shift_scalar(' not in path.read_text()
    checks.append(str(path))
    # Foreign calls with the same scalar shape retain their original ABI.
    path = work / 'foreign.ll'
    text = ('declare i1025 @foreign_shift(i1025, i1025)\n'
            'define i1025 @foreign(i1025 %amount) {\n'
            '  %r = call i1025 @foreign_shift(i1025 15, i1025 %amount)\n'
            '  ret i1025 %r\n}\n')
    path.write_text(text)
    assert lower(path) == 0 and path.read_text() == text
    checks.append(str(path))
    # An occupied private wrapper name conservatively keeps the generic call.
    path = work / 'occupied.ll'
    path.write_text(source.replace('i1025 %d,', 'i1025 15,') +
                    'declare void @chipyard_arc_private_scalar_1025_4()\n')
    assert lower(path) == 1
    assert '@chipyard_arc_private_shift_scalar(' not in path.read_text()
    checks.append(str(path))
    # A malformed helper declaration must preserve input atomically.
    path = work / 'invalid.ll'
    text = 'declare void @chipyard_arc_private_shift_scalar(ptr, i64)\n'
    path.write_text(text)
    try:
        lower(path)
    except ValueError:
        pass
    else:
        raise AssertionError('invalid scalar helper ABI accepted')
    assert path.read_text() == text and not list(work.glob(path.name + '.*'))
    return checks


def poison_edges(circt, cxx, work):
    """Preserve an unused poison data operand and an effectful amount producer.

    The LLVM-only helper double observes data only for an in-range shift. Its
    overshift result matches the real helper; other arithmetic is tested using
    the native implementation above. This double is never linked to firmware.
    """
    work.mkdir(parents=True, exist_ok=True)
    lines = [r"""
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
define void @chipyard_arc_private_shift_scalar(ptr %out, i64 %data, i64 %low, i64 %high, i64 %tail, i64 %width) noinline nounwind willreturn memory(argmem: write) {
  %plus = add i64 %width, 7
  %size = udiv i64 %plus, 8
  call void @llvm.memset.p0.i64(ptr %out, i8 0, i64 %size, i1 false)
  %live = icmp ult i64 %low, %width
  br i1 %live, label %read_data, label %done
read_data:
  store i64 %data, ptr %out, align 1
  br label %done
done:
  ret void
}
"""]
    cases = []
    for width in (1025, 4099):
        lines += [
            f'define i{width} @ordinary_amount_{width}(ptr %counter) noinline {{',
            '  %old = load i64, ptr %counter, align 1',
            '  %next = add i64 %old, 1',
            '  store i64 %next, ptr %counter, align 1',
            f'  ret i{width} {width}', '}'
        ]
        for small in (1, 4, 64):
            lines.append(scalar_wrapper(width, small))
            name = f'chipyard_arc_private_scalar_{width}_{small}'
            for kind in ('poison', 'zero_effect'):
                entry = f'{kind}_{width}_{small}'
                cases.append(entry)
                lines.append(f'define i64 @{entry}(ptr %counter) {{')
                if kind == 'poison':
                    lines += [
                        f'  %v = call i{width} @{name}(i{small} poison, i{width} {width})',
                        '  %old = load i64, ptr %counter, align 1',
                        '  %next = add i64 %old, 1',
                        '  store i64 %next, ptr %counter, align 1'
                    ]
                else:
                    lines += [
                        f'  %a = call i{width} @ordinary_amount_{width}(ptr %counter)',
                        f'  %v = call i{width} @{name}(i{small} 0, i{width} %a)'
                    ]
                lines += [f'  %ret = trunc i{width} %v to i64',
                          '  ret i64 %ret', '}']
    source = work / 'source.ll'
    source.write_text('\n'.join(lines) + '\n')
    results = []
    for level in ('0', '3'):
        for pipeline in ('stock', 'bounded'):
            obj = work / f'{pipeline}-O{level}.o'
            compile_model(source, obj, circt / 'opt', circt / 'llc',
                          SIM / 'generated-src/compiler', cxx, level, level,
                          abi_buffers=False, pipeline=pipeline)
            library = obj.with_suffix('.so')
            subprocess.run([*shlex.split(cxx), '-shared', str(obj), '-o',
                            str(library)], check=True)
            native = ctypes.CDLL(str(library.resolve()))
            count = 0
            for case in cases:
                fn = getattr(native, case)
                fn.argtypes = [ctypes.c_void_p]
                fn.restype = ctypes.c_uint64
                for initial in (0, 1, (1 << 64) - 1):
                    state = ctypes.c_uint64(initial)
                    value = fn(ctypes.byref(state))
                    assert value == 0
                    assert state.value == (initial + 1) & ((1 << 64) - 1)
                    count += 1
            results.append(dict(level=level, pipeline=pipeline, checks=count))
    return dict(status='passed', runs=results,
                scope='Defined poison-data overshift and effectful amount producer; LLVM-only helper double')


def check(circt, cxx, work):
    work.mkdir(parents=True, exist_ok=True)
    checked_contracts = contracts(work)
    for path in checked_contracts:
        subprocess.run([str(circt / 'opt'), '-passes=verify', path,
                        '-disable-output'], check=True)
    raw = work / 'source.ll'
    raw.write_text(toy_source())
    assert lower(raw) == 6
    assert raw.read_text().count('call void @chipyard_arc_private_shift_scalar(') == 42
    lowered = raw.read_bytes()
    assert lower(raw) == 0 and raw.read_bytes() == lowered
    results = []
    for level in ('0', '3'):
        helper = work / f'wide-O{level}.o'
        library = work / f'wide-O{level}.so'
        subprocess.run([*shlex.split(cxx), '-O' + level, '-std=c++17',
                        '-fPIC', '-c', str(SIM / 'native/wide.cpp'), '-o',
                        str(helper)], check=True)
        subprocess.run([*shlex.split(cxx), '-shared', str(helper), '-o',
                        str(library)], check=True)
        native = ctypes.CDLL(str(library.resolve()))
        math, protected = direct(native), guard(native)
        for pipeline in ('stock', 'bounded'):
            obj = work / f'wrapper-{pipeline}-O{level}.o'
            compile_model(raw, obj, circt / 'opt', circt / 'llc',
                          SIM / 'generated-src/compiler', cxx, level, level,
                          pipeline=pipeline)
            library = obj.with_suffix('.so')
            subprocess.run([*shlex.split(cxx), '-shared', str(obj), str(helper),
                            '-o', str(library)], check=True)
            vectors = exercise(ctypes.CDLL(str(library.resolve())))
            results.append(dict(level=level, pipeline=pipeline,
                                direct_vectors=math,
                                protected_page_checks=protected,
                                LLVM_vectors=vectors))
    result = dict(status='passed', contracts=len(checked_contracts) + 1,
                  runs=results, poison_edges=poison_edges(circt, cxx, work / 'poison-edges'))
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: scalar-input shift contracts, independent integer and '
          'protected-page checks, both LLVM pipelines at O0/O3', flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path,
                        default=Path(os.environ.get('CIRCT_BIN', '.')))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--build', type=Path,
                        default=SIM / 'generated-src/scalar-shift-test')
    args = parser.parse_args()
    check(args.circt_bin.resolve(), args.cxx, args.build.resolve())


if __name__ == '__main__':
    main()
