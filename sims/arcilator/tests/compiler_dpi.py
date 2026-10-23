#!/usr/bin/env python3
"""Execute scalar, Boolean and multiple-output DPI across suspension edges."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

here = Path(__file__).resolve().parent
sim = here.parent
sys.path.insert(0, str(sim))
from native_hw import prepare_rtl, prepare_native_signals
from compiler.partition import plugin

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=sim / 'generated-src/compiler-dpi-test')
parser.add_argument('--input', type=Path, default=here / 'compiler_dpi.mlir')
parser.add_argument('--opt-level', choices=('0', '1', '2', '3'), default='2')
args = parser.parse_args()
sdk, work = args.circt_bin.resolve(), args.work.resolve()
work.mkdir(parents=True, exist_ok=True)


def run(*command):
    subprocess.run([str(value) for value in command], check=True)


library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                 sim / 'generated-src/compiler', sim / 'compiler/RTL.cpp')
prepare_rtl(args.input, work / 'prepared-rtl.mlir', sdk / 'circt-opt', library)
state_library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                       sim / 'generated-src/compiler',
                       sim / 'compiler/SignalState.cpp')
prepare_native_signals(work / 'prepared-rtl.mlir', work / 'prepared.mlir',
                       sdk / 'circt-opt', state_library)
run('python3', sim / 'compiler/model.py', '--input', work / 'prepared.mlir',
    '--output', work / 'model.ll', '--state-file', work / 'model.json',
    '--arcilator', sdk / 'arcilator', '--circt-opt', sdk / 'circt-opt',
    '--mlir-translate', sdk / 'mlir-translate', '--cache',
    sim / 'generated-src/compiler', '--max-ops', '2')
model, = json.loads((work / 'model.json').read_text())
ports = [p for p in model['states'] if p['type'] in ('input', 'output')]
(work / 'compiler_dpi_layout.h').write_text(''.join(
    f'constexpr unsigned {p["name"]}Offset = {p["offset"]};\n' for p in ports))
optimization = '-O' + args.opt_level
run(sdk / 'opt', optimization, work / 'model.ll', '-o', work / 'model.bc')
run(sdk / 'llc', optimization, '-relocation-model=pic', '-filetype=obj',
    work / 'model.bc', '-o', work / 'model.o')
libs = shlex.split(
    subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                            text=True))
run(*shlex.split(os.environ.get('CXX', 'c++')), optimization, '-std=c++17',
    '-I' + str(work), '-I' + str(sdk.parent / 'include'),
    here / 'compiler_dpi.cpp', work / 'model.o',
    '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz',
    '-lpthread', '-ldl', '-o', work / 'test')
run(work / 'test')
