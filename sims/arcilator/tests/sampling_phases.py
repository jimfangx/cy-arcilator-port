#!/usr/bin/env python3
"""Compare DPI callbacks across held sampling phases and transient inputs."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
HERE = SIM / 'tests'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=SIM / 'generated-src/sampling-phase-test')
args = parser.parse_args()
sdk = args.circt_bin.resolve().parent
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
cxx = shlex.split(os.environ.get('CXX', 'c++'))
libs = shlex.split(
    subprocess.check_output([sdk / 'bin/llvm-config', '--libs', 'support'],
                            text=True))
traces = {}


def run(label, command):
    with (work / (label + '.log')).open('w') as stream:
        subprocess.run([str(x) for x in command],
                       stdout=stream,
                       stderr=subprocess.STDOUT,
                       check=True)


for mode in ('baseline', 'optimized'):
    build = work / mode
    build.mkdir(exist_ok=True)
    run(mode + '-model', [
        sys.executable, SIM / 'compiler/model.py', '--input',
        HERE / 'sensitized_dpi.mlir', '--output', build / 'model.ll',
        '--state-file', build / 'model.json', '--arcilator',
        sdk / 'bin/arcilator', '--circt-opt', sdk / 'bin/circt-opt',
        '--mlir-translate', sdk / 'bin/mlir-translate', '--cache',
        SIM / 'generated-src/compiler', '--cxx',
        os.environ.get('CXX', 'c++'), '--max-ops', '2',
        *(['--freeze-sampling=false'] if mode == 'baseline' else [])
    ])
    model, = json.loads((build / 'model.json').read_text())
    ports = sorted(
        (p for p in model['states'] if p['type'] in ('input', 'output')),
        key=lambda p: p['name'])
    header = ''.join(f'constexpr unsigned {p["name"]}Offset={p["offset"]};\n'
                     for p in ports)
    header += ('struct PhasePort { const char *name; unsigned offset,bits; '
               'bool input; };\nconstexpr PhasePort phasePorts[]={')
    header += ','.join('{' + json.dumps(p['name']) +
                       f',{p["offset"]},{p["numBits"]},' +
                       str(p['type'] == 'input').lower() + '}' for p in ports)
    (build / 'sensitized_dpi_layout.h').write_text(header + '};\n')
    assert '@__chipyard_arc_sampling_identity' not in (build /
                                                       'model.ll').read_text()
    for level in ('0', '2'):
        bc, obj = build / ('model-O' + level + '.bc'), build / ('model-O' +
                                                                level + '.o')
        exe = build / ('test-O' + level)
        label = mode + '-O' + level
        run(label + '-opt',
            [sdk / 'bin/opt', '-O' + level, build / 'model.ll', '-o', bc])
        run(label + '-llc', [
            sdk / 'bin/llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj', bc, '-o', obj
        ])
        run(label + '-link', [
            *cxx, '-O' + level, '-std=c++17', '-I' + str(build),
            '-I' + str(sdk / 'include'), HERE / 'sampling_phases.cpp',
            SIM / 'native/evaluation.cpp', obj, '-L' + str(sdk / 'lib'),
            '-lCIRCTArcRuntime', *libs, '-lz', '-lpthread', '-ldl', '-o', exe
        ])
        output = subprocess.check_output([exe])
        (work / (label + '.trace')).write_bytes(output)
        traces[label] = output
assert len(set(
    traces.values())) == 1, 'sampling phase output/callback trace changed'
(work / 'report.json').write_text(
    json.dumps(dict(
        status='passed',
        steps_per_variant=800,
        variants=list(traces),
        scope=
        'Exact all-output and cumulative callback/once-call traces at O0/O2; '
        'initial sampling0/1/2/255, held-phase transient updates, stable repeats, '
        'clock, aggregate and real inputs. Baseline disables the optimization; '
        'optimized uses the production default.'),
               indent=2) + '\n')
print(
    'PASS: 800 held/initial sampling steps, all output and callback traces at O0/O2'
)
