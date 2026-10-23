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
w = SIM / 'generated-src/large-argument-test'
w.mkdir(parents=True, exist_ok=True)
library = plugin(sdk / 'opt', os.environ.get('CXX', 'c++'),
                 SIM / 'generated-src/compiler',
                 SIM / 'compiler/LargeArguments.cpp')
layout = next(line for line in subprocess.check_output(
    [sdk / 'opt', '-S', '-o', '-', '-'],
    input='target triple = "x86_64-unknown-linux-gnu"\n',
    text=True).splitlines() if line.startswith('target datalayout'))
types = ['i64'] * 64 + ['ptr', 'double', 'i65', '%Packet']
params = ', '.join(f'{ty} %p{i}' for i, ty in enumerate(types))
intparams = ', '.join(f'i64 %p{i}' for i in range(64))
intargs = ', '.join(f'i64 %p{i}' for i in range(64))
lines = [
    layout.strip(), 'target triple = "x86_64-unknown-linux-gnu"',
    '%Packet = type <{i8, [3 x i16], ptr}>',
    '%Many = type {' + ', '.join(['i64'] * 64) + '}', '%Record = type {i64}',
    'define internal fastcc i64 @sum64(' + intparams +
    ') noinline memory(none) {'
]
previous = '0'
for i in range(64):
    lines += [
        f' %m{i}=mul i64 %p{i},{i*5+1}', f' %s{i}=add i64 {previous},%m{i}'
    ]
    previous = f'%s{i}'
lines += [
    f' ret i64 {previous}', '}',
    'define internal %Many @leaf(' + params + ') noinline {',
    ' %sum=call fastcc i64 @sum64(' + intargs + ') memory(none)',
    ' %old=load i64,ptr %p64', ' %delta=and i64 %p0,255',
    ' %marker=add i64 %old,%delta', ' store i64 %marker,ptr %p64',
    ' %weight=mul i64 %marker,3', ' %f=bitcast double %p65 to i64',
    ' %low=trunc i65 %p66 to i64', ' %shift=lshr i65 %p66,64',
    ' %high=trunc i65 %shift to i64', ' %wl=mul i64 %low,17',
    ' %wh=mul i64 %high,23', ' %byte=extractvalue %Packet %p67,0',
    ' %byte64=zext i8 %byte to i64', ' %a=extractvalue %Packet %p67,1,0',
    ' %a64=zext i16 %a to i64', ' %b=extractvalue %Packet %p67,1,1',
    ' %b64=zext i16 %b to i64', ' %c=extractvalue %Packet %p67,1,2',
    ' %c64=zext i16 %c to i64', ' %wb=mul i64 %b64,3', ' %wc=mul i64 %c64,5',
    ' %ptr=extractvalue %Packet %p67,2', ' %ptr64=ptrtoint ptr %ptr to i64'
]
prev = '%sum'
for i, value in enumerate(
    ['%weight', '%f', '%wl', '%wh', '%byte64', '%a64', '%wb', '%wc',
     '%ptr64']):
    lines += [f' %total{i}=add i64 {prev},{value}']
    prev = f'%total{i}'
aggregate = 'poison'
for i in range(64):
    lines += [
        f' %v{i}=add i64 {prev},{i*13}',
        f' %r{i}=insertvalue %Many {aggregate},i64 %v{i},{i}'
    ]
    aggregate = f'%r{i}'
lines += [
    f' ret %Many {aggregate}', '}',
    'define internal %Many @middle(' + params + ') noinline {',
    ' %other=xor i64 %p0,12345'
]


def args(first):
    return ', '.join(f'{ty} {first if i==0 else "%p"+str(i)}'
                     for i, ty in enumerate(types))


lines += [
    ' %a=call %Many @leaf(' + args('%p0') + ')',
    ' %b=call %Many @leaf(' + args('%other') + ')'
]
aggregate = 'poison'
for i in range(64):
    lines += [
        f' %a{i}=extractvalue %Many %a,{i}',
        f' %b{i}=extractvalue %Many %b,{i}', f' %v{i}=xor i64 %a{i},%b{i}',
        f' %r{i}=insertvalue %Many {aggregate},i64 %v{i},{i}'
    ]
    aggregate = f'%r{i}'
lines += [f' ret %Many {aggregate}', '}']
# These definitions must retain their ABI, including a musttail caller whose
# incoming signature must remain congruent with the target it forwards to.
lines += [
    'define internal i64 @bycopy(ptr byval(%Record) %copy,' +
    ','.join(f'i64 %p{i}'
             for i in range(63)) + ') {', ' %old=load i64,ptr %copy',
    ' %new=add i64 %old,5', ' store i64 %new,ptr %copy', ' ret i64 %new', '}',
    'define internal i64 @escaped(' +
    ','.join(f'i64 %p{i}' for i in range(63)) + ',ptr %self) { ret i64 %p0 }',
    'define i64 @public64(' + intparams + ') { ret i64 %p63 }',
    'define internal i64 @tail_target(' + intparams + ') { ret i64 %p0 }',
    'define internal i64 @tail_forward(' + intparams + ') {',
    ' %r=musttail call i64 @tail_target(' + intargs + ')', ' ret i64 %r', '}',
    'define i64 @args_driver(i64 %seed,ptr %marker) {'
]
for i in range(64):
    lines += [f' %p{i}=xor i64 %seed,{i*37+9}']
