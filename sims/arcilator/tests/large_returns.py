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
lines = [layout.strip(), 'target triple = "x86_64-unknown-linux-gnu"']
# All helper outputs have independent, simple reference formulas in C++.
types = {'Small': 64, 'Big': 129}
for name, n in types.items():
    lines.append(f'%{name} = type {{' + ', '.join(['i64'] * n) + '}')


def function(name, ty, n, linkage='internal', operation='add'):
    body = [
        f'define {linkage} %{ty} @{name}(i64 %seed) noinline memory(none) {{'
    ]
    previous = 'poison'
    for i in range(n):
        body += [
            f' %v{i} = {operation} i64 %seed, {i*13+7}',
            f' %r{i} = insertvalue %{ty} {previous}, i64 %v{i}, {i}'
        ]
        previous = f'%r{i}'
    body += [f' ret %{ty} {previous}', '}']
    return body


lines += function('small', 'Small', 64)
lines += function('big', 'Big', 129, operation='xor')
lines += function('escaped', 'Small', 64)
lines += ['@address = global ptr @escaped']
lines += [
    'define internal %Small @self_address(i64 %seed, ptr %self) {',
    ' %r = insertvalue %Small zeroinitializer, i64 %seed, 0', ' ret %Small %r',
    '}', 'define i64 @self_address_driver(i64 %seed) {',
    ' %r = call %Small @self_address(i64 %seed, ptr @self_address)',
    ' %v = extractvalue %Small %r, 0', ' ret i64 %v', '}'
]
lines += function('external_api', 'Small', 64, linkage='')
lines += function('stored_return', 'Small', 64)
lines += [
    '@stored = global %Small zeroinitializer',
    'define void @save(i64 %seed) {',
    ' %v = call %Small @stored_return(i64 %seed)',
    ' store %Small %v,ptr @stored', ' ret void', '}'
]
lines += [
    'define i64 @driver(i64 %seed) {', ' %other = xor i64 %seed, 123456789',
    ' %last = add i64 %seed, 987654321',
    ' %a = call %Small @small(i64 %seed) memory(none)',
    ' %b = call %Small @small(i64 %other) memory(none)',
    ' %c = call %Big @big(i64 %last) memory(none)'
]
acc = '0'
k = 0
# Deliberately extract the first call's fields after all three calls completed.
for value, ty, n in [('a', 'Small', 64), ('b', 'Small', 64),
                     ('c', 'Big', 129)]:
    for i in range(n):
        lines += [
            f' %e{k} = extractvalue %{ty} %{value}, {i}',
            f' %m{k} = mul i64 %e{k}, {k*7+1}',
            f' %s{k} = add i64 {acc}, %m{k}'
        ]
        acc = f'%s{k}'
        k += 1
lines += [f' ret i64 {acc}', '}']
# A packed heterogeneous tuple exercises byte offsets and nested indices.
fieldtypes = [
    'i8', 'i64', 'i13', '[3 x i16]', '{ i32, i8 }', 'ptr', 'double', 'i65'
] * 8
lines += [
    '%Packed = type <{' + ', '.join(fieldtypes) + '}>',
    'define internal %Packed @packed(i64 %seed, ptr %pointer) noinline memory(none) {'
]
values = []
for i, ty in enumerate(fieldtypes):
    tag = f'f{i}'
    kind = i % 8
    if kind == 0:
        lines += [f' %{tag}=trunc i64 %seed to i8']
        value = f'%{tag}'
    elif kind == 1:
        lines += [f' %{tag}=add i64 %seed,{i*19+3}']
        value = f'%{tag}'
    elif kind == 2:
        lines += [f' %{tag}=trunc i64 %seed to i13']
        value = f'%{tag}'
    elif kind == 3:
        lines += [
            f' %{tag}v=trunc i64 %seed to i16',
            f' %{tag}a=insertvalue [3 x i16] poison,i16 %{tag}v,0',
            f' %{tag}b=insertvalue [3 x i16] %{tag}a,i16 {i},1',
            f' %{tag}=insertvalue [3 x i16] %{tag}b,i16 65535,2'
        ]
        value = f'%{tag}'
    elif kind == 4:
        lines += [
            f' %{tag}v=trunc i64 %seed to i32',
            f' %{tag}a=insertvalue {{i32,i8}} poison,i32 %{tag}v,0',
            f' %{tag}=insertvalue {{i32,i8}} %{tag}a,i8 {i},1'
        ]
        value = f'%{tag}'
    elif kind == 5:
        value = '%pointer'
    elif kind == 6:
        lines += [f' %{tag}=bitcast i64 %seed to double']
        value = f'%{tag}'
    else:
        lines += [
            f' %{tag}a=zext i64 %seed to i65',
            f' %{tag}=or i65 %{tag}a,18446744073709551616'
        ]
        value = f'%{tag}'
    values.append(value)
