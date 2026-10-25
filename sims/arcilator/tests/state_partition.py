#!/usr/bin/env python3
"""Exercise state-version partitioning with independent native state/effect oracles."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM / 'compiler'))
from partition import plugin


def fixture():
    lines = ['module {',
             'func.func private @state_partition_event(i64, i64, i64) -> i64',
             'arc.model @state_partition_fixture io !hw.modty<> storageBytes 192 {',
             '^bb0(%storage: !arc.storage):']
    names = ['a', 'b', 'input', 'mask', 'clock', 'last_clock', 'enable', 'reset',
             'out_old', 'out_fresh', 'out_mix', 'out_before', 'out_after',
             'epoch0', 'epoch1', 'epoch2', 'event1_result', 'event3_result',
             'out_late', 'event2_result']
    for n, name in enumerate(names):
        lines.append(f'%{name} = arc.alloc_state %storage {{name = "{name}", '
                     f'offset = {n * 8} : i32}} : (!arc.storage) -> !arc.state<i64>')
    lines += ['%memory = arc.alloc_memory %storage {name = "memory", offset = 160 : i32, '
              'stride = 8 : i32} : (!arc.storage) -> !arc.memory<4 x i64, i2>']
    for name, value in [('zero', 0), ('one', 1), ('two', 2), ('three', 3),
                        ('nine', 9), ('seven', 7), ('fifty_seven', 57),
                        ('all', -1), ('reset_b', 0x6a09e667f3bcc909)]:
        lines.append(f'%{name} = hw.constant {value} : i64')
    for name, state in [('old_a', 'a'), ('old_b', 'b'), ('incoming', 'input'),
                        ('mask_value', 'mask'), ('clock_value', 'clock'),
                        ('last_value', 'last_clock'), ('enabled_value', 'enable'),
                        ('reset_value', 'reset')]:
        lines.append(f'%{name} = arc.state_read %{state} : <i64>')
    lines += ['%clock_bit = comb.icmp ne %clock_value, %zero : i64',
              '%last_bit = comb.icmp eq %last_value, %zero : i64',
              '%edge = comb.and %clock_bit, %last_bit : i1',
              '%enabled = comb.icmp ne %enabled_value, %zero : i64',
              '%in_reset = comb.icmp ne %reset_value, %zero : i64',
              '%index = comb.extract %incoming from 0 : (i64) -> i2']

    def mix(prefix, count, indentation=''):
        previous = '%old_a'
        for n in range(count):
            stem = f'%{prefix}_{n}'
            lines.extend(indentation + text for text in [
                f'{stem}_constant = hw.constant {0x13579 + n * 97} : i64',
                f'{stem}_xor = comb.xor {previous}, %incoming : i64',
                f'{stem}_add = comb.add {stem}_xor, {stem}_constant : i64',
                f'{stem}_left = comb.shl {stem}_add, %seven : i64',
                f'{stem}_right = comb.shru {stem}_add, %fifty_seven : i64',
                f'{stem}_rotate = comb.or {stem}_left, {stem}_right : i64',
                f'{stem} = comb.xor {stem}_rotate, %old_b : i64'])
            previous = stem
        return previous

    result = mix('mix', 24)
    lines += [f'arc.state_write %out_mix = {result} : <i64>',
              'arc.state_write %out_old = %old_a : <i64>',
              'arc.state_write %epoch0 = %old_a : <i64>',
              'arc.state_write %last_clock = %clock_value : <i64>',
              '%before = arc.memory_read %memory[%index] : <4 x i64, i2>',
              'arc.state_write %out_before = %before : <i64>',
              'scf.if %edge {',
              '  scf.if %in_reset {',
              '    arc.state_write %a = %zero : <i64>',
              '    arc.state_write %b = %reset_b : <i64>',
              '  } else {',
              '    scf.if %enabled {',
              '      %next = comb.add %old_a, %incoming : i64',
              '      arc.state_write %a = %next : <i64>',
              '      %epoch1_read = arc.state_read %a : <i64>',
              '      arc.state_write %epoch1 = %epoch1_read : <i64>',
              '      %keep_mask = comb.xor %mask_value, %all : i64',
              '      %kept = comb.and %before, %keep_mask : i64',
              f'      %new_bits = comb.and {result}, %mask_value : i64',
              '      %new_memory = comb.or %kept, %new_bits : i64',
              '      arc.memory_write %memory[%index], %new_memory : <4 x i64, i2>',
              '      %memory_after = arc.memory_read %memory[%index] : <4 x i64, i2>',
              '      arc.state_write %out_after = %memory_after : <i64>']
    late = mix('late', 12, '      ')
    lines += [f'      arc.state_write %out_late = {late} : <i64>',
              '      %ret1 = func.call @state_partition_event(%one, %old_a, %epoch1_read) : (i64, i64, i64) -> i64',
              '      arc.state_write %event1_result = %ret1 : <i64>',
              '      %after_callback = arc.state_read %a : <i64>',
              '      %incremented = comb.add %after_callback, %nine : i64',
              '      arc.state_write %a = %incremented : <i64>',
              '      %epoch2_read = arc.state_read %a : <i64>',
              '      arc.state_write %epoch2 = %epoch2_read : <i64>',
              f'      %next_b = comb.xor %old_b, {result} : i64',
              '      arc.state_write %b = %next_b : <i64>',
              '      %ret2 = func.call @state_partition_event(%two, %old_b, %next_b) : (i64, i64, i64) -> i64',
              '      arc.state_write %event2_result = %ret2 : <i64>',
              '      %fresh_a = arc.state_read %a : <i64>',
              '      arc.state_write %out_fresh = %fresh_a : <i64>',
              '      %fresh_memory = arc.memory_read %memory[%index] : <4 x i64, i2>',
              '      arc.state_write %out_after = %fresh_memory : <i64>',
              '    }', '  }', '}',
              '%end_a = arc.state_read %a : <i64>',
              '%end_b = arc.state_read %b : <i64>',
              '%ret3 = func.call @state_partition_event(%three, %end_a, %end_b) : (i64, i64, i64) -> i64',
              'arc.state_write %event3_result = %ret3 : <i64>',
              '}', '''
func.func private @state_partition_array_event()
arc.model @state_partition_array io !hw.modty<> storageBytes 64 {
^bb0(%storage: !arc.storage):
  %pointer = arc.alloc_state %storage {name = "pointer", offset = 0 : i32} : (!arc.storage) -> !arc.state<!arc.arrayref<2xi64>>
  %out0 = arc.alloc_state %storage {name = "out0", offset = 16 : i32} : (!arc.storage) -> !arc.state<i64>
  %out1 = arc.alloc_state %storage {name = "out1", offset = 24 : i32} : (!arc.storage) -> !arc.state<i64>
  %zero = arith.constant 0 : index
  %one = arith.constant 1 : index
  %constant = hw.constant 11259375 : i64
  %snapshot = arc.state_read %pointer : <!arc.arrayref<2xi64>>
  func.call @state_partition_array_event() : () -> ()
  %a = arc.arrayref.get %snapshot[%zero] : !arc.arrayref<2xi64> -> i64
  %b = arc.arrayref.get %snapshot[%one] : !arc.arrayref<2xi64> -> i64
  %x = comb.xor %a, %constant : i64
  %y = comb.add %a, %b : i64
  arc.state_write %out0 = %x : <i64>
  arc.state_write %out1 = %y : <i64>
}
arc.model @state_partition_alias io !hw.modty<> storageBytes 64 {
^bb0(%storage: !arc.storage):
  %a = arc.alloc_state %storage {name = "a", offset = 0 : i32} : (!arc.storage) -> !arc.state<i64>
  %old_out = arc.alloc_state %storage {name = "old", offset = 8 : i32} : (!arc.storage) -> !arc.state<i64>
  %new_out = arc.alloc_state %storage {name = "new", offset = 16 : i32} : (!arc.storage) -> !arc.state<i64>
  %alias = arc.storage.get %storage[1] : !arc.storage -> !arc.state<i8>
  %value = hw.constant 165 : i8
  %old = arc.state_read %a : <i64>
  arc.state_write %alias = %value : <i8>
  %new = arc.state_read %a : <i64>
  arc.state_write %old_out = %old : <i64>
  arc.state_write %new_out = %new : <i64>
}
''', '}']
    lines.insert(-1, 'func.func private @state_partition_guard_event()')
    lines.insert(-1, guard_fixture('state_partition_versions', False))
    lines.insert(-1, guard_fixture('state_partition_effects', True))
    lines.insert(-1, guard_fixture('state_partition_crosshelper', False,
                                   cross_helper=True))
    lines.insert(-1, '''
arc.model @state_partition_atomic io !hw.modty<> storageBytes 64 {
^bb0(%storage: !arc.storage):
  %a = arc.alloc_state %storage {name = "a", offset = 0 : i32} : (!arc.storage) -> !arc.state<i64>
  %input = arc.alloc_state %storage {name = "input", offset = 8 : i32} : (!arc.storage) -> !arc.state<i64>
  %out = arc.alloc_state %storage {name = "out", offset = 16 : i32} : (!arc.storage) -> !arc.state<i64>
  %fresh = arc.alloc_state %storage {name = "fresh", offset = 24 : i32} : (!arc.storage) -> !arc.state<i64>
  %old = arc.state_read %a : <i64>
  %x = arc.state_read %input : <i64>
  %next = comb.add %old, %x : i64
  arc.state_write %a = %next : <i64>
  %new = arc.state_read %a : <i64>
  %both = comb.xor %old, %new : i64
  arc.state_write %out = %both : <i64>
  arc.state_write %fresh = %new : <i64>
}
''')
    return '\n'.join(lines) + '\n'


def guard_fixture(name, opaque_effect, cross_helper=False):
    """Separate write-version and foreign-epoch witnesses, with no other barrier."""
    lines = [f'arc.model @{name} io !hw.modty<> storageBytes 64 {{',
             '^bb0(%storage: !arc.storage):']
    for offset, state in enumerate(('a', 'input', 'out', 'fresh')):
        lines.append(f'%{state} = arc.alloc_state %storage {{name = "{state}", '
                     f'offset = {offset * 8} : i32}} : (!arc.storage) -> !arc.state<i64>')
    lines += ['%old_a = arc.state_read %a : <i64>',
              '%incoming = arc.state_read %input : <i64>',
              '%old_b = hw.constant 7 : i64',
              '%seven = hw.constant 7 : i64',
              '%fifty_seven = hw.constant 57 : i64']
    if opaque_effect:
        lines += ['func.call @state_partition_guard_event() : () -> ()']
    else:
        lines += ['%next = comb.add %old_a, %incoming : i64']
        if cross_helper:
            lines += ['arc.state_write %a = %next : <i64>']
        else:
            lines += ['%write_gate = hw.constant true',
                      '%zero = hw.constant 0 : i64',
                      'scf.if %write_gate {',
                      '  %stored = comb.add %next, %zero : i64',
                      '  arc.state_write %a = %stored : <i64>', '}']
    previous = '%old_a'
    for n in range(12):
        stem = f'%step{n}'
        lines += [f'{stem}_constant = hw.constant {0x13579 + n * 97} : i64',
                  f'{stem}_xor = comb.xor {previous}, %incoming : i64',
                  f'{stem}_add = comb.add {stem}_xor, {stem}_constant : i64',
                  f'{stem}_left = comb.shl {stem}_add, %seven : i64',
                  f'{stem}_right = comb.shru {stem}_add, %fifty_seven : i64',
                  f'{stem}_rotate = comb.or {stem}_left, {stem}_right : i64',
                  f'{stem} = comb.xor {stem}_rotate, %old_b : i64']
        previous = stem
    lines += [f'arc.state_write %out = {previous} : <i64>',
              '%after = arc.state_read %a : <i64>']
    if cross_helper:
        lines += ['%cross_epoch = comb.xor %old_a, %after : i64',
                  'arc.state_write %fresh = %cross_epoch : <i64>']
    else:
        lines += ['arc.state_write %fresh = %after : <i64>']
    lines += ['}']
    return '\n'.join(lines)


def run(command, log=None, expect=0):
    result = subprocess.run([str(x) for x in command], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if log:
        Path(log).write_text(result.stdout + result.stderr)
    if (result.returncode == 0) != (expect == 0):
        raise AssertionError((shlex.join([str(x) for x in command]),
                              result.returncode, result.stdout, result.stderr))
    return result


def lower(circt, source, work, label):
    llvm_mlir = work / (label + '.llvm.mlir')
    llvm = work / (label + '.ll')
    run([circt / 'circt-opt', source,
         '--pass-pipeline=builtin.module(arc-lower-arcs-to-funcs,'
         'arc-lower-clocks-to-funcs,arc-infer-context,lower-arc-to-llvm,'
         'reconcile-unrealized-casts)', '-o', llvm_mlir],
        work / (label + '-lower.log'))
    run([circt / 'mlir-translate', '--mlir-to-llvmir', llvm_mlir, '-o', llvm],
        work / (label + '-translate.log'))
    return llvm


def execute(circt, cxx, llvm, work, label, expect=0):
    reports = []
    for level in ('0', '3'):
        bc = work / (label + '-O' + level + '.bc')
        obj = bc.with_suffix('.o')
        executable = bc.with_suffix('.exe')
        run([circt / 'opt', '-passes=default<O' + level + '>,verify', llvm,
             '-o', bc], work / (label + '-opt-O' + level + '.log'))
        run([circt / 'llc', '-O' + level, '-relocation-model=pic',
             '-filetype=obj', bc, '-o', obj],
            work / (label + '-llc-O' + level + '.log'))
        run([*shlex.split(cxx), '-O2', '-std=c++17',
             SIM / 'tests/state_partition.cpp', obj, '-o', executable],
            work / (label + '-link-O' + level + '.log'))
        result = run([executable], work / (label + '-execute-O' + level + '.log'),
                     expect=expect)
        report = dict(level=level, returncode=result.returncode,
                      output=result.stdout.strip())
        if expect:
            assert result.returncode == 1 and 'oracle mismatch' in result.stderr
            report['diagnostic'] = result.stderr.strip()
        reports.append(report)
    return reports


def fault_sources(source):
    """Actual bad IR alternatives exercise the oracle rather than text checks."""
    faults = {}
    faults['lost-old-version'] = source.replace(
        '      %late_0_xor = comb.xor %old_a, %incoming : i64',
        '      %unsafe_old = arc.state_read %a : <i64>\n'
        '      %late_0_xor = comb.xor %unsafe_old, %incoming : i64')
    faults['ignored-effect-write'] = source.replace(
        '%after_callback = arc.state_read %a : <i64>',
        '%after_callback = comb.add %epoch1_read, %zero : i64')
    faults['callback-order'] = source.replace(
        '@state_partition_event(%one, %old_a, %epoch1_read)',
        '@state_partition_event(%two, %old_a, %epoch1_read)').replace(
        '@state_partition_event(%two, %old_b, %next_b)',
        '@state_partition_event(%one, %old_b, %next_b)')
    call = '  func.call @state_partition_array_event() : () -> ()\n'
    read = '  %a = arc.arrayref.get %snapshot[%zero] : !arc.arrayref<2xi64> -> i64\n'
    faults['retimed-array-load'] = source.replace(call + read, read + call)
    faults['lost-partial-alias-version'] = source.replace(
        'arc.state_write %old_out = %old : <i64>',
        'arc.state_write %old_out = %new : <i64>')
    for name, text in faults.items():
        assert text != source, name
    return faults


def clock_fixture():
    return '''module {
arc.define @Identity(%x: i8) -> i8 { arc.output %x : i8 }
hw.module @state_partition_clock(in %clock: !seq.clock, in %data: i8, out a: i8, out b: i8) {
 %a = arc.state @Identity(%data) clock %clock latency 1 {names = ["state_a"]} : (i8) -> i8
 %derived = comb.extract %a from 0 : (i8) -> i1
 %derived_clock = seq.to_clock %derived
 %b = arc.state @Identity(%a) clock %derived_clock latency 1 {names = ["state_b"]} : (i8) -> i8
 hw.output %a, %b : i8, i8
}
}
'''


def clock_layout(text):
    size = int(re.search(r'@state_partition_clock\b[^\n]*storageBytes (\d+)', text)[1])
    result = dict(size=size)
    for line in text.splitlines():
        if 'offset =' not in line:
            continue
        offset = int(re.search(r'offset = (\d+)', line)[1])
        port = re.search(r'arc.root_(input|output) "([^\"]+)"', line)
        named = re.search(r'name = "([^\"]+)"', line)
        if port:
            result[port[1] + '_' + port[2]] = offset
        elif named:
            result[named[1]] = offset
    assert set(result) == {'size', 'input_clock', 'input_data', 'output_a',
                           'output_b', 'state_a', 'state_b'}, result
    return result


def clock_execute(circt, cxx, llvm, layout, work, label, expect_failure=False):
    import ctypes
    import random
    results = []
    for level in ('0', '3'):
        bc = work / (label + '-O' + level + '.bc')
        obj = bc.with_suffix('.o')
        library = bc.with_suffix('.so')
        run([circt / 'opt', '-passes=default<O' + level + '>,verify', llvm,
             '-o', bc], work / (label + '-opt-O' + level + '.log'))
        run([circt / 'llc', '-O' + level, '-relocation-model=pic', '-filetype=obj',
             bc, '-o', obj], work / (label + '-llc-O' + level + '.log'))
        run([*shlex.split(cxx), '-shared', obj, '-o', library],
            work / (label + '-link-O' + level + '.log'))
        loaded = ctypes.CDLL(str(library))
        evaluate = loaded.state_partition_clock_eval
        evaluate.argtypes = [ctypes.c_void_p]
        evaluate.restype = None
        sequence = [(0, 11), (1, 11), (1, 33), (0, 20), (1, 20),
                    (0, 31), (1, 31)]
        rng = random.Random(0xabc9021)
        sequence += [(rng.randrange(2), rng.randrange(256)) for _ in range(1024)]
        hashes, checks, mismatch = [], 0, None
        for recreation in range(2):
            state = (ctypes.c_uint8 * layout['size'])()
            state[layout['state_a']] = 10
            state[layout['state_b']] = 0xa5
            a, b, old_clock, old_derived = 10, 0xa5, 0, 0
            for n, (clock, data) in enumerate(sequence):
                old_a = a
                if clock and not old_clock:
                    a = data
                derived = a & 1
                if derived and not old_derived:
                    b = old_a
                old_clock, old_derived = clock, derived
                state[layout['input_clock']] = clock
                state[layout['input_data']] = data
                evaluate(state)
                actual = tuple(state[layout[k]] for k in
                               ('state_a', 'state_b', 'output_a', 'output_b'))
                expected = (a, b, a, b)
                if actual != expected:
                    mismatch = dict(sample=n, expected=expected, actual=actual)
                    break
                hashes.append(hashlib.sha256(bytes(state)).hexdigest())
                checks += 1
            if mismatch:
                break
        assert bool(mismatch) == expect_failure, (label, level, mismatch)
        results.append(dict(level=level, checked_frames=checks,
                            image_hash=hashlib.sha256(''.join(hashes).encode()).hexdigest(),
                            fault=mismatch))
    return results


def check_clocks(circt, cxx, library, work):
    source = work / 'clocks.hw.mlir'
    source.write_text(clock_fixture())
    state = work / 'clocks.state.mlir'
    run([circt / 'circt-opt', source, '--arc-lower-state', '-o', state],
        work / 'clocks-state.log')
    variants = []
    layout = None
    for label, pipeline in [
            ('clock-baseline', 'arc-lower-arcs-to-funcs,arc.model(arc-allocate-state)'),
            ('clock-partitioned', 'chipyard-state-partition-plan,'
             'arc-lower-arcs-to-funcs,arc.model(arc-allocate-state),'
             'chipyard-state-partition-materialize{min-ops=2 max-ops=16 max-input-words=4}')]:
        allocated = work / (label + '.mlir')
        run([circt / 'circt-opt', state, '--load-pass-plugin=' + str(library),
             '--pass-pipeline=builtin.module(' + pipeline + ')', '-o', allocated],
            work / (label + '-allocate.log'))
        actual_layout = clock_layout(allocated.read_text())
        if layout is None:
            layout = actual_layout
        assert actual_layout == layout
        llvm = lower(circt, allocated, work, label)
        variants.append(clock_execute(circt, cxx, llvm, layout, work, label))
    assert variants[0] == variants[1], 'complete derived-clock model images differ'
    # Corrupt actual allocated IR: use New A for the derived-clock B data.
    allocated_text = (work / 'clock-baseline.mlir').read_text()
    new_a = re.search(r'comb.extract (%[^ ]+) from 0', allocated_text)[1]
    calls = list(re.finditer(r'func.call @Identity\((%[^)]+)\)', allocated_text))
    assert len(calls) == 2
    start, end = calls[-1].span(1)
    bad = work / 'clock-new-instead-of-old.mlir'
    bad.write_text(allocated_text[:start] + new_a + allocated_text[end:])
    llvm = lower(circt, bad, work, 'clock-fault')
    fault = clock_execute(circt, cxx, llvm, layout, work, 'clock-fault', True)
    return dict(baseline=variants[0], partitioned=variants[1],
                same_physical_layout=layout, old_new_fault_detected=fault)


def check_parallel(circt, cxx, llvm, work, expected):
    from codegen import compile_model
    reports = []
    for level in ('0', '3'):
        obj = work / ('parallel-O' + level + '.o')
        report = work / ('parallel-O' + level + '.json')
        compile_model(llvm, obj, circt / 'opt', circt / 'llc',
                      work / 'plugin-cache', cxx=cxx, opt_level=level,
                      llc_opt_level=level, jobs=2, report=report,
                      abi_buffers=False, pipeline='stock', parallel_stock=True)
        details = json.loads(report.read_text())
        assert details['jobs'] == 2 and details['pipeline'] == 'stock'
        assert details['parallel_stock'] is True
        assert len(details['partitions']) == 2, details
        assert obj.with_suffix('.bc').read_bytes() == \
            (work / ('candidate-O' + level + '.bc')).read_bytes(), \
            'parallel backend altered pre-codegen stock optimization'
        executable = obj.with_suffix('.exe')
        run([*shlex.split(cxx), '-O2', '-std=c++17',
             SIM / 'tests/state_partition.cpp', obj, '-o', executable],
            work / ('parallel-link-O' + level + '.log'))
        result = run([executable], work / ('parallel-execute-O' + level + '.log'))
        assert result.stdout.strip() == expected[int(level == '3')]['output']
        reports.append(dict(level=level, parts=len(details['partitions']),
                            stock_bitcode_exact=True, oracle=result.stdout.strip()))
    return reports


def check_fallbacks(circt, library, source, work):
    normalized = work / 'unplanned-normalized.mlir'
    missing = work / 'unplanned-materialized.mlir'
    missing_report = work / 'unplanned.json'
    run([circt / 'circt-opt', source, '-o', normalized])
    run([circt / 'circt-opt', source, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-materialize{'
         'min-ops=2 max-ops=16 max-input-words=4 report-file=' +
         str(missing_report) + '})', '-o', missing])
    assert normalized.read_bytes() == missing.read_bytes(), 'missing plan changed IR'
    no_plan = json.loads(missing_report.read_text())
    assert no_plan['helpers'] == 0 and no_plan['rejected_missing_plan'] == 7

    traced = work / 'traced.mlir'
    traced.write_text(source.read_text().replace(
        'storageBytes 192 {', 'storageBytes 192 attributes {traceTaps = []} {').replace(
        'storageBytes 64 {', 'storageBytes 64 attributes {traceTaps = []} {'))
    planned = work / 'traced-planned.mlir'
    materialized = work / 'traced-materialized.mlir'
    trace_report = work / 'traced.json'
    trace_control = work / 'traced-control.mlir'
    run([circt / 'circt-opt', traced, '-o', trace_control])
    run([circt / 'circt-opt', traced, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-plan)', '-o', planned])
    run([circt / 'circt-opt', planned, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-materialize{'
         'min-ops=2 max-ops=16 max-input-words=4 report-file=' + str(trace_report) +
         '})', '-o', materialized])
    assert materialized.read_bytes() == trace_control.read_bytes(), \
        'trace observer fallback changed original IR beyond removing certificates'
    trace = json.loads(trace_report.read_text())
    assert trace['helpers'] == 0 and trace['rejected_trace_observers'] == 7

    wide = work / 'wide-interface.mlir'
    lines = ['module {', 'func.func private @unknown_pair() -> (i64, i64)',
             'arc.model @wide_interface io !hw.modty<> storageBytes 64 {',
             '^bb0(%storage: !arc.storage):',
             '%out = arc.alloc_state %storage {name = "out", offset = 0 : i32} : (!arc.storage) -> !arc.state<i64>',
             '%v:2 = func.call @unknown_pair() : () -> (i64, i64)']
    previous = '%v#0'
    for n in range(24):
        lines.append(f'%x{n} = comb.add {previous}, %v#1 : i64')
        previous = f'%x{n}'
    lines += [f'arc.state_write %out = {previous} : <i64>', '}', '}']
    wide.write_text('\n'.join(lines) + '\n')
    planned = work / 'wide-interface-plan.mlir'
    materialized = work / 'wide-interface-materialized.mlir'
    wide_report = work / 'wide-interface.json'
    wide_control = work / 'wide-interface-control.mlir'
    run([circt / 'circt-opt', wide, '-o', wide_control])
    run([circt / 'circt-opt', wide, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-plan)', '-o', planned])
    run([circt / 'circt-opt', planned, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-materialize{'
         'min-ops=4 max-ops=64 max-input-words=1 report-file=' + str(wide_report) +
         '})', '-o', materialized])
    assert wide_control.read_bytes() == materialized.read_bytes(), \
        'wide interface fallback changed original IR beyond removing certificates'
    width = json.loads(wide_report.read_text())
    assert width['helpers'] == 0 and width['rejected_interface'] > 0
    invalid = []
    for budget in ('min-ops=0', 'min-ops=8 max-ops=2', 'max-input-words=0'):
        result = run([circt / 'circt-opt', source,
                      '--load-pass-plugin=' + str(library),
                      '--pass-pipeline=builtin.module(chipyard-state-partition-materialize{'
                      + budget + '})', '-o', work / 'invalid.mlir'], expect=1)
        assert 'invalid state partition' in result.stderr
        invalid.append(budget)
    return dict(missing_plan=no_plan, trace_observers=trace,
                wide_interface=width, invalid_budgets_rejected=invalid)


def check_guard_faults(circt, cxx, source, work):
    """Remove real pass checks independently; the executable oracle must fail."""
    original = (SIM / 'compiler/StatePartition.cpp').read_text()
    reports = []
    for label, needle in [
            ('effect-epoch', 'noEventBetween(barriers, from, to)'),
            ('state-version', 'noEventBetween(objects[*index].writes, from, to)')]:
        assert original.count(needle) == 1, (label, 'guard mutation no longer matches')
        bad = work / ('StatePartition_missing_' + label.replace('-', '_') + '.cpp')
        bad.write_text(original.replace(needle, 'true /* intentionally removed test guard */'))
        library = plugin(circt / 'circt-opt', cxx, work / 'mutation-plugin-cache', bad)
        transformed = work / ('missing-' + label + '.mlir')
        run([circt / 'circt-opt', source, '--load-pass-plugin=' + str(library),
             '--pass-pipeline=builtin.module(chipyard-state-partition-plan,'
             'chipyard-state-partition-materialize{min-ops=5 max-ops=16 max-input-words=4})',
             '-o', transformed], work / ('missing-' + label + '.log'))
        llvm = lower(circt, transformed, work, 'missing-' + label)
        result = execute(circt, cxx, llvm, work, 'missing-' + label, expect=1)
        reports.append(dict(removed_actual_guard=label, oracle_failures=result))
    return reports


def helper_structure(text):
    helpers = re.findall(r'  func\.func private @([^\s(]+)([^\n]*)\{\n(.*?)\n  \}',
                         text, re.S)
    assert helpers
    atomic = [(name, body) for name, _, body in helpers
              if name.startswith('state_partition_atomic_state_region_')]
    assert len(atomic) == 1, 'one-helper old/write/new fixture was not kept together'
    name, body = atomic[0]
    a = re.search(r'(%[^ ]+) = arc.storage.get %[^[]+\[0\]', body)[1]
    reads = list(re.finditer(r'(%[^ ]+) = arc.state_read ' + re.escape(a) + r' :', body))
    write = re.search(r'arc.state_write ' + re.escape(a) + ' =', body)
    assert len(reads) == 2 and reads[0].start() < write.start() < reads[1].start()
    assert 'comb.xor ' + reads[0][1] + ', ' + reads[1][1] in body
    cross = [(name, body) for name, _, body in helpers
             if name.startswith('state_partition_crosshelper_state_region_')]
    assert len(cross) >= 2 and 'arc.state_write' in cross[0][1]
    assert 'comb.xor %arg' in cross[-1][1]
    void_writes = [name for name, header, body in helpers
                   if '->' not in header and 'arc.state_write' in body]
    assert void_writes, 'no effectful void helper was exercised'
    conditional_void = [name for name in void_writes
                        if name.startswith('state_partition_versions_state_region_')]
    assert conditional_void, 'conditional state-write helper was not exercised'
    return dict(helpers=len(helpers), one_helper_old_write_new_old=name,
                cross_helper_regions=[name for name, _ in cross],
                void_write_helpers=void_writes,
                conditional_void_write_helpers=conditional_void)


def check(circt, cxx, work, baseline_only=False):
    work.mkdir(parents=True, exist_ok=True)
    source = work / 'source.mlir'
    source.write_text(fixture())
    baseline = lower(circt, source, work, 'baseline')
    baseline_results = execute(circt, cxx, baseline, work, 'baseline')
    assert baseline_results[0]['output'] == baseline_results[1]['output']
    report = dict(status='baseline_passed', baseline=baseline_results)
    fault_reports = []
    for name, text in fault_sources(source.read_text()).items():
        path = work / ('fault-' + name + '.mlir')
        path.write_text(text)
        llvm = lower(circt, path, work, 'fault-' + name)
        results = execute(circt, cxx, llvm, work, 'fault-' + name, expect=1)
        fault_reports.append(dict(name=name, results=results))
    report['unsafe_IR_faults_detected'] = fault_reports
    if baseline_only:
        (work / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        return report
    implementation = (SIM / 'compiler/StatePartition.cpp').read_bytes()
    implementation_hash = hashlib.sha256(implementation).hexdigest()
    library = plugin(circt / 'circt-opt', cxx, work / 'plugin-cache',
                     SIM / 'compiler/StatePartition.cpp')
    transformed = work / 'transformed.mlir'
    run([circt / 'circt-opt', source, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-plan,'
         'chipyard-state-partition-materialize{'
         'min-ops=2 max-ops=16 max-input-words=4})',
         '-o', transformed], work / 'partition.log')
    text = transformed.read_text()
    assert (text.count('func.call ') > source.read_text().count('func.call ')
            and text.count('func.func') > source.read_text().count('func.func')), \
        'positive fixture did not create partition helpers'
    structure = helper_structure(text)
    candidate = lower(circt, transformed, work, 'candidate')
    candidate_results = execute(circt, cxx, candidate, work, 'candidate')
    assert candidate_results == baseline_results
    optimized_text = run([circt / 'llvm-dis', work / 'candidate-O3.bc', '-o', '-']).stdout
    for helper in structure['void_write_helpers']:
        assert re.search(r'define[^\n]*void @' + re.escape(helper) + r'\(', optimized_text), \
            'effectful void helper disappeared under O3'
    report.update(status='passed', candidate=candidate_results,
                  implementation_sha256=implementation_hash,
                  helper_structure=structure,
                  transformed_sha256=hashlib.sha256(transformed.read_bytes()).hexdigest(),
                  derived_clock=check_clocks(circt, cxx, library, work),
                  stock_parallel=check_parallel(circt, cxx, candidate, work,
                                                candidate_results),
                  fallback=check_fallbacks(circt, library, source, work),
                  actual_pass_faults=check_guard_faults(circt, cxx, source, work))
    assert implementation == (SIM / 'compiler/StatePartition.cpp').read_bytes(), \
        'implementation changed during qualification'
    (work / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--work', type=Path, default=SIM / 'generated-src/state-partition-test')
    parser.add_argument('--baseline-only', action='store_true')
    args = parser.parse_args()
    if args.circt_bin is None:
        found = shutil.which('circt-opt')
        if found:
            args.circt_bin = Path(found).resolve().parent
        else:
            parser.error('source env.sh or supply --circt-bin')
    result = check(args.circt_bin.resolve(), args.cxx, args.work.resolve(),
                   args.baseline_only)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
