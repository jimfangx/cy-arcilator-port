#!/usr/bin/env python3
"""Compare the native byte arithmetic ABI with independent Python integers."""
import argparse
import ctypes
import os
from pathlib import Path
import random
import shlex
import subprocess


def check(library):
    native = ctypes.CDLL(str(library))
    shift_bytes = native.chipyard_arc_wide_shift_bytes
    shift_words = native.chipyard_arc_wide_shift_words
    shift_words.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
        ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint32
    ]
    shift_words.restype = None
    shift_bytes.argtypes = [
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64,
        ctypes.c_uint32
    ]
    shift_bytes.restype = None
    rng, count = random.Random(932), 0
    widths = [
        *range(1, 81), 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024,
        1025, 1027, 2047, 2048, 2049, 2051, 4095, 4096, 4097, 4099, 8191, 8192,
        8193, 16383, 16384, 16399, 32767, 32768, 32769, 65537
    ]
    for width in widths:
        size, mask = (width + 7) // 8, (1 << width) - 1
        for sample in range(90):
            data = [
                0, mask, 1 << (width - 1), 1, mask ^ (1 << (width - 1)),
                rng.getrandbits(width)
            ][sample % 6]
            amount = rng.choice([
                0, 1, 7, 8, 9, 31, 32, 63, 64, 65, 127, 128, 129, width - 1,
                width, width + 1,
                rng.randrange(2 * width), 1 << (width - 1)
            ]) & mask
            inputs = [
                data.to_bytes(size, 'little'),
                amount.to_bytes(size, 'little')
            ]
            if width % 8:
                padding = 255 ^ ((1 << (width % 8)) - 1)
                inputs = [
                    value[:-1] +
                    bytes([value[-1] | (rng.getrandbits(8) & padding)])
                    for value in inputs
                ]
            signed = data - (1 << width) if data >> (width - 1) else data
            expected = [(data << amount) if amount < width else 0,
                        (data >> amount) if amount < width else 0,
                        signed >> min(amount, width)]
            for mode, reference in enumerate(expected):
                for api in ('byte', 'words'):
                    alignment = (sample + mode) % 8
                    before, after = bytes([0x93]) * (16 + alignment), bytes(
                        [0x5a]) * 16
                    output = ctypes.create_string_buffer(before + bytes(size) +
                                                         after)
                    operands = [
                        ctypes.create_string_buffer(before + value + after)
                        for value in inputs
                    ]
                    pointers = [
                        ctypes.addressof(value) + len(before)
                        for value in operands
                    ]
                    if api == 'byte':
                        shift_bytes(
                            ctypes.addressof(output) + len(before), *pointers,
                            width, mode)
                    else:
                        word_mask = (1 << 64) - 1
                        low = amount & word_mask
                        high = 0
                        for index in range(1, width // 64):
                            high |= (amount >> (64 * index)) & word_mask
                        tail = amount >> (
                            64 *
                            (width // 64)) if width > 64 and width % 64 else 0
                        if width > 64 and width % 64:
                            tail |= word_mask ^ ((1 << (width % 64)) - 1)
                        if width < 64:
                            low |= word_mask ^ ((1 << width) - 1)
                        shift_words(
                            ctypes.addressof(output) + len(before),
                            pointers[0], low, high, tail, width, mode)
                    raw = output.raw
                    assert raw[:len(before
                                    )] == before and raw[len(before) +
                                                         size:-1] == after
                    assert all(value.raw[:-1] == before + expected + after
                               for value, expected in zip(operands, inputs))
                    actual = int.from_bytes(
                        raw[len(before):len(before) + size], 'little')
                    if actual != reference & mask:
                        raise AssertionError(
                            f'width={width}, mode={mode}, amount={amount}: '
                            f'{actual:x} != {reference & mask:x}')
                    count += 1
    shift_bytes(None, None, None, 0, 0)
    shift_words(None, None, 0, 0, 0, 0, 0)
    print(
        f'PASS: {count} native byte/word-ABI vectors across {len(widths)} widths, '
        'all shift modes, dirty padding, zero width, unaligned buffers and guards',
        flush=True)


def main():
    sim = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--build',
                        type=Path,
                        default=sim / 'generated-src/wide-helper-test')
    args = parser.parse_args()
    args.build.mkdir(parents=True, exist_ok=True)
    library = (args.build / 'wide.so').resolve()
    subprocess.run([
        *shlex.split(args.cxx), '-O3', '-std=c++17', '-shared', '-fPIC',
        str(sim / 'native/wide.cpp'), '-o',
        str(library)
    ],
                   check=True)
    check(library)


if __name__ == '__main__':
    main()
