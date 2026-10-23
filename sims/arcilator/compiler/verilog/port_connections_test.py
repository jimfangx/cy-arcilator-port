#!/usr/bin/env python3
"""Numerically check signed and unsigned SV instance-port conversions."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-verilog', type=Path, required=True)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work', type=Path, required=True)
args = parser.parse_args()
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
sdk = args.circt_bin.resolve().parent


def run(*command):
    subprocess.run([str(x) for x in command], check=True)


source = work / 'ports.sv'
source.write_text('''module PortSink(input [8:0] value, output [8:0] observed);
assign observed = value;
endmodule
module PortProducer(input [4:0] value, output signed [4:0] signed_out, output [4:0] unsigned_out);
assign signed_out = value;
assign unsigned_out = value;
endmodule
module Top(input [31:0] request, output [31:0] response);
wire signed [4:0] signed_input = request[4:0];
wire [8:0] expanded_input, expanded_signed, expanded_unsigned;
PortSink input_conversion(.value(signed_input), .observed(expanded_input));
PortProducer output_conversion(.value(request[4:0]), .signed_out(expanded_signed), .unsigned_out(expanded_unsigned));
assign response = {5'd0, expanded_input, expanded_unsigned, expanded_signed};
endmodule
''')
hw, flat, ll, layout, obj = [
    work / ('ports.' + x) for x in ('mlir', 'flat.mlir', 'll', 'json', 'o')
]
run(args.circt_verilog, '--ir-hw', '--infer-clock-ports=false', '--top=Top',
    source, '-o', hw)
run(sdk / 'bin/circt-opt', hw, '--hw-flatten-modules', '--llhd-sig2reg',
    '--canonicalize', '--symbol-dce', '-o', flat)
run(sdk / 'bin/arcilator', flat, '--state-file=' + str(layout), '-o', ll)
model, = json.loads(layout.read_text())
states = {s['name']: s for s in model['states']}
run(sdk / 'bin/llc', '-O2', '-relocation-model=pic', '-filetype=obj', ll, '-o',
    obj)
cpp = work / 'test.cpp'
cpp.write_text(f'''#include <cstdint>
#include <cstring>
#include <cstdio>
extern "C" void Top_eval(void *);
int main() {{
 alignas(64) unsigned char state[{model['numStateBytes']}] = {{}};
 uint32_t random = 0x12345678;
 for (unsigned i=0;i<1000;++i) {{
  random ^= random<<13; random ^= random>>17; random ^= random<<5;
  uint32_t input = i < 32 ? i : random;
  std::memcpy(state+{states['request']['offset']}, &input, 4);
  Top_eval(state);
  uint32_t output;
  std::memcpy(&output, state+{states['response']['offset']}, 4);
  uint32_t raw=input&31, sign=raw&16 ? raw|0x1e0 : raw;
  uint32_t expected=sign | (raw<<9) | (sign<<18);
  if (output != expected) {{
   std::fprintf(stderr,"port conversion vector %u: actual %08x expected %08x\\n",i,output,expected);
   return 1;
  }}
 }}
 std::puts("PASS: 1000 independent signed/unsigned instance-port conversion vectors");
}}
''')
libs = subprocess.check_output([sdk / 'bin/llvm-config', '--libs', 'support'],
                               text=True).split()
run('c++', '-std=c++17', cpp, obj, '-L' + str(sdk / 'lib'),
    '-lCIRCTArcRuntime', *libs, '-lz', '-lpthread', '-ldl', '-o',
    work / 'test')
run(work / 'test')
