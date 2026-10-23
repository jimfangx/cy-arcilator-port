#!/usr/bin/env python3
"""Check explicit native string/readmem limits and original ABI provenance."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def check_contracts(path):
    strings = 0
    for variant in json.loads(path.read_text()):
        for export in variant.get('exports', []):
            for arg in export['abi']:
                if arg['kind'] == 'string':
                    assert arg.get('native_string_handle') is True
                    assert 'width' not in arg, 'SV strings must retain their dynamic original type'
                    strings += 1
    assert strings, 'fixture did not exercise a string C ABI'
    print(
        'PASS: original dynamic string ABI retained separately from native handle storage',
        flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/native-string-guards')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    library = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/NativeStrings.cpp')
    cases = {
        'dynamic-format': ('''module {
          func.func @unsupported(%value:!sim.dstring)->!sim.fstring {
            %format=sim.fmt.string %value : !sim.dstring
            return %format:!sim.fstring
          }
        }''', 'unsupported native dynamic string consumer'),
        'missing-unpacked-dimension':
        ('''module {
          func.func @unsupported(%file:!sim.dstring,%memory:!hw.array<2x!hw.array<3xi13>>)->!hw.array<2x!hw.array<3xi13>> {
            %loaded=sim.sv.readmem %file,%memory {dimLows=array<i64:0,0,0>,dimDescending=array<i1:false,false,false>}:!hw.array<2x!hw.array<3xi13>>
            return %loaded:!hw.array<2x!hw.array<3xi13>>
          }
        }''',
         'native readmem requires fixed storage for every declared unpacked dimension'
         )
    }
    for name, (text, diagnostic) in cases.items():
        source = args.work / (name + '.mlir')
        source.write_text(text)
        result = subprocess.run([
            args.circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-strings)', '-o',
            source.with_suffix('.lowered.mlir')
        ],
                                capture_output=True,
                                text=True)
        assert result.returncode == 1 and diagnostic in result.stderr, result.stderr
        (args.work / (name + '.log')).write_text(result.stderr)
    print(
        'PASS: unlowered dynamic formatting and inconsistent readmem dimensions report explicit errors',
        flush=True)
    # The frontend's original $timeformat state is a global struct containing
    # a dynamic string. Nested arrays/structs use the same transport conversion.
    source = args.work / 'struct-transport.mlir'
    output = args.work / 'struct-transport.lowered.mlir'
    source.write_text("""module {
      llhd.global_signal @state : !hw.struct<tag:i32,text:!sim.dstring> init {
        %tag=hw.constant 73:i32
        %empty=sim.string.literal ""
        %state=hw.struct_create (%tag,%empty):!hw.struct<tag:i32,text:!sim.dstring>
        llhd.yield %state:!hw.struct<tag:i32,text:!sim.dstring>
      }
      func.func @transport(%s:!sim.dstring,%ref:!llhd.ref<!hw.struct<tag:i32,text:!sim.dstring>>)->!hw.array<2x!hw.struct<tag:i32,text:!sim.dstring>> {
        %tag=hw.constant 91:i32
        %state=hw.struct_create (%tag,%s):!hw.struct<tag:i32,text:!sim.dstring>
        %old=llhd.prb %ref:!hw.struct<tag:i32,text:!sim.dstring>
        %next=hw.struct_inject %old["text"],%s:!hw.struct<tag:i32,text:!sim.dstring>
        %field=llhd.sig.struct_extract %ref["text"]:<!hw.struct<tag:i32,text:!sim.dstring>>
        %time=llhd.constant_time <0ns,0d,1e>
        llhd.drv %field,%s after %time:!sim.dstring
        %both=hw.array_create %state,%next:!hw.struct<tag:i32,text:!sim.dstring>
        return %both:!hw.array<2x!hw.struct<tag:i32,text:!sim.dstring>>
      }
    }""")
    subprocess.run([args.circt_opt, source, '--load-pass-plugin=' + str(library),
                    '--pass-pipeline=builtin.module(chipyard-native-strings,canonicalize)',
                    '-o', output], check=True)
    text = output.read_text()
    assert '!sim.dstring' not in text and 'text: i64' in text, text
    assert 'hw.aggregate_constant [73 : i32, 0]' in text and 'hw.constant 91 : i32' in text, text
    print('PASS: nested string struct/array transport, global startup type and member references', flush=True)



if __name__ == '__main__':
    main()
