#!/usr/bin/env python3
"""Check optional private ABIs and stock/bounded LLVM codegen numerically."""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]


def fixture():
    fields = ', '.join(['i64'] * 72)
    formals = ', '.join(f'i64 %x{i}' for i in range(64))
    lines = [
        f'%R = type {{ {fields} }}',
        f'define internal %R @helper({formals}, i64 %seed, ptr %effects) noinline {{',
        '  %old = load i64, ptr %effects, align 8',
        '  %next = add i64 %old, 1', '  store i64 %next, ptr %effects, align 8'
    ]
    previous = 'poison'
    for i in range(72):
        constant = (i * 0x9e3779b97f4a7c15) & ((1 << 64) - 1)
        lines += [
            f'  %salt{i} = add i64 %seed, {constant}',
            f'  %mixed{i} = xor i64 %x{(i * 7 + 3) % 64}, %salt{i}',
            f'  %value{i} = add i64 %mixed{i}, %x{(i * 13 + 9) % 64}',
            f'  %r{i} = insertvalue %R {previous}, i64 %value{i}, {i}'
        ]
        previous = f'%r{i}'
    lines += [
        f'  ret %R {previous}', '}',
        'define void @mode_driver(ptr %out, ptr %input, i64 %seed, ptr %effects) {'
    ]
    for i in range(64):
        lines += [
            f'  %p{i} = getelementptr i64, ptr %input, i64 {i}',
            f'  %v{i} = load i64, ptr %p{i}, align 8'
        ]
    actuals = ', '.join(f'i64 %v{i}' for i in range(64))
    # Two direct uses keep this helper outside the single-use shared workspace.
    # Extract the first return after the second call to check scratch reuse.
    lines += [
        f'  %a = call %R @helper({actuals}, i64 %seed, ptr %effects)',
        '  %other = xor i64 %seed, 1311768467463790320',
        f'  %b = call %R @helper({actuals}, i64 %other, ptr %effects)'
    ]
    for call, value in enumerate(('a', 'b')):
        for i in range(72):
            index = call * 72 + i
            lines += [
                f'  %o{index} = getelementptr i64, ptr %out, i64 {index}',
                f'  %e{index} = extractvalue %R %{value}, {i}',
                f'  store i64 %e{index}, ptr %o{index}, align 8'
            ]
    return '\n'.join(lines + [
        '  ret void', '}', '''
define double @mode_float(double %a, double %b, double %c) {
  %product = fmul double %a, %b
  %result = fadd double %product, %c
  ret double %result
}
'''
    ])


