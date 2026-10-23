#!/usr/bin/env python3
"""Execute original Ara shuffle/deshuffle functions with native automatic storage."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shlex
import subprocess
import sys
from types import SimpleNamespace

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.verilog.build import build
from coverage import implementation_hashes


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--circt-bin', type=Path, required=True)
    p.add_argument('--filelist',
                   type=Path,
                   required=True,
                   help='complete Ara SV dependency manifest')
    p.add_argument('--ara-root',
                   type=Path,
                   default=SIM.parent.parent / 'generators/ara/ara')
    p.add_argument('--work', type=Path, required=True)
    a = p.parse_args()
    sdk, work, ara = a.circt_bin.resolve(), a.work.resolve(
    ), a.ara_root.resolve()
    work.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get('CXX', 'c++')
    report = dict(status='incomplete',
                  implementation_sha256=implementation_hashes('native'),
                  test_source_sha256=digest(Path(__file__)),
                  levels=[])

    def save():
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(*command):
        subprocess.run(list(map(str, command)), check=True)

    save()
    paths = [
        Path(s) for s in shlex.split(a.filelist.read_text(), comments=True)
    ]
    headers = [
        p for d in ('hardware/include', 'hardware/deps/axi/include',
                    'hardware/deps/common_cells/include',
                    'hardware/deps/apb/include',
                    'hardware/deps/cva6/common/local/util')
        for p in (ara / d).rglob('*.svh')
    ]
    pkg = ara / 'hardware/include/ara_pkg.sv'
    sources = sorted(set([pkg, *paths, *headers]))
    source_hashes = {str(p): digest(p) for p in sources}
    report.update(source_sha256=source_hashes,
                  filelist_sha256=digest(a.filelist))
    frontend = build(
        SimpleNamespace(circt_verilog=sdk / 'circt-verilog',
                        cache=SIM / 'generated-src/compiler/verilog',
                        circt_source=None,
                        slang_source=None,
                        slang_build=None,
                        jobs=4))
    report.update(frontend=str(frontend), frontend_sha256=digest(frontend))
    save()
    names = [
        f'{prefix}{lanes}' for lanes in (1, 2, 4, 8, 16)
        for prefix in ('s', 'd')
    ]
    declarations = ', '.join('output logic[6:0] ' + name for name in names)
    statements = []
    for lanes in (1, 2, 4, 8, 16):
        for prefix, function in (('s', 'shuffle_index'), ('d',
                                                          'deshuffle_index')):
            statements.append(
                f'assign {prefix}{lanes}=ara_pkg::{function}(byte_index,{lanes},rvv_pkg::vew_e\'(ew));'
            )
    (work / 'source.sv').write_text(
        'module OriginalAraFunctions(input logic[15:0] byte_index, input logic[2:0] ew, '
        + declarations + ');\n' + '\n'.join(statements) + '\nendmodule\n')
    ports = 'in %byte_index: i16, in %ew: i3, ' + ', '.join(f'out {n}: i7'
                                                            for n in names)
    instance = ', '.join(
        '%' + n for n in names
    ) + ' = hw.instance "functions" @OriginalAraFunctions(byte_index: %byte_index: i16, ew: %ew: i3) -> (' + ', '.join(
        n + ': i7' for n in names) + ')'
    output = 'hw.output ' + ', '.join(
        '%' + n for n in names) + ' : ' + ', '.join('i7' for _ in names)
    (work / 'input.mlir'
     ).write_text('module {\n hw.module.extern @OriginalAraFunctions(' +
                  ports + ')\n hw.module @AraFunctionsHarness(' + ports +
                  ') {\n  ' + instance + '\n  ' + output + '\n }\n}\n')
    (work / 'blackboxes.f').write_text(
        shlex.quote(str(work / 'source.sv')) + '\n' + a.filelist.read_text())
    flags = ' '.join('-I' + shlex.quote(str(ara / d))
                     for d in ('hardware/include', 'hardware/deps/axi/include',
                               'hardware/deps/common_cells/include',
                               'hardware/deps/apb/include',
                               'hardware/deps/cva6/common/local/util'))
    flags += ' --Xslang=--relax-enum-conversions --Xslang=-Wno-multiple-always-assigns'
    run(sdk / 'circt-opt', work / 'input.mlir', '-o', work / 'normalized.mlir')
    run(sys.executable, SIM / 'native_hw.py', '--input',
        work / 'normalized.mlir', '--output', work / 'design.mlir',
        '--filelist', work / 'blackboxes.f', '--work', work / 'rtl',
        '--circt-verilog', sdk / 'circt-verilog', '--circt-opt',
        sdk / 'circt-opt', '--cxx', cxx, '--flags=' + flags)
    imported = json.loads((work / 'rtl.json').read_text())
    assert [m['module'] for m in imported] == ['OriginalAraFunctions']
    run(sys.executable, SIM / 'partition.py', '--input', work / 'design.mlir',
        '--output', work, '--top', 'AraFunctionsHarness', '--circt-opt',
        sdk / 'circt-opt')
    run(sys.executable, SIM / 'compiler/partition.py', '--input',
        work / 'partitioned.mlir', '--output', work / 'outlined.mlirbc',
        '--circt-opt', sdk / 'circt-opt', '--cache',
        SIM / 'generated-src/compiler', '--cxx', cxx, '--max-ops', '8',
        '--emit-bytecode', '--native-feedback')
    run(sdk / 'circt-opt', work / 'outlined.mlirbc', '--symbol-dce',
        '--canonicalize', '-o', work / 'model.mlir')
    run(sys.executable, SIM / 'compiler/model.py', '--input',
        work / 'model.mlir', '--output', work / 'model.ll', '--state-file',
        work / 'model.json', '--arcilator', sdk / 'arcilator', '--circt-opt',
        sdk / 'circt-opt', '--mlir-translate', sdk / 'mlir-translate',
        '--cache', SIM / 'generated-src/compiler', '--cxx', cxx, '--max-ops',
        '16')
    run(sys.executable, SIM / 'compiler/llvm.py', work / 'model.ll')
    # The literal tables are the specification. Compute their inverses in Python;
    # no compiled shuffle/deshuffle implementation contributes expected values.
    text = pkg.read_text()
    body = text[text.index(
        '  function automatic logic [$clog2(8*MaxNrLanes)-1:0] shuffle_index'
    ):text.index('  endfunction : shuffle_index')]
    tables = {}
    for m in re.finditer(
            r'^\s*(1|2|4|8|16): unique case \(ew\)(.*?)(?=^\s*endcase)', body,
            re.M | re.S):
        lanes, parts = int(m[1]), {}
        for c in re.finditer(
                r'(rvv_pkg::EW(?:8|16|32|64)|default): begin(.*?)\bend\b',
                m[2], re.S):
            table = {
                int(i): int(v)
                for i, v in re.findall(r'idx\[(\d+)\]\s*=\s*(\d+)', c[2])
            }
            assert len(table) == 8 * lanes and sorted(table.values()) == list(
                range(8 * lanes))
            parts[c[1]] = [table[i] for i in range(8 * lanes)]
        for ew in range(8):
            forward = parts['rvv_pkg::EW' +
                            str(8 << ew)] if ew < 4 else parts['default']
            inverse = [0] * len(forward)
            for i, v in enumerate(forward):
                inverse[v] = i
            tables[lanes, ew] = forward, inverse
    assert len(tables) == 40
    rng = random.Random(1709)
    indices = list(range(256)) + [rng.randrange(65536) for _ in range(256)]
    vectors = []
    for ew in range(8):
        for index in indices:
            expected = []
            for lanes in (1, 2, 4, 8, 16):
                forward, inverse = tables[lanes, ew]
                expected.extend([
                    forward[index % (8 * lanes)], inverse[index % (8 * lanes)]
                ])
            vectors.append(' '.join(map(str, [index, ew, *expected])))
    (work / 'vectors.txt').write_text('\n'.join(vectors) + '\n')
    model, = json.loads((work / 'model.json').read_text())
    assert not model.get('initialFnSym')
    states = {p['name']: p['offset'] for p in model['states']}
    offsets = ','.join(str(states[n]) for n in names)
    (work / 'test.cpp').write_text(f'''#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
extern "C" void ArcModel_eval(void*);
int main(int argc,char**argv){{if(argc!=2)return 2;std::ifstream vectors(argv[1]);unsigned input,ew,n=0;
alignas(64) unsigned char state[{model['numStateBytes']}]={{}};unsigned offsets[]={{{offsets}}};
while(vectors>>input>>ew){{uint16_t value=input;uint8_t width=ew;unsigned expected[10];for(auto &e:expected)if(!(vectors>>e))return 3;
 std::memcpy(state+{states['byte_index']},&value,2);std::memcpy(state+{states['ew']},&width,1);ArcModel_eval(state);
 for(unsigned i=0;i<10;++i){{unsigned actual=state[offsets[i]]&127;if(actual!=expected[i]){{std::fprintf(stderr,"original Ara functions vector %u byte %u ew %u port %u actual %u expected %u\\n",n,input,ew,i,actual,expected[i]);return 1;}}}}++n;
}}if(n!=4096)return 4;
std::puts("PASS: 40960 original Ara shuffle/deshuffle checks, lanes 1/2/4/8/16, all element widths/default branches, rebased byte indices and automatic local arrays");}}
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
        executable = work / f'test-O{level}'
        run(*shlex.split(cxx), '-std=c++17', work / 'test.cpp',
            work / f'model-O{level}.o', '-L' + str(sdk.parent / 'lib'),
            '-lCIRCTArcRuntime', *libs, '-lz', '-lpthread', '-ldl', '-o',
            executable)
        result = subprocess.run([executable, work / 'vectors.txt'],
                                capture_output=True,
                                text=True)
        (work / f'run-O{level}.log').write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end='', flush=True)
        report['levels'].append(
            dict(optimization=level,
                 returncode=result.returncode,
                 executable_sha256=digest(executable)))
        save()
        result.check_returncode()
    changed = implementation_hashes(
        'native') != report['implementation_sha256'] or any(
            digest(Path(p)) != h for p, h in source_hashes.items())
    report.update(
        status='source_changed' if changed else 'passed',
        comparisons=81920,
        vectors_sha256=digest(work / 'vectors.txt'),
        scope=
        'original Ara package functions; full Ara CPU workload remains unverified'
    )
    save()
    if changed:
        raise RuntimeError(
            'compiler or original resource sources changed during validation')
    print('Original Ara function report:', work / 'report.json', flush=True)


if __name__ == '__main__':
    main()
