#!/usr/bin/env python3
"""Execute fixed-array packed-element conversions across bounds and type domains."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

CASES = [
    ('aes', 1, 1, 128, 'logic', 'logic', '[3:0][3:0][7:0]', False),
    ('reversed', 5, 1, 128, 'logic signed', 'logic signed', '[0:3][3:0][0:7]',
     False),
    ('nested', 2, 3, 129, 'logic', 'logic', '[2:0][42:0]', True),
    ('struct', 3, 2, 65, 'logic', 'bit', 'struct', True),
    ('nested_singleton', 1, 4, 17, 'bit signed', 'logic signed', '[0:0][16:0]',
     True),
    ('one_bit', 5, 1, 1, 'logic', 'bit', '[0:0][0:0]', True),
    ('wide_struct', 2, 2, 257, 'logic signed', 'logic', 'struct', True),
    ('nested_reversed', 2, 1, 128, 'bit', 'logic', '[3:0][0:3][7:0]', True),
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-verilog', type=Path, required=True)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    sdk = args.circt_bin.resolve().parent
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    records = []
    # A failed recompilation must not leave a previous passing report behind.
    (work / 'report.json').write_text(
        json.dumps(dict(status='incomplete',
                        frontend=str(args.circt_verilog.resolve()),
                        test_source_sha256=digest(Path(__file__))),
                   indent=2) + '\n')

    def run(*command):
        subprocess.run([str(value) for value in command], check=True)

    libs = shlex.split(
        subprocess.check_output([sdk / 'bin/llvm-config', '--libs', 'support'],
                                text=True))
    for name, n, m, bits, flat_kind, shape_kind, shape, nested in CASES:
        case = work / name
        case.mkdir(exist_ok=True)
        width = n * m * bits * 2
        if shape == 'struct':
            low = bits // 2
            typedef = (
                f'typedef struct packed {{ {shape_kind} [{bits-low-1}:0] high; '
                f'{shape_kind} [{low-1}:0] low; }} shape_t;')
        else:
            typedef = f'typedef {shape_kind} {shape} shape_t;'
        tail = lambda left, right: f'[{left}:{right}]'
        flat_dims = tail(9, 10 - n) + (tail(19, 20 - m) if nested else '')
        shape_dims = tail(7, 6 + n) + (tail(21, 20 + m) if nested else '')
        input_dims = tail(-3, -4 + n) + (tail(4, 5 - m) if nested else '')
        plain_dims = tail(37, 38 - n) + (tail(6, 5 + m) if nested else '')
        source = [
            f'module Top(input logic [{width-1}:0] request, output logic [{width-1}:0] response);',
            typedef, f'typedef shape_t shape_array_t {shape_dims};',
            f'typedef {flat_kind} [{bits-1}:0] plain_array_t {plain_dims};',
            f'{flat_kind} [{bits-1}:0] flat {flat_dims};',
            f'shape_t shaped {shape_dims};',
            f'shape_t shape_input {input_dims};',
            f'{flat_kind} [{bits-1}:0] plain {plain_dims};',
            'always_comb begin'
        ]
        result_a, result_b = [], []
        for i in range(n):
            for j in range(m):
                index = i * m + j
                flat_index = f'[{9-i}]' + (f'[{19-j}]' if nested else '')
                shape_index = f'[{7+i}]' + (f'[{21+j}]' if nested else '')
                input_index = f'[{-3+i}]' + (f'[{4-j}]' if nested else '')
                plain_index = f'[{37-i}]' + (f'[{6+j}]' if nested else '')
                source += [
                    f'  flat{flat_index} = request[{width-1-index*bits} -: {bits}];',
                    f"  shape_input{input_index} = shape_t'(request[{width//2-1-index*bits} -: {bits}]);"
                ]
                result_a.append('shaped' + shape_index)
                result_b.append('plain' + plain_index)
        explicit = flat_kind.split()[0] != shape_kind.split(
        )[0] or shape == 'struct'
        source += [
            "  shaped = shape_array_t'(flat);" if explicit else
            '  shaped = flat;', "  plain = plain_array_t'(shape_input);"
            if explicit else '  plain = shape_input;',
            '  response = {' + ', '.join(result_a + result_b) + '};', 'end',
            'endmodule'
        ]
        sv = case / 'source.sv'
        sv.write_text('\n'.join(source) + '\n')
        hw, flat, ll, layout = [
            case / ('model.' + ext)
            for ext in ('mlir', 'flat.mlir', 'll', 'json')
        ]
        run(args.circt_verilog, '--ir-hw', '--infer-clock-ports=false',
            '--top=Top', sv, '-o', hw)
        run(sdk / 'bin/circt-opt', hw, '--hw-flatten-modules',
            '--llhd-sig2reg', '--canonicalize', '--symbol-dce', '-o', flat)
        run(sdk / 'bin/arcilator', flat, '--state-file=' + str(layout), '-o',
            ll)
        model, = json.loads(layout.read_text())
        states = {state['name']: state for state in model['states']}
        assert states['request']['numBits'] == states['response'][
            'numBits'] == width
        cpp = case / 'test.cpp'
        cpp.write_text(f'''#include <cstdint>
#include <cstring>
#include <cstdio>
extern "C" void Top_eval(void *);
int main() {{
 alignas(64) unsigned char state[{model['numStateBytes']}] = {{}};
 uint64_t rng=0x92a4bfed786531ULL;
 auto *input=state+{states['request']['offset']};
 auto *output=state+{states['response']['offset']};
 for(unsigned vector=0;vector<1024;++vector) {{
  for(unsigned byte=0;byte<{(width+7)//8};++byte) {{
   rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
   input[byte]=vector==0?0:vector==1?255:rng;
  }}
  if(vector>=2 && vector<2+{width}) {{
   std::memset(input,0,{(width+7)//8});
   input[(vector-2)/8]=1u<<((vector-2)%8);
  }}
  input[{(width-1)//8}]&={((1<<(width%8))-1) if width%8 else 255};
  Top_eval(state);
  for(unsigned bit=0;bit<{width};++bit)
   if(((input[bit/8]^output[bit/8])>>(bit%8))&1) {{
    std::fprintf(stderr,"{name}: vector %u bit %u packed-array conversion mismatch\\n",vector,bit);
    return 1;
   }}
 }}
 std::puts("PASS: 1024 bidirectional {name} packed-array reshape vectors");
}}
''')
        record = dict(case=name,
                      vectors=1024,
                      packed_width=bits,
                      dimensions=[n, m] if nested else [n],
                      source_sha256=digest(sv),
                      levels=[])
        for level in ('0', '2'):
            obj, executable = case / ('model-O' + level +
                                      '.o'), case / ('test-O' + level)
            run(sdk / 'bin/opt', '-O' + level, ll, '-o',
                case / ('model-O' + level + '.bc'))
            run(sdk / 'bin/llc', '-O' + level, '-relocation-model=pic',
                '-filetype=obj', case / ('model-O' + level + '.bc'), '-o', obj)
            run(*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', cpp,
                obj, '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', *libs,
                '-lz', '-lpthread', '-ldl', '-o', executable)
            result = subprocess.run([executable],
                                    capture_output=True,
                                    text=True)
            (case / ('run-O' + level + '.log')).write_text(result.stdout +
                                                           result.stderr)
            print(result.stdout + result.stderr, end='', flush=True)
            result.check_returncode()
            record['levels'].append(
                dict(optimization=level,
                     status='passed',
                     executable_sha256=digest(executable),
                     llvm_sha256=digest(ll)))
        records.append(record)
    report = dict(
        status='passed',
        frontend=str(args.circt_verilog.resolve()),
        frontend_sha256=digest(args.circt_verilog),
        test_source_sha256=digest(Path(__file__)),
        cases=records,
        vectors=1024 * len(records) * 2,
        scope=
        'equal-width packed leaf conversions in fixed unpacked arrays; width-changing conversions excluded'
    )
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"PASS: {report['vectors']} packed-array reshape vectors at O0/O2",
          flush=True)


if __name__ == '__main__':
    main()
