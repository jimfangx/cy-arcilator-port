#!/usr/bin/env python3
"""Compile and compare the mixed simulator against an independent SV reference."""
import os
import argparse
import resource
import random
import shlex
import subprocess
from pathlib import Path

# Assertion failures are intentional in this regression.
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

here = Path(__file__).resolve().parent
sim = here.parent
bin_dir = Path(os.environ['CIRCT_BIN']).resolve()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--variants', nargs='+', choices=('normal', 'debug', 'analog', 'parameters', 'probe', 'ports', 'events', 'tracerom', 'shift', 'procedural', 'dpi-chain'),
                    default=['normal', 'debug', 'analog', 'parameters', 'probe', 'ports', 'events', 'tracerom', 'shift', 'procedural', 'dpi-chain'])
args = parser.parse_args()

def run(*args):
    subprocess.run([str(x) for x in args], check=True)

driver_build = sim / 'generated-src' / 'dpi-driver-test'
driver_build.mkdir(parents=True, exist_ok=True)
driver_test = driver_build / 'debug-rob'
run(*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++17', here / 'debug_rob.cc',
    sim.parent.parent / 'generators/rocket-chip/src/main/resources/csrc/debug_rob.cc',
    '-o', driver_test)
run(driver_test)
print('PASS: debug ROB empty/waiting outputs and completed writeback data', flush=True)

