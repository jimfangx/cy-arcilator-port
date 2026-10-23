#!/usr/bin/env python3
"""Build the unchanged upstream RV32 ADD test with Ibex-compatible trap alignment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

SIM = Path(__file__).resolve().parent.parent
ROOT = SIM.parent.parent


def build(riscv, work, check_reference=False):
    source = ROOT / 'toolchains/riscv-tools/riscv-tests'
    inputs = [
        source / name
        for name in ('env/p/riscv_test.h', 'env/encoding.h', 'env/p/link.ld',
                     'isa/rv32ui/add.S', 'isa/rv64ui/add.S',
                     'isa/macros/scalar/test_macros.h')
    ]
    for path in inputs:
        if not path.is_file():
            raise ValueError('the riscv-tests source checkout is required: ' +
                             str(path))
    work.mkdir(parents=True, exist_ok=True)
    env = work / 'env/p'
    env.mkdir(parents=True, exist_ok=True)
    original = inputs[0].read_text()
    # The stock environment probes optional CSRs by setting mtvec to a local
    # recovery label, and places its ECALL handler after a four-byte jump.
    # Ibex rounds those addresses down to 256 bytes. Preserve all probes,
    # privilege setup, test instructions and pass/fail logic; align their
    # existing landing pads to the core's documented WARL constraint.
    header, changes = re.subn(r'(\.align\s+)2(\s*;)', r'\g<1>8\2', original)
    if changes < 5:
        raise ValueError('unrecognized riscv-tests trap-alignment environment')
    (env / 'riscv_test.h').write_text(header)
    shutil.copyfile(inputs[1], env.parent / 'encoding.h')
    binary = work / 'rv32ui-p-add.riscv'
    tools = riscv / 'bin'
    compiler = tools / 'riscv64-unknown-elf-gcc'
    command = [
        compiler, '-march=rv32ima_zicsr', '-mabi=ilp32', '-I' + str(env),
        '-I' + str(source / 'isa/macros/scalar'),
        '-T' + str(source / 'env/p/link.ld'), '-nostdlib', '-static',
        source / 'isa/rv32ui/add.S', '-o', binary
    ]
    subprocess.run(command, check=True)
    symbols = subprocess.check_output(
        [tools / 'riscv64-unknown-elf-nm', '-n', binary], text=True)
    names = {
        row[2]: int(row[0], 16)
        for line in symbols.splitlines() if len(row := line.split()) == 3
    }
    if names['trap_vector'] % 256:
        raise ValueError('the ADD test trap vector is not Ibex-aligned')
    numbers = sorted(
        int(name[5:]) for name in names if re.fullmatch(r'test_\d+', name))
    if numbers != list(range(2, 39)):
        raise ValueError('the upstream ADD test cases changed: ' +
                         str(numbers))
    image = work / 'rv32ui-p-add.bin'
    subprocess.run(
        [tools / 'riscv64-unknown-elf-objcopy', '-O', 'binary', binary, image],
        check=True)
    data = image.read_bytes()
    targets = []
    for offset in range(8, len(data) - 3, 4):
        word = int.from_bytes(data[offset:offset + 4], 'little')
        if word & 0xfff07fff != 0x30501073:  # csrw mtvec, rs1
            continue
        first = int.from_bytes(data[offset - 8:offset - 4], 'little')
        second = int.from_bytes(data[offset - 4:offset], 'little')
        register = (word >> 15) & 31
        if (first & 127 != 0x17 or (first >> 7) & 31 != register
                or second & 0x707f != 0x13 or (second >> 7) & 31 != register
                or (second >> 15) & 31 != register):
            raise ValueError('unrecognized mtvec address calculation')
        high = first & 0xfffff000
        if high & 0x80000000:
            high -= 1 << 32
        low = second >> 20
        if low & 0x800:
            low -= 4096
        target = (names['_start'] + offset - 8 + high + low) & 0xffffffff
        if target % 256:
            raise ValueError('unaligned optional-CSR recovery address: ' +
                             hex(target))
        targets.append(target)
    if len(targets) != 5:
        raise ValueError('unexpected number of mtvec writes: ' + str(targets))
    report = dict(source_sha256={
        str(p.relative_to(ROOT)):
        hashlib.sha256(p.read_bytes()).hexdigest()
        for p in inputs
    },
                  environment_alignment_edits=changes,
                  trap_vector=names['trap_vector'],
                  mtvec_targets=targets,
                  test_numbers=numbers,
                  binary_sha256=hashlib.sha256(
                      binary.read_bytes()).hexdigest())
    if check_reference:
        spike = tools / 'spike'
        positive = subprocess.run([spike, '--isa=rv32ima_zicsr', binary],
                                  capture_output=True,
                                  text=True)
        (work / 'spike-positive.log').write_text(positive.stdout +
                                                 positive.stderr)
        positive.check_returncode()
        # Replacing ADD with SUB must fail the unchanged expected-value checks.
        negative = work / 'rv32ui-sub-injected.riscv'
        subprocess.run([*command[:-2], '-Dadd=sub', '-o', negative],
                       check=True)
        result = subprocess.run([spike, '--isa=rv32ima_zicsr', negative],
                                capture_output=True,
                                text=True)
        (work / 'spike-injected-sub.log').write_text(result.stdout +
                                                     result.stderr)
        if result.returncode != 3:
            raise ValueError(
                'the injected arithmetic fault did not fail test 3')
        report['spike_reference'] = dict(positive_exit=0,
                                         injected_sub_exit=result.returncode)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--riscv',
                        type=Path,
                        default=os.environ.get('RISCV'),
                        required=not os.environ.get('RISCV'))
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/ibex-isa-test')
    parser.add_argument('--check-reference', action='store_true')
    args = parser.parse_args()
    try:
        binary = build(args.riscv.resolve(), args.work.resolve(),
                       args.check_reference)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Ibex ISA workload: {error}\n')
    print(
        'PASS: all 37 upstream ADD cases and five aligned trap/recovery targets; '
        + str(binary),
        flush=True)


if __name__ == '__main__':
    main()
