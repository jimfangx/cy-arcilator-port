#!/usr/bin/env python3
"""Check bulk Boolean array initialization, dynamic patches and fallbacks."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import shlex
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from compiler.partition import plugin

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=SIM / 'generated-src/boolean-array-test')
args = parser.parse_args()
sdk = args.circt_bin.resolve().parent
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
cxx = os.environ.get('CXX', 'c++')
lines = ['module {']
cases = []
driver = [
    '#include <cstdint>', '#include <cstdio>', '#include <stdexcept>',
    '#include <vector>'
]
for idx, (n, fill, pattern) in enumerate([(256, 0, 'uniform'),
                                          (257, 1, 'uniform'),
                                          (4096, 1, 'sparse'),
                                          (8192, 0, 'sparse'),
                                          (32768, 1, 'sparse'),
                                          (255, 1, 'sparse'),
                                          (256, 0, 'mixed'),
                                          (1024, 1, 'boundary')]):
    name = 'fill' + str(idx)
    entries = []
    for i in range(n):
        if pattern == 'sparse' and i % 239 == 0: entries.append('dynamic')
        elif pattern == 'sparse' and i % 127 == 0:
            entries.append(str(1 - fill))
        elif pattern == 'mixed':
            entries.append(str(i % 2))
        elif pattern == 'boundary' and i < n // 4:
            entries.append('dynamic')
        else:
            entries.append(str(fill))
    lines += [
        f'func.func @{name}(%dynamic:i1) -> !hw.array<{n}xi1> {{',
        '%zero = hw.constant false', '%one = hw.constant true',
        '%array = hw.array_create ' + ', '.join(
            '%dynamic' if e == 'dynamic' else '%one' if e == '1' else '%zero'
            for e in reversed(entries)) + ' : i1',
        f'return %array : !hw.array<{n}xi1>', '}'
    ]
    driver += [f'extern "C" void {name}(uint8_t*,bool);']
    cases.append(dict(name=name, n=n, entries=entries))
lines += ['}']
(work / 'input.mlir').write_text('\n'.join(lines) + '\n')
driver += ['int main(){ unsigned long count=0;']
for case in cases:
    name, n, entries = case['name'], case['n'], case['entries']
    driver += [
        'for(unsigned trial=0;trial<4;++trial){',
        f'std::vector<uint8_t> actual({n}+32,0xa5);',
        f'{name}(actual.data()+16,trial&1);'
    ]
    expected = ', '.join('trial&1' if e == 'dynamic' else e for e in entries)
    driver += [
        f'uint8_t expected[]={{ {expected} }};',
        f'for(unsigned i=0;i<{n};++i){{ if((actual[i+16]&1)!=expected[i])throw std::runtime_error("{name} value/order");++count;}}',
        f'for(unsigned i=0;i<16;++i) if(actual[i]!=0xa5 || actual[{n}+16+i]!=0xa5)throw std::runtime_error("guard");',
        '}'
    ]
driver += [
    'std::printf("PASS: %lu Boolean array entries and adjacent memory guards\\n",count);',
    '}'
]
(work / 'check.cpp').write_text('\n'.join(driver) + '\n')


def run(label, cmd):
    print(label, flush=True)
    with (work / (label + '.log')).open('w') as f:
        subprocess.run([str(x) for x in cmd],
                       stdout=f,
                       stderr=subprocess.STDOUT,
                       check=True)


libs = [
    plugin(sdk / 'bin/circt-opt', cxx, SIM / 'generated-src/compiler',
           SIM / 'compiler' / name)
    for name in ['LowerArrays.cpp', 'ArcToLLVM.cpp']
]
run('lower', [
    sdk / 'bin/circt-opt', work / 'input.mlir',
    *['--load-pass-plugin=' + str(x) for x in libs],
    '--pass-pipeline=builtin.module(chipyard-lower-arrays,chipyard-lower-arc-to-llvm,reconcile-unrealized-casts)',
    '-o', work / 'llvm.mlir'
])
run('translate', [
    sdk / 'bin/mlir-translate', '--mlir-to-llvmir', work / 'llvm.mlir', '-o',
    work / 'model.ll'
])
ir = (work / 'model.ll').read_text()
assert ir.count('call void @llvm.memset') == 6, ir.count(
    'call void @llvm.memset')
for level in ['0', '3']:
    run('opt' + level, [
        sdk / 'bin/opt', '-O' + level, work / 'model.ll', '-o',
        work / 'model.bc'
    ])
    run('llc' + level, [
        sdk / 'bin/llc', '-O' + level, '-relocation-model=pic',
        '-filetype=obj', work / 'model.bc', '-o', work / 'model.o'
    ])
    run('link' + level, [
        *shlex.split(cxx), '-O' + level, work / 'check.cpp', work / 'model.o',
        '-o', work / ('test' + level)
    ])
    run('run' + level, [work / ('test' + level)])
(work / 'report.json').write_text(
    json.dumps(dict(
        status='passed',
        scope=
        'Production Arc lowering O0/O3, independent array values and adjacent-memory guards; Boolean size thresholds, true/false, differing constants/dynamic patches, mixed fallback,75pct boundary',
        memset_calls=6),
               indent=2) + '\n')
print(
    'PASS: 188416 independent Boolean array entries and adjacent guards at O0/O3',
    flush=True)
