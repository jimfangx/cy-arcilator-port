#!/usr/bin/env python3
"""Check open DPI array provenance, opaque C prototypes and runtime accessors."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    args = parser.parse_args()
    work = SIM / 'generated-src/native-open-guards'
    work.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get('CXX', 'c++')
    library = plugin(args.circt_opt, compiler, SIM / 'generated-src/compiler',
                     SIM / 'compiler/Model.cpp')

    def lower(name, text, error=None):
        source, target = work / (name + '.mlir'), work / (name + '-out.mlir')
        source.write_text(text)
        result = subprocess.run([
            args.circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-dpi-abi,chipyard-coalesce-dpi)',
            '-o', target
        ],
                                capture_output=True,
                                text=True)
        if error:
            assert result.returncode == 1 and error in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        return target.read_text()

    leaf = '{kind="int",width=32:i64,signed=true,four_state=false}'

    def decl(name='sv', count=3, direction='in', actual_leaf=leaf):
        return f'''sim.func.dpi private @{name}({direction} %value:!hw.array<{count}xi32>) attributes {{
          verilogName="open_c",chipyard.dpi_decl={{context=false,task=false,pure=false}},
          chipyard.dpi_abi=[{{kind="open_array",packed=false,direction="{direction}",element={leaf},
            native_open_actual={{kind="unpacked_array",left=-2:i64,right={count-3}:i64,element={actual_leaf}}}}}]}}'''

    valid = 'module {' + decl() + '}'
    converted = lower('valid', valid)
    assert 'kind = "open_array"' in converted and 'native_open_actual' in converted
    assert 'open_array_handle' in converted and 'chipyard_arc_open_array_create' in converted
    assert 'chipyard_arc_open_array_delete' in converted
    assert lower('again', converted) == converted
    lower(
        'different-actuals',
        'module {' + decl() + decl('other', count=5, direction='inout') + '}')
    lower(
        'wrong-element', 'module {' +
        decl(actual_leaf=leaf.replace('int', 'longint').replace('32', '64')) +
        '}', 'invalid native open-array actual provenance')
    lower(
        'not-open',
        valid.replace('kind="open_array",packed=false',
                      'kind="unpacked_array",left=-2:i64,right=0:i64'),
        'invalid native open-array actual provenance')
    lower('packed-open', valid.replace('packed=false', 'packed=true'),
          'invalid native open-array actual provenance')
    lower('wrong-shape', valid.replace('right=0:i64', 'right=1:i64'),
          'invalid native unpacked DPI array shape or direction')
    lower('missing-shape', valid.replace('left=-2:i64,', ''),
          'invalid native unpacked DPI array shape or direction')
    lower(
        'opaque-fixed-alias', 'module {' + decl() +
        decl('fixed').replace('kind="open_array",packed=false',
                              'kind="unpacked_array",left=-2:i64,right=0:i64').
        replace(
            ',\n            native_open_actual={kind="unpacked_array",left=-2:i64,right=0:i64,element='
            + leaf + '}', '') + '}',
        'conflicting native DPI C pointer element prototype')
    for level in ('0', '2'):
        executable = work / ('api-O' + level)
        subprocess.run([
            *shlex.split(compiler), '-std=c++17', '-O' + level,
            '-I' + str(SIM), '-I' + str(SIM / 'native/include'),
            SIM / 'tests/native_open_api.cpp', SIM / 'native/open_arrays.cpp',
            '-o', executable
        ],
                       check=True)
        subprocess.run([executable], check=True)
    print(
        'PASS: open-array actual shape/element/provenance guards, opaque C prototype aliases, original ABI and idempotence',
        flush=True)


if __name__ == '__main__':
    main()
