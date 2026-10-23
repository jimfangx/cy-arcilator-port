#!/usr/bin/env python3
"""Compile native DPI calls against the unmodified vendor Vortex driver."""
import argparse
import json
import os
import resource
import signal
from pathlib import Path
import shlex
import subprocess

here = Path(__file__).resolve().parent
sim = here.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=sim / 'generated-src/vortex-dpi-test')
parser.add_argument('--xlen',
                    choices=('32', '64'),
                    nargs='+',
                    default=['32', '64'])
parser.add_argument('--opt-level', choices=('0', '1', '2', '3'), default='2')
parser.add_argument('--unwritten-outputs', action='store_true',
                    help='invoke arithmetic callbacks even when their input enable is false')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
sdk = args.circt_bin.resolve().parent
vendor = sim.parent.parent / 'generators/radiance/src/main/resources/vsrc/vortex'
driver = vendor / 'hw/dpi/util_dpi.cpp'
if not driver.is_file():
    parser.error('the Vortex submodule is required: ' + str(driver))


def run(*command):
    subprocess.run([str(value) for value in command], check=True)


libs = shlex.split(
    subprocess.check_output([sdk / 'bin/llvm-config', '--libs', 'support'],
                            text=True))
for xlen in args.xlen:
    work = args.work.resolve() / ('xlen' + xlen)
    work.mkdir(parents=True, exist_ok=True)
    source = work / 'design.mlir'
    text = (here / 'vortex_dpi.mlir').read_text().replace('iWORD', 'i' + xlen)
    if args.unwritten_outputs:
        text = text.replace('clock %clk enable %enable', 'clock %clk')
    source.write_text(text)
    run('python3', sim / 'compiler/model.py', '--input', source, '--output',
        work / 'model.ll', '--state-file', work / 'model.json', '--arcilator',
        sdk / 'bin/arcilator', '--circt-opt', sdk / 'bin/circt-opt',
        '--mlir-translate', sdk / 'bin/mlir-translate', '--cache',
        sim / 'generated-src/compiler', '--max-ops', '2')
    model, = json.loads((work / 'model.json').read_text())
    (work / 'vortex_dpi_layout.h').write_text(''.join(
        f'constexpr unsigned {p["name"]}Offset = {p["offset"]};\n'
        for p in model['states'] if p['type'] in ('input', 'output')))
    optimization = '-O' + args.opt_level
    run(sdk / 'bin/opt', optimization, work / 'model.ll', '-o',
        work / 'model.bc')
    run(sdk / 'bin/llc', optimization, '-relocation-model=pic',
        '-filetype=obj', work / 'model.bc', '-o', work / 'model.o')
    run(*shlex.split(os.environ.get('CXX', 'c++')), optimization,
        '-std=c++17', '-DVORTEX_DPI_XLEN=' + xlen,
        *(['-DVORTEX_DPI_CALL_WHEN_DISABLED'] if args.unwritten_outputs else []),
        *(['-DXLEN_64'] if xlen == '64' else []), '-I' + str(sim),
        '-I' + str(sim / 'native/include'), '-I' + str(work),
        '-I' + str(sdk / 'include'), '-I' + str(vendor / 'sim/common'),
        here / 'vortex_dpi.cpp', driver, sim / 'native/compat.cpp',
        work / 'model.o',
        '-Wl,--wrap=dpi_imul,--wrap=dpi_idiv,--wrap=dpi_uuid_gen',
        '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz',
        '-lpthread', '-ldl', '-o', work / 'test')
    run(work / 'test')
    failure = subprocess.run([work / 'test', '--assert-failure'],
                             stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT,
                             text=True)
    if failure.returncode != -signal.SIGABRT or \
            'delayed assertion at VortexDPIHarness!' not in failure.stdout:
        raise RuntimeError(
            'original DPI assertion scope/termination mismatch: ' +
            failure.stdout)
    print(
        'PASS: original Vortex DPI assertion reports its native scope and aborts',
        flush=True)
