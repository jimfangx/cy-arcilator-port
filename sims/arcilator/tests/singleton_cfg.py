#!/usr/bin/env python3
"""Check singleton arrays and zero-width values across complete function CFGs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/singleton-cfg')
    args = parser.parse_args()
    sdk, work = args.circt_bin.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = SIM / 'compiler/RemoveI0Types.cpp'
    library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler', source)

    def run(*command):
        subprocess.run(list(map(str, command)), check=True)

    original = subprocess.run([
        sdk / 'circt-opt', HERE / 'singleton_cfg.mlir',
        '--pass-pipeline=builtin.module(arc-remove-i0-types)', '-o',
        work / 'stock.mlir'
    ],
                              capture_output=True,
                              text=True)
    (work / 'stock.log').write_text(original.stdout + original.stderr)
    assert original.returncode == 1 and 'failed to legalize unresolved target materialization' in original.stderr, original.stderr
    run(sdk / 'circt-opt', HERE / 'singleton_cfg.mlir',
        '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-remove-i0-types)', '-o',
        work / 'converted.mlir')
    text = (work / 'converted.mlir').read_text()
    assert 'i0' not in text and 'hw.array<1x' not in text, text
    head = next(line for line in text.splitlines()
                if 'func.func @singleton_cfg' in line)
    assert 'chipyard.slot = "data"' in head and 'chipyard.slot = "discard"' not in head, head
    run(
        sdk / 'circt-opt', work / 'converted.mlir',
        '--pass-pipeline=builtin.module(lower-arc-to-llvm,reconcile-unrealized-casts)',
        '-o', work / 'llvm.mlir')
    run(sdk / 'mlir-translate', '--mlir-to-llvmir', work / 'llvm.mlir', '-o',
        work / 'model.ll')
    levels = []
    for level in ('0', '2'):
        run(sdk / 'opt', '-O' + level, work / 'model.ll', '-o',
            work / ('model-O' + level + '.bc'))
        run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj', work / ('model-O' + level + '.bc'), '-o',
            work / ('model-O' + level + '.o'))
        executable = work / ('test-O' + level)
        run(*shlex.split(os.environ.get('CXX', 'c++')), '-O' + level,
            HERE / 'singleton_cfg.cpp', work / ('model-O' + level + '.o'),
            '-o', executable)
        result = subprocess.run([executable], capture_output=True, text=True)
        (work / ('run-O' + level + '.log')).write_text(result.stdout +
                                                       result.stderr)
        result.check_returncode()
        assert 'PASS: 65536 singleton CFG checks' in result.stdout
        print(result.stdout, end='', flush=True)
        levels.append(
            dict(optimization=level,
                 checks=65536,
                 executable_sha256=hashlib.sha256(
                     executable.read_bytes()).hexdigest()))
    report = dict(status='passed',
                  source_sha256=hashlib.sha256(
                      source.read_bytes()).hexdigest(),
                  stock_returncode=original.returncode,
                  levels=levels)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
