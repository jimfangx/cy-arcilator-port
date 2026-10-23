#!/usr/bin/env python3
"""Numerically verify the private LLVM aggregate return ABI."""
from pathlib import Path
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from compiler.partition import plugin

sdk = Path(os.environ['CIRCT_BIN']).resolve()
cxx = shlex.split(os.environ.get('CXX', 'c++'))
w = SIM / 'generated-src/large-return-test'
w.mkdir(parents=True, exist_ok=True)
library = plugin(sdk / 'opt', os.environ.get('CXX', 'c++'),
                 SIM / 'generated-src/compiler',
                 SIM / 'compiler/LargeReturns.cpp')
layout = next(line for line in subprocess.check_output(
    [sdk / 'opt', '-S', '-o', '-', '-'],
    input='target triple = "x86_64-unknown-linux-gnu"\n',
    text=True).splitlines() if line.startswith('target datalayout'))
lines = [
    layout.strip(), 'target triple = "x86_64-unknown-linux-gnu"',
    '%Tuple = type {' + ', '.join(['i64'] * 64) + '}'
]
# Multiple exits, constant-base fields, overwritten fields, and fastcc. Put the
# callee first so the caller is rewritten after it has acquired scratch storage.
lines += [
    'define internal fastcc %Tuple @leaf(i64 %seed) memory(none) {',
    ' %bit = and i64 %seed, 1', ' %odd = icmp ne i64 %bit, 0',
    ' br i1 %odd, label %left, label %right'
]
for label, op in [('left', 'add'), ('right', 'xor')]:
    lines += [label + ':']
    prev = 'zeroinitializer'
    for i in range(0, 64, 2):
        lines += [
            f' %{label}v{i} = {op} i64 %seed, {i * 17 + 3}',
            f' %{label}r{i} = insertvalue %Tuple {prev}, '
            f'i64 %{label}v{i}, {i}'
        ]
        prev = f'%{label}r{i}'
    lines += [
        f' %{label}old = insertvalue %Tuple {prev}, i64 999, 63',
        f' %{label}last = insertvalue %Tuple %{label}old, i64 %seed, 63',
        f' ret %Tuple %{label}last'
    ]
lines += [
    '}', 'define internal fastcc %Tuple @nested(i64 %seed) memory(none) {',
    ' %other = xor i64 %seed, 123456789',
    ' %a = call fastcc %Tuple @leaf(i64 %seed) memory(none)',
    ' %b = call fastcc %Tuple @leaf(i64 %other) memory(none)'
]
prev = 'zeroinitializer'
for i in range(64):
    lines += [
        f' %a{i} = extractvalue %Tuple %a, {i}',
        f' %b{i} = extractvalue %Tuple %b, {i}',
        f' %v{i} = add i64 %a{i}, %b{i}',
        f' %r{i} = insertvalue %Tuple {prev}, i64 %v{i}, {i}'
    ]
    prev = f'%r{i}'
lines += [
    f' ret %Tuple {prev}', '}',
    'define i64 @cfg_driver(i64 %seed) memory(none) {',
    ' %other = add i64 %seed, 987654321',
    ' %a = call fastcc %Tuple @nested(i64 %seed) memory(none)',
    ' %b = call fastcc %Tuple @leaf(i64 %other) memory(none)'
]
acc = '0'
for i in range(64):
    lines += [
        f' %a{i} = extractvalue %Tuple %a, {i}',
        f' %b{i} = extractvalue %Tuple %b, {i}',
        f' %v{i} = xor i64 %a{i}, %b{i}',
        f' %m{i} = mul i64 %v{i}, {i * 7 + 1}',
        f' %s{i} = add i64 {acc}, %m{i}'
    ]
    acc = f'%s{i}'
lines += [f' ret i64 {acc}', '}']
(w / 'cfg.ll').write_text('\n'.join(lines) + '\n')
(w / 'cfg.cpp').write_text(r'''
#include <cstdint>
#include <cstdio>
#include <stdexcept>
extern "C" uint64_t cfg_driver(uint64_t);
uint64_t leaf(uint64_t s, unsigned i) {
  if (i == 63) return s;
  if (i & 1) return 0;
  return (s & 1) ? s + i*17+3 : s ^ (i*17+3);
}
int main() {
  uint64_t seed=123456789;
  for (unsigned it=0;it<4096;++it) {
    seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
    uint64_t expected=0;
    for(unsigned i=0;i<64;++i)
      expected+=((leaf(seed,i)+leaf(seed^123456789,i))^
                 leaf(seed+987654321,i))*(i*7+1);
    if(cfg_driver(seed)!=expected)
      throw std::runtime_error("nested return or control flow changed values");
  }
  std::puts("PASS: 4096 CFG/fastcc/nested-call/constant-base checks");
}
''')


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


plugin = '-load-pass-plugin=' + str(library)
passes = '-passes=chipyard-buffer-large-returns,verify'
run(sdk / 'opt', plugin, passes, '-S', w / 'cfg.ll', '-o',
    w / 'cfg-buffered.ll')
converted = (w / 'cfg-buffered.ll').read_text()
for name in ['leaf', 'nested']:
    assert f'define internal fastcc void @{name}(' in converted
run(sdk / 'opt', plugin, passes, '-S', w / 'cfg-buffered.ll', '-o',
    w / 'cfg-twice.ll')
normalize = lambda s: re.sub(r'^; ModuleID.*\n', '', s)
assert normalize(converted) == normalize((w / 'cfg-twice.ll').read_text())
# Missing data layout must be left to the normal target pipeline.
(w / 'cfg-no-layout.ll').write_text('\n'.join(lines[1:]) + '\n')
run(sdk / 'opt', '-mtriple=unknown-unknown-unknown', plugin, passes, '-S',
    w / 'cfg-no-layout.ll', '-o', w / 'cfg-no-layout-out.ll')
assert 'define internal fastcc %Tuple @leaf(' in (
    w / 'cfg-no-layout-out.ll').read_text()
for source in ['cfg', 'cfg-buffered']:
    for level in ['0', '2']:
        base = w / (source + '-O' + level)
        run(sdk / 'opt', '-O' + level, w / (source + '.ll'), '-o',
            str(base) + '.bc')
        run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj',
            str(base) + '.bc', '-o',
            str(base) + '.o')
        run(*shlex.split(os.environ.get('CXX', 'c++')), '-O' + level,
            w / 'cfg.cpp',
            str(base) + '.o', '-o', base)
        run(base)
print(
    'PASS: control flow, fastcc, nested scratch ownership, missing-layout guard, '
    'verification and idempotence',
    flush=True)
