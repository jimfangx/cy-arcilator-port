#!/usr/bin/env python3
"""Validate fixed-array DPI shapes, C pointer prototypes and ABI provenance."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    args = parser.parse_args()
    work = SIM / 'generated-src/native-unpacked-guards'
    work.mkdir(parents=True, exist_ok=True)
    library = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/Model.cpp')

    def lower(name, text, error=None):
        source = work / (name + '.mlir')
        target = work / (name + '-lowered.mlir')
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

    def decl(name='sv',
             kind='shortint',
             width=16,
             left=3,
             right=1,
             direction='in',
             count=3):
        return f'''sim.func.dpi private @{name}({direction} %value:!hw.array<{count}xi{width}>) attributes {{
          verilogName="array_c",chipyard.dpi_decl={{context=false,task=false,pure=false}},
          chipyard.dpi_abi=[{{kind="unpacked_array",left={left}:i64,right={right}:i64,direction="{direction}",
           element={{kind="{kind}",width={width}:i64,signed=true,four_state=false}}}}]}}'''

    valid = 'module {' + decl() + '}'
    converted = lower('valid', valid)
    assert 'kind = "unpacked_array"' in converted and 'left = 3 : i64' in converted
    assert 'signed16_const_pointer' in converted and 'llvm.alloca' in converted
    assert lower('again', converted) == converted
    lower('same-c-type-shifted-ranges',
          'module {' + decl() + decl('alias', left=-2, right=0) + '}')
    lower('different-pointee',
          'module {' + decl() + decl('alias', kind='byte', width=8) + '}',
          'conflicting native DPI C pointer element prototype')
    lower('const-pointee',
          'module {' + decl() + decl('alias', direction='inout') + '}',
          'conflicting native DPI C pointer element prototype')
    lower('shape-mismatch', 'module {' + decl(count=4) + '}',
          'invalid native unpacked DPI array shape or direction')
    lower(
        'return-array', 'module {' +
        decl(direction='return').replace('return %value', 'return value') +
        '}', 'invalid native unpacked DPI array shape or direction')
    lower(
        'ref-array', 'module {' +
        decl(direction='ref').replace('!hw.array<3xi16>', '!llvm.ptr') + '}',
        'invalid native unpacked DPI array shape or direction')
    malformed = valid.replace(
        'element={kind="shortint",width=16:i64,signed=true,four_state=false}',
        'element={kind="int",width=32:i64,signed=true,four_state=false}')
    lower('wrong-element', malformed, 'does not match logical formal type')
    lower('missing-bounds', valid.replace('left=3:i64,', ''),
          'invalid native unpacked DPI array shape or direction')
    print(
        'PASS: fixed-array shape/type/direction guards, original ABI, shifted-range aliases, C pointer element/constness conflicts and idempotence',
        flush=True)


if __name__ == '__main__':
    main()