lines += [
    ' %p64=getelementptr i8,ptr %marker,i64 0',
    ' %p65=bitcast i64 %seed to double', ' %low=mul i64 %seed,3',
    ' %wide=zext i64 %low to i65', ' %p66=or i65 %wide,18446744073709551616',
    ' %byte=trunc i64 %seed to i8', ' %half=trunc i64 %seed to i16',
    ' %ar0=insertvalue [3 x i16] poison,i16 %half,0',
    ' %ar1=insertvalue [3 x i16] %ar0,i16 4660,1',
    ' %ar2=insertvalue [3 x i16] %ar1,i16 43981,2',
    ' %pk0=insertvalue %Packet poison,i8 %byte,0',
    ' %pk1=insertvalue %Packet %pk0,[3 x i16] %ar2,1',
    ' %p67=insertvalue %Packet %pk1,ptr %marker,2',
    ' %small=call fastcc i64 @sum64(' + intargs + ') memory(none)',
    ' %mid=call %Many @middle(' + args('%p0') + ')',
    ' %other=add i64 %p0,987654321',
    ' %last=call %Many @leaf(' + args('%other') + ')'
]
prev = '%small'
for i in range(64):
    lines += [
        f' %a{i}=extractvalue %Many %mid,{i}',
        f' %b{i}=extractvalue %Many %last,{i}', f' %v{i}=add i64 %a{i},%b{i}',
        f' %m{i}=mul i64 %v{i},{i*7+1}', f' %s{i}=add i64 {prev},%m{i}'
    ]
    prev = f'%s{i}'
zeros = ', '.join(['i64 0'] * 63)
lines += [
    ' %record=alloca %Record', ' store i64 %seed,ptr %record',
    ' %copied=call i64 @bycopy(ptr byval(%Record) %record,' + zeros + ')',
    ' %retained=load i64,ptr %record', ' %copycheck=xor i64 %copied,%retained',
    ' %escaped=call i64 @escaped(i64 %p0,' + ','.join(['i64 0'] * 62) +
    ',ptr @escaped)', ' %public=call i64 @public64(' + intargs + ')',
    ' %tail=call i64 @tail_forward(' + intargs + ')'
]
for i, value in enumerate(['%copycheck', '%escaped', '%public', '%tail']):
    lines += [f' %extra{i}=add i64 {prev},{value}']
    prev = f'%extra{i}'
lines += [f' ret i64 {prev}', '}']
(w / 'args-input.ll').write_text('\n'.join(lines) + '\n')
(w / 'args-check.cpp').write_text(r'''#include <cstdint>
#include <cstdio>
#include <stdexcept>
extern "C" uint64_t args_driver(uint64_t,uint64_t*);
int main(){uint64_t seed=123456789;for(unsigned it=0;it<4096;++it){
seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
uint64_t marker=seed^0xdeadbeef12345678ULL,reference=marker,base=0;
for(unsigned i=0;i<64;++i)base+=(seed^(i*37+9))*(i*5+1);
auto leaf=[&](uint64_t arg0){reference+=arg0&255;
return base-(seed^9)+arg0+reference*3+seed+(seed*3)*17+23+
uint8_t(seed)+uint16_t(seed)+4660*3+43981*5+reinterpret_cast<uintptr_t>(&marker);};
uint64_t a=leaf(seed^9),b=leaf((seed^9)^12345),c=leaf((seed^9)+987654321),expected=base;
for(unsigned i=0;i<64;++i)expected+=(((a+i*13)^(b+i*13))+(c+i*13))*(i*7+1);
expected+=((seed+5)^seed)+(seed^9)+(seed^(63*37+9))+(seed^9);
if(args_driver(seed,&marker)!=expected||marker!=reference)
throw std::runtime_error("packed argument/return ABI, nested scratch or pointer effects changed");
}std::puts("PASS: 4096 mixed argument/return checks; nested calls, scratch reuse/resize, 65-bit/float/packed fields, effects, byval and musttail guards");}
''')


def run(*command):
    subprocess.run(list(map(str, command)), check=True)


shutil.copyfile(w / 'args-input.ll', w / 'args-buffered.ll')
run(sys.executable, SIM / 'compiler/llvm.py', w / 'args-buffered.ll',
    '--llvm-opt', sdk / 'opt', '--cache', SIM / 'generated-src/compiler',
    '--cxx', os.environ.get('CXX', 'c++'))
text = (w / 'args-buffered.ll').read_text()
for name in ['sum64', 'leaf', 'middle']:
    header = next(line for line in text.splitlines()
                  if line.startswith('define ') and '@' + name + '(' in line)
    assert re.search('@' + name + r'\(ptr %chipyard.arguments\)',
                     header), header
for name in ['bycopy', 'escaped', 'public64', 'tail_target', 'tail_forward']:
    header = next(line for line in text.splitlines()
                  if line.startswith('define ') and '@' + name + '(' in line)
    assert header.count('%p') >= 62, header
run(sdk / 'opt', '-load-pass-plugin=' + str(library),
    '-passes=chipyard-buffer-large-arguments,verify', '-S',
    w / 'args-buffered.ll', '-o', w / 'args-twice.ll')
normalize = lambda s: re.sub(r'^; ModuleID.*\n', '', s)
assert normalize(text) == normalize((w / 'args-twice.ll').read_text())
for source in ['args-input', 'args-buffered']:
    for level in ['0', '2']:
        base = w / (source + '-O' + level)
        run(sdk / 'opt', '-O' + level, w / (source + '.ll'), '-o',
            str(base) + '.bc')
        run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj',
            str(base) + '.bc', '-o',
            str(base) + '.o')
        run(*shlex.split(os.environ.get('CXX', 'c++')), '-O' + level,
            w / 'args-check.cpp',
            str(base) + '.o', '-o', base)
        run(base)
print(
    'PASS: private input-buffer transform verified and idempotent; ABI-sensitive/public/address-taken functions retained',
    flush=True)
