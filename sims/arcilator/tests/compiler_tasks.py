#!/usr/bin/env python3
"""Check non-suspending task calls through native CFG and LLVM lowering."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(SIM))
from native_hw import prepare_rtl, prepare_native_signals
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/compiler-task-test')
    args = parser.parse_args()
    sdk, work = args.circt_bin.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)

    def run(*command):
        subprocess.run([str(value) for value in command], check=True)

    cxx = os.environ.get('CXX', 'c++')
    library = plugin(sdk / 'circt-opt', cxx, SIM / 'generated-src/compiler',
                     SIM / 'compiler/RTL.cpp')
    # Without canonicalization or DCE, every operation and edge in these
    # suspending and recursive call graphs must be byte-for-byte unchanged.
    guard = HERE / 'compiler_tasks_guards.mlir'
    run(sdk / 'circt-opt', guard, '-o', work / 'guards-before.mlir')
    run(sdk / 'circt-opt', guard, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-inline-instant-tasks)', '-o',
        work / 'guards-after.mlir')
    assert (work / 'guards-before.mlir').read_bytes() == (
        work / 'guards-after.mlir').read_bytes()
    print(
        'PASS: direct/transitive waits, halts, self recursion and mutual recursion remain unchanged',
        flush=True)
    prepare_rtl(HERE / 'compiler_tasks.mlir', work / 'rtl.mlir',
                sdk / 'circt-opt', library)
    assert 'llhd.call_coroutine' not in (work / 'rtl.mlir').read_text()
    assert 'llhd.coroutine' not in (work / 'rtl.mlir').read_text()
    signals = plugin(sdk / 'circt-opt', cxx, SIM / 'generated-src/compiler',
                     SIM / 'compiler/SignalState.cpp')
    prepare_native_signals(work / 'rtl.mlir', work / 'prepared.mlir',
                           sdk / 'circt-opt', signals)
    run(sys.executable, SIM / 'compiler/model.py', '--input',
        work / 'prepared.mlir', '--output', work / 'model.ll', '--state-file',
        work / 'model.json', '--arcilator', sdk / 'arcilator', '--circt-opt',
        sdk / 'circt-opt', '--mlir-translate', sdk / 'mlir-translate',
        '--cache', SIM / 'generated-src/compiler', '--max-ops', '2')
    model, = json.loads((work / 'model.json').read_text())
    (work / 'compiler_tasks_layout.h').write_text(''.join(
        f'constexpr unsigned {p["name"]}Offset = {p["offset"]};\n'
        for p in model['states'] if p['type'] in ('input', 'output')))
    libs = shlex.split(
        subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                                text=True))
    for level in ('0', '2'):
        run(sdk / 'opt', '-O' + level, work / 'model.ll', '-o',
            work / 'model.bc')
        run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj', work / 'model.bc', '-o', work / 'model.o')
        run(*shlex.split(cxx), '-O' + level, '-std=c++17', '-I' + str(work),
            '-I' + str(sdk.parent / 'include'), HERE / 'compiler_tasks.cpp',
            work / 'model.o', '-L' + str(sdk.parent / 'lib'),
            '-lCIRCTArcRuntime', *libs, '-lz', '-lpthread', '-ldl', '-o',
            work / 'test')
        run(work / 'test')


if __name__ == '__main__':
    main()
