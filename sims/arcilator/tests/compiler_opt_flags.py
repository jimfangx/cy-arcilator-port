#!/usr/bin/env python3
"""Exercise standard LLVM target/CPU/vectorization options through codegen."""
import json
import os
from pathlib import Path
import resource
import runpy
import shlex
import signal
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM / 'compiler'))
from compiler_modes import check
from codegen import __file__ as codegen_source


def check_flags(sdk, cxx, work):
    work.mkdir(parents=True, exist_ok=True)
    triple = subprocess.check_output([sdk / 'llvm-config', '--host-target'],
                                     text=True).strip()
    native = '-mtriple=' + triple + ' -mcpu=native'
    results = []
    for name, flags in [('native', native),
                        ('native-slp-off', native + ' -vectorize-slp=false')]:
        directory = work / name
        check(sdk, cxx, directory, opt_flags=flags)
        results.append(json.loads((directory / 'report.json').read_text()))

    # Reuse the packed-cache fixture which exhausts the admitted memory with
    # SLP enabled. This tests the extra option through the actual CLI pipeline.
    directory = work / 'packed-cache'
    directory.mkdir(exist_ok=True)
    source = directory / 'cache.ll'
    source.write_text(
        runpy.run_path(str(SIM / 'tests/codegen.py'))['cache_ir']())
    obj = directory / 'cache.o'
    flags = native + ' -vectorize-slp=false'
    command = [
        sys.executable, codegen_source, '--input=' + str(source),
        '--output=' + str(obj), '--llvm-opt=' + str(sdk / 'opt'),
        '--llc=' + str(sdk / 'llc'),
        '--cache=' + str(SIM / 'generated-src/compiler'),
        '--cxx=/not-installed/c++', '--pipeline=stock', '--no-abi-buffers',
        '--opt-flags=' + flags
    ]

    def limits():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))

    with (directory / 'compile.log').open('w') as log:
        child = subprocess.Popen(command,
                                 stdout=log,
                                 stderr=subprocess.STDOUT,
                                 preexec_fn=limits,
                                 start_new_session=True)
        try:
            code = child.wait(timeout=45)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
            raise AssertionError(
                'packed-cache standard option exceeded admission budget')
    assert code == 0, (directory / 'compile.log').read_text()
    executable = directory / 'check'
    subprocess.run([
        *shlex.split(cxx), '-O2', SIM / 'tests/codegen_cache.cpp', obj, '-o',
        executable
    ],
                   check=True)
    output = subprocess.check_output([executable], text=True)
    assert output.startswith(
        'PASS: 1536 exact full4096-byte cache images'), output
    report = dict(status='passed',
                  cases=results,
                  total_result_checks=589824,
                  floating_point_checks=65536,
                  packed_cache_images=1536,
                  packed_cache_under_2GiB_45seconds=True,
                  packed_cache_result=output.strip(),
                  host_triple=triple)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(
        'PASS: standard opt flags, 589824 integer/ABI results, 65536 FP cases and 1536 packed-cache images'
    )


if __name__ == '__main__':
    check_flags(
        Path(os.environ['CIRCT_BIN']).resolve(), os.environ.get('CXX', 'c++'),
        SIM / 'generated-src/compiler-opt-flags-test')
