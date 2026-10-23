#!/usr/bin/env python3
"""Check pointer/string array transport, original ABI and initializer effects."""
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
    work = SIM / 'generated-src/native-pointer-array-guards'
    work.mkdir(parents=True, exist_ok=True)
    libs = [
        plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
               SIM / 'generated-src/compiler',
               SIM / ('compiler/' + name + '.cpp'))
        for name in ('NativeHandles', 'NativeStrings', 'Model')
    ]
    transport = 'chipyard-native-handles,chipyard-native-strings'

    def lower(name, text, pipeline=transport, error=None):
        source, target = work / (name + '.mlir'), work / (name + '-out.mlir')
        source.write_text(text)
        result = subprocess.run([
            args.circt_opt, source, *('--load-pass-plugin=' + str(p)
                                      for p in libs),
            '--pass-pipeline=builtin.module(' + pipeline + ')', '-o', target
        ],
                                capture_output=True,
                                text=True)
        if error:
            assert result.returncode == 1 and error in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        return target.read_text()

    def declaration(name='array', kind='chandle', count=2):
        ty = '!llvm.ptr' if kind == 'chandle' else '!sim.dstring'
        return f'''sim.func.dpi private @{name}(in %values:!hw.array<{count}x{ty}>) attributes {{
          verilogName="array_c",chipyard.dpi_decl={{context=false,task=false,pure=false}},
          chipyard.dpi_abi=[{{kind="unpacked_array",left=3:i64,right={4-count}:i64,direction="in",element={{kind="{kind}"}}}}]}}'''

    for kind in ('chandle', 'string'):
        original = 'module {' + declaration(kind=kind) + '}'
        converted = lower(kind, original)
        assert '!hw.array<2xi64>' in converted and 'kind = "' + kind + '"' in converted
        assert 'native_' + kind + '_handle = true' in converted
        assert lower(kind + '-again', converted) == converted
        bridged = lower(kind + '-bridge', converted, 'chipyard-native-dpi-abi')
        assert ('void_pointer'
                if kind == 'chandle' else 'const_char_pointer') in bridged
        if kind == 'string':
            assert 'chipyard_arc_string_c_str' in bridged
        else:
            assert 'llvm.inttoptr' in bridged
    lower('incompatible-pointer-elements',
          'module {' + declaration() + declaration('text', 'string') + '}',
          transport + ',chipyard-native-dpi-abi',
          'conflicting native DPI C pointer element prototype')
    lower('wrong-array-shape',
          ('module {' + declaration() + '}').replace('right=2:i64',
                                                     'right=1:i64'),
          transport + ',chipyard-native-dpi-abi',
          'invalid native unpacked DPI array shape or direction')
    for kind in ('chandle', 'string'):
        hidden = '{kind="longint",width=64:i64,signed=false,direction="in",native_context_scope=true}'
        original = '{kind="unpacked_array",left=3:i64,right=2:i64,direction="in",element={kind="' + kind + '"}}'
        scoped = declaration(kind=kind).replace('(in %values:',
                                                '(in %scope:i64,in %values:')
        scoped = scoped.replace('context=false', 'context=true')
        scoped = scoped.replace(
            'chipyard.dpi_abi=[',
            'chipyard.native_context_scope,chipyard.original_dpi_abi=[' +
            original + '],chipyard.dpi_abi=[' + hidden + ',')
        converted = lower(kind + '-context', 'module {' + scoped + '}',
                          transport + ',chipyard-native-dpi-abi')
        assert 'chipyard.original_dpi_abi' in converted
        lower(
            kind + '-context-provenance', 'module {' + scoped.replace(
                'chipyard.original_dpi_abi=[' + original + ']',
                'chipyard.original_dpi_abi=[' +
                original.replace('left=3', 'left=4') + ']') + '}',
            transport + ',chipyard-native-dpi-abi',
            'invalid native context scope transport provenance')
    # Nested default constructors must become typed constant initializers.
    # A DPI-produced pointer is effectful runtime data and must stay a call.
    null = '''module {DECL
      hw.module @Init() {
        %zero=llvm.mlir.zero:!llvm.ptr
        %row=hw.array_create %zero,%zero:!llvm.ptr
        %all=hw.array_create %row,%row:!hw.array<2x!llvm.ptr>
        %s=llhd.sig %all:<!hw.array<2xarray<2x!llvm.ptr>>>
        hw.output
      }
    }'''.replace('DECL', declaration())
    converted = lower('nested-default', null)
    assert 'hw.aggregate_constant [[0, 0], [0, 0]]' in converted
    effect = '''module {DECL
      sim.func.dpi private @get(return result:!llvm.ptr) attributes {
        verilogName="owned_ptr",chipyard.dpi_abi=[{kind="chandle",direction="return"}],
        chipyard.dpi_decl={context=false,task=false,pure=false}}
      hw.module @Init() {
        %p=sim.func.dpi.call @get():()->!llvm.ptr
        %array=hw.array_create %p,%p:!llvm.ptr
        %s=llhd.sig %array:<!hw.array<2x!llvm.ptr>>
        hw.output
      }
    }'''.replace('DECL', declaration())
    converted = lower('effectful-initializer', effect)
    assert 'sim.func.dpi.call @get' in converted and 'hw.array_create' in converted
    print(
        'PASS: recursive pointer/string array admission, ABI/context provenance, C prototype guards, constant defaults and retained initializer effects',
        flush=True)


if __name__ == '__main__':
    main()
