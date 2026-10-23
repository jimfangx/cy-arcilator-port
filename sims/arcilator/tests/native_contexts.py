#!/usr/bin/env python3
"""Preserve physical scope provenance through nested function inlining."""
import argparse
import json
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
    work = SIM / 'generated-src/native-context-guards'
    work.mkdir(parents=True, exist_ok=True)
    library = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/NativeContexts.cpp')

    def lower(name, text, error=None):
        source, target = work / (name + '.mlir'), work / (name +
                                                          '-lowered.mlir')
        source.write_text(text)
        result = subprocess.run([
            args.circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-contexts{root=Root})',
            '-o', target
        ],
                                capture_output=True,
                                text=True)
        if error:
            assert result.returncode == 1 and error in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        return target.read_text()

    source = '''module {
      sim.func.dpi private @probe(in %v:i32, return result:i32) attributes {
        chipyard.dpi_abi=[{kind="int",direction="in",width=32:i64,signed=true},
                         {kind="int",direction="return",width=32:i64,signed=true}],
        chipyard.dpi_decl={context=true,pure=false,task=false,scope="Root.a"}}
      func.func private @inner(%v:i32)->i32 {
        %r=sim.func.dpi.call @probe(%v) {chipyard.dpi_context_lookup="lexical"}:(i32)->i32
        return %r:i32
      }
      func.func private @outer(%v:i32)->i32 {
        %r=func.call @inner(%v) {chipyard.sv_call_lookup="lexical"}:(i32)->i32
        return %r:i32
      }
      hw.module @Leaf(in %v:i32,out local:i32,out absolute:i32) {
        %l=func.call @outer(%v) {chipyard.sv_call_lookup="lexical"}:(i32)->i32
        %a=func.call @outer(%v) {chipyard.sv_call_lookup="absolute"}:(i32)->i32
        hw.output %l,%a:i32,i32
      }
      hw.module @Root(in %v:i32,out result:i32) {
        %a:2=hw.instance "a" @Leaf(v:%v:i32)->(local:i32,absolute:i32)
        %b:2=hw.instance "b" @Leaf(v:%v:i32)->(local:i32,absolute:i32)
        hw.output %b#1:i32
      }
    }'''
    converted = lower('nested', source)
    assert 'func.call @outer' not in converted and 'func.call @inner' not in converted
    assert 'func.func private @outer' not in converted and 'func.func private @inner' not in converted
    assert 'MODE: !hw.string = "local"' in converted and 'MODE: !hw.string = "root"' in converted, converted
    assert 'SCOPE: !hw.string = ".a"' in converted, converted
    assert 'chipyard.original_dpi_abi' in converted and 'native_context_scope = true' in converted
    assert lower('nested-again', converted) == converted
    generated = source.replace(
        'scope="Root.a"', 'scope=' + json.dumps(r'Root.banks[0].\leaf.dot '))
    generated = generated.replace(
        'out absolute:i32) {',
        'out absolute:i32) attributes {chipyard.sv_source_scope=' +
        json.dumps(r'Root.banks[0].\leaf.dot ') + '} {')
    generated = generated.replace(
        'out result:i32) {',
        'out result:i32) attributes {chipyard.sv_source_scope="Root"} {')
    for i, name in enumerate(('a', 'b')):
        generated = generated.replace(
            '"' + name + '" @Leaf(v:%v:i32)->(local:i32,absolute:i32)',
            '"banks_' + str(i) +
            '.leaf.dot" @Leaf(v:%v:i32)->(local:i32,absolute:i32) {chipyard.sv_source_scope='
            + json.dumps('Root.banks[' + str(i) + r'].\leaf.dot ') + '}')
    mapped = lower('generated-escaped', generated)
    assert 'MODE: !hw.string = "local"' in mapped and 'banks_0.leaf.dot ' in mapped, mapped
    assert lower('generated-escaped-again', mapped) == mapped
    lower('invalid-source-hierarchy',
          generated.replace('Root.banks[1]', 'Other.banks[1]'),
          'native DPI hierarchy lacks relative source provenance')
    lower('missing-call-lookup',
          source.replace('chipyard.sv_call_lookup="absolute"', ''),
          'context SV function requires call lookup provenance')
    lower(
        'invalid-call-lookup',
        source.replace('chipyard.sv_call_lookup="absolute"',
                       'chipyard.sv_call_lookup="unknown"'),
        'context SV function requires call lookup provenance')
    lower('missing-dpi-lookup',
          source.replace('chipyard.dpi_context_lookup="lexical"', ''),
          'native context DPI requires call lookup provenance')
    lower(
        'invalid-dpi-lookup',
        source.replace('chipyard.dpi_context_lookup="lexical"',
                       'chipyard.dpi_context_lookup="unknown"'),
        'native context DPI requires call lookup provenance')
    recursive = source.replace(
        'return %r:i32\n      }\n      func.func private @outer',
        '%cycle=func.call @inner(%r) {chipyard.sv_call_lookup="lexical"}:(i32)->i32\n        return %cycle:i32\n      }\n      func.func private @outer',
        1)
    lower('recursive', recursive,
          'recursive context SV functions require native call frames')
    print(
        'PASS: nested SV function scope provenance, lexical/absolute bindings, generated/escaped source mapping, original ABI and idempotence; missing/invalid provenance and recursive context frames rejected',
        flush=True)


if __name__ == '__main__':
    main()
