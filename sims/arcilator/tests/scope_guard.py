"""Check native DPI scope ABI, unwind behavior, threads, and late loading.

This is a C++ test; it needs no RTL model, firmware, or CIRCT SDK.
"""
from pathlib import Path
import argparse
import ctypes
import json
import os
import shlex
import subprocess
import sys

main = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
parser.add_argument('--build', type=Path, default=main / 'generated-src/scope-guard-test')
args = parser.parse_args()
root = args.build.resolve()
root.mkdir(parents=True, exist_ok=True)
report = {'status': 'running', 'steps': [], 'semantics': [], 'late_loading': []}
report_path = root / 'report.json'
cxx = shlex.split(args.cxx)
includes = ['-I' + str(main), '-I' + str(main / 'native/include')]
fixture = main / 'tests/scope_guard.cpp'
compat = main / 'native/compat.cpp'

def save():
    report_path.write_text(json.dumps(report, indent=2) + '\n')

def run(name, command):
    command = list(map(str, command))
    log = root / (name + '.log')
    row = {'name': name, 'command': command, 'log': str(log)}
    report['steps'].append(row)
    save()
    with log.open('w') as stream:
        process = subprocess.run(command, cwd=root, stdout=stream,
                                 stderr=subprocess.STDOUT)
    row['returncode'] = process.returncode
    save()
    if process.returncode:
        raise RuntimeError(name + ': ' + log.read_text()[-3000:])
    return log.read_text()

try:
    abi = root / 'legacy.o'
    run('compile-legacy-ABI', cxx + ['-O3', '-std=c++17', *includes,
        '-DARC_SCOPE_ABI_PROBE', '-c', fixture, '-o', abi])
    expected = {'nested': 1024, 'exceptions': 1024, 'legacy_ABI': 1024,
                'thread_scopes': 4096, 'registry_cleanup': 1,
                'scope_bytes': ctypes.sizeof(ctypes.c_void_p)}
    for label, flags in [('O0', ['-O0']), ('O3', ['-O3']),
                         ('ubsan', ['-O2', '-fsanitize=undefined',
                                    '-fno-sanitize-recover=all'])]:
        for inline in [False, True]:
            name = label + ('-inline' if inline else '-public')
            executable = root / name
            run('compile-' + name, cxx + [*flags, '-std=c++17', '-pthread',
                *includes, '-DINLINE_SCOPE=' + str(int(inline)),
                fixture, abi, compat, '-o', executable])
            text = run('execute-' + name, [executable])
            assert text.startswith('PASS '), text
            checks = {key: int(value) for key, value in
                      (word.split('=', 1) for word in text.split()[1:])}
            assert checks == expected, checks
            report['semantics'].append({'variant': name, 'checks': checks})
    if sys.platform.startswith('linux'):
        host = root / 'late-host'
        run('compile-late-host', cxx + ['-O3', '-std=c++17', '-pthread',
            '-DARC_SCOPE_SHARED_HOST', fixture, '-ldl', '-o', host])
        for level in [0, 3]:
            module = root / ('late-O' + str(level) + '.so')
            run('compile-late-O' + str(level), cxx + ['-O' + str(level),
                '-std=c++17', '-pthread', '-shared', '-fPIC', *includes,
                '-DARC_SCOPE_SHARED_MODULE', fixture, compat, '-o', module])
            relocations = run('relocations-O' + str(level), ['readelf', '-rW', module])
            symbols = run('symbols-O' + str(level), ['readelf', '-sW', module])
            dynamic = run('exports-O' + str(level), ['nm', '-D', module])
            assert '_TPOFF' not in relocations and '_TPREL' not in relocations
            assert ('DTPMOD' in relocations or 'TLSDESC' in relocations), relocations
            assert 'current_scope' not in dynamic
            private = [line for line in symbols.splitlines() if 'current_scope' in line]
            assert private and all('TLS' in line and
                ('LOCAL' in line or 'HIDDEN' in line) for line in private), private
            assert 'DpiContextC' in dynamic and 'DpiContextD' in dynamic
            text = run('execute-late-O' + str(level), [host, module])
            assert text.startswith('PASS recreations=2 threads_started_before_dlopen=8 '), text
            report['late_loading'].append({'optimization': level,
                'private_dynamic_TLS': True, 'public_ABI_preserved': True,
                'checks': text.strip()})
    report['status'] = 'passed'
    save()
    print('DPI scope guards passed: public/inline ABI, unwind, threads, late loading')
except BaseException as error:
    report.update(status='failed', error=repr(error))
    save()
    raise