CPP = r'''
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
extern "C" void mode_driver(uint64_t *, const uint64_t *, uint64_t, uint64_t *);
extern "C" double mode_float(double, double, double);
int main() {
  uint64_t random = 0x6275696c646d6f64ULL;
  unsigned checks = 0;
  for (unsigned trial = 0; trial < 128; ++trial) {
    std::array<uint64_t, 64> source;
    for (auto &x : source) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      x = random;
    }
    uint64_t seed = random ^ trial;
    for (bool alias : {false, true}) {
      std::array<uint64_t, 146> storage;
      storage.fill(0xa5a5a5a5a5a5a5a5ULL);
      if (alias) for (unsigned i = 0; i < 64; ++i) storage[i+1] = source[i];
      uint64_t effects = trial;
      mode_driver(storage.data()+1, alias ? storage.data()+1 : source.data(), seed, &effects);
      if (effects != trial+2) throw std::runtime_error("lost/reordered effects");
      if (storage.front() != 0xa5a5a5a5a5a5a5a5ULL ||
          storage.back() != 0xa5a5a5a5a5a5a5a5ULL)
        throw std::runtime_error("output bounds");
      for (unsigned call = 0; call < 2; ++call) {
        uint64_t salt = call ? seed ^ 0x123456789abcdef0ULL : seed;
        for (unsigned i = 0; i < 72; ++i) {
          uint64_t expected = (source[(i*7+3)%64] ^
              (salt + uint64_t(i)*0x9e3779b97f4a7c15ULL)) + source[(i*13+9)%64];
          if (storage[1+call*72+i] != expected)
            throw std::runtime_error("private argument/result ABI or scratch capture");
          ++checks;
        }
      }
    }
  }
  std::printf("PASS: %u result checks, aliasing, delayed captures, effects and bounds\n", checks);
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double subnormal = std::numeric_limits<double>::denorm_min();
  const std::array<std::array<double,3>,8> special = {{{0.,-0.,-0.},
      {-0.,1.,-0.},{1.+0x1p-27,1.-0x1p-27,-1.},
      {subnormal,1.,0.},{1e300,1e300,-inf},{inf,0.,1.},
      {nan,1.,2.},{-inf,2.,inf}}};
  for (unsigned i = 0; i < 4096; ++i) {
    std::array<double,3> values;
    if (i < special.size()) values = special[i];
    else for (auto &v : values) {
      random ^= random << 13; random ^= random >> 7; random ^= random << 17;
      std::memcpy(&v,&random,sizeof(v));
    }
    volatile double product = values[0]*values[1];
    volatile double rounded = product+values[2];
    double expected = rounded;
    double observed = mode_float(values[0],values[1],values[2]);
    if (std::isnan(expected) && std::isnan(observed)) continue;
    uint64_t a,b;
    std::memcpy(&a,&expected,sizeof(a));
    std::memcpy(&b,&observed,sizeof(b));
    if (a != b) throw std::runtime_error("FP rounding, contraction, signed zero or subnormal");
  }
  std::puts("PASS: 4096 FP cases, separate rounding, signed zero, subnormals, infinities and NaNs");
}
'''


