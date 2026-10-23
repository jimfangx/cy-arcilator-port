#!/usr/bin/env python3
"""Run original Vortex DPI providers through imported native Arcilator RTL."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import resource
import shlex
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def vectors(width, path):
    """Independent arbitrary precision math; no production provider is called."""
    mask = (1 << width) - 1
    mask64 = (1 << 64) - 1
    def signed(value):
        return value - (1 << width) if value >> (width - 1) else value
    def extend(value):
        return signed(value & mask) & mask64
    rng = random.Random(0x9696 + width)
    corners = [0, 1, 2, mask, mask - 1, 1 << (width - 1),
               (1 << (width - 1)) - 1, 0x123456789abcdef & mask]
    pairs = [(a, b) for a in corners for b in corners]
    pairs += [(rng.getrandbits(width), rng.getrandbits(width))
              for _ in range(2304)]
    previous = [0] * 5
    warps = {}
    traces = []
    rows = []
    for cycle, (a, b) in enumerate(pairs):
        reset = cycle % 127 == 0
        enabled = cycle % 7 != 0
        arithmetic = cycle % 11 != 0
        sa, sb, sd = (cycle >> 1) & 1, (cycle >> 2) & 1, cycle & 1
        warp = (cycle // 3) % 7
        pc = 0x8000000080000000 | ((cycle % 17) * 4)
        trace = cycle in (1, 2, 3)
        if reset:
            previous = [0] * 5
            warps = {}
        elif enabled:
            if arithmetic:
                product = (signed(a) if sa else a) * (signed(b) if sb else b)
                dividend, divisor = (signed(a), signed(b)) if sd else (a, b)
                if divisor == 0:
                    q, r = mask, a
                else:
                    q = abs(dividend) // abs(divisor)
                    if (dividend < 0) != (divisor < 0):
                        q = -q
                    r = dividend - q * divisor
                previous[:4] = [extend(product), extend(product >> width),
                                extend(q), extend(r)]
            else:
                # Native two-state DPI output arguments initialize every call.
                previous[:4] = [0] * 4
            pcs = warps.setdefault(warp, {})
            identity, reference = pcs.get(pc, (len(pcs), -1))
            reference += 1
            pcs[pc] = identity, reference
            previous[4] = (reference << 32) | (warp << 16) | identity
            if trace:
                traces.append(f'VORTEX_DPI warp={warp} pc={pc:016x}')
        rows.append([int(reset), int(enabled), int(arithmetic), sa, sb, sd,
                     a, b, warp, pc, int(trace), *previous])
    path.write_text(''.join(' '.join(f'{value:x}' for value in row) + '\n'
                            for row in rows))
    return len(rows), traces


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sim', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--runtime-objects', type=Path)
    args = parser.parse_args()
    sim, work, circt = args.sim.resolve(), args.work.resolve(), args.circt_bin.resolve()
    root = sim.parent.parent
    source = Path(__file__).resolve().parent
    vortex = root / 'generators/radiance/src/main/resources/vsrc/vortex'
    driver = vortex / 'hw/dpi/util_dpi.cpp'
    work.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    cxx = shlex.split(os.environ.get('CXX', 'c++'))
    report = dict(status='running', scope='Original eight Vortex DPI C providers linked against native headers/runtime and called by imported RTL. Both XLEN profiles and LLVM O0/O2/O3. This is a peripheral regression, not a stock Chipyard config pass.', stages=[], executions=[])
    watched = [driver, vortex / 'sim/common/uuid_gen.h',
               vortex / 'hw/dpi/util_dpi.vh', vortex / 'hw/rtl/VX_config.vh',
               source / 'native_vortex_dpi.sv', source / 'native_vortex_dpi.cpp',
               Path(__file__).resolve()]
    watched += sorted(p for p in (sim / 'native').rglob('*')
                      if p.suffix in ('.h', '.cpp', '.def'))
    watched += [sim / name for name in ('native_hw.py', 'native.py', 'native_exports.py', 'partition.py')]
    watched += sorted(p for p in (sim / 'compiler').glob('*')
                      if p.suffix in ('.h', '.cpp', '.py'))
    watched += sorted(p for p in (sim / 'compiler/verilog').iterdir()
                      if p.suffix in ('.py', '.patch', '.json') or p.name == 'CMakeLists.txt')
    report['source_sha256'] = {str(p): digest(p) for p in watched}
    def save():
        temp = work / 'report.tmp'
        temp.write_text(json.dumps(report, indent=2) + '\n')
        temp.replace(work / 'report.json')
    def guard():
        assert all(digest(Path(p)) == h for p, h in report['source_sha256'].items())
    def run(name, command):
        guard()
        item = dict(stage=name, command=[str(x) for x in command], status='running')
        report['stages'].append(item)
        save()
        start = time.monotonic()
        with (work / (name + '.log')).open('w') as log:
            process = subprocess.Popen(item['command'], stdout=log, stderr=subprocess.STDOUT)
            item['child_pid'] = process.pid
            save()
            rc = process.wait()
        item.pop('child_pid')
        item.update(returncode=rc, seconds=time.monotonic() - start,
                    status='passed' if rc == 0 else 'failed')
        save()
        guard()
        if rc:
            raise RuntimeError(name + ' failed; see ' + str(work / (name + '.log')))
    save()
    try:
        cache = sim / 'generated-src/compiler'
        link = shlex.split(subprocess.check_output([circt / 'llvm-config', '--libs', 'support'], text=True))
        runtime_objects = []
        if args.runtime_objects:
            # Reuse completed original native objects, excluding generated model
            # and selected hardware drivers. Keep exact object hashes as evidence.
            names = {p.stem for p in (sim / 'native').glob('*.cpp')}
            for p in args.runtime_objects.glob('*.o'):
                if p.stem.partition('-')[2] in names:
                    runtime_objects.append(p.resolve())
            report['runtime_objects_sha256'] = {str(p): digest(p) for p in runtime_objects}
        available = {p.stem.partition('-')[2] for p in runtime_objects}
        for p in sorted((sim / 'native').glob('*.cpp')):
            if p.stem not in available:
                obj = work / (p.stem + '.o')
                flags = ['-fno-rtti'] if p.stem == 'files' else []
                run('runtime-' + p.stem, [*cxx, '-O2', '-std=c++17', '-DARC_NATIVE_NO_MAIN',
                    *flags, '-I' + str(sim), '-I' + str(sim / 'native/include'),
                    '-I' + str(circt.parent / 'include'), '-c', p, '-o', obj])
                runtime_objects.append(obj)
        inputs = [('clock', 1), ('reset', 1), ('enable', 1), ('arithmetic_enable', 1),
                  ('signed_a', 1), ('signed_b', 1), ('signed_division', 1),
                  ('assertion_failure', 1), ('trace_enable', 1), ('a', 64), ('b', 64),
                  ('pc', 64), ('warp', 32)]
        outputs = [(name, 64) for name in ('low_product', 'high_product', 'quotient', 'remainder', 'uuid')]
        ports = ', '.join([f'in %{n}:i{w}' for n, w in inputs] +
                          [f'out {n}:i{w}' for n, w in outputs])
        actual = ', '.join(f'{n}:%{n}:i{w}' for n, w in inputs)
        results = ', '.join(f'{n}:i{w}' for n, w in outputs)
        fixture = ('module {\n  hw.module.extern @VortexDpiProbe(' + ports + ')\n' +
            '  hw.module @VortexDpiHarness(' + ports + ') {\n' +
            '    %lo, %hi, %q, %r, %u = hw.instance "rtl" @VortexDpiProbe(' + actual + ') -> (' + results + ')\n' +
            '    hw.output %lo, %hi, %q, %r, %u : i64, i64, i64, i64, i64\n  }\n}\n')
        for width in (32, 64):
            build = work / ('xlen' + str(width))
            build.mkdir()
            design = build / 'input.mlir'
            design.write_text(fixture)
            filelist = build / 'blackboxes.f'
            filelist.write_text(str(source / 'native_vortex_dpi.sv') + '\n')
            corpus = build / 'expected.txt'
            cycles, traces = vectors(width, corpus)
            flags = '+define+SIMULATION -I' + str(vortex / 'hw/rtl')
            flags += ' -I' + str(vortex / 'hw/dpi')
            if width == 64:
                flags += ' +define+XLEN_64'
            prefix = 'xlen' + str(width)
            run(prefix + '-import', ['python3', sim / 'native_hw.py', '--input', design,
                '--output', build / 'design.mlir', '--filelist', filelist, '--work', build / 'rtl',
                '--circt-verilog', circt / 'circt-verilog', '--circt-opt', circt / 'circt-opt',
                '--cxx', ' '.join(cxx), '--flags=' + flags])
            run(prefix + '-partition', ['python3', sim / 'partition.py', '--input', build / 'design.mlir',
                '--output', build, '--top', 'VortexDpiHarness', '--circt-opt', circt / 'circt-opt'])
            run(prefix + '-model', ['python3', sim / 'compiler/model.py', '--input', build / 'partitioned.mlir',
                '--output', build / 'model.ll', '--state-file', build / 'model.json',
                '--arcilator', circt / 'arcilator', '--circt-opt', circt / 'circt-opt',
                '--mlir-translate', circt / 'mlir-translate', '--cache', cache, '--max-ops', '16'])
            run(prefix + '-llvm', ['python3', sim / 'compiler/llvm.py', build / 'model.ll',
                '--llvm-opt', circt / 'opt', '--cache', cache, '--cxx', ' '.join(cxx)])
            run(prefix + '-bindings', ['python3', sim / 'native.py', build / 'model.json',
                build / 'partition.json', build / 'model.cpp', '--filelist=' + str(filelist)])
            for level in (0, 2, 3):
                mode = prefix + '-O' + str(level)
                obj = build / ('model-O' + str(level) + '.o')
                run(mode + '-codegen', ['python3', sim / 'compiler/codegen.py', '--input', build / 'model.ll',
                    '--output', obj, '--llvm-opt', circt / 'opt', '--llc', circt / 'llc', '--cache', cache,
                    '--cxx', ' '.join(cxx), '--opt-level=' + str(level), '--llc-opt-level=' + str(level)])
                exe = build / ('test-O' + str(level))
                run(mode + '-link', [*cxx, '-O' + str(level), '-std=c++17', '-DARC_NATIVE_NO_MAIN',
                    *(['-DXLEN_64'] if width == 64 else []), '-I' + str(sim), '-I' + str(sim / 'native/include'),
                    '-I' + str(circt.parent / 'include'), '-I' + str(vortex / 'sim/common'),
                    build / 'model.cpp', source / 'native_vortex_dpi.cpp', driver, *runtime_objects, obj,
                    '-L' + str(circt.parent / 'lib'), '-lCIRCTArcRuntime', *link,
                    '-lz', '-lpthread', '-ldl', '-o', exe])
                for kind, extra in [('positive', []), ('value-fault', ['+fault-value']), ('assertion-fault', ['+fault-assert'])]:
                    result = subprocess.run([str(exe), str(corpus), *extra], capture_output=True, text=True)
                    (build / (mode + '-' + kind + '.stdout')).write_text(result.stdout)
                    (build / (mode + '-' + kind + '.stderr')).write_text(result.stderr)
                    record = dict(mode=mode, case=kind, returncode=result.returncode)
                    report['executions'].append(record)
                    save()
                    if kind == 'positive':
                        assert result.returncode == 0, result.stdout + result.stderr
                        assert f'PASS: {cycles * 5} original Vortex DPI results across {cycles} cycles' in result.stdout
                        assert [line for line in result.stdout.splitlines() if line.startswith('VORTEX_DPI')] == traces
                        record['checks'] = cycles * 5
                    elif kind == 'value-fault':
                        assert result.returncode == 1 and 'Vortex DPI mismatch cycle=1 output=low_product' in result.stderr
                    else:
                        assert result.returncode == -6, result.stdout + result.stderr
                        assert 'delayed assertion at ' in result.stdout
                        calls = [int(line.split()[1]) for line in result.stdout.splitlines() if line.startswith('ASSERT_CALL ')]
                        assert calls[-1] == 12, calls
                    record['status'] = 'passed'
                    save()
                symbols = subprocess.check_output(['nm', '-C', exe], text=True)
                assert 'Verilated::' not in symbols
                assert all(name in symbols for name in ('dpi_imul', 'dpi_idiv', 'dpi_register', 'dpi_assert', 'dpi_trace', 'dpi_trace_start', 'dpi_trace_stop', 'dpi_uuid_gen'))
                report.setdefault('executables', {})[mode] = dict(path=str(exe), sha256=digest(exe))
        guard()
        if args.runtime_objects:
            assert all(digest(Path(p)) == h for p, h in report['runtime_objects_sha256'].items())
        report['status'] = 'passed'
        save()
        print(json.dumps(dict(status='passed', positive_runs=6, fault_controls=12,
                              checks=sum(x.get('checks', 0) for x in report['executions']))), flush=True)
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        save()
        raise


if __name__ == '__main__':
    main()