prev = 'poison'
for i, (ty, value) in enumerate(zip(fieldtypes, values)):
    lines += [f' %r{i}=insertvalue %Packed {prev},{ty} {value},{i}']
    prev = f'%r{i}'
lines += [
    f' ret %Packed {prev}', '}',
    'define i64 @packed_driver(i64 %seed, ptr %pointer) {',
    ' %a=call %Packed @packed(i64 %seed,ptr %pointer) memory(none)',
    ' %other=xor i64 %seed,123456789',
    ' %b=call %Packed @packed(i64 %other,ptr %pointer) memory(none)'
]
acc = '0'
k = 0
for call in ['a', 'b']:
    for i, ty in enumerate(fieldtypes):
        kind = i % 8
        pieces = [([], ty)]
        if kind == 3: pieces = [([0], 'i16'), ([1], 'i16'), ([2], 'i16')]
        if kind == 4: pieces = [([0], 'i32'), ([1], 'i8')]
        if kind == 7: pieces = [([], 'i65'), ([], 'i65')]
        for j, (indices, scalar) in enumerate(pieces):
            name = f'e{k}'
            suffix = ', '.join(map(str, [i, *indices]))
            lines += [f' %{name}=extractvalue %Packed %{call},{suffix}']
            if scalar == 'ptr':
                lines += [f' %v{k}=ptrtoint ptr %{name} to i64']
                val = f'%v{k}'
            elif scalar == 'double':
                lines += [f' %v{k}=bitcast double %{name} to i64']
                val = f'%v{k}'
            elif scalar == 'i65':
                if j:
                    lines += [f' %h{k}=lshr i65 %{name},64']
                    name = f'h{k}'
                lines += [f' %v{k}=trunc i65 %{name} to i64']
                val = f'%v{k}'
            elif scalar != 'i64':
                lines += [f' %v{k}=zext {scalar} %{name} to i64']
                val = f'%v{k}'
            else:
                val = f'%{name}'
            lines += [
                f' %m{k}=mul i64 {val},{k*7+1}', f' %s{k}=add i64 {acc},%m{k}'
            ]
            acc = f'%s{k}'
            k += 1
