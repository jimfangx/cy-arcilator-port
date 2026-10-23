#!/usr/bin/env python3
"""Validate shared ROM startup for direct and Ibex-style vectored traps."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

SIM = Path(__file__).resolve().parent.parent
ROM = SIM.parent.parent / 'generators/testchipip/src/main/resources/testchipip/bootrom'


def jump_target(data, pc, base):
    word = int.from_bytes(data[pc - base:pc - base + 4], 'little')
    if word & 3 == 3:
        assert word & 0xfff == 0x6f, hex(word)  # jal x0
        immediate = (((word >> 31) & 1) << 20 | ((word >> 21) & 1023) << 1 |
                     ((word >> 20) & 1) << 11 | ((word >> 12) & 255) << 12)
        bits = 21
    else:
        assert word & 0xe003 == 0xa001, hex(word)  # c.j
        immediate = (((word >> 12) & 1) << 11 | ((word >> 11) & 1) << 4 |
                     ((word >> 9) & 3) << 8 | ((word >> 8) & 1) << 10 |
                     ((word >> 7) & 1) << 6 | ((word >> 6) & 1) << 7 |
                     ((word >> 3) & 7) << 1 | ((word >> 2) & 1) << 5)
        bits = 12
    if immediate & (1 << (bits - 1)):
        immediate -= 1 << bits
    return pc + immediate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--riscv', type=Path, default=os.environ.get('RISCV'))
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/bootrom-tests')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)

    def tool(name):
        name = 'riscv64-unknown-elf-' + name
        path = args.riscv / 'bin' / name if args.riscv else shutil.which(name)
        if not path or not Path(path).is_file():
            parser.error('missing RISC-V tool: ' + name)
        return str(path)

    results = []
    for xlen, abi in ((32, 'ilp32'), (64, 'lp64')):
        for compressed in (False, True):
            name = f'rv{xlen}' + ('-compressed' if compressed else '')
            elf, image = args.work / (name + '.elf'), args.work / (name +
                                                                   '.img')
            subprocess.run([
                tool('gcc'), '-mabi=' + abi, f'-march=rv{xlen}ima' +
                ('c' if compressed else '') + '_zicsr',
                '-T' + str(ROM / 'linker.ld'), ROM / 'bootrom.S', '-nostdlib',
                '-static', '-o', elf
            ],
                           check=True)
            subprocess.run([
                tool('objcopy'), '-O', 'binary', '--change-addresses=-0x10000',
                elf, image
            ],
                           check=True)
            nm = subprocess.check_output([tool('nm'), '-n', elf], text=True)
            symbols = {
                row[2]: int(row[0], 16)
                for line in nm.splitlines() if len(row := line.split()) == 3
            }
            data = image.read_bytes()
            base, vector = symbols['ROM_BASE'], symbols['_trap_vector']
            assert vector % 256 == 0, symbols
            # The appended DTB starts at EOF, without trailing assembler or
            # linker padding after its symbol. The ROM passes this address on.
            assert symbols['_dtb'] == base + len(data), (symbols, len(data))
            assert symbols['_dtb'] % 8 == 0
            assert jump_target(data, symbols['_hang80'],
                               base) == symbols['_hang']
            # Check what the startup code actually writes into mtvec.
            first = int.from_bytes(data[:4], 'little')
            second = int.from_bytes(data[4:8], 'little')
            assert first & 0xfff == 0x517 and second & 0xfffff == 0x50513
            immediate = second >> 20
            if immediate & 0x800:
                immediate -= 4096
            written = base + (first & 0xfffff000) + immediate
            assert written == vector
            # Direct mode enters the first slot. Ibex forces MODE=1 and
            # BASE[7:0]=0; every interrupt cause must still reach _start.
            assert jump_target(data, written, base) == symbols['_start']
            for cause in range(32):
                pc = (written & ~255) + 4 * cause
                assert data[pc - base] & 3 == 3, (name, cause)
                assert jump_target(data, pc,
                                   base) == symbols['_start'], (name, cause)
            if not compressed:
                assert data == (ROM /
                                f'bootrom.rv{xlen}.img').read_bytes(), name
            results.append(
                dict(variant=name,
                     bytes=len(data),
                     vector=vector,
                     dtb=symbols['_dtb'],
                     interrupt_slots=32))
    (args.work /
     'report.json').write_text(json.dumps(results, indent=2) + '\n')
    print(
        'PASS: RV32/RV64 ROM images, direct and 32 vectored trap entries, '
        'reset trampoline, mtvec alignment, compressed-code variants and exact DTB placement',
        flush=True)


if __name__ == '__main__':
    main()
