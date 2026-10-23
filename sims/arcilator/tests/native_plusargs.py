#!/usr/bin/env python3
"""Check native test-plusargs declaration safety and repeated lowering."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim))
from compiler.partition import plugin

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--circt-opt', required=True, type=Path)
p.add_argument('--work',
               type=Path,
               default=sim / 'generated-src/native-plusargs-guards')
a = p.parse_args()
a.work.mkdir(parents=True, exist_ok=True)
lib = plugin(a.circt_opt, os.environ.get('CXX', 'c++'),
             sim / 'generated-src/compiler',
             sim / 'compiler/NativePlusArgs.cpp')


def run(source, target):
    return subprocess.run([
        a.circt_opt, source, '--load-pass-plugin=' + str(lib),
        '--pass-pipeline=builtin.module(chipyard-native-plusargs)', '-o',
        target
    ],
                          capture_output=True,
                          text=True)


src = a.work / 'positive.mlir'
src.write_text('''module {
 func.func @check()->i1 {
  %a=sim.plusargs.test "foo"
  %b=sim.plusargs.test "foo"
  %both=comb.and %a,%b:i1
  return %both:i1
 }
}''')
first = a.work / 'first.mlir'
second = a.work / 'second.mlir'
r = run(src, first)
assert r.returncode == 0, r.stderr
r = run(first, second)
assert r.returncode == 0, r.stderr
assert first.read_bytes() == second.read_bytes()
text = first.read_text()
assert 'sim.plusargs.test' not in text
assert text.count('func.func private @__chipyard_test_plusargs(') == 1
for name, signature in [('vpi_get_vlog_info', '(i32)->i32'),
                        ('strncmp', '(i32,i32,i64)->i32')]:
    src = a.work / (name + '.mlir')
    src.write_text(
        'module {\n func.func private @' + name + signature +
        '\n func.func @test()->i1 {\n %a=sim.plusargs.test "foo"\n return %a:i1\n }\n}\n'
    )
    r = run(src, a.work / (name + '-output.mlir'))
    (a.work / (name + '.log')).write_text(r.stdout + r.stderr)
    assert r.returncode != 0 and 'conflicting native plusargs C ABI declaration' in r.stderr, r.stderr
print(
    'PASS: native test-plusargs shared pattern helper, idempotence and explicit incompatible VPI/libc ABI rejection',
    flush=True)
