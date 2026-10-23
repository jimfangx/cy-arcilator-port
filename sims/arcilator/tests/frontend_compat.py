#!/usr/bin/env python3
"""Run the numerical compatibility regressions for the selected SV frontend."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.verilog.build import build
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path)
    args = parser.parse_args()
    sdk = args.circt_bin.resolve()
    (SIM / 'generated-src').mkdir(exist_ok=True)
    work = args.work or Path(
        tempfile.mkdtemp(prefix='frontend-compat-', dir=SIM / 'generated-src'))
    work.mkdir(parents=True, exist_ok=True)
    report = dict(status='running', cases=[])

    def save():
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    save()
    frontend = build(
        SimpleNamespace(circt_verilog=sdk / 'circt-verilog',
                        cache=SIM / 'generated-src/compiler/verilog',
                        circt_source=None,
                        slang_source=None,
                        slang_build=None,
                        jobs=4))
    report.update(frontend=str(frontend),
                  frontend_sha256=hashlib.sha256(
                      frontend.read_bytes()).hexdigest(),
                  compiler=os.environ.get('CXX', 'c++'))
    union_library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                           SIM / 'generated-src/compiler',
                           SIM / 'compiler/UnionBits.cpp')
    report['union_source_sha256'] = hashlib.sha256(
        (SIM / 'compiler/UnionBits.cpp').read_bytes()).hexdigest()
    for test in ('test.py', 'port_connections_test.py', 'increments_test.py',
                 'unpacked_reshape_test.py', 'packed_cases_test.py',
                 'nonoverlap_test.py', 'unknown_comparisons_test.py'):
        source = SIM / 'compiler/verilog' / test
        case = dict(test=test,
                    status='running',
                    source_sha256=hashlib.sha256(
                        source.read_bytes()).hexdigest())
        report['cases'].append(case)
        save()
        log = work / (source.stem + '.log')
        with log.open('w') as stream:
            result = subprocess.run([
                sys.executable, source, '--circt-verilog', frontend,
                '--circt-bin', sdk, '--work', work / source.stem,
                *(['--union-library', union_library]
                  if test == 'packed_cases_test.py' else [])
            ],
                                    stdout=stream,
                                    stderr=subprocess.STDOUT)
        case.update(status='passed' if result.returncode == 0 else 'failed',
                    returncode=result.returncode,
                    log=str(log))
        save()
        print(log.read_text()[-3000:], end='', flush=True)
        if result.returncode:
            report['status'] = 'failed'
            save()
            result.check_returncode()
    report['status'] = 'passed'
    save()
    print('Frontend compatibility report:', work / 'report.json', flush=True)


if __name__ == '__main__':
    main()
