#!/usr/bin/env python3
"""Verify independent conditional-write definitions across sibling CFG blocks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path)
    args = parser.parse_args()
    sdk = args.circt_bin.resolve()
    (SIM / 'generated-src').mkdir(exist_ok=True)
    work = args.work or Path(
        tempfile.mkdtemp(prefix='conditional-defs-',
                         dir=SIM / 'generated-src'))
    work.mkdir(parents=True, exist_ok=True)
    report = dict(status='incomplete')

    def save():
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(*command):
        subprocess.run(list(map(str, command)), check=True)

    save()
    source = SIM / 'compiler/NativeMem2Reg.cpp'
    library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler', source)
    baseline = subprocess.run([
        sdk / 'circt-opt', SIM / 'tests/mem2reg_dominance.mlir',
        '--pass-pipeline=builtin.module(hw.module(llhd-mem2reg))', '-o',
        '/dev/null'
    ],
                              capture_output=True,
                              text=True)
    (work / 'baseline.log').write_text(baseline.stderr)
    if baseline.returncode == 0 or 'does not dominate' not in baseline.stderr:
        raise RuntimeError(
            'the pinned SDK no longer reproduces the expected defect')
    run(sdk / 'circt-opt', SIM / 'tests/mem2reg_dominance.mlir',
        '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(hw.module(chipyard-native-mem2reg))',
        '-o', work / 'dominance-fixed.mlir')
    run(sdk / 'circt-opt', work / 'dominance-fixed.mlir', '-o', '/dev/null')
    # Sibling writes share data/enable operands but see different incoming
    # values and conditions. They must build independent muxes and merge them.
    cases = [('narrow', 13, False), ('narrow_cond', 13, True),
             ('wide', 65, False), ('wide_cond', 65, True)]
    ports = ', '.join(f'out {name}: i{width}' for name, width, _ in cases)
    lines = [
        'module {', 'hw.module @ConditionalValues(in %select: i1, '
        'in %enable: i1, in %side_enable: i1, in %data: i65, ' + ports + ') {',
        '%time = llhd.constant_time <0ns, 0d, 1e>'
    ]
    for name, width, conditional in cases:
        ty = 'i13' if width == 13 else '!hw.struct<hi: i13, lo: !hw.array<4xi13>>'
        mask_a = ((1 << width) - 1) // 3
        mask_b = ((1 << width) - 1) ^ mask_a
        lines += [
            f'%{name}_input = comb.extract %data from 0 : (i65) -> i{width}',
            f'%{name}_a = hw.constant {mask_a} : i{width}',
            f'%{name}_b = hw.constant {mask_b} : i{width}',
            f'%{name}_z = hw.constant 13 : i{width}',
            f'%{name}_left = comb.xor %{name}_input, %{name}_a : i{width}',
            f'%{name}_right = comb.xor %{name}_input, %{name}_b : i{width}'
        ]
        for field in ('input', 'left', 'right', 'z'):
            lines.append(
                f'%{name}_{field}_typed = hw.bitcast %{name}_{field} : (i{width}) -> {ty}'
            )
        lines += [
            f'%{name}_result = llhd.combinational -> {ty} {{',
            f'%q = llhd.sig %{name}_z_typed : <{ty}>',
            'cf.cond_br %select, ^left, ^right', '^left:'
        ]
        guard = ' if %side_enable' if conditional else ''
        for branch in ('left', 'right'):
            if branch == 'right':
                lines.append('^right:')
            lines += [
                f'llhd.drv %q, %{name}_{branch}_typed after %time{guard} : {ty}',
                f'llhd.drv %q, %{name}_input_typed after %time if %enable : {ty}',
                'cf.br ^join'
            ]
        lines += [
            '^join:', f'%value = llhd.prb %q : {ty}',
            f'llhd.yield %value : {ty}', '}',
            f'%{name} = hw.bitcast %{name}_result : ({ty}) -> i{width}'
        ]
    lines += [
        'hw.output ' + ', '.join('%' + n for n, _, _ in cases) + ' : ' +
        ', '.join('i' + str(w) for _, w, _ in cases), '}', '}'
    ]
    fixture = work / 'input.mlir'
    fixture.write_text('\n'.join(lines) + '\n')
    run(
        sdk / 'circt-opt', fixture, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(hw.module(chipyard-native-mem2reg,llhd-remove-control-flow),canonicalize)',
        '-o', work / 'lowered.mlir')
    run(sdk / 'circt-opt', work / 'lowered.mlir', '-o', '/dev/null')
    assert '!llhd.ref' not in (work / 'lowered.mlir').read_text()
    state_library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                           SIM / 'generated-src/compiler',
                           SIM / 'compiler/SignalState.cpp')
    run(sdk / 'circt-opt', fixture, '--load-pass-plugin=' + str(library),
        '--load-pass-plugin=' + str(state_library),
        '--pass-pipeline=builtin.module(chipyard-prepare-native-llhd)', '-o',
        work / 'early-lowered.mlir')
    run(sdk / 'circt-opt', work / 'early-lowered.mlir', '-o', '/dev/null')
    assert '!llhd.ref' not in (work / 'early-lowered.mlir').read_text()
    run(sdk / 'arcilator', work / 'early-lowered.mlir',
        '--state-file=' + str(work / 'model.json'), '-o', work / 'model.ll')
    model, = json.loads((work / 'model.json').read_text())
    offsets = {p['name']: p['offset'] for p in model['states']}
    layout = ''.join(f'constexpr unsigned {name}Offset={offset};\n'
                     for name, offset in offsets.items())
    checks = []
    for name, width, conditional in cases:
        checks.append(
            f'if(get({name}Offset,{(width + 7) // 8}) != '
            f'expected({width},{str(conditional).lower()})) '
            f'{{std::fprintf(stderr,"{name} mismatch at %u\\n",i);return 1;}}')
    (work / 'test.cpp').write_text('''#include <cstdint>
#include <cstring>
#include <cstdio>
extern "C" void ConditionalValues_eval(void*);
''' + layout + f'''
int main(){{alignas(64) unsigned char state[{model['numStateBytes']}]={{}};
using Bits=__uint128_t;
auto get=[&](unsigned offset,unsigned bytes){{Bits value=0;std::memcpy(&value,state+offset,bytes);return value;}};
uint64_t rng=1709;
for(unsigned i=0;i<65536;++i){{
 rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
 Bits data=Bits(rng)|((Bits((i>>3)&1))<<64);
 data=(data&~Bits(8191))|(i>>3);
 unsigned char select=i&1,enable=(i>>1)&1,side=(i>>2)&1;
 std::memcpy(state+dataOffset,&data,9);std::memcpy(state+selectOffset,&select,1);
 std::memcpy(state+enableOffset,&enable,1);std::memcpy(state+side_enableOffset,&side,1);
 ConditionalValues_eval(state);
 auto expected=[&](unsigned width,bool conditional){{
  Bits mask=(Bits(1)<<width)-1,mask_a=mask/3,mask_b=mask^mask_a;
  Bits value=conditional&&!side?13:((data&mask)^(select?mask_a:mask_b));
  return enable?(data&mask):value;
 }};
''' + '\n'.join(checks) + '''
}
std::puts("PASS: 65536 sibling conditional-drive vectors, 13/65-bit values, packed structs/arrays, distinct incoming values and enables");
}
''')
    libs = shlex.split(
        subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                                text=True))
    for level in (0, 2):
        run(sdk / 'opt', f'-O{level}', work / 'model.ll', '-o',
            work / f'model-O{level}.bc')
        run(sdk / 'llc', f'-O{level}', '-relocation-model=pic',
            '-filetype=obj', work / f'model-O{level}.bc', '-o',
            work / f'model-O{level}.o')
        run(*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17',
            work / 'test.cpp', work / f'model-O{level}.o',
            '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz',
            '-lpthread', '-ldl', '-o', work / f'test-O{level}')
        run(work / f'test-O{level}')
    report.update(
        status='passed',
        vectors_per_level=65536,
        output_checks=524288,
        numerical_pipeline='chipyard-prepare-native-llhd',
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest())
    save()


if __name__ == '__main__':
    main()
