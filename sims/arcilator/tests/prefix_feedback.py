#!/usr/bin/env python3
"""Check finite shifted recurrences against an independent bit recurrence."""
import argparse
from pathlib import Path
import random
import re
import subprocess
import sys

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim))
from compiler.partition import plugin

WIDTHS = (1, 4, 5, 9, 65, 129)
OPERATORS = ('xor', 'and', 'or')


def expression(name,
               width,
               step,
               operation,
               reverse,
               style='concat',
               bases=None):
    """Build the source equation, independently of its compiler implementation."""
    bases = bases or [name + '_a', name + '_b']
    identity = operation == 'and'
    lines = []
    if style == 'shift':
        lines += [
            f'{name}_distance = hw.constant {step} : i{width}',
            f'{name}_shifted = comb.{"shru" if reverse else "shl"} {name}, {name}_distance : i{width}'
        ]
    else:
        low = step if reverse else 0
        fill = (1 << step) - 1 if identity else 0
        lines += [
            f'{name}_fill = hw.constant {fill} : i{step}',
            f'{name}_kept = comb.extract {name} from {low} : (i{width}) -> i{width-step}'
        ]
        parts = [name + '_fill', name +
                 '_kept'] if reverse else [name + '_kept', name + '_fill']
        sizes = [step, width - step] if reverse else [width - step, step]
        lines.append(
            f'{name}_shifted = comb.concat {", ".join(parts)} : i{sizes[0]}, i{sizes[1]}'
        )
    lines.append(f'{name} = comb.{operation} ' +
                 ', '.join([name + '_shifted', *bases]) + f' : i{width}')
    return lines


def reference(a, b, width, step, operation, reverse):
    result = 0
    for bit in range(width - 1, -1, -1) if reverse else range(width):
        previous = bit + step if reverse else bit - step
        carry = bool((result >> previous)
                     & 1) if 0 <= previous < width else operation == 'and'
        left, right = bool((a >> bit) & 1), bool((b >> bit) & 1)
        if operation == 'and':
            value = carry and left and right
        elif operation == 'or':
            value = carry or left or right
        else:
            value = carry ^ left ^ right
        result |= int(value) << bit
    return result


def native_design():
    outputs = []
    body = [
        '%zero = hw.constant 0 : i129', '%false = hw.constant false',
        '%clk = seq.to_clock %clock'
    ]
    for width in WIDTHS:
        for arg in ('data', 'other'):
            body.append(
                f'%{arg}{width} = comb.extract %{arg} from 0 : (i129) -> i{width}'
            )
        for index, operation in enumerate(OPERATORS):
            for reverse in (0, 1):
                for step in sorted({1, min(3, width)}):
                    name = f'prefix_{width}_{index}_{reverse}_{step}'
                    style = 'shift' if step > 1 and operation != 'and' else 'concat'
                    body += expression('%' + name, width, step, operation,
                                       reverse, style,
                                       [f'%data{width}', f'%other{width}'])
                    outputs.append((name, width))
    body += [
        '%q = seq.firreg %prefix_129_2_0_1 clock %clk reset sync %reset, %zero : i129',
        'hw.output ' + ', '.join('%' + name for name, _ in outputs) +
        ', %q, %false : ' + ', '.join('i' + str(width)
                                      for _, width in outputs) + ', i129, i1'
    ]
    ports = 'in %clock: i1, in %reset: i1, in %data: i129, in %other: i129, '
    ports += ', '.join(
        f'out {name}: i{width}'
        for name, width in outputs) + ', out q: i129, out io_success: i1'
    return 'module { hw.module @NativePrefixFeedbackHarness(' + ports + ') {\n' + '\n'.join(
        body) + '\n} }\n'