lines += [f' ret i64 {acc}', '}']
(w / 'input.ll').write_text('\n'.join(lines) + '\n')
(w / 'check.cpp').write_text(r'''#include <cstdint>
#include <cstdio>
#include <stdexcept>
extern "C" uint64_t driver(uint64_t);
extern "C" uint64_t packed_driver(uint64_t,void*);
int main(){uint64_t seed=123456789,marker=0;unsigned checks=0;
for(unsigned it=0;it<4096;++it){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
uint64_t expected=0;unsigned k=0;
for(unsigned call=0;call<3;++call){uint64_t s=call==0?seed:call==1?(seed^123456789):(seed+987654321);unsigned n=call==2?129:64;
for(unsigned i=0;i<n;++i){uint64_t value=call==2?(s^(i*13+7)):(s+i*13+7);expected+=value*(k*7+1);++k;}}
if(driver(seed)!=expected)throw std::runtime_error("late extracts or shared return-buffer reuse changed values");++checks;
expected=0;k=0;for(unsigned call=0;call<2;++call){uint64_t s=call?(seed^123456789):seed;
for(unsigned i=0;i<64;++i){auto add=[&](uint64_t value){expected+=value*(k*7+1);++k;};
switch(i%8){case 0:add(uint8_t(s));break;case 1:add(s+i*19+3);break;case 2:add(s&8191);break;
case 3:add(uint16_t(s));add(i);add(65535);break;case 4:add(uint32_t(s));add(i);break;
case 5:add(reinterpret_cast<uintptr_t>(&marker));break;case 6:add(s);break;case 7:add(s);add(1);break;}}}
if(packed_driver(seed,&marker)!=expected)throw std::runtime_error("packed/nested/float/pointer/65-bit return ABI changed values");++checks;
}std::printf("PASS: %u large-return checks; delayed extracts, shared scratch, resize, packed offsets, nested arrays/structs, pointers, float bits and 65-bit values\n",checks);}
''')


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


shutil.copyfile(w / 'input.ll', w / 'converted.ll')
run(sys.executable, SIM / 'compiler/llvm.py', w / 'converted.ll', '--llvm-opt',
    sdk / 'opt', '--cache', SIM / 'generated-src/compiler', '--cxx',
    os.environ.get('CXX', 'c++'))
text = (w / 'converted.ll').read_text()
assert 'define internal void @small(' in text and 'define internal void @big(' in text and 'define internal void @packed(' in text
assert 'define internal void @stored_return(' in text
for name in ['escaped', 'self_address', 'external_api']:
    assert re.search(r'define (?:internal )?%Small @' + name + r'\(',
                     text), name
assert 'call void @small(' in text
run(sdk / 'opt', '-load-pass-plugin=' + str(library),
    '-passes=chipyard-buffer-large-returns,verify', '-S', w / 'converted.ll',
    '-o', w / 'twice.ll')
normalize = lambda s: re.sub(r'^; ModuleID.*\n', '', s)
assert normalize(text) == normalize(
    (w / 'twice.ll').read_text()), 'non-idempotent return conversion'
for source in ['input', 'converted']:
    for level in ['0', '2']:
        run(sdk / 'opt', '-O' + level, w / (source + '.ll'), '-o',
            w / (source + '-O' + level + '.bc'))
        run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj', w / (source + '-O' + level + '.bc'), '-o',
            w / (source + '-O' + level + '.o'))
        run(*cxx, '-O' + level, w / 'check.cpp',
            w / (source + '-O' + level + '.o'), '-o',
            w / (source + '-O' + level))
        run(w / (source + '-O' + level))
print(
    'PASS: public/address-taken ABIs retained, whole-aggregate return buffered; verified IR and idempotence',
    flush=True)

# A later failure must preserve the complete untransformed input and remove
# only transaction files owned by this invocation.
for name, suffix in [
    ('bad-wide',
     '\ndeclare i2049 @chipyard_arc_wide_shift_0_2048(i2049, i2049)\n'),
    ('bad-ir', '\nnot valid LLVM\n')
]:
    original = (w / 'input.ll').read_bytes() + suffix.encode()
    target = w / (name + '.ll')
    target.write_bytes(original)
    before = set(w.glob(target.name + '.*'))
    failed = subprocess.run([
        sys.executable, SIM / 'compiler/llvm.py', target, '--llvm-opt',
        sdk / 'opt', '--cache', SIM / 'generated-src/compiler', '--cxx',
        os.environ.get('CXX', 'c++')
    ],
                            capture_output=True)
    assert failed.returncode, name
    assert target.read_bytes() == original, name + ' changed after failure'
    assert set(w.glob(target.name +
                      '.*')) == before, name + ' leaked transaction files'
print(
    'PASS: native LLVM pipeline preserves input on invalid IR or arithmetic ABI failure',
    flush=True)
