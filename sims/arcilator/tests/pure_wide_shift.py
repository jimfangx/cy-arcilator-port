#!/usr/bin/env python3
'''Check generated pure arithmetic provenance and full compiled shift dataflow.'''
import argparse
import json
import os
from pathlib import Path
import random
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from compiler.partition import plugin

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=SIM / 'generated-src/pure-wide-test')
args = parser.parse_args()
sdk = args.circt_bin.resolve()
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
cxx = os.environ.get('CXX', 'c++')
library = plugin(sdk / 'circt-opt', cxx, SIM / 'generated-src/compiler',
                 SIM / 'compiler/Model.cpp')
report = dict(status='running', guards=[], variants=[])


def run(label, command):
    with (work / (label + '.log')).open('w') as stream:
        subprocess.run(list(map(str, command)),
                       stdout=stream,
                       stderr=subprocess.STDOUT,
                       check=True)


# A familiar name or no-readback contract alone must never exempt ordinary DPI.
base = '''module {
 func.func private @chipyard_arc_wide_shift_0_1025(i1025,i1025)->i1025 attributes {chipyard.no_output_readback,chipyard.native_pure_wide_shift}
 hw.module @H(in %d:i1025,in %a:i1025,out y:i1025) {
  %y=sim.func.dpi.call @chipyard_arc_wide_shift_0_1025(%d,%a):(i1025,i1025)->i1025
  hw.output %y:i1025
 }
}'''
guards = {
    'generated': (base, False),
    'no-purity-marker': (base.replace(',chipyard.native_pure_wide_shift',
                                      ''), True),
    'no-readback-only': (base.replace('chipyard.no_output_readback,',
                                      ''), True),
    'name-only': (base.replace(
        ' attributes {chipyard.no_output_readback,chipyard.native_pure_wide_shift}',
        ''), True),
    'unknown-name': (base.replace('chipyard_arc_wide_shift_0_1025',
                                  'user_shift'), True),
    'wrong-width-suffix': (base.replace('shift_0_1025', 'shift_0_1027'), True),
    'invalid-mode': (base.replace('shift_0_1025', 'shift_3_1025'), True),
    'small-width': (base.replace('1025', '64'), True),
    'non-unit-marker': (base.replace('native_pure_wide_shift}',
                                     'native_pure_wide_shift=false}'), True),
    'defined-function':
    (base.replace('func.func private @', 'func.func @').replace(
        '(i1025,i1025)->i1025 attributes',
        '(%a:i1025,%b:i1025)->i1025 attributes').replace(
            'chipyard.native_pure_wide_shift}\n',
            'chipyard.native_pure_wide_shift} {return %a:i1025}\n'), True),
}
for name, (text, sensitized) in guards.items():
    source, output = work / (name + '.mlir'), work / (name + '-after.mlir')
    source.write_text(text)
    run(name, [
        sdk / 'circt-opt', source, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-sensitize-dpi)', '-o', output
    ])
    assert ('llhd.process' in output.read_text()) == sensitized, name
    report['guards'].append(dict(name=name, sensitized=sensitized))

widths = [1025, 1027, 2048, 2051, 4099, 16399]
ports = ['in %clock:i1', 'in %reset:i1']
for bits in widths:
    ports += [f'in %d{bits}:i{bits}', f'in %a{bits}:i{bits}']
for bits in widths:
    ports += [
        f'out {name}{bits}:i{bits}'
        for name in ['left', 'right', 'sign', 'chain', 'latched']
    ]
body = [
    'hw.module @ArcModel(' + ','.join(ports) + ') {',
    '%clk=seq.to_clock %clock'
]
values = []
for bits in widths:
    for name, mode, data in [('left', 0, 'd'), ('right', 1, 'd'),
                             ('sign', 2, 'd'), ('chain', 1, 'left')]:
        body.append(
            f'%{name}{bits}=sim.func.dpi.call @chipyard_arc_wide_shift_{mode}_{bits}(%{data}{bits},%a{bits}):(i{bits},i{bits})->i{bits}'
        )
    body += [
        f'%z{bits}=hw.constant 0:i{bits}',
        f'%latched{bits}=seq.firreg %left{bits} clock %clk reset sync %reset,%z{bits}:i{bits}'
    ]
    values += [(f'%{name}{bits}', f'i{bits}')
               for name in ['left', 'right', 'sign', 'chain', 'latched']]
body += [
    'hw.output ' + ','.join(v for v, _ in values) + ':' +
    ','.join(t for _, t in values), '}'
]
rng = random.Random(1979)
reference = work / 'expected.txt'
with reference.open('w') as stream:
    for index, bits in enumerate(widths):
        mask = (1 << bits) - 1
        for data in [
                0, 1, mask, 1 << (bits - 1),
                *[rng.getrandbits(bits) for _ in range(8)]
        ]:
            signed = data - (1 << bits) if data >> (bits - 1) else data
            for amount in [
                    0, 1, 7, 8, 63, 64, bits - 3, bits - 1, bits, 100000,
                    1 << 1000
            ]:
                left = (data << amount) & mask if amount < bits else 0
                right = data >> amount if amount < bits else 0
                sign = (signed >> min(amount, bits)) & mask
                chain = left >> amount if amount < bits else 0
                stream.write(
                    str(index) + ' ' + ' '.join(
                        format(x, 'x')
                        for x in [data, amount, left, right, sign, chain]) +
                    '\n')
