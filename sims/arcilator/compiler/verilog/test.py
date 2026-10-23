from pathlib import Path
from itertools import product
import json
import subprocess
import argparse

parser = argparse.ArgumentParser(
    description='Check interface array imports through LLVM execution.')
parser.add_argument('--circt-verilog', type=Path, required=True)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work', type=Path, required=True)
args = parser.parse_args()
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
tool = args.circt_verilog.resolve()
sdk = args.circt_bin.resolve().parent


def run(*args):
    result = subprocess.run([str(x) for x in args],
                            text=True,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(' '.join(map(str, args)) + '\n' +
                           result.stdout[-6000:])
    return result.stdout


def indices(dims):
    return list(
        product(*(range(a, b + (1 if b >= a else -1), 1 if b >= a else -1)
                  for a, b in dims))) or [()]


def suffix(index):
    return ''.join(f'[{i}]' for i in index)


def make_case(name,
              portdims,
              actualdims,
              modport=True,
              forward=False,
              siblings=1):
    pi, ai = indices(portdims), indices(actualdims)
    assert len(pi) == len(ai)
    n = len(pi)
    qualifier = 'Pipe.consumer' if modport else 'Pipe'
    dims = lambda ds: ''.join(f'[{a}:{b}]' for a, b in ds)
    lines = [
        'interface Pipe;', ' logic [3:0] request,response;',
        ' modport consumer(input request,output response);', 'endinterface',
        f'module Child({qualifier} bus{dims(portdims)});'
    ]
    for i, p in enumerate(pi):
        lines.append(
            f" assign bus{suffix(p)}.response = bus{suffix(p)}.request ^ 4'd{(i*3+1)&15};"
        )
    lines += ['endmodule']
    if forward:
        lines += [
            f'module Forward({qualifier} bus{dims(portdims)});',
            ' Child nested(.bus(bus));', 'endmodule'
        ]
    bits = n * 4 * siblings
    lines += [
        f'module Top(input logic [{bits-1}:0] request, output logic [{bits-1}:0] response);'
    ]
    for sib in range(siblings):
        lines.append(f' Pipe buses{sib}{dims(actualdims)}();')
        for i, a in enumerate(ai):
            lo = (sib * n + i) * 4
            lines += [
                f' assign buses{sib}{suffix(a)}.request = request[{lo}+:4];',
                f' assign response[{lo}+:4] = buses{sib}{suffix(a)}.response;'
            ]
        lines += [
            f' {"Forward" if forward else "Child"} child{sib}(.bus(buses{sib}));'
        ]
    lines += ['endmodule']
    source = work / (name + '.sv')
    source.write_text('\n'.join(lines) + '\n')
    imported = work / (name + '.mlir')
    run(tool, '--import-only', '--top=Top', source, '-o', imported)
    # Compare actual numerical behavior with Python, through the full HW/Arc
    # lowering and LLVM generated evaluator (no reference simulator).
    hw = work / (name + '.hw.mlir')
    run(tool, '--ir-hw', '--infer-clock-ports=false', '--top=Top', source,
        '-o', hw)
    flat = work / (name + '.flat.mlir')
    run(sdk / 'bin/circt-opt', hw, '--hw-flatten-modules', '--llhd-sig2reg',
        '--canonicalize', '--symbol-dce', '-o', flat)
    ll = work / (name + '.ll')
    layout = work / (name + '.json')
    run(sdk / 'bin/arcilator', flat, '--state-file=' + str(layout), '-o', ll)
    model, = json.loads(layout.read_text())
    states = {s['name']: s for s in model['states']}
    obj = work / (name + '.o')
    run(sdk / 'bin/llc', '-O2', '-relocation-model=pic', '-filetype=obj', ll,
        '-o', obj)
    # <=64 data bits keeps the independent evaluator simple.
    assert bits <= 64
    xor = sum(((i * 3 + 1) & 15) << (4 * (sib * n + i))
              for sib in range(siblings) for i in range(n))
    inp, out = states['request'], states['response']
    cpp = work / (name + '.cpp')
    exe = work / name
    cpp.write_text(f'''#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
extern "C" void Top_eval(void *);
int main() {{
 alignas(64) unsigned char state[{model['numStateBytes']}] = {{}};
 uint64_t seed = 123456789;
 for(unsigned i=0;i<1000;++i) {{
  seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
  uint64_t input=seed & UINT64_C({(1<<bits)-1});
  std::memcpy(state+{inp['offset']}, &input, {(bits+7)//8});
  Top_eval(state);
  uint64_t output=0;
  std::memcpy(&output, state+{out['offset']}, {(bits+7)//8});
  if(output != (input ^ UINT64_C({xor}))) {{
   std::fprintf(stderr,"{name}: vector %u input %llx output %llx expected %llx\\n",i,(unsigned long long)input,(unsigned long long)output,(unsigned long long)(input ^ UINT64_C({xor}))); return 1;
  }}
 }}
 std::puts("PASS: {name}, 1000 independent input vectors");
}}
''')
    run('c++', '-std=c++17', cpp, obj, '-L' + str(sdk / 'lib'),
        '-lCIRCTArcRuntime',
        *run(sdk / 'bin/llvm-config', '--libs',
             'support').split(), '-lz', '-lpthread', '-ldl', '-o', exe)
    print(run(exe).strip(), flush=True)


cases = [('scalar', [], [], True, False, 1),
         ('singleton', [(0, 0)], [(0, 0)], True, False, 1),
         ('pair', [(0, 1)], [(0, 1)], True, False, 1),
         ('rebased', [(5, 3)], [(0, 2)], True, False, 1),
         ('multidimensional', [(1, 0), (-1, 0)], [(3, 4),
                                                  (7, 6)], True, False, 1),
         ('forwarded', [(1, 0)], [(0, 1)], True, True, 1),
         ('siblings', [(0, 1)], [(2, 1)], True, False, 2),
         ('inout', [(0, 2)], [(5, 3)], False, True, 1)]
for case in cases:
    make_case(*case)
print(run('python3', Path(__file__).with_name('port_connections_test.py'),
          '--circt-verilog', tool, '--circt-bin', args.circt_bin,
          '--work', work / 'port-connections').strip(), flush=True)
