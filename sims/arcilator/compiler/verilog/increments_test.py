#!/usr/bin/env python3
"""Check increment/decrement values and side effects through LLVM execution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-verilog', type=Path, required=True)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work', type=Path, required=True)
args = parser.parse_args()
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
sdk = args.circt_bin.resolve().parent
types = [('logic', 1), ('logic signed', 1), ('bit', 1), ('bit signed', 1),
         ('logic', 2), ('logic signed', 2), ('logic', 7), ('logic signed', 7)]
width = sum(8 * bits for _, bits in types)
lines = [
    f'module Top(input logic [15:0] request, output logic [{width-1}:0] response);'
]
offset = 0
results = []
for index, (kind, bits) in enumerate(types):
    for op, expression in enumerate(
        ('++value', 'value++', '--value', 'value--')):
        result = f'result_{index}_{op}'
        results.append(result)
        lines += [
            f'logic [{2*bits-1}:0] {result};',
            f'always_comb begin : case_{index}_{op}',
            f'  automatic {kind} [{bits-1}:0] value;',
            f'  automatic {kind} [{bits-1}:0] returned;',
            f'  value = request[{bits-1}:0];', "  returned = '1;",
            f'  if(request[8]) returned = {expression};',
            f'  {result} = {{returned, value}};', 'end'
        ]
        offset += 2 * bits
lines += [
    'assign response = {' + ', '.join(reversed(results)) + '};', 'endmodule'
]
source = work / 'increments.sv'
source.write_text('\n'.join(lines) + '\n')


def run(*command):
    subprocess.run([str(x) for x in command], check=True)


hw, flat, ll, layout, obj = [
    work / ('increments.' + suffix)
    for suffix in ('mlir', 'flat.mlir', 'll', 'json', 'o')
]
run(args.circt_verilog, '--ir-hw', '--infer-clock-ports=false', '--top=Top',
    source, '-o', hw)
run(sdk / 'bin/circt-opt', hw, '--hw-flatten-modules', '--llhd-sig2reg',
    '--canonicalize', '--symbol-dce', '-o', flat)
run(sdk / 'bin/arcilator', flat, '--state-file=' + str(layout), '-o', ll)
model, = json.loads(layout.read_text())
states = {s['name']: s for s in model['states']}
assert states['response']['numBits'] == width
run(sdk / 'bin/llc', '-O2', '-relocation-model=pic', '-filetype=obj', ll, '-o',
    obj)
cpp = work / 'test.cpp'
cpp.write_text(f'''#include <cstdint>
#include <cstring>
#include <cstdio>
#include <initializer_list>
extern "C" void Top_eval(void *);
int main() {{
 alignas(64) unsigned char state[{model['numStateBytes']}] = {{}};
 unsigned checks = 0;
 for(unsigned vector=0; vector<512; ++vector) {{
  uint16_t request=vector;
  std::memcpy(state+{states['request']['offset']}, &request, 2);
  Top_eval(state);
  const auto *out=state+{states['response']['offset']};
  unsigned offset=0;
  for(unsigned bits : {{1u,1u,1u,1u,2u,2u,7u,7u}}) {{
   const unsigned mask=(1u<<bits)-1, original=request&mask;
   for(unsigned op=0;op<4;++op) {{
    unsigned next=(original+(op<2 ? 1u : mask))&mask;
    unsigned value=(request&256) ? next : original;
    unsigned returned=(request&256) ? (op%2 ? original : next) : mask;
    unsigned expected=value|(returned<<bits), actual=0;
    for(unsigned bit=0;bit<2*bits;++bit)
     actual|=unsigned((out[(offset+bit)/8]>>((offset+bit)%8))&1)<<bit;
    if(actual!=expected) {{
     std::fprintf(stderr,"vector %u width %u op %u: actual %x expected %x\\n",
                  vector,bits,op,actual,expected);
     return 1;
    }}
    offset+=2*bits;
    ++checks;
   }}
  }}
 }}
 std::printf("PASS: %u increment/decrement comparisons, pre/post values, "
             "side effects, conditional execution, signed/unsigned bit/logic\\n",checks);
}}
''')
libs = shlex.split(
    subprocess.check_output([sdk / 'bin/llvm-config', '--libs', 'support'],
                            text=True))
run(*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', cpp, obj,
    '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz', '-lpthread',
    '-ldl', '-o', work / 'test')
# Preserve a useful numerical failure for comparisons with an older importer.
result = subprocess.run([work / 'test'],
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
(work / 'run.log').write_text(result.stdout)
report = dict(status='passed' if result.returncode == 0 else 'failed',
              comparisons=16384,
              frontend=str(args.circt_verilog.resolve()),
              sha256={
                  str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (Path(__file__), args.circt_verilog, source, cpp,
                            ll, obj, work / 'test')
              })
(work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(result.stdout, end='')
result.check_returncode()
