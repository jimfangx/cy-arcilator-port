#!/usr/bin/env python3
"""Verify few-argument wide LLVM values, aggregates and constrained ABIs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM / 'compiler'))
from codegen import compile_model


def check(sdk, cxx, work):
    work.mkdir(parents=True, exist_ok=True)
    layout = next(line for line in subprocess.check_output(
        [sdk / 'opt', '-S', '-o', '-', '-'],
        input='target triple = "x86_64-unknown-linux-gnu"\n', text=True).splitlines()
        if line.startswith('target datalayout'))
    lines = [layout, 'target triple = "x86_64-unknown-linux-gnu"']
    widths = (4099, 8207, 32771)
    for bits in widths:
        size = (bits + 7) // 8
        lines += [f'''
define internal i{bits} @wide{bits}(ptr %source, i64 %seed, ptr %effects) noinline {{
entry:
  %input = load i{bits}, ptr %source, align 1
  %wseed = zext i64 %seed to i{bits}
  %hi = shl i{bits} %wseed, {bits - 64}
  %mask = or i{bits} %wseed, %hi
  %value = xor i{bits} %input, %mask
  %old = load i64, ptr %effects
  %next = add i64 %old, 1
  store i64 %next, ptr %effects
  %branch = and i64 %seed, 1
  %odd = icmp ne i64 %branch, 0
  br i1 %odd, label %left, label %right
left:
  br label %merge
right:
  br label %merge
merge:
  %result = phi i{bits} [ %value, %left ], [ %input, %right ]
  ret i{bits} %result
}}
define internal void @args{bits}(ptr %out, i{bits} %a, i{bits} %b) noinline {{
  %mixed = xor i{bits} %a, %b
  store i{bits} %mixed, ptr %out, align 1
  ret void
}}
define void @driver{bits}(ptr %out, ptr %source, i64 %seed, ptr %effects) {{
  %a = call i{bits} @wide{bits}(ptr %source, i64 %seed, ptr %effects)
  %other = xor i64 %seed, 123456789
  %b = call i{bits} @wide{bits}(ptr %source, i64 %other, ptr %effects)
  %second = getelementptr i8, ptr %out, i64 {size}
  %third = getelementptr i8, ptr %out, i64 {size * 2}
  call void @args{bits}(ptr %third, i{bits} %a, i{bits} %b)
  store i{bits} %a, ptr %out, align 1
  store i{bits} %b, ptr %second, align 1
  ret void
}}
''']
    # Both a whole-array consumer and a PHI struct result exercise the generic
    # load/store path, while a few-field tuple tests the flattened path.
    lines += ['''
%Few = type <{i13, i4099, [3 x i16]}>
define internal [129 x i64] @array(ptr %source) noinline {
  %a = load [129 x i64], ptr %source, align 1
  ret [129 x i64] %a
}
define internal %Few @tuple(ptr %source, i13 %tag) noinline {
  %a = load i4099, ptr %source, align 1
  %t0 = insertvalue %Few poison, i13 %tag, 0
  %t1 = insertvalue %Few %t0, i4099 %a, 1
  %t2 = insertvalue %Few %t1, [3 x i16] [i16 3, i16 5, i16 7], 2
  ret %Few %t2
}
define internal %Few @tuple_phi(ptr %source, i13 %tag) noinline {
entry:
  %a = call %Few @tuple(ptr %source, i13 %tag)
  %b = insertvalue %Few %a, i13 17, 0
  %cmp = icmp eq i13 %tag, 0
  br i1 %cmp, label %left, label %right
left:
  br label %end
right:
  br label %end
end:
  %result = phi %Few [ %a, %left ], [ %b, %right ]
  ret %Few %result
}
define i64 @aggregate_driver(ptr %out, ptr %source, i13 %tag) {
  %a = call [129 x i64] @array(ptr %source)
  %b = call [129 x i64] @array(ptr %source)
  %t = call %Few @tuple_phi(ptr %source, i13 %tag)
  store [129 x i64] %a, ptr %out, align 1
  %next = getelementptr i8, ptr %out, i64 1032
  store [129 x i64] %b, ptr %next, align 1
  %v = extractvalue %Few %t, 1
  %third = getelementptr i8, ptr %out, i64 2064
  store i4099 %v, ptr %third, align 1
  %tag0 = extractvalue %Few %t, 0
  %tag1 = zext i13 %tag0 to i64
  %last = extractvalue %Few %t, 2, 2
  %last64 = zext i16 %last to i64
  %sum = add i64 %tag1, %last64
  ret i64 %sum
}
''']
    runtime_lines = list(lines)
    # These are verifier-only signature constraints. Stock X86 cannot lower
    # the wide musttail signature at O0, even without either buffering pass.
    lines += ['''
define internal i4099 @tail_target(i4099 %value) { ret i4099 %value }
define internal i4099 @tail_forward(i4099 %value) {
  %r = musttail call i4099 @tail_target(i4099 %value)
  ret i4099 %r
}
define internal i4099 @inlined(i4099 %value) alwaysinline { ret i4099 %value }
define i4099 @public_api(i4099 %value) { ret i4099 %value }
define internal i4099 @escaped(i4099 %value) { ret i4099 %value }
@address = global ptr @escaped
define internal i4099 @self_address(i4099 %value, ptr %self) noinline { ret i4099 %value }
define i4099 @self_driver(i4099 %value) {
  %r = call i4099 @self_address(i4099 %value, ptr @self_address)
  ret i4099 %r
}
define internal i4099 @bycopy(ptr byval(i64) %copy) {
  %v = load i64, ptr %copy
  %r = zext i64 %v to i4099
  ret i4099 %r
}
''']
    source = work / 'input.ll'
    source.write_text('\n'.join(lines) + '\n')
    converted = work / 'converted.ll'
    converted.write_bytes(source.read_bytes())
    command = [sys.executable, str(SIM / 'compiler/llvm.py'), str(converted),
               '--llvm-opt', str(sdk / 'opt'), '--cache', str(SIM / 'generated-src/compiler'),
               '--cxx', cxx]
    subprocess.run(command, check=True)
    text = converted.read_text()
    for bits in widths:
        assert re.search(r'define internal void @wide' + str(bits) + r'\(', text)
        assert re.search(r'define internal void @args' + str(bits) + r'\(ptr ', text)
    for name in ('array', 'tuple', 'tuple_phi'):
        assert f'define internal void @{name}(' in text, name
    for name in ('tail_target', 'tail_forward', 'inlined', 'public_api', 'escaped', 'self_address', 'bycopy'):
        assert re.search(r'define (?:internal )?i4099 @' + name + r'\(', text), name
    assert 'musttail call i4099 @tail_target' in text
    # Running both passes twice is a fixed point, including generic results.
    first = converted.read_bytes()
    subprocess.run(command, check=True)
    assert converted.read_bytes() == first, 'ABI lowering is not idempotent'
    runtime = work / 'runtime.ll'
    runtime.write_text('\n'.join(runtime_lines) + '\n')
    subprocess.run([*command[:2], str(runtime), *command[3:]], check=True)
    declarations = '\n'.join(f'extern "C" void driver{b}(void*,void*,uint64_t,uint64_t*);' for b in widths)
    cpp = work / 'check.cpp'
    cpp.write_text('''#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <algorithm>
''' + declarations + r'''
extern "C" uint64_t aggregate_driver(void*,void*,uint16_t);
int main() {
  uint64_t seed=123456789;
  unsigned long checks=0;
  for (unsigned iteration=0;iteration<64;++iteration) {
    seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
    std::vector<uint8_t> source(8192);
    for (unsigned i=0;i<source.size();++i) source[i]=uint8_t((seed>>(i%8)*8)^i*37);
    for (unsigned bits : {4099u,8207u,32771u}) {
      unsigned size=(bits+7)/8;
      std::vector<uint8_t> out(size*3+16,0xa5),expected(size*3);
      uint64_t effects=iteration;
      if (bits==4099) driver4099(out.data(),source.data(),seed,&effects);
      if (bits==8207) driver8207(out.data(),source.data(),seed,&effects);
      if (bits==32771) driver32771(out.data(),source.data(),seed,&effects);
      if (effects!=iteration+2) throw std::runtime_error("effect ordering/count");
      for (unsigned call=0;call<2;++call) {
        uint64_t s=call?(seed^123456789):seed;
        std::copy_n(source.begin(),size,expected.begin()+call*size);
        if (s&1) for (unsigned bit=0;bit<64;++bit) if ((s>>bit)&1) {
          expected[call*size+bit/8]^=1u<<(bit%8);
          unsigned hi=bits-64+bit;
          expected[call*size+hi/8]^=1u<<(hi%8);
        }
      }
      for(unsigned i=0;i<size;++i) expected[size*2+i]=expected[i]^expected[size+i];
      for(unsigned call=0;call<3;++call) for(unsigned i=0;i<size;++i) {
        unsigned mask=i+1==size?((1u<<(bits%8))-1):255;
        if ((out[call*size+i]&mask)!=(expected[call*size+i]&mask))
          throw std::runtime_error("wide result/arguments or scratch reuse");
        ++checks;
      }
      for(unsigned i=size*3;i<out.size();++i) if(out[i]!=0xa5)
        throw std::runtime_error("write beyond result size");
    }
    std::vector<uint8_t> output(2064+513+16,0xa5);
    uint16_t tag=iteration%2?uint16_t(seed&8191):0;
    if(aggregate_driver(output.data(),source.data(),tag)!=(tag?24:7))
      throw std::runtime_error("PHI/packed struct result");
    for(unsigned i=0;i<2064;++i) {
      if(output[i]!=source[i%1032]) throw std::runtime_error("whole-array result");
      ++checks;
    }
    for(unsigned i=0;i<513;++i) {
      unsigned mask=i==512?7:255;
      if((output[2064+i]&mask)!=(source[i]&mask)) throw std::runtime_error("few-field wide tuple");
      ++checks;
    }
    for(unsigned i=2577;i<output.size();++i) if(output[i]!=0xa5)
      throw std::runtime_error("aggregate output bounds");
  }
  std::printf("PASS: %lu byte comparisons; wide values, delayed captures, PHIs, arrays, packed tuples, effects and output bounds\n",checks);
}
''')
    results = []
    for level in ('0', '2', '3'):
        obj = work / ('model-O' + level + '.o')
        compile_model(runtime, obj, sdk / 'opt', sdk / 'llc',
                      SIM / 'generated-src/compiler', cxx, level, level)
        optimized = subprocess.check_output([sdk / 'llvm-dis', obj.with_suffix('.bc'), '-o', '-'], text=True)
        for line in optimized.splitlines():
            if line.startswith('define internal '):
                assert all(int(bits) < 4096 for bits in re.findall(r'\bi(\d+)\b', line)), line
        executable = work / ('check-O' + level)
        subprocess.run([*shlex.split(cxx), '-O2', str(cpp), str(obj), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], check=True, capture_output=True, text=True)
        print(result.stdout, end='', flush=True)
        results.append(dict(level=level, result=result.stdout.strip(),
                            object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest()))
    (work / 'report.json').write_text(json.dumps(dict(status='passed', widths=widths,
        codegen_levels=results, original_constrained_ABIs_preserved=True,
        idempotent=True), indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--work', type=Path, default=SIM / 'generated-src/large-value-abi')
    args = parser.parse_args()
    if args.circt_bin is None:
        parser.error('set CIRCT_BIN or supply --circt-bin')
    check(args.circt_bin, os.environ.get('CXX', 'c++'), args.work)
