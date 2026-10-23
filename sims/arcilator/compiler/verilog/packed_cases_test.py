from pathlib import Path
import argparse, subprocess, json, os, shlex

p = argparse.ArgumentParser(
    description=
    'Execute packed struct/array/union case comparisons and wildcard matching.'
)
p.add_argument('--circt-verilog', type=Path, required=True)
p.add_argument('--circt-bin', type=Path, required=True)
p.add_argument('--work', type=Path, required=True)
p.add_argument('--union-library', type=Path, required=True)
a = p.parse_args()
w = a.work
w.mkdir(parents=True, exist_ok=True)
sdk = a.circt_bin.resolve()
(w / 'report.json').write_text(json.dumps(dict(status='incomplete')) + '\n')
(w / 'source.sv').write_text(
    '''module Top(input logic[11:0] request,output logic[31:0] response);
typedef struct packed {logic[1:0] rw;logic[1:0] privilege;logic[7:0] address;} csr_t;
typedef union packed {bit[11:0] raw;logic[2:0][3:0] nibbles;} union_t;
csr_t csr;
logic[1:0][1:0][2:0] grid;
union_t united;
logic[3:0] normal_result,x_result,z_result,union_result,scalar_result;
always_comb begin
 csr=csr_t'(request);
 grid=request;
 united.raw=request;
 case(csr)
  12'h008,12'hfff:normal_result=7;
  csr_t'(12'h350):normal_result=9;
  default:normal_result=0;
 endcase
 casex(grid)
  12'b1010_xxxx_zzzz:x_result=5;
  default:x_result=0;
 endcase
 casez(csr)
  12'b?111_1010_zzzz:z_result=6;
  default:z_result=0;
 endcase
 case(united)
  12'habc:union_result=13;
  default:union_result=0;
 endcase
 case(request[3:0])
  1:scalar_result=3;
  2:scalar_result=5;
  default:scalar_result=0;
 endcase
 response={12'd0,scalar_result,normal_result,x_result,z_result,union_result};
end
endmodule
''')


def run(*cmd):
    subprocess.run(list(map(str, cmd)), check=True)


run(a.circt_verilog, '--ir-hw', '--infer-clock-ports=false', '--top=Top',
    w / 'source.sv', '-o', w / 'hw.mlir')
run(
    sdk / 'circt-opt', w / 'hw.mlir',
    '--load-pass-plugin=' + str(a.union_library),
    '--pass-pipeline=builtin.module(chipyard-lower-unions,hw-flatten-modules,hw.module(llhd-sig2reg),canonicalize,symbol-dce)',
    '-o', w / 'flat.mlir')
run(sdk / 'arcilator', w / 'flat.mlir',
    '--state-file=' + str(w / 'model.json'), '-o', w / 'model.ll')
m, = json.loads((w / 'model.json').read_text())
s = {q['name']: q for q in m['states']}
(w / 'test.cpp').write_text(f'''#include <cstdint>
#include <cstring>
#include <cstdio>
extern "C" void Top_eval(void*);
int main(){{alignas(64) unsigned char state[{m['numStateBytes']}]={{}};
for(uint16_t i=0;i<4096;++i){{std::memcpy(state+{s['request']['offset']},&i,2);Top_eval(state);uint32_t actual;std::memcpy(&actual,state+{s['response']['offset']},4);
unsigned expected=(i==0xabc?13:0)|(((i&0x7f0)==0x7a0?6:0)<<4)|(((i&0xf00)==0xa00?5:0)<<8)|((i==8||i==4095?7:i==0x350?9:0)<<12)|((i%16==1?3:i%16==2?5:0)<<16);
if(actual!=expected){{std::fprintf(stderr,"packed case %u actual %x expected %x\\n",i,actual,expected);return 1;}}}}
std::puts("PASS: 4096 packed struct, array, union, casex/casez and scalar case vectors");}}
''')
libs = shlex.split(
    subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                            text=True))
for level in (0, 2):
    run(sdk / 'llc', f'-O{level}', '-relocation-model=pic', '-filetype=obj',
        w / 'model.ll', '-o', w / f'model-O{level}.o')
    run(os.environ.get('CXX', 'c++'), '-std=c++17', w / 'test.cpp',
        w / f'model-O{level}.o', '-L' + str(sdk.parent / 'lib'),
        '-lCIRCTArcRuntime', *libs, '-lz', '-lpthread', '-ldl', '-o',
        w / f'test-O{level}')
    run(w / f'test-O{level}')

(w / 'report.json').write_text(
    json.dumps(dict(status='passed',
                    comparisons=8192,
                    frontend=str(a.circt_verilog.resolve()),
                    cases=[
                        'packed struct', 'packed array',
                        'packed union with bit/logic views', 'casex', 'casez',
                        'scalar control'
                    ]),
               indent=2) + '\n')
