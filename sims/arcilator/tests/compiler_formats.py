#!/usr/bin/env python3
"""Compare dynamic real printing with the matching SDK's constant formatters."""
import argparse
import json
import os
from pathlib import Path
import random
import re
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def literal(data):
    return '"' + ''.join(f'\\{byte:02X}' for byte in data) + '"'


def decode(text):
    return re.sub(rb'\\([0-9A-Fa-f]{2})', lambda m: bytes([int(m[1], 16)]),
                  text.encode())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/compiler-format-test')
    args = parser.parse_args()
    sdk, work = args.circt_bin.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)

    def run(*command):
        subprocess.run([str(x) for x in command], check=True)

    library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/Model.cpp')
    guard = work / 'guard.mlir'
    guard.write_text("""module {
      func.func @unknown(%clk: !seq.clock, %dynamic: !sim.fstring, %value: f64) {
        sim.triggered %clk {
          %real = sim.fmt.flt %value : f64
          %message = sim.fmt.concat (%dynamic, %real)
          sim.proc.print %message
        }
        func.return
      }
      func.func @integer_only(%clk: !seq.clock, %value: i33) {
        sim.triggered %clk {
          %message = sim.fmt.dec %value : i33
          sim.proc.print %message
        }
        func.return
      }
    }\n""")
    run(sdk / 'circt-opt', guard, '-o', work / 'guard-before.mlir')
    run(sdk / 'circt-opt', guard, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-lower-real-formats)', '-o',
        work / 'guard-after.mlir')
    assert (work / 'guard-before.mlir').read_bytes() == (
        work / 'guard-after.mlir').read_bytes()
    print(
        'PASS: unsupported format trees and integer-only prints remain unchanged',
        flush=True)

    rng = random.Random(1729)
    values = [
        0, 1, 0x8000000000000000, 0x3ff0000000000000, 0xbff8000000000000,
        0x3fd5555555555555, 0x7ff0000000000000, 0xfff0000000000000,
        0x7ff8000000000123, 0xfff8000000000123, 0x7fefffffffffffff,
        0x0010000000000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000,
        0x7fc00001, 0x7f7fffff, 0x3c00, 0xbc00, 0x7c00, 0xfc00, 0x7e00, 0x7bff
    ] + [rng.getrandbits(64) for _ in range(96)]
    # Repeated bit patterns on distinct edges still produce distinct prints.
    values = [value for value in values for _ in range(2)]
    styles = [('flt', None, 6, False), ('flt', 0, 0, False),
              ('flt', 18, 3, True), ('flt', 9, 30, False),
              ('exp', 24, 17, False), ('exp', 8, 0, True),
              ('gen', 12, 0, True), ('gen', 1024, 8, False),
              ('gen', None, 17, False)]
    specs = [(bits, *style) for bits in (16, 32, 64) for style in styles]
    golden = ['module {']
    for index, raw in enumerate(values):
        for number, (bits, kind, width, digits, left) in enumerate(specs):
            spelling = f'0x{raw & ((1 << bits) - 1):0{bits // 4}X}'
            attrs = (' isLeftAligned true' if left else '')
            attrs += f' fieldWidth {width}' if width is not None else ''
            attrs += f' fracDigits {digits}'
            golden += [
                f'func.func @g_{index}_{number}() -> !sim.fstring {{',
                f'%v = arith.constant {spelling} : f{bits}',
                f'%f = sim.fmt.{kind} %v{attrs} : f{bits}',
                'func.return %f : !sim.fstring', '}'
            ]
    golden += ['}']
    (work / 'golden.mlir').write_text('\n'.join(golden) + '\n')
    run(sdk / 'circt-opt', work / 'golden.mlir', '--sim-fold-value-formatters',
        '--canonicalize', '-o', work / 'golden-folded.mlir')
    text = (work / 'golden-folded.mlir').read_text()
    folded = re.findall(
        r'func.func @g_(\d+)_(\d+)\(\).*?sim.fmt.literal "((?:[^"\\]|\\.)*)"',
        text, re.S)
    assert len(folded) == len(values) * len(specs)
    references = {(int(i), int(j)): decode(s) for i, j, s in folded}
    lines = [
        'module {',
        'hw.module @RealHarness(in %clock: i1, in %enable: i1, in %bits: i64) {',
        '%clk = seq.to_clock %clock', 'sim.triggered %clk if %enable {',
        '%one = hw.constant true',
        '%wide = comb.concat %one, %bits, %bits : i1, i64, i64',
        '%dec = sim.fmt.dec %wide specifierWidth 0 signed : i129',
        '%hex = sim.fmt.hex %bits, isUpper true specifierWidth 0 : i64',
        '%byte = comb.extract %bits from 0 : (i64) -> i8',
        '%char = sim.fmt.char %byte : i8', '%comma = sim.fmt.literal ","'
    ]
    expected = [bytearray(), bytearray()]
    for number, (bits, kind, width, digits, left) in enumerate(specs):
        source = '%bits'
        if bits < 64:
            source = f'%b{number}'
            lines.append(
                f'{source} = comb.extract %bits from 0 : (i64) -> i{bits}')
        lines += [f'%v{number} = arith.bitcast {source} : i{bits} to f{bits}']
        attrs = (' isLeftAligned true' if left else '')
        attrs += f' fieldWidth {width}' if width is not None else ''
        attrs += f' fracDigits {digits}'
        prefix = f'{number}:'.encode() + 'é%['.encode() + b'\0'
        suffix = b']\n'
        lines += [
            f'%p{number} = sim.fmt.literal {literal(prefix)}',
            f'%f{number} = sim.fmt.{kind} %v{number}{attrs} : f{bits}',
            f'%s{number} = sim.fmt.literal {literal(suffix)}',
            f'%m{number} = sim.fmt.concat (%p{number}, %f{number}, %comma, %dec, %comma, %hex, %comma, %char, %s{number})',
            f'sim.proc.print %m{number}' +
            (' to %stderr' if number & 1 else '')
        ]
    # Stream handles are shared by fragments, and their identity must survive
    # both allocated-model outlining and Func-to-LLVM signature conversion.
    lines.insert(2, '%stderr = sim.stderr_stream')
    lines += ['}', 'hw.output', '}', '}']
    for index in range(len(values)):
        if index % 3 == 1:
            continue
        for number in range(len(specs)):
            prefix = f'{number}:'.encode() + 'é%['.encode() + b'\0'
            raw = values[index]
            signed = ((1 << 128) | (raw << 64) | raw) - (1 << 129)
            integers = b',' + str(signed).encode() + b',' + f'{raw:X}'.encode(
            ) + b',' + bytes([raw & 255])
            expected[number
                     & 1] += prefix + references[index,
                                                 number] + integers + b']\n'
    (work / 'input.mlir').write_text('\n'.join(lines) + '\n')
    (work / 'vectors.txt').write_text(''.join(f'{v:016x}\n' for v in values))
    run(sys.executable, SIM / 'compiler/model.py', '--input',
        work / 'input.mlir', '--output', work / 'model.ll', '--state-file',
        work / 'model.json', '--arcilator', sdk / 'arcilator', '--circt-opt',
        sdk / 'circt-opt', '--mlir-translate', sdk / 'mlir-translate',
        '--cache', SIM / 'generated-src/compiler', '--max-ops', '1')
    model, = json.loads((work / 'model.json').read_text())
    (work / 'compiler_formats_layout.h').write_text(
        ''.join(f'constexpr unsigned {p["name"]}Offset={p["offset"]};\n'
                for p in model['states'] if p['type'] in ('input', 'output')) +
        f'constexpr unsigned formatsPerActivation={len(specs)};\n')
    libs = shlex.split(
        subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                                text=True))
    for level in ('0', '2'):
        run(sdk / 'opt', '-O' + level, work / 'model.ll', '-o',
            work / 'model.bc')
        run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj', work / 'model.bc', '-o', work / 'model.o')
        run(*shlex.split(os.environ.get('CXX', 'c++')), '-O' + level,
            '-std=c++17', '-I' + str(work), '-I' + str(sdk.parent / 'include'),
            HERE / 'compiler_formats.cpp', SIM / 'native/format.cpp',
            work / 'model.o', '-Wl,--wrap=chipyard_arc_format_real',
            '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz',
            '-lpthread', '-ldl', '-o', work / 'test')
        result = subprocess.run([work / 'test', work / 'vectors.txt'],
                                capture_output=True,
                                check=True)
        marker = f'PASS: {len(values)} real-format clock cycles and '.encode()
        assert result.stdout == expected[
            0], f'O{level} stdout format differs from SDK constant folding'
        assert result.stderr.startswith(
            expected[1]) and result.stderr[len(expected[1]):].startswith(
                marker
            ), f'O{level} stderr format differs from SDK constant folding'
        (work / f'O{level}.stdout').write_bytes(result.stdout)
        (work / f'O{level}.stderr').write_bytes(result.stderr)
    print(
        f'PASS: {len(values) * len(specs)} real-format references, f16/f32/f64, fixed/scientific/general notation, widths/alignment/precision, signed zero/NaN/infinity/subnormals, stdout/stderr, mixed signed 129-bit integers/hex/characters, embedded NUL/UTF-8, exact activations at O0/O2'
    )


if __name__ == '__main__':
    main()
