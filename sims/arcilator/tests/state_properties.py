#!/usr/bin/env python3
"""Exercise enabled aggregate state through the production model compiler."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-bin', type=Path, required=True)
args = parser.parse_args()
sdk = args.circt_bin.resolve()
here = Path(__file__).resolve().parent
sim = here.parent
work = sim / 'generated-src/enabled-state-test'
work.mkdir(parents=True, exist_ok=True)


def run(*command):
    subprocess.run([str(value) for value in command], check=True)


run('python3', sim / 'compiler/model.py', '--input',
    here / 'state_properties.mlir', '--output', work / 'model.ll',
    '--state-file', work / 'model.json', '--arcilator', sdk / 'arcilator',
    '--circt-opt', sdk / 'circt-opt', '--mlir-translate',
    sdk / 'mlir-translate', '--cache', sim / 'generated-src/compiler',
    '--max-ops', '2')
model, = json.loads((work / 'model.json').read_text())
ports = [p for p in model['states'] if p['type'] in ('input', 'output')]
(work / 'state_properties_layout.h').write_text(''.join(
    f'constexpr unsigned {p["name"]}Offset = {p["offset"]};\n' for p in ports))
run(sdk / 'opt', '-O2', work / 'model.ll', '-o', work / 'model.bc')
run(sdk / 'llc', '-O2', '-relocation-model=pic', '-filetype=obj',
    work / 'model.bc', '-o', work / 'model.o')
libs = shlex.split(
    subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                            text=True))
run(*shlex.split(os.environ.get('CXX',
                                'c++')), '-O2', '-std=c++17', '-I' + str(work),
    '-I' + str(sdk.parent / 'include'), here / 'state_properties.cpp',
    work / 'model.o', '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime',
    *libs, '-lz', '-lpthread', '-ldl', '-o', work / 'test')
run(work / 'test')
