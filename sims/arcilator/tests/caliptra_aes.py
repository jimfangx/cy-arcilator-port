#!/usr/bin/env python3
"""Execute the original generated Caliptra driver and RTL against OpenSSL."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
ROOT = SIM.parent.parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument(
        '--design',
        type=Path,
        required=True,
        help='Chisel-generated design.mlir for AES256ECBRocketConfig')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--openssl',
                        type=Path,
                        default=shutil.which('openssl'))
    args = parser.parse_args()
    if args.openssl is None:
        parser.error('OpenSSL is required for independent expected values')
    sdk, work = args.circt_bin.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    cxx = shlex.split(os.environ.get('CXX', 'c++'))
    report = dict(status='incomplete',
                  design=str(args.design.resolve()),
                  design_sha256=digest(args.design),
                  levels=[])

    def save():
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    save()

    def run(*command):
        subprocess.run([str(value) for value in command], check=True)

    text = args.design.read_text()
    modules = {
        m[1]: m[0]
        for m in re.finditer(
            r'^  hw.module(?: private)? @([^ (]+)\([^\n]*\) (?:attributes [^\n]* )?\{\n.*?^  }',
            text, re.M | re.S)
    }
    selected, pending, seen = [], ['AesCipherCoreDriver'], set()
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        if name not in modules:
            continue
        selected.append(modules[name])
        pending.extend(
            re.findall(r'hw.instance "[^"]+" @([^ (]+)', modules[name]))
    if not selected:
        raise ValueError(
            'design must contain the generated AesCipherCoreDriver')
    extern = next(line for line in text.splitlines() if line.startswith(
        '  sv.verbatim.module private @AesCipherCoreWrapper_AES256_ECB_NoMask('
    ))
    extern = extern.split(' attributes ')[0].replace('sv.verbatim.module',
                                                     'hw.module.extern')
    harness = '''  hw.module @AESNativeHarness(in %clock: i1, in %reset: i1, in %in_valid: i1, in %encrypt: i1, in %data: i128, in %key: i256, in %out_ready: i1, out in_ready: i1, out out_valid: i1, out result: i128) {
    %clk = seq.to_clock %clock
    %ready, %valid, %data_out = hw.instance "driver" @AesCipherCoreDriver(clock: %clk: !seq.clock, reset: %reset: i1, io_in_valid: %in_valid: i1, io_in_bits_encrypt: %encrypt: i1, io_in_bits_data: %data: i128, io_in_bits_key: %key: i256, io_out_ready: %out_ready: i1) -> (io_in_ready: i1, io_out_valid: i1, io_out_bits_data: i128)
    hw.output %ready, %valid, %data_out : i1, i1, i128
  }'''
    source = work / 'input.mlir'
    source.write_text('module {\n' + extern + '\n' + '\n'.join(selected) +
                      '\n' + harness + '\n}\n')
    core = ROOT / 'generators/caliptra-aes-acc/src/main/resources/vsrc/aes/aes_cipher_core/core.sv'
    report.update(original_core_sha256=digest(core),
                  selected_module_sha256={
                      re.search(r'@([^ (]+)', m)[1]:
                      hashlib.sha256(m.encode()).hexdigest()
                      for m in selected
                  },
                  test_source_sha256=digest(Path(__file__)),
                  test_cpp_sha256=digest(SIM /
                                         'tests/native_caliptra_aes.cpp'))
    save()
    filelist = work / 'blackboxes.f'
    filelist.write_text(str(core) + '\n')
    run(sys.executable, SIM / 'native_hw.py', '--input', source, '--output',
        work / 'design.mlir', '--filelist', filelist, '--work', work / 'rtl',
        '--circt-verilog', sdk / 'circt-verilog', '--circt-opt',
        sdk / 'circt-opt', '--cxx', shlex.join(cxx))
    run(sys.executable, SIM / 'partition.py', '--input', work / 'design.mlir',
        '--output', work, '--top', 'AESNativeHarness', '--circt-opt',
        sdk / 'circt-opt', '--cxx', shlex.join(cxx))
    run(sys.executable, SIM / 'compiler/partition.py', '--input',
        work / 'partitioned.mlir', '--output', work / 'outlined.mlirbc',
        '--circt-opt',
        sdk / 'circt-opt', '--cache', SIM / 'generated-src/compiler', '--cxx',
        shlex.join(cxx), '--max-ops', 4096, '--emit-bytecode',
        '--native-feedback')
    run(sdk / 'circt-opt', work / 'outlined.mlirbc', '--symbol-dce',
        '--canonicalize', '-o', work / 'model.mlir')
    run(sys.executable, SIM / 'compiler/model.py', '--input',
        work / 'model.mlir', '--output', work / 'model.ll', '--state-file',
        work / 'model.json', '--arcilator', sdk / 'arcilator', '--circt-opt',
        sdk / 'circt-opt', '--mlir-translate', sdk / 'mlir-translate',
        '--cache', SIM / 'generated-src/compiler', '--cxx', shlex.join(cxx),
        '--max-ops', 4096, '--', '--split-funcs-threshold=8192')
    run(sys.executable, SIM / 'compiler/llvm.py', work / 'model.ll')
    run(sys.executable, SIM / 'native.py', work / 'model.json',
        work / 'partition.json', work / 'model.cpp')

    rng, vectors = random.Random(0x7635), []
    for key in (bytes(32), bytes(range(32)), bytes([255] * 32),
                rng.randbytes(32)):
        vectors.append('2 0 0 0')
        blocks = [bytes(16), bytes(range(16)), rng.randbytes(16)]
        results = [
            subprocess.check_output([
                args.openssl, 'enc', '-aes-256-ecb', '-K',
                key.hex(), '-nosalt', '-nopad'
            ],
                                    input=block) for block in blocks
        ]
        for block, result in zip(blocks, results):
            vectors.append(f'1 {key.hex()} {block.hex()} {result.hex()}')
        for block, result in zip(blocks, results):
            vectors.append(f'0 {key.hex()} {result.hex()} {block.hex()}')
        vectors.append(f'1 {key.hex()} {blocks[1].hex()} {results[1].hex()}')
    vector_file = work / 'vectors.txt'
    vector_file.write_text('\n'.join(vectors) + '\n')
    report.update(reference=subprocess.check_output([args.openssl, 'version'],
                                                    text=True).strip(),
                  reference_binary_sha256=digest(args.openssl),
                  vector_sha256=digest(vector_file))
    objects = []
    for native in sorted((SIM / 'native').glob('*.cpp')):
        obj = work / (native.stem + '.o')
        objects.append(obj)
        run(*cxx, '-O2', '-std=c++17', '-DARC_NATIVE_NO_MAIN',
            *(['-fno-rtti'] if native.name == 'files.cpp' else []),
            '-I' + str(SIM), '-I' + str(SIM / 'native/include'),
            '-I' + str(sdk.parent / 'include'), '-c', native, '-o', obj)
    libs = shlex.split(
        subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                                text=True))
    for level in (0, 2):
        obj, executable = work / f'model-O{level}.o', work / f'test-O{level}'
        run(sdk / 'opt', f'-O{level}', work / 'model.ll', '-o',
            work / f'model-O{level}.bc')
        run(sdk / 'llc', f'-O{level}', '-relocation-model=pic',
            '-filetype=obj', work / f'model-O{level}.bc', '-o', obj)
        run(*cxx, '-O2', '-std=c++17', '-DARC_NATIVE_NO_MAIN', '-I' + str(SIM),
            '-I' + str(SIM / 'native/include'),
            '-I' + str(sdk.parent / 'include'), work / 'model.cpp',
            SIM / 'tests/native_caliptra_aes.cpp', *objects, obj,
            '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz',
            '-lpthread', '-ldl', '-o', executable)
        result = subprocess.run([executable, vector_file],
                                capture_output=True,
                                text=True)
        (work / f'run-O{level}.log').write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='', flush=True)
        report['levels'].append(
            dict(optimization=level,
                 returncode=result.returncode,
                 executable_sha256=digest(executable)))
        save()
        result.check_returncode()
    report.update(
        status='passed',
        checks_per_level=28,
        resets_per_level=4,
        scope=
        'original generated cipher driver and Caliptra RTL; full RoCC/DMA CPU workload unverified'
    )
    save()
    print('Caliptra AES report:', work / 'report.json', flush=True)


if __name__ == '__main__':
    main()
