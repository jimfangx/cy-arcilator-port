#!/usr/bin/env python3
"""Check output initialization without changing the external DPI C ABI."""
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
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/seeded-dpi-test')
    args = parser.parse_args()
    sdk, work = args.circt_bin.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)

    def run(*command):
        subprocess.run([str(value) for value in command], check=True)

    cxx = os.environ.get('CXX', 'c++')
    library = plugin(sdk / 'circt-opt', cxx, SIM / 'generated-src/compiler',
                     SIM / 'compiler/Model.cpp')
    load = '--load-pass-plugin=' + str(library)
    seed = '--pass-pipeline=builtin.module(chipyard-coalesce-dpi,chipyard-seed-dpi-outputs)'
    run(sdk / 'circt-opt', HERE / 'seeded_dpi.mlir', load, seed, '-o',
        work / 'seeded.mlir')
    run(sdk / 'circt-opt', work / 'seeded.mlir', load, seed, '-o',
        work / 'seeded-twice.mlir')
    assert (work /
            'seeded.mlir').read_bytes() == (work /
                                            'seeded-twice.mlir').read_bytes()
    text = (work / 'seeded.mlir').read_text()
    assert 'chipyard.seeded_output_arguments = array<i32: 1, 4, 6>' in text
    assert 'sim.func.dpi private @partial_alias' not in text
    guard = work / 'guards.mlir'
    guard.write_text('''module {
      sim.func.dpi private @input_only(in %a: i32, return result: i32)
      sim.func.dpi private @inout_only(inout %a: i32)
      sim.func.dpi private @ref_only(ref %a: !llvm.ptr)
      sim.func.dpi private @unsupported(out aggregate: !hw.array<2xi8>)
    }
    ''')
    run(sdk / 'circt-opt', guard, '-o', work / 'guards-before.mlir')
    run(sdk / 'circt-opt', guard, load, seed, '-o', work / 'guards-after.mlir')
    assert (work / 'guards-before.mlir').read_bytes() == (
        work / 'guards-after.mlir').read_bytes()
    print(
        'PASS: output seeding is idempotent, preserves original output indices and aliases, and leaves input/inout/ref-only and unsupported output declarations unchanged',
        flush=True)
    run(sys.executable, SIM / 'compiler/model.py', '--input',
        HERE / 'seeded_dpi.mlir', '--output', work / 'model.ll',
        '--state-file', work / 'model.json', '--arcilator', sdk / 'arcilator',
        '--circt-opt', sdk / 'circt-opt', '--mlir-translate',
        sdk / 'mlir-translate', '--cache', SIM / 'generated-src/compiler',
        '--max-ops', '2')
    declaration, = [
        line for line in (work / 'model.ll').read_text().splitlines()
        if line.startswith('declare i64 @chipyard_seeded_partial(')
    ]
    assert declaration == 'declare i64 @chipyard_seeded_partial(i32, ptr, ptr, i32, ptr, ptr, ptr)'
    model, = json.loads((work / 'model.json').read_text())
    (work / 'seeded_dpi_layout.h').write_text(''.join(
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
            '-I' + str(sdk.parent / 'include'), HERE / 'seeded_dpi.cpp',
            work / 'model.o', '-L' + str(sdk.parent / 'lib'),
            '-lCIRCTArcRuntime', *libs, '-lz', '-lpthread', '-ldl', '-o',
            work / 'test')
        run(work / 'test')


if __name__ == '__main__':
    main()