libs = shlex.split(
    subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                            text=True))
for mode in ['legacy', 'pure', 'late']:
    build = work / mode
    build.mkdir(exist_ok=True)
    contract = ',chipyard.native_pure_wide_shift' if mode in ('pure', 'late') else ''
    declarations = [
        f'func.func private @chipyard_arc_wide_shift_{op}_{bits}(i{bits},i{bits})->i{bits} attributes {{chipyard.no_output_readback{contract}}}'
        for bits in widths for op in range(3)
    ]
    source = build / 'input.mlir'
    source.write_text('module {\n' + '\n'.join(declarations + body) + '\n}\n')
    run(mode + '-model', [
        sys.executable, SIM / 'compiler/model.py', '--input', source,
        '--output', build / 'model.ll', '--state-file', build / 'model.json',
        '--arcilator', sdk / 'arcilator', '--circt-opt', sdk / 'circt-opt',
        '--mlir-translate', sdk / 'mlir-translate', '--cache',
        SIM / 'generated-src/compiler', '--cxx', cxx, '--late-wide-arithmetic', str(mode == 'late').lower()
    ])
    run(mode + '-bridge',
        [sys.executable, SIM / 'compiler/llvm.py', build / 'model.ll'])
    model, = json.loads((build / 'model.json').read_text())
    states = {
        p['name']: p
        for p in model['states'] if p['type'] in ['input', 'output']
    }
    assert ('samplingInput' in model) == (mode == 'legacy')
    header = '#include <cstdint>\n'
    for name in ['clock', 'reset']:
        header += f'constexpr unsigned {name}Offset={states[name]["offset"]};\n'
    initial = model.get('initialFnSym')
    header += (
        f'extern "C" void {initial}(void*);\ninline void initialize_model(void*p){{{initial}(p);}}\n'
        if initial else 'inline void initialize_model(void*){}\n')
    header += 'struct PortCase {unsigned bits,bytes,data,amount,left,right,sign,chain,latched;};\nconstexpr PortCase ports[]={\n'
    for bits in widths:
        names = [
            f'd{bits}', f'a{bits}', *[
                f'{name}{bits}'
                for name in ['left', 'right', 'sign', 'chain', 'latched']
            ]
        ]
        header += '{' + ','.join(
            map(str, [
                bits,
                (bits + 7) // 8, *[states[name]['offset'] for name in names]
            ])) + '},\n'
    (build / 'pure_wide_layout.h').write_text(header + '};\n')
    for level in ['0', '3']:
        for pipeline in (['stock', 'bounded'] if mode == 'late' else ['stock']):
            run(mode + '-' + pipeline + '-codegen' + level, [
                sys.executable, SIM / 'compiler/codegen.py', '--input',
                build / 'model.ll', '--output', build / 'model.o', '--bitcode',
                build / 'model.bc', '--llvm-opt', sdk / 'opt', '--llc', sdk / 'llc',
                '--cache', SIM / 'generated-src/compiler', '--cxx', cxx,
                '--opt-level', level, '--llc-opt-level', level, '--jobs', '1',
                '--no-abi-buffers', '--pipeline', pipeline
            ])
            run(mode + '-' + pipeline + '-link' + level, [
                *shlex.split(cxx), '-O' + level, '-std=c++17',
                '-I' + str(sdk.parent / 'include'), '-I' + str(build),
                SIM / 'tests/pure_wide_shift.cpp', SIM / 'native/wide.cpp',
                SIM / 'native/evaluation.cpp', build / 'model.o',
                '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz',
                '-lpthread', '-ldl', '-o', build / 'test'
            ])
            run(mode + '-' + pipeline + '-run' + level, [build / 'test', reference])
            report['variants'].append(
                dict(mode=mode, pipeline=pipeline, opt_level=level,
                     vectors=792, value_checks=6336,
                     state_bytes=model['numStateBytes']))
from wide_arithmetic_guards import check as check_wide_guards
report['wide_boundary_guards'] = check_wide_guards(sdk, work / 'wide-boundary-guards', cxx)
from wide_arithmetic_effects import check as check_wide_effects
report['wide_boundary_effects'] = check_wide_effects(sdk, work / 'wide-boundary-effects', cxx)
report['status'] = 'passed'
(work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(
    'PASS: 10 pure-helper provenance guards and 50,688 independent compiled value/stable/sampling checks at O0/O3, with legacy sensitivity controls'
)
