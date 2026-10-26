#!/usr/bin/env python3
"""Check ThinLTO visibility, mutable data, fallback and failed-build preservation."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM / 'compiler'))
from codegen import compile_model

SOURCE = '''target datalayout = "e-m:e-n8:16:32:64-S128-p270:32:32:32:32-p271:32:32:32:32-p272:64:64:64:64-i64:64-i128:128-f80:128-p0:64:64:64:64-i1:8-i8:8-i16:16-i32:32-f16:16-f64:64-f128:128"
@state_partition_codegen_public = global i64 7
@private_table = internal constant [4 x i64] [i64 37, i64 11, i64 83, i64 29]
define internal i64 @pick(i64 %x) noinline {
  %i = and i64 %x, 3
  %p = getelementptr [4 x i64], ptr @private_table, i64 0, i64 %i
  %v = load i64, ptr %p
  %r = add i64 %x, %v
  ret i64 %r
}
define i64 @state_partition_codegen_step(i64 %x) {
  %v = call i64 @pick(i64 %x)
  %old = load i64, ptr @state_partition_codegen_public
  %next = xor i64 %old, %v
  store i64 %next, ptr @state_partition_codegen_public
  ret i64 %next
}
define i64 @state_partition_codegen_get() {
  %v = load i64, ptr @state_partition_codegen_public
  ret i64 %v
}
'''


def check(sdk, cxx, work):
    work.mkdir(parents=True, exist_ok=True)
    triple = subprocess.check_output([sdk / 'llvm-config', '--host-target'],
                                     text=True).strip()
    version = subprocess.check_output([sdk / 'llc', '--version'], text=True)
    import re
    cpu = re.search(r'^\s*Host CPU:\s*(\S+)', version, re.M)[1]
    source = work / 'source.ll'
    source.write_text(SOURCE)
    cache = work / 'cache'
    cases = []
    for mode in ('serial', 'thin', 'weak-fallback', 'flags-fallback'):
        directory = work / mode
        directory.mkdir(exist_ok=True)
        active = source
        if mode == 'weak-fallback':
            active = directory / 'source.ll'
            active.write_text(SOURCE.replace('define internal i64 @pick',
                                             'define weak i64 @pick'))
        obj, report = directory / 'model.o', directory / 'report.json'
        compile_model(active, obj, sdk / 'opt', sdk / 'llc', cache, cxx,
                      abi_buffers=False, pipeline='stock', jobs=3,
                      state_partitioning=mode != 'serial',
                      opt_flags=('-mtriple=' + triple + (' -S' if mode == 'flags-fallback' else
                                 ' -mcpu=native -vectorize-slp=false -vectorize-loops=false')),
                      report=report)
        shared = directory / 'model.so'
        subprocess.run([*shlex.split(cxx), '-shared', str(obj), '-o', str(shared)],
                       check=True)
        lib = ctypes.CDLL(str(shared))
        step = lib.state_partition_codegen_step
        step.argtypes, step.restype = [ctypes.c_uint64], ctypes.c_uint64
        lib.state_partition_codegen_get.restype = ctypes.c_uint64
        public = ctypes.c_uint64.in_dll(lib, 'state_partition_codegen_public')
        for i in range(2048):
            x = (i * 0x9e3779b97f4a7c15) & ((1 << 64) - 1)
            public.value = (i * 19) ^ 0xfedcba98
            expected = public.value ^ ((x + (37, 11, 83, 29)[x & 3]) & ((1 << 64) - 1))
            assert step(x) == expected
            assert lib.state_partition_codegen_get() == expected
        details = json.loads(report.read_text())
        if mode == 'thin':
            assert details['optimization_strategy'] == 'thinlto-summary-parallel'
            assert details['canonical_data_layout_applied'] is True
            assert details['data_layout_equivalence'] == (
                'LLVM-DataLayout-operator==-after-proven-redundant-integer-spec-removal')
            assert 'i1:8' in details['redundant_original_integer_specs']
            assert details['thinlto_opt_flags'] == [
                '-override-triple=' + triple, '-mcpu=' + cpu,
                '-vectorize-slp=false', '-vectorize-loops=false']
            symbols = subprocess.check_output([
                sdk / 'llvm-nm', '--defined-only', '--extern-only', str(obj)
            ], text=True)
            assert '__chipyard_codegen_local_' not in symbols
        if mode == 'weak-fallback':
            assert 'weak/common' in details['thinlto_fallback']
        if mode == 'flags-fallback':
            assert 'thinlto_fallback' in details
            assert details.get('optimization_strategy') != 'thinlto-summary-parallel'
        cases.append(dict(mode=mode, independent_calls=2048, report=details))

    # Exercise the actual failed ThinLTO subprocess after successful prelink and
    # resolution. The last good public object/bitcode must survive unchanged.
    fake = work / 'fake-sdk/bin'
    fake.mkdir(parents=True, exist_ok=True)
    opt = fake / 'opt'
    if not opt.exists():
        opt.symlink_to(sdk / 'opt')
    driver = fake / 'llvm-lto2'
    driver.write_text('#!' + sys.executable + '\nimport os,sys\n'
                      'if sys.argv[1:2]==["run"] and not any("help" in x for x in sys.argv[2:]):\n'
                      ' print("expected-thin-failure",file=sys.stderr);sys.exit(73)\n'
                      'os.execv(' + repr(str(sdk / 'llvm-lto2')) + ',[' +
                      repr(str(sdk / 'llvm-lto2')) + ',*sys.argv[1:]])\n')
    driver.chmod(0o755)
    target = work / 'thin/model.o'
    bitcode = target.with_suffix('.bc')
    before = (target.read_bytes(), bitcode.read_bytes())
    try:
        compile_model(source, target, opt, sdk / 'llc', cache, cxx,
                      abi_buffers=False, pipeline='stock', jobs=3,
                      state_partitioning=True, opt_flags='-mtriple=' + triple)
    except ValueError as error:
        assert 'expected-thin-failure' in str(error)
    else:
        raise AssertionError('deliberate backend failure was accepted')
    assert (target.read_bytes(), bitcode.read_bytes()) == before
    result = dict(status='passed', cases=cases, private_bindings_restored=True,
                  failed_build_preserves_object_and_bitcode=True)
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(status='passed', calls=8192,
                          failure_preserves_artifacts=True)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path,
                        default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--work', type=Path,
                        default=SIM / 'generated-src/state-partition-codegen-test')
    args = parser.parse_args()
    if args.circt_bin is None:
        parser.error('use the matching CIRCT SDK')
    check(args.circt_bin.resolve(), args.cxx, args.work.resolve())


if __name__ == '__main__':
    main()