def rr_design():
    widths = (1, 2, 3, 4, 5, 8, 17)
    declarations, body, outputs = [], [
        '%clk = seq.to_clock %clock', '%false = hw.constant false'
    ], []
    for width in widths:
        bits = max(1, (width - 1).bit_length())
        declarations.append(
            f'hw.module.extern private @RR{width}<NUM_REQS: ui32, LOCK_ENABLE: ui32, MODEL: ui32>(in %clk: !seq.clock, in %reset: i1, in %requests: i{width}, in %grant_unlock: i1, out grant_index: i{bits}, out grant_onehot: i{width}, out grant_valid: i1) attributes {{verilogName = "VX_rr_arbiter"}}'
        )
        body += [
            f'%requests{width} = comb.extract %requests from 0 : (i17) -> i{width}',
            f'%zero{width} = hw.constant 0 : i{width}'
        ]
        for lock in (0, 1):
            name = f'rr_{width}_{lock}'
            body.append(
                f'%{name}.index, %{name}.hot, %{name}.valid = hw.instance "{name}" @RR{width}<NUM_REQS: ui32 = {width}, LOCK_ENABLE: ui32 = {lock}, MODEL: ui32 = 1>(clk: %clk: !seq.clock, reset: %reset: i1, requests: %requests{width}: i{width}, grant_unlock: %unlock: i1) -> (grant_index: i{bits}, grant_onehot: i{width}, grant_valid: i1)'
            )
            body.append(
                f'%{name}.captured = seq.firreg %{name}.hot clock %clk reset sync %reset, %zero{width} : i{width}'
            )
            outputs += [(name + '.index', bits), (name + '.hot', width),
                        (name + '.valid', 1), (name + '.captured', width)]
    ports = 'in %clock: i1, in %reset: i1, in %requests: i17, in %unlock: i1, '
    ports += ', '.join(f'out {name}: i{width}'
                       for name, width in outputs) + ', out io_success: i1'
    body.append('hw.output ' + ', '.join('%' + name for name, _ in outputs) +
                ', %false : ' + ', '.join('i' + str(width)
                                          for _, width in outputs) + ', i1')
    return 'module {\n' + '\n'.join(
        declarations
    ) + '\nhw.module @NativeRRArbiterHarness(' + ports + ') {\n' + '\n'.join(
        body) + '\n} }\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=sim / 'generated-src/prefix-feedback-guards')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    library = plugin(args.circt_opt, 'c++', sim / 'generated-src/compiler')

    def lower(name, text, fold=False):
        source, output = args.work / (name + '.mlir'), args.work / (
            name + '-lowered.mlir')
        source.write_text(text)
        subprocess.run([
            args.circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-partition{max-ops=0 resolve-feedback=false}' +
            (',canonicalize' if fold else '') + ')', '-o', output
        ],
                       check=True)
        return output

    rng, expected, total_bits, count = random.Random(1979), 0, 0, 0
    lines, roots = [], []
    for width in (1, 2, 3, 4, 5, 9, 17, 65, 129):
        mask = (1 << width) - 1
        values = list(range(1 << width)) if width <= 4 else [
            0, 1, mask, 1 << (width - 1),
            rng.getrandbits(width),
            rng.getrandbits(width)
        ]
        for a in values:
            b = rng.getrandbits(width)
            for step in sorted({1, min(2, width), min(3, width), width}):
                for operation in OPERATORS:
                    for reverse in (False, True):
                        for style in ('concat', ) if operation == 'and' else (
                                'concat', 'shift'):
                            name = f'%v{count}'
                            lines += [
                                f'{name}_a = hw.constant {a} : i{width}',
                                f'{name}_b = hw.constant {b} : i{width}'
                            ]
                            lines += expression(name, width, step, operation,
                                                reverse, style)
                            expected = (expected << width) | reference(
                                a, b, width, step, operation, reverse)
                            total_bits += width
                            count += 1
                            roots.append((name, width))
    text = 'module { hw.module @ArcModel(out result: i' + str(
        total_bits) + ') {\n'
    text += '\n'.join(lines) + '\n%packed = comb.concat ' + ', '.join(
        name for name, _ in roots)
    text += ' : ' + ', '.join('i' + str(width) for _, width in roots)
    text += f'\nhw.output %packed : i{total_bits}\n}} }}\n'
    output = lower('truth', text, True)
    sys.set_int_max_str_digits(0)
    match = re.search(r'hw.constant (-?\d+) : i' + str(total_bits) + r'\b',
                      output.read_text())
    assert match and int(match[1]) % (1 << total_bits) == expected

    start = '''module { hw.module @ArcModel(in %a: i9, in %b: i9, in %distance: i9, out result: i9) {
      %zero = hw.constant 0 : i1
      %low = comb.extract %cycle from 0 : (i9) -> i8
      %shifted = comb.concat %low, %zero : i8, i1
      %cycle = comb.or %shifted, %a : i9
      hw.output %cycle : i9
    } }'''
    guards = {
        'zero_shift':
        start.replace(
            '%low = comb.extract %cycle from 0 : (i9) -> i8\n      %shifted = comb.concat %low, %zero : i8, i1',
            '%c0 = hw.constant 0 : i9\n%shifted = comb.shl %cycle, %c0 : i9'),
        'dynamic_shift':
        start.replace(
            '%low = comb.extract %cycle from 0 : (i9) -> i8\n      %shifted = comb.concat %low, %zero : i8, i1',
            '%shifted = comb.shl %cycle, %distance : i9'),
        'arithmetic_shift':
        start.replace(
            '%low = comb.extract %cycle from 0 : (i9) -> i8\n      %shifted = comb.concat %low, %zero : i8, i1',
            '%c1 = hw.constant 1 : i9\n%shifted = comb.shrs %cycle, %c1 : i9'),
        'wrong_padding':
        start.replace('%zero = hw.constant 0', '%zero = hw.constant 1'),
        'dependent_base':
        start.replace(
            '%a : i9\n      hw.output',
            '%base : i9\n%base = comb.xor %cycle, %a : i9\n      hw.output'),
        'two_feedback_edges':
        start.replace(
            '%a : i9\n      hw.output',
            '%second : i9\n%c2 = hw.constant 2 : i9\n%second = comb.shl %cycle, %c2 : i9\n      hw.output'
        ),
    }
    for name, text in guards.items():
        output = lower(name, text)
        result = subprocess.run([
            args.circt_opt, output, '--convert-to-arcs', '-o',
            output.with_suffix('.arc.mlir')
        ],
                                capture_output=True,
                                text=True)
        assert result.returncode != 0 and 'combinational loop detected' in result.stderr, result.stderr
    metadata = start.replace(
        'comb.or %shifted, %a : i9',
        'comb.or bin %shifted, %a {sv.namehint = "retained_prefix"} : i9')
    output = lower('metadata', metadata)
    generic = output.with_suffix('.generic.mlir')
    subprocess.run(
        [args.circt_opt, output, '--mlir-print-op-generic', '-o', generic],
        check=True)
    assert 'sv.namehint = "retained_prefix"' in generic.read_text(
    ) and 'twoState' in generic.read_text()
    print(
        f'PASS: {count} independent shifted-prefix truth checks, 1..129 bits, OR/AND/XOR, both directions and nonunit/full-width strides; six prefix-specific exclusions (general solver disabled), names and two-state attributes',
        flush=True)


if __name__ == '__main__':
    main()
