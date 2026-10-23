#!/usr/bin/env python3
"""Check native Dromajo retirement/trap bindings against the real SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
ROOT = SIM.parent.parent
sys.path.insert(0, str(HERE))
from coverage import implementation_hashes


def program(work):
    """Encode RV64 instructions and calculate their results independently."""
    mask = (1 << 64) - 1
    registers = [0] * 32
    memory, events = {}, []
    pc = 0x80000000
    rng = random.Random(890174)

    def signed(value, bits):
        return (value & ((1 << (bits - 1)) - 1)) - (value & (1 << (bits - 1)))

    def emit(instruction, rd=0, value=0):
        nonlocal pc
        memory[pc] = instruction
        if rd:
            registers[rd] = value & mask
        check = len(events) % 7 != 4
        data = registers[rd] if rd else 0
        if not check:
            data ^= 0x80000000000000ff
        events.append(
            dict(pc=pc,
                 instruction=instruction,
                 data=data,
                 mstatus=(len(events) * 0x937ac41d01234567) & mask,
                 check=int(check)))
        pc += 4

    def immediate(rd, rs, imm, funct=0):
        a = registers[rs]
        if funct == 0:
            value = a + imm
        elif funct == 4:
            value = a ^ (imm & mask)
        elif funct == 1:
            value = (a << imm) & mask
        else:
            value = a >> imm
        emit(((imm & 4095) << 20) | (rs << 15) | (funct << 12) | (rd << 7)
             | 0x13, rd, value)

    def arithmetic(count):
        for _ in range(count):
            rd, rs, rt = [rng.randrange(1, 30) for _ in range(3)]
            a, b = registers[rs], registers[rt]
            choice = rng.randrange(10)
            if choice < 2:
                immediate(rd, rs, rng.randrange(-2048, 2048),
                          0 if choice == 0 else 4)
            elif choice < 4:
                immediate(rd, rs, rng.randrange(64), 1 if choice == 2 else 5)
            else:
                funct, high, opcode, value = {
                    4: (0, 0, 0x33, a + b),
                    5: (0, 0x20, 0x33, a - b),
                    6: (0, 1, 0x33, a * b),
                    7: (4, 0, 0x33, a ^ b),
                    8: (0, 0, 0x3b, signed((a + b) & 0xffffffff, 32)),
                    9: (0, 1, 0x3b, signed((a * b) & 0xffffffff, 32))
                }[choice]
                emit((high << 25) | (rt << 20) | (rs << 15) | (funct << 12) |
                     (rd << 7) | opcode, rd, value)

    emit(0x00001f17, 30, pc + 0x1000)  # AUIPC x30, 1: interrupt vector.
    emit(0x305f1073)  # CSRW mtvec, x30.
    immediate(31, 0, 128)
    emit(0x304fa073)  # CSRS mie, x31: enable timer interrupt.
    emit(0x30046073)  # CSRSI mstatus, 8: global interrupt enable.
    for rd in range(1, 30):
        immediate(rd, 0, rng.randrange(-2048, 2048))
    arithmetic(128)
    resume = pc
    events.append(dict(trap=(1 << 63) | 7))
    pc = 0x80001000
    emit(0x34102f73, 30, resume)  # Read MEPC.
    emit(0x304fb073)  # Clear timer interrupt enable.
    emit(0x30200073)  # MRET to interrupted instruction.
    pc = resume
    emit(0x80002f37, 30, signed(0x80002000, 32))
    immediate(30, 30, 32, 1)
    immediate(30, 30, 32, 5)
    emit(0x305f1073)  # Synchronous exception vector at 0x80002000.
    arithmetic(128)
    ecall = pc
    # Dromajo consumes ECALL itself as a step before entering the handler.
    emit(0x00000073)
    resume = pc
    events.append(dict(trap=11))
    pc = 0x80002000
    emit(0x34102f73, 30, ecall)
    immediate(30, 30, 4)
    emit(0x341f1073)  # Advance MEPC past ECALL.
    emit(0x30200073)
    pc = resume
    arithmetic(128)
    scratch = pc + 0x3000
    emit(0x00003e97, 29, scratch)  # Scratch address via AUIPC.
    immediate(29, 29, -(scratch % 8))  # Align the doubleword store/load.
    emit((17 << 20) | (29 << 15) | (3 << 12) | 0x23)  # SD x17, 0(x29).
    emit((29 << 15) | (3 << 12) | (13 << 7) | 0x03, 13, registers[17])
    memory[pc] = 0x0000006f  # End in an unexecuted JAL loop.
    source = ['.section .text', '.option norvc', '.globl _start', '_start:']
    offset = 0
    for address, word in sorted(memory.items()):
        if address - 0x80000000 != offset:
            source.append(f'.org {address - 0x80000000}')
        source.append(f'.word 0x{word:08x}')
        offset = address - 0x80000000 + 4
    (work / 'program.S').write_text('\n'.join(source) + '\n')
    (work / 'bootrom.bin').write_bytes(bytes.fromhex('13000000') * 4)
    index = cycle = 0
    lines = []
    masks = [7, 5, 2, 3, 0, 4, 6, 1]
    while index < len(events):
        reset = cycle < 3 or cycle % 29 == 0
        valid = 7 if reset else masks[cycle % len(masks)]
        slots = [
            dict(pc=0xffff000000000000 + i,
                 instruction=0xdeadbeef,
                 data=mask,
                 mstatus=mask,
                 check=1) for i in range(3)
        ]
        trap = cause = 0
        actual = 0
        if not reset:
            for slot in range(3):
                if index == len(events) or 'trap' in events[index]:
                    break
                if valid >> slot & 1:
                    slots[slot] = events[index]
                    actual |= 1 << slot
                    index += 1
            if index < len(events) and 'trap' in events[index]:
                trap, cause = 1, events[index]['trap']
                index += 1
        fields = [int(reset), valid if reset else actual, trap, cause]
        for slot in slots:
            fields.extend(slot[k] for k in ('pc', 'instruction', 'data',
                                            'mstatus', 'check'))
        lines.append(' '.join(map(str, fields)))
        cycle += 1
    (work / 'frames.txt').write_text('\n'.join(lines) + '\n')
    return dict(retirements=sum('trap' not in e for e in events),
                trap_causes=[e['trap'] for e in events if 'trap' in e],
                frames=cycle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', required=True, type=Path)
    parser.add_argument('--riscv', required=True, type=Path)
    parser.add_argument('--sdk-source', required=True, type=Path)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--build',
                        type=Path,
                        default=SIM / 'generated-src/dromajo-real-test')
    parser.add_argument('--reference-only', action='store_true')
    args = parser.parse_args()
    circt, riscv, sdk, library, work = [
        p.resolve() for p in (args.circt_bin, args.riscv, args.sdk_source,
                              args.library, args.build)
    ]
    work.mkdir(parents=True, exist_ok=True)
    drivers = ROOT / 'generators/testchipip/src/main/resources/testchipip/csrc'
    sources = [
        HERE / ('dromajo.' + suffix) for suffix in ('py', 'cpp', 'mlir')
    ]
    sources += [
        drivers / name for name in ('SimDromajoCosim.cc', 'dromajo_wrapper.cc',
                                    'dromajo_wrapper.h')
    ]
    sources += [library, sdk / 'include/dromajo_cosim.h']
    hashes = {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sources
    }
    before = implementation_hashes('native')
    report = dict(status='running',
                  implementation_sha256=before,
                  source_and_library_sha256=hashes)
    path = work / 'report.json'

    def run(*command, **kwargs):
        subprocess.run([str(p) for p in command], check=True, **kwargs)

    def save():
        path.write_text(json.dumps(report, indent=2) + '\n')

    save()
    try:
        report['program'] = program(work)
        params = dict(RESET_VECTOR='0x80000000',
                      MMIO_START='0x2000000',
                      MMIO_END='0x80000000',
                      PLIC_BASE='0xc000000',
                      PLIC_SIZE='0x4000000',
                      CLINT_BASE='0x2000000',
                      CLINT_SIZE='0x10000',
                      MEM_SIZE='0x10')
        (work / 'dromajo_params.h').write_text(''.join(
            f'#define DROMAJO_{key} "{value}"\n'
            for key, value in params.items()))
        run(riscv / 'bin/riscv64-unknown-elf-gcc', '-nostdlib',
            '-nostartfiles', '-march=rv64im_zicsr', '-mabi=lp64',
            '-Wl,-Ttext=0x80000000', '-Wl,-e,_start', '-Wl,--build-id=none',
            work / 'program.S', '-o', work / 'program.elf')
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        includes = [
            '-I' + str(p)
            for p in (work, sdk / 'include', drivers, SIM / 'native/include')
        ]
        wrappers = [
            '-Wl,--wrap=dromajo_' + name
            for name in ('init', 'step', 'raise_trap')
        ]
        selected = [
            drivers / 'SimDromajoCosim.cc', drivers / 'dromajo_wrapper.cc'
        ]
        run(*cxx, '-O2', '-std=c++17', '-DDROMAJO_REFERENCE', *includes,
            HERE / 'dromajo.cpp', *selected, library, *wrappers, '-lrt', '-o',
            work / 'reference')
        arguments = [
            work / 'frames.txt', '+drj_rom=' + str(work / 'bootrom.bin'),
            '+drj_bin=' + str(work / 'program.elf'), '+max-cycles=2000',
            '+expected_steps=' + str(report['program']['retirements']),
            '+expected_frames=' + str(report['program']['frames'])
        ]
        for negative in (False, True):
            with (work /
                  ('reference-negative.log' if negative else 'reference.log')
                  ).open('w') as log:
                run(work / 'reference',
                    *arguments,
                    *(['+negative=1'] if negative else []),
                    cwd=work,
                    stdout=log,
                    stderr=subprocess.STDOUT)
            completed = (work / ('reference-negative.log' if negative else
                                 'reference.log')).read_text()
            marker = 'mismatch rejected' if negative else 'trace accepted'
            if 'PASS: real Dromajo ' + marker not in completed:
                raise RuntimeError(
                    'original Dromajo test exited before completing its checks'
                )
        if not args.reference_only:
            run(sys.executable, SIM / 'partition.py', '--input',
                HERE / 'dromajo.mlir', '--output', work, '--top',
                'DromajoHarness', '--circt-opt', circt / 'circt-opt')
            run(sys.executable, SIM / 'compiler/model.py', '--input',
                work / 'partitioned.mlir', '--output', work / 'model.ll',
                '--state-file', work / 'model.json', '--arcilator',
                circt / 'arcilator', '--circt-opt', circt / 'circt-opt',
                '--mlir-translate', circt / 'mlir-translate', '--cache',
                SIM / 'generated-src/compiler')
            run(circt / 'llc', '-O2', '-relocation-model=pic', '-filetype=obj',
                work / 'model.ll', '-o', work / 'model.o')
            run(sys.executable, SIM / 'native.py', work / 'model.json',
                work / 'partition.json', work / 'model.cpp')
            (work / 'drivers.f').write_text(
                '\n'.join(map(str, [*selected, work / 'model.cpp'])) + '\n')
            support = shlex.split(
                subprocess.check_output(
                    [circt / 'llvm-config', '--libs', 'support'], text=True))
            flags = shlex.join([
                '-DARC_NATIVE_NO_MAIN', '-I' + str(circt.parent / 'include'),
                *includes
            ])
            link = shlex.join([
                '-L' + str(circt.parent / 'lib'), '-lCIRCTArcRuntime',
                *support,
                str(library), *wrappers, '-lrt', '-lz', '-lpthread', '-ldl'
            ])
            run(sys.executable, SIM / 'native_build.py', '--filelist',
                work / 'drivers.f', '--main', HERE / 'dromajo.cpp', '--model',
                work / 'model.o', '--build',
                work / 'objects', '--output', work / 'test', '--cxx',
                os.environ.get('CXX', 'c++'), '--cxxflags=' + flags,
                '--ldflags=' + link)
            symbols = subprocess.check_output(['nm', '-C', work / 'test'],
                                              text=True)
            if 'Verilated' in symbols or 'dromajo_cosim_step' not in symbols:
                raise RuntimeError(
                    'test did not link the real native Dromajo SDK')
            for negative in (False, True):
                log_path = work / ('native-negative.log'
                                   if negative else 'native.log')
                with log_path.open('w') as log:
                    run(work / 'test',
                        *arguments,
                        *(['+negative=1'] if negative else []),
                        cwd=work,
                        stdout=log,
                        stderr=subprocess.STDOUT)
                marker = 'mismatch rejected' if negative else 'trace accepted'
                if 'PASS: real Dromajo ' + marker not in log_path.read_text():
                    raise RuntimeError(
                        'native Dromajo test exited before completing its checks'
                    )
                if negative and 'Dromajo failed with exit code 8191' not in log_path.read_text(
                ):
                    raise RuntimeError(
                        'real SDK mismatch did not reach the native runtime')
            report['verilated_symbols_absent'] = True
        if implementation_hashes('native') != before or any(
                hashlib.sha256(Path(p).read_bytes()).hexdigest() != h
                for p, h in hashes.items()):
            raise RuntimeError('sources changed during the real Dromajo test')
        report[
            'status'] = 'reference_passed' if args.reference_only else 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        save()
        print('Report:', path, flush=True)


if __name__ == '__main__':
    main()
