#!/usr/bin/env python3
"""Execute overlapping union views transported through aggregate and CFG types."""
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
        tempfile.mkdtemp(prefix='union-bits-', dir=SIM / 'generated-src'))
    work.mkdir(parents=True, exist_ok=True)
    report = dict(status='incomplete')

    def save():
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(*command):
        subprocess.run(list(map(str, command)), check=True)

    save()
    cxx = shlex.split(os.environ.get('CXX', 'c++'))
    source = SIM / 'compiler/UnionBits.cpp'
    library = plugin(sdk / 'circt-opt', os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler', source)
    fixture = Path(__file__).with_name('native_union_transport.mlir')
    run(
        sdk / 'circt-opt', fixture, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-lower-unions,inline,canonicalize,hw.module(llhd-remove-control-flow),canonicalize)',
        '-o', work / 'lowered.mlir')
    # Reparse the printed result: a stale module port type can otherwise be
    # hidden by independently converted body arguments during verification.
    run(sdk / 'circt-opt', work / 'lowered.mlir', '-o', '/dev/null')
    assert '!hw.union' not in (work / 'lowered.mlir').read_text()
    run(sdk / 'arcilator', work / 'lowered.mlir',
        '--state-file=' + str(work / 'model.json'), '-o', work / 'model.ll')
    model, = json.loads((work / 'model.json').read_text())
    offsets = {v['name']: v['offset'] for v in model['states']}
    layout = ''.join(f'constexpr unsigned {name}Offset={offset};\n'
                     for name, offset in offsets.items())
    (work / 'test.cpp').write_text('''#include <cstdint>
#include <cstring>
#include <cstdio>
extern "C" void UnionHarness_eval(void*);
''' + layout + f'''
int main() {{ alignas(64) unsigned char state[{model['numStateBytes']}]={{}};
using Bits=__uint128_t;
auto mask=(Bits(1)<<65)-1;
auto put=[&](unsigned offset,Bits value){{std::memcpy(state+offset,&value,9);}};
auto get=[&](unsigned offset){{Bits value=0;std::memcpy(&value,state+offset,9);return value&mask;}};
auto narrow=[&](unsigned offset){{uint32_t value=0;std::memcpy(&value,state+offset,3);return value&131071;}};
uint64_t rng=1709;
for(unsigned i=0;i<16384;++i) {{
 auto next=[&](){{rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}};
 Bits a=next()|(Bits(i&1)<<64), b=next()|(Bits((i>>1)&1)<<64);
 uint32_t tag=next()&131071; unsigned char select=(i>>2)&1;
 put(aOffset,a);put(bOffset,b);
 std::memcpy(state+selectOffset,&select,1);std::memcpy(state+tagOffset,&tag,3);
 UnionHarness_eval(state);
 if(get(viewOffset)!=a || get(backOffset)!=b || get(selectedOffset)!=(select?a:b) ||
    get(array_aOffset)!=a || get(array_bOffset)!=b || narrow(tag_outOffset)!=tag ||
    get(shiftedOffset)!=(Bits(tag)<<41) || narrow(laneOffset)!=((a>>41)&131071)) {{
  std::fprintf(stderr,"union transport mismatch at %u\\n",i);return 1;
 }}
}}
std::puts("PASS: 16384 union vectors through modules, extern signatures, calls, CFG merges, arrays, structs, offset views and zero-width fields");
}}
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
        run(*cxx, '-std=c++17', work / 'test.cpp', work / f'model-O{level}.o',
            '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz',
            '-lpthread', '-ldl', '-o', work / f'test-O{level}')
        run(work / f'test-O{level}')
    report.update(
        status='passed',
        comparisons=32768,
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        fixture_sha256=hashlib.sha256(fixture.read_bytes()).hexdigest())
    save()


if __name__ == '__main__':
    main()
