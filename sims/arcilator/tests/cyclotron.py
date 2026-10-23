#!/usr/bin/env python3
"""Exercise native fuzzer/emulator bindings with the real Cyclotron library."""
import argparse
import hashlib
import json
import os
import re
import shlex
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(HERE))
from coverage import implementation_hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', required=True, type=Path)
    parser.add_argument('--library', required=True, type=Path)
    parser.add_argument('--kind',
                        choices=('fuzzer', 'emulator'),
                        default='fuzzer')
    parser.add_argument('--build', type=Path)
    args = parser.parse_args()
    if args.build is None:
        args.build = SIM / ('generated-src/cyclotron-real-test'
                            if args.kind == 'fuzzer' else
                            'generated-src/cyclotron-emulator-real-test')
    sdk, library, work = (p.resolve()
                          for p in (args.circt_bin, args.library, args.build))
    work.mkdir(parents=True, exist_ok=True)
    before = implementation_hashes('native')
    fixture = 'cyclotron' if args.kind == 'fuzzer' else 'cyclotron_emulator'
    sources = [HERE / (fixture + '.' + suffix) for suffix in ('cpp', 'mlir')]
    sources.append(Path(__file__).resolve())
    driver_name = 'SimMemFuzzer.cc' if args.kind == 'fuzzer' else 'SimEmulator.cc'
    driver = SIM.parent.parent / 'generators/radiance/src/main/resources/csrc' / driver_name
    sources.append(driver)
    hashes = {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in sources + [library]
    }
    report = dict(status='running',
                  kind=args.kind,
                  implementation_sha256=before,
                  source_and_library_sha256=hashes)
    path = work / 'report.json'

    def save():
        path.write_text(json.dumps(report, indent=2) + '\n')

    def run(*command, **kwargs):
        subprocess.run([str(value) for value in command], check=True, **kwargs)

    save()
    try:
        run(
            sys.executable, SIM / 'partition.py', '--input',
            HERE / (fixture + '.mlir'), '--output', work, '--top',
            'CyclotronHarness' if args.kind == 'fuzzer' else
            'CyclotronEmulatorHarness', '--circt-opt', sdk / 'circt-opt')
        run(sys.executable, SIM / 'compiler/model.py', '--input',
            work / 'partitioned.mlir', '--output', work / 'model.ll',
            '--state-file', work / 'model.json', '--arcilator',
            sdk / 'arcilator', '--circt-opt', sdk / 'circt-opt',
            '--mlir-translate', sdk / 'mlir-translate', '--cache',
            SIM / 'generated-src/compiler')
        run(sdk / 'llc', '-O2', '-relocation-model=pic', '-filetype=obj',
            work / 'model.ll', '-o', work / 'model.o')
        run(sys.executable, SIM / 'native.py', work / 'model.json',
            work / 'partition.json', work / 'model.cpp')
        (work / 'drivers.f'
         ).write_text(str(driver) + '\n' + str(work / 'model.cpp') + '\n')
        support = subprocess.check_output(
            [sdk / 'llvm-config', '--libs', 'support'], text=True).strip()
        flags = shlex.join(
            ['-DARC_NATIVE_NO_MAIN', '-I' + str(sdk.parent / 'include')])
        ldflags = shlex.join([
            '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime',
            *shlex.split(support),
            str(library), '-Wl,-rpath,' + str(library.parent), '-lz',
            '-lpthread', '-ldl'
        ])
        run(sys.executable, SIM / 'native_build.py', '--filelist',
            work / 'drivers.f', '--main', HERE / (fixture + '.cpp'), '--model',
            work / 'model.o', '--output',
            work / 'test', '--build', work / 'objects', '--cxx',
            os.environ.get('CXX', 'c++'), '--cxxflags=' + flags,
            '--ldflags=' + ldflags)
        symbols = subprocess.check_output(['nm', '-C', work / 'test'],
                                          text=True)
        callback = 'memfuzz_generate_rs' if args.kind == 'fuzzer' else 'emulator_tick_rs'
        if 'Verilated' in symbols or ' U ' + callback not in symbols:
            raise RuntimeError(
                'test did not link the real native Cyclotron interface')
        test_args = []
        if args.kind == 'emulator':
            run(*shlex.split(os.environ.get('CXX',
                                            'c++')), '-O2', '-std=c++17',
                '-DCYCLOTRON_REFERENCE', '-I' + str(SIM / 'native/include'),
                HERE / (fixture + '.cpp'), driver, library,
                '-Wl,-rpath,' + str(library.parent), '-o', work / 'reference')
            test_args = [work / 'expected.txt']
            with (work / 'reference.log').open('w') as log:
                run(work / 'reference',
                    *test_args,
                    stdout=log,
                    stderr=subprocess.STDOUT)
        log_path = work / (args.kind + '.log')
        with log_path.open('w') as log:
            run(work / 'test',
                *test_args,
                stdout=log,
                stderr=subprocess.STDOUT)
        if args.kind == 'emulator':
            direct = re.findall(r'\[@(\d+)\] emulator_tick_rs\(\)',
                                (work / 'reference.log').read_text())
            native = re.findall(r'\[@(\d+)\] emulator_tick_rs\(\)',
                                log_path.read_text())
            expected = sum(cycle >= 3 and cycle % 31 != 0
                           for cycle in range(512))
            if direct != native or native != [str(i) for i in range(expected)]:
                raise RuntimeError(
                    'real emulator call count/order or reset guard changed')
            report['exact_callback_count'] = expected
        if implementation_hashes('native') != before or any(
                hashlib.sha256(Path(p).read_bytes()).hexdigest() != digest
                for p, digest in hashes.items()):
            raise RuntimeError('sources changed during the real-library test')
        report.update(status='passed',
                      verilated_symbols_absent=True,
                      executable_sha256=hashlib.sha256(
                          (work / 'test').read_bytes()).hexdigest())
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        save()
        print('Report:', path, flush=True)


if __name__ == '__main__':
    main()