def check(sdk, cxx, work, opt_flags=''):
    work.mkdir(parents=True, exist_ok=True)
    cache = SIM / 'generated-src/compiler'
    cpp = work / 'oracle.cpp'
    cpp.write_text(CPP)
    target = subprocess.check_output([sdk / 'llvm-config', '--host-target'],
                                     text=True).strip()
    target_line = 'target triple = ' + json.dumps(target)
    layout = next(line for line in subprocess.check_output(
        [sdk / 'opt', '-S', '-o', '-', '-'],
        input=target_line + '\n',
        text=True).splitlines() if line.startswith('target datalayout'))
    cases = []
    for pipeline, buffers, shared in [('bounded', True, True),
                                      ('bounded', False, True),
                                      ('stock', False, False),
                                      ('stock', True, True)]:
        for level in ('0', '3'):
            name = f'{pipeline}-buffers{int(buffers)}-O{level}'
            directory = work / name
            directory.mkdir(exist_ok=True)
            source = directory / 'model.ll'
            source.write_text(layout + '\n' + target_line + '\n' + fixture())
            # The pure stock path must not build or load custom LLVM plugins.
            selected_cxx = cxx if pipeline == 'bounded' or buffers else '/not-installed/c++'
            lower = [
                sys.executable, SIM / 'compiler/llvm.py', source,
                '--llvm-opt=' + str(sdk / 'opt'), '--cache=' + str(cache),
                '--cxx=' + selected_cxx,
                '--share-procedures' if shared else '--no-share-procedures'
            ]
            if not buffers:
                lower.append('--no-abi-buffers')
            subprocess.run(lower, check=True)
            signature = next(
                line for line in source.read_text().splitlines()
                if line.startswith('define internal ') and '@helper(' in line)
            expected = r'define internal void @helper\(ptr ' if buffers else r'define internal %R @helper\(i64 '
            assert re.match(expected, signature), signature
            obj = directory / 'model.o'
            report = directory / 'codegen-report.json'
            command = [
                sys.executable, SIM / 'compiler/codegen.py',
                '--input=' + str(source), '--output=' + str(obj),
                '--llvm-opt=' + str(sdk / 'opt'), '--llc=' + str(sdk / 'llc'),
                '--cache=' + str(cache), '--cxx=' + selected_cxx,
                '--opt-level=' + level, '--llc-opt-level=' + level, '--jobs=2',
                '--pipeline=' + pipeline, '--report=' + str(report)
            ]
            if not buffers:
                command.append('--no-abi-buffers')
            command.append('--opt-flags=' + opt_flags)
            subprocess.run(command, check=True)
            info = json.loads(report.read_text())
            assert info['pipeline'] == pipeline and info[
                'abi_buffers'] == buffers
            assert info['requested_jobs'] == 2
            assert info['opt_flags'] == shlex.split(opt_flags)
            assert info['jobs'] == (1 if pipeline == 'stock' else 2)
            executable = directory / 'check'
            subprocess.run([
                *shlex.split(cxx), '-O2', '-std=c++17', cpp, obj, '-o',
                executable
            ],
                           check=True)
            result = subprocess.check_output([executable], text=True)
            assert result.startswith('PASS: 36864 result checks'), result
            assert 'PASS: 4096 FP cases' in result
            cases.append(dict(name=name, checks=36864, result=result.strip()))

    # Both pipelines preserve the last complete outputs when llc fails.
    tiny = work / 'tiny.ll'
    tiny.write_text('define i64 @tiny(i64 %x) { ret i64 %x }\n')
    for pipeline in ('bounded', 'stock'):
        obj, bitcode = work / (pipeline + '.o'), work / (pipeline + '.bc')
        obj.write_bytes(b'previous complete object')
        bitcode.write_bytes(b'previous complete bitcode')
        result = subprocess.run([
            sys.executable, SIM / 'compiler/codegen.py',
            '--input=' + str(tiny), '--output=' + str(obj),
            '--bitcode=' + str(bitcode), '--llvm-opt=' + str(sdk / 'opt'),
            '--llc=/bin/false', '--cache=' + str(cache), '--cxx=' + cxx,
            '--pipeline=' + pipeline, '--no-abi-buffers'
        ],
                                capture_output=True)
        assert result.returncode != 0
        assert obj.read_bytes() == b'previous complete object'
        assert bitcode.read_bytes() == b'previous complete bitcode'
        invalid_flags = subprocess.run([
            sys.executable, SIM / 'compiler/codegen.py', '--input=' +
            str(tiny), '--output=' + str(obj), '--bitcode=' + str(bitcode),
            '--llvm-opt=' + str(sdk / 'opt'), '--llc=' + str(sdk / 'llc'),
            '--cache=' + str(cache), "--opt-flags='"
        ],
                                       capture_output=True)
        assert invalid_flags.returncode != 0 and b'No closing quotation' in invalid_flags.stderr
        assert obj.read_bytes() == b'previous complete object'
        assert bitcode.read_bytes() == b'previous complete bitcode'
        invalid = subprocess.run([
            sys.executable, SIM / 'compiler/codegen.py', '--input=' +
            str(tiny), '--output=' + str(obj), '--bitcode=' + str(bitcode),
            '--llvm-opt=' + str(sdk / 'opt'), '--llc=' + str(sdk / 'llc'),
            '--cache=' + str(cache), '--pipeline=unknown'
        ],
                                 capture_output=True)
        assert invalid.returncode == 2
        assert obj.read_bytes() == b'previous complete object'
        assert bitcode.read_bytes() == b'previous complete bitcode'
    result = dict(status='passed',
                  cases=cases,
                  total_result_checks=294912,
                  floating_point_checks=32768,
                  opt_flags=shlex.split(opt_flags),
                  aliasing_delayed_captures_effects_and_bounds=True,
                  stock_without_plugins_or_host_compiler=True,
                  invalid_pipeline_preserves_complete_outputs=True,
                  failed_build_preserves_complete_outputs=True)
    (work / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(
        'PASS: compiler modes, 294912 result checks and failed-build preservation'
    )


if __name__ == '__main__':
    check(
        Path(os.environ['CIRCT_BIN']).resolve(), os.environ.get('CXX', 'c++'),
        SIM / 'generated-src/compiler-modes-test')