for variant in args.variants:
    if variant == 'procedural':
        run('python3', here / 'procedural_reference.py', '--work', sim / 'generated-src/procedural-reference')
        continue
    observe = ['--observe-registers', '--observe-ports', '--observe-wires'] if variant == 'debug' else []
    print(f'Running Arcilator regression: {variant}', flush=True)
    build = sim / 'generated-src' / ('boundary-test-' + variant)
    build.mkdir(parents=True, exist_ok=True)
    source, model_top, test_top, sv = here / 'boundary.mlir', 'Harness', 'BoundaryTest', here / 'boundary.sv'
    if variant == 'analog':
        source, model_top, test_top, sv = build / 'design.mlir', 'AnalogHarness', 'AnalogTest', here / 'analog.sv'
        run('python3', sim / 'frontend.py', '--input', here / 'analog.fir', '--output', source,
            '--top', model_top, '--firtool', bin_dir / 'firtool')
    elif variant == 'parameters':
        source, model_top, test_top, sv = here / 'parameters.mlir', 'Parameters', 'ParameterTest', here / 'parameters.sv'
    elif variant == 'events':
        source, model_top, test_top, sv = here / 'events.mlir', 'DelayHarness', 'EventTest', here / 'events.sv'
    elif variant == 'dpi-chain':
        source, model_top, test_top, sv = here / 'native_dpi_chain.mlir', 'DPIChainHarness', 'DPIChainTest', here / 'hybrid_dpi_chain.sv'
    elif variant == 'tracerom':
        source, model_top, test_top, sv = here / 'tracerom.mlir', 'TraceHarness', 'TraceTest', here / 'tracerom.sv'
    elif variant == 'probe':
        source, model_top, test_top, sv = build / 'design.mlir', 'ProbeHarness', 'ProbeTest', here / 'probe.sv'
        run('python3', sim / 'frontend.py', '--input', here / 'probe.mlir', '--output', source,
            '--top', model_top, '--firtool', bin_dir / 'firtool')
    elif variant == 'ports':
        # Thousands of legal ports must not exceed the foreign preprocessor's
        # per-line token limit. Check every returned bit across the actual DPI.
        source, model_top, test_top, sv = build / 'ports.mlir', 'ManyPorts', 'ManyPortTest', build / 'ports.sv'
        count = 5000
        source.write_text('module {\n  hw.module @ManyPorts(' +
            ', '.join(f'out p{i} : i1' for i in range(count)) + ') {\n' +
            '    %zero = hw.constant false\n    %one = hw.constant true\n    hw.output ' +
            ', '.join('%one' if i % 2 else '%zero' for i in range(count)) + ' : ' +
            ', '.join('i1' for _ in range(count)) + '\n  }\n}\n')
        sv.write_text('`timescale 1ns/1ps\nmodule ManyPortTest;\n' +
            f'  wire [{count-1}:0] bits;\n  ManyPorts dut(\n' +
            ',\n'.join(f'    .p{i}(bits[{i}])' for i in range(count)) + '\n  );\n' +
            f'  initial begin #1; if (bits !== {{{count//2}{{2\'b10}}}}) $fatal(1, "large DPI boundary mismatch");\n' +
            '    $display("PASS: 5000-port native DPI boundary"); $finish; end\nendmodule\n')
    elif variant == 'shift':
        source, model_top, test_top, sv = here / 'native_shift.mlir', 'ShiftHarness', 'ShiftTest', build / 'shift.sv'
        bits, rng = 2051, random.Random(1979)
        mask = (1 << bits) - 1
        lines = ['`timescale 1ns/1ps', 'module ShiftTest;',
                 f'  reg [{bits-1}:0] data, amount;',
                 f'  wire [{bits-1}:0] left, right, signed_result, latched;',
                 '  ShiftHarness dut(.clock(1\'b0), .reset(1\'b0), .data(data), .amount(amount),',
                 '    .left(left), .right(right), .\\signed (signed_result), .latched(latched));', '  initial begin']
        for data in (0, 1, mask, 1 << (bits - 1), *[rng.getrandbits(bits) for _ in range(8)]):
            signed = data - (1 << bits) if data >> (bits - 1) else data
            for amount in (0, 1, 7, 8, 63, 64, 2048, 2050, 2051, 100000, 1 << 1000):
                expected = ((data << amount) & mask if amount < bits else 0,
                            data >> amount if amount < bits else 0,
                            (signed >> amount) & mask if amount < bits else mask if signed < 0 else 0)
                lines += [f"    data = {bits}'h{data:x}; amount = {bits}'h{amount:x}; #1;",
                          '    if (' + ' || '.join(f"{port} !== {bits}'h{value:x}" for port, value in
                                                  zip(('left', 'right', 'signed_result'), expected)) +
                          ') $fatal(1, "wide shift mismatch");']
        lines += ['    $display("PASS: wide shift SV reference, signed extension and oversized amounts");',
                  '    $finish;', '  end', 'endmodule']
        sv.write_text('\n'.join(lines) + '\n')
    run('python3', sim / 'partition.py', '--input', source, '--output', build,
        '--top', model_top, '--circt-opt', bin_dir / 'circt-opt')
    run('python3', sim / 'compiler/partition.py', '--input', build / 'partitioned.mlir',
        '--output', build / 'outlined.mlirbc', '--circt-opt', bin_dir / 'circt-opt',
        '--cache', sim / 'generated-src/compiler', '--max-ops', '8', '--emit-bytecode')
    run(bin_dir / 'circt-opt', build / 'outlined.mlirbc', '--symbol-dce', '--canonicalize', '-o', build / 'model.mlir')
    run('python3', sim / 'compiler/model.py', '--input', build / 'model.mlir',
        '--output', build / 'model.ll', '--state-file', build / 'model.json',
        '--arcilator', bin_dir / 'arcilator', '--circt-opt', bin_dir / 'circt-opt',
        '--mlir-translate', bin_dir / 'mlir-translate', '--cache', sim / 'generated-src/compiler',
        '--max-ops', '16', '--', '--split-funcs-threshold=8192', *observe)
    run('python3', sim / 'compiler/llvm.py', build / 'model.ll')
    run(bin_dir / 'opt', '--passes=always-inline', build / 'model.ll', '-o', build / 'model.bc')
    run(bin_dir / 'llc', '-O2', '-relocation-model=pic', '-filetype=obj', build / 'model.bc', '-o', build / 'model.o')
    run('python3', sim / 'adapter.py', build / 'model.json', build / 'partition.json', build / 'adapter.cpp')
    if variant == 'events':
        # A coarse combinational scheduling dependency can repeatedly launch
        # the unchanged foreign delay. Bound actual DPI calls, independently
        # of machine speed, while checking the waveform against SV above.
        adapter = build / 'adapter.cpp'
        text = adapter.read_text().replace('  if (!s) return;',
            '  if (!s) return;\n  static unsigned calls = 0;\n'
            '  if (++calls > 1000) VL_FATAL_MT(__FILE__, __LINE__, "events", "foreign event multiplication");')
        adapter.write_text(text)
    libs = subprocess.check_output([bin_dir / 'llvm-config', '--libs', 'support'], text=True)
    link = '-L' + str(bin_dir.parent / 'lib') + ' -lCIRCTArcRuntime ' + ' '.join(shlex.split(libs)) + ' -lz -lpthread -ldl'
    c_sources = [here / 'c_driver.c'] if variant in ('normal', 'debug') else []
    foreign_sources, runtime_flags = [], []
    if variant == 'dpi-chain':
        foreign_sources = [here / 'hybrid_dpi_chain.cpp']
    if variant == 'tracerom':
        resource_dir = sim.parent.parent / 'generators/icenet/src/main/resources'
        foreign_sources = [resource_dir / 'vsrc/TraceROM.v', resource_dir / 'csrc/TraceROM.cc']
        trace_file = build / 'packets.txt'
        trace_file.write_text('fedcba9876543210 ff 0\n0123456789abcdef ff 0\n8000000000001357 03 1\n')
        runtime_flags = ['+trace=' + str(trace_file), '+macaddr=102030405060']
    run(os.environ.get('VERILATOR', 'verilator'), '--main', '--timing', '--cc', '--exe',
        '-Wno-fatal', '--top-module', test_top, '-Mdir', build / 'verilated',
        '+define+PRINTF_COND=$test$plusargs("native-prints")',
        '+define+STOP_COND=(!$test$plusargs("disable-native-stops"))',
        '-CFLAGS', '-std=c++17 -I' + str(sim) + ' -I' + str(bin_dir.parent / 'include'), '-LDFLAGS', link,
        build / 'ArcHarness.sv', sv, build / 'adapter.cpp', sim / 'native/wide.cpp', sim / 'native/format.cpp', sim / 'native/evaluation.cpp', build / 'model.o', *c_sources, *foreign_sources)
    run('python3', sim / 'c_sources.py', build / 'verilated' / ('V' + test_top + '.mk'),
        build / 'verilated' / 'arc-c-sources.mk')
    run('make', '-C', build / 'verilated', '-f', 'V' + test_top + '.mk', '-f', 'arc-c-sources.mk', '-j', '4')
    executable = str(build / 'verilated' / ('V' + test_top))
    def simulate(*flags):
        return subprocess.run([executable, *runtime_flags, *flags], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    quiet = simulate('+arc-vcdfile=' + str(build / 'native.vcd'))
    if quiet.returncode != 0 or 'ARC_PRINT:' in quiet.stdout:
        raise RuntimeError('quiet simulation or PRINTF_COND failed: ' + quiet.stdout)
    print(quiet.stdout, end='')
    trace = (build / 'native.vcd').read_text()
    expected_time = {'parameters': '#0', 'ports': '#0', 'probe': '#1000', 'events': '#137', 'tracerom': '#1000', 'shift': '#1000', 'dpi-chain': '#1000'}.get(variant, '#1700')
    if '$enddefinitions $end' not in trace or expected_time not in trace:
        raise RuntimeError('native waveform is missing definitions or physical timestamps')
    if variant not in ('normal', 'debug'):
        continue
    printed = simulate('+native-prints')
    if printed.returncode != 0 or 'ARC_PRINT:' not in printed.stdout:
        raise RuntimeError('native print was not enabled: ' + printed.stdout)
    failed = simulate('+force-fail')
    if failed.returncode == 0 or 'RTL assertion failed' not in failed.stdout:
        raise RuntimeError('native assertion did not fail the simulation: ' + failed.stdout)
    print('PASS: native RTL assertions terminate simulation with failure')
    finished = simulate('+force-finish')
    if finished.returncode != 0 or 'PASS: sequential' in finished.stdout:
        raise RuntimeError('native successful stop did not finish early: ' + finished.stdout)
    stopped = simulate('+force-stop')
    if stopped.returncode == 0 or 'RTL stopped with failure' not in stopped.stdout:
        raise RuntimeError('native failing stop did not fail: ' + stopped.stdout)
    masked = simulate('+force-fail', '+force-finish', '+force-stop', '+disable-native-stops')
    if masked.returncode != 0 or 'PASS: sequential' not in masked.stdout:
        raise RuntimeError('STOP_COND did not mask native stops: ' + masked.stdout)
    print('PASS: PRINTF_COND, STOP_COND, and native stop exit status')
