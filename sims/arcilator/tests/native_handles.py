#!/usr/bin/env python3
"""Check opaque pointer storage admission, ABI provenance and idempotence."""
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
    work = SIM / 'generated-src/native-handle-guards'
    work.mkdir(parents=True, exist_ok=True)
    library = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/NativeHandles.cpp')

    def lower(name, text, error=None):
        source, target = work / (name + '.mlir'), work / (name +
                                                          '-lowered.mlir')
        source.write_text(text)
        result = subprocess.run([
            args.circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-handles)', '-o',
            target
        ],
                                capture_output=True,
                                text=True)
        if error:
            assert result.returncode == 1 and error in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        return target.read_text()

    declaration = '''sim.func.dpi private @opaque(in %v: !llvm.ptr) attributes {
      chipyard.dpi_abi = [{kind = "chandle", direction = "in"}],
      chipyard.dpi_decl = {context = false, task = false}}
    '''
    source = '''module {DECL
      func.func @nonnull(%value: !llvm.ptr) -> i1 {
        %zero = llvm.mlir.zero : !llvm.ptr
        %result = llvm.icmp "ne" %value, %zero : !llvm.ptr
        return %result : i1
      }
    }'''.replace('DECL', declaration)
    converted = lower('storage', source)
    assert '!llvm.ptr' not in converted and 'native_chandle_handle = true' in converted
    assert 'kind = "chandle"' in converted and 'width =' not in converted
    assert 'hw.constant 0 : i64' in converted and 'arith.cmpi ne' in converted
    assert lower('storage-again', converted) == converted
    unrelated = lower('unrelated', source.replace(declaration, ''))
    assert '!llvm.ptr' in unrelated and 'llvm.icmp' in unrelated
    for name, body in [
        ('ordered-pointer',
         '''func.func @invalid(%p:!llvm.ptr,%q:!llvm.ptr)->i1 {
          %v=llvm.icmp "ult" %p,%q:!llvm.ptr
          return %v:i1
        }'''),
        ('pointer-dereference', '''func.func @invalid(%p:!llvm.ptr)->i32 {
          %v=llvm.load %p : !llvm.ptr -> i32
          return %v:i32
        }'''),
    ]:
        lower(name,
              'module {' + declaration + body + '}',
              error='unsupported native opaque handle operation')
    print(
        'PASS: native chandle storage preserves original ABI, null/equality, '
        'idempotence and unrelated pointers; ordered comparisons and dereferences rejected',
        flush=True)


if __name__ == '__main__':
    main()
