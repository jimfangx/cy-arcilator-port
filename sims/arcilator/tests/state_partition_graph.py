#!/usr/bin/env python3
"""Independent semantic and scheduling witnesses for state-dependence graphs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import sys

from state_partition import SIM, lower, run


def pure_functions():
    lines = []
    for count in (3, 4, 8, 12, 20):
        lines += [f'func.func private @graph_work_{count}(%x: i64, %y: i64) -> i64 {{',
                  '%left_amount = hw.constant 7 : i64',
                  '%right_amount = hw.constant 57 : i64']
        previous = '%x'
        for n in range(count):
            stem = f'%work{n}'
            lines += [f'{stem}_constant = hw.constant {0x1739 + n * 71} : i64',
                      f'{stem}_add = comb.add {previous}, %y : i64',
                      f'{stem}_sum = comb.add {stem}_add, {stem}_constant : i64',
                      f'{stem}_left = comb.shl {stem}_sum, %left_amount : i64',
                      f'{stem}_right = comb.shru {stem}_sum, %right_amount : i64',
                      f'{stem}_rotate = comb.or {stem}_left, {stem}_right : i64',
                      f'{stem} = comb.xor {stem}_rotate, %y : i64']
            previous = stem
        lines += [f'return {previous} : i64', '}']
    return '\n'.join(lines)


class Model:
    def __init__(self, name):
        self.lines = [f'arc.model @{name} io !hw.modty<> storageBytes 256 {{',
                      '^bb0(%storage: !arc.storage):']
        for n in range(32):
            self.lines.append(f'%s{n} = arc.alloc_state %storage {{name = "s{n}", '
                              f'offset = {8*n} : i32}} : (!arc.storage) -> !arc.state<i64>')
        self.lines += ['%zero = hw.constant 0 : i64',
                       '%three = hw.constant 3 : i64',
                       '%seven = hw.constant 7 : i64',
                       '%fifty_seven = hw.constant 57 : i64',
                       '%magic = hw.constant 11259375 : i64']

    def read(self, name, slot, tag=None):
        attr = f' {{test.node = "{tag}"}}' if tag else ''
        self.lines.append(f'%{name} = arc.state_read %s{slot}{attr} : <i64>')
        return '%' + name

    def write(self, slot, value, tag=None):
        attr = f' {{test.node = "{tag}"}}' if tag else ''
        self.lines.append(f'arc.state_write %s{slot} = {value}{attr} : <i64>')

    def op(self, name, opcode, args, tag=None):
        attr = f' {{test.node = "{tag}"}}' if tag else ''
        self.lines.append(f'%{name} = comb.{opcode} ' + ', '.join(args) + attr + ' : i64')
        return '%' + name

    def work(self, name, count, left, right, tag=None):
        attr = f' {{test.node = "{tag}"}}' if tag else ''
        self.lines.append(f'%{name} = func.call @graph_work_{count}({left}, {right})'
                          + attr + ' : (i64, i64) -> i64')
        return '%' + name

    def event(self, name, tag, left, right):
        self.lines += [f'%event_tag_{tag} = hw.constant {tag} : i64',
                       f'%{name} = func.call @state_partition_graph_event(%event_tag_{tag}, '
                       f'{left}, {right}) {{test.node = "event_{tag}"}} : (i64, i64, i64) -> i64']
        return '%' + name

    def finish(self):
        return '\n'.join(self.lines + ['}'])


def pressure_reads(model):
    # A realistic independent frontier ensures the optimization cost gate is
    # not accidentally a correctness guard for the missing-dependence mutants.
    return [model.read(f'independent{n}', 10+n) for n in range(6)]


def pressure_uses(model, values):
    for n, value in enumerate(values):
        result = model.op(f'independent_result{n}', 'xor', [value, '%magic'])
        model.write(16+n, result)


def fixture():
    models = []
    m = Model('graph_interleaved')
    old = [m.read(f'old{n}', n, f'interleaved_read_{n}') for n in range(4)]
    incoming = m.read('input', 4)
    current = old[:]
    for n in range(12):
        m.lines.append(f'%k{n} = hw.constant {0x1729 + n*97} : i64')
        x = m.op(f'a{n}_sum', 'add', [current[0], incoming, f'%k{n}'])
        y = m.op(f'b{n}_xor', 'xor', [current[1], incoming])
        z = m.op(f'c{n}_product', 'mul', [current[2], '%three'])
        q = m.op(f'd{n}_salt', 'xor', [incoming, f'%k{n}'])
        aleft = m.op(f'a{n}_left', 'shl', [x, '%seven'])
        bleft = m.op(f'b{n}_left', 'shl', [y, '%seven'])
        csum = m.op(f'c{n}_salt', 'add', [incoming, f'%k{n}'])
        dsum = m.op(f'd{n}_sum', 'add', [current[3], q])
        aright = m.op(f'a{n}_right', 'shru', [x, '%fifty_seven'])
        bright = m.op(f'b{n}_right', 'shru', [y, '%fifty_seven'])
        z = m.op(f'c{n}', 'xor', [z, csum], f'chain_c_{n}')
        dleft = m.op(f'd{n}_left', 'shl', [dsum, '%seven'])
        x = m.op(f'a{n}_rotate', 'or', [aleft, aright])
        y = m.op(f'b{n}_rotate', 'or', [bleft, bright])
        dright = m.op(f'd{n}_right', 'shru', [dsum, '%fifty_seven'])
        x = m.op(f'a{n}', 'xor', [x, incoming], f'chain_a_{n}')
        y = m.op(f'b{n}', 'add', [y, f'%k{n}'], f'chain_b_{n}')
        q = m.op(f'd{n}', 'or', [dleft, dright], f'chain_d_{n}')
        current = [x, y, z, q]
    for n in range(4):
        m.write(n, current[n], f'interleaved_write_{n}')
        value = m.op(f'out{n}', 'xor' if n % 2 == 0 else 'add',
                     [current[n], old[n]])
        m.write(6+n, value)
    models.append(m.finish())

    m = Model('graph_unrelated')
    old_a = m.read('old_a', 0, 'unrelated_old_read')
    incoming = m.read('input', 4)
    m.write(1, incoming, 'unrelated_write_b')
    value = m.work('cone', 20, old_a, incoming, 'unrelated_cone')
    m.write(6, value)
    m.write(7, old_a)
    models.append(m.finish())

    m = Model('graph_raw')
    pressure = pressure_reads(m)
    old_a = m.read('old_a', 0)
    incoming = m.read('input', 4)
    old_b = m.read('old_b', 1)
    old_c = m.read('old_c', 2)
    value = m.work('next', 3, incoming, old_b, 'raw_writer_producer')
    m.write(0, value, 'raw_write_a')
    fresh = m.read('fresh', 0, 'raw_read_a')
    result = m.work('cone', 20, fresh, old_c, 'raw_consumer')
    m.write(6, result)
    both = m.op('old_and_new', 'xor', [old_a, fresh])
    m.write(7, both)
    pressure_uses(m, pressure)
    models.append(m.finish())

    m = Model('graph_war')
    pressure = pressure_reads(m)
    incoming = m.read('input', 4)
    old_b = m.read('old_b', 1)
    old_c = m.read('old_c', 2)
    value = m.work('next', 20, incoming, old_b, 'war_writer_producer')
    old_a = m.read('old_a', 0, 'war_old_read')
    m.write(0, value, 'war_write_a')
    result = m.work('cone', 3, old_a, old_c, 'war_consumer')
    m.write(6, result)
    m.write(7, old_a)
    pressure_uses(m, pressure)
    models.append(m.finish())

    m = Model('graph_waw')
    pressure = pressure_reads(m)
    incoming = m.read('input', 4)
    old_b = m.read('old_b', 1)
    value = m.work('next', 12, old_b, incoming, 'waw_second_producer')
    first_input = m.read('first_input', 2)
    first_value = m.op('first_value', 'add', [first_input, incoming])
    m.write(0, first_value, 'waw_first_write')
    m.write(0, value, 'waw_second_write')
    fresh = m.read('fresh', 0, 'waw_read_final')
    m.write(6, fresh)
    value = m.op('result', 'xor', [incoming, fresh])
    m.write(7, value)
    pressure_uses(m, pressure)
    models.append(m.finish())

    m = Model('graph_branch')
    old_a = m.read('old_a', 0, 'branch_old_read')
    old_b = m.read('old_b', 1)
    old_c = m.read('old_c', 2)
    incoming = m.read('input', 4)
    flag = m.read('flag', 5)
    m.lines += [f'%condition = comb.extract {flag} from 0 : (i64) -> i1',
                'scf.if %condition {']
    m.write(0, incoming, 'branch_then_write')
    value = m.work('then_value', 4, old_a, old_b)
    m.write(6, value)
    m.lines += ['} else {']
    value = m.op('alternate', 'xor', [incoming, '%magic'])
    m.write(0, value, 'branch_else_write')
    value = m.work('else_value', 4, old_a, old_c)
    m.write(6, value)
    m.lines += ['}']
    fresh = m.read('fresh', 0, 'branch_join_read')
    value = m.op('both', 'xor', [old_a, fresh])
    m.write(7, value)
    value = m.event('effect', 17, old_a, fresh)
    m.write(15, value)
    models.append(m.finish())

    m = Model('graph_shared')
    old_a = m.read('old_a', 0)
    old_b = m.read('old_b', 1)
    old_c = m.read('old_c', 2)
    incoming = m.read('input', 4)
    flag = m.read('flag', 5)
    shared = m.work('shared', 12, old_a, old_b, 'shared_producer')
    m.write(0, incoming)
    value = m.op('first_use', 'add', [shared, old_a])
    m.write(6, value)
    value = m.op('new_b', 'xor', [shared, incoming])
    m.write(1, value)
    value = m.op('second_use', 'add', [shared, old_b])
    m.write(7, value)
    m.write(8, shared)
    m.lines += [f'%condition = comb.extract {flag} from 0 : (i64) -> i1',
                'scf.if %condition {']
    value = m.op('external_use', 'xor', [shared, old_c])
    m.write(9, value)
    m.lines += ['}']
    models.append(m.finish())

    m = Model('graph_barriers')
    old_a = m.read('old_a', 0)
    incoming = m.read('input', 4)
    value = m.op('next', 'add', [old_a, incoming])
    m.write(0, value)
    m.lines += [f'%frozen = "llvm.freeze"({old_a}) '
                '{test.node = "freeze_boundary"} : (i64) -> i64']
    value = m.event('first_effect', 19, '%frozen', value)
    m.write(15, value)
    fresh = m.read('fresh', 0)
    value = m.work('cone', 8, old_a, fresh)
    m.write(6, value)
    m.write(7, fresh)
    m.lines += ['%never = hw.constant false', 'scf.if %never {',
                '  "llvm.intr.trap"() {test.node = "trap_boundary"} : () -> ()', '}']
    value = m.event('last_effect', 23, old_a, fresh)
    m.write(14, value)
    models.append(m.finish())
    return '\n'.join(['module {', pure_functions(),
                      'func.func private @state_partition_graph_event(i64, i64, i64) -> i64',
                      *models, '}']) + '\n'


def execute(circt, cxx, llvm, work, label, expect_failure=False):
    reports = []
    for level in ('0', '3'):
        bc = work / (label + '-O' + level + '.bc')
        obj = bc.with_suffix('.o')
        exe = bc.with_suffix('.exe')
        run([circt / 'opt', '-passes=default<O' + level + '>,verify', llvm, '-o', bc],
            work / (label + '-opt-O' + level + '.log'))
        run([circt / 'llc', '-O' + level, '-relocation-model=pic', '-filetype=obj',
             bc, '-o', obj], work / (label + '-llc-O' + level + '.log'))
        run([*shlex.split(cxx), '-O2', '-std=c++17', SIM / 'tests/state_partition_graph.cpp',
             obj, '-o', exe], work / (label + '-link-O' + level + '.log'))
        result = run([exe], work / (label + '-execute-O' + level + '.log'),
                     expect=1 if expect_failure else 0)
        if expect_failure:
            assert result.returncode == 1 and 'graph oracle mismatch' in result.stderr
        reports.append(dict(level=level, returncode=result.returncode,
                            output=result.stdout.strip(), diagnostic=result.stderr.strip()))
    return reports


def descriptor_signature(text):
    """Inspect physical declarations in actual transformed IR, not exported JSON."""
    headers = list(re.finditer(r'^\s*arc.model @([^ ]+)[^\n]*storageBytes (\d+)', text, re.M))
    result = {}
    total = 0
    for n, header in enumerate(headers):
        end = headers[n+1].start() if n+1 < len(headers) else len(text)
        body = text[header.end():end]
        fields = []
        for match in re.finditer(r'arc.alloc_state [^\n]*\{([^}]+)\}[^\n]*!arc.state<([^>]+)>', body):
            attributes = match[1]
            fields.append((re.search(r'name = "([^\"]+)"', attributes)[1],
                           int(re.search(r'offset = (\d+)', attributes)[1]), match[2]))
        result[header[1]] = dict(bytes=int(header[2]), fields=fields)
        total += len(fields)
    assert len(result) == 8 and total == 256, (len(result), total)
    assert text.count('arc.alloc_state ') == 256, 'allocation duplicated in helper'
    return result


def schedule(circt, source, library, work, label):
    output = work / (label + '.mlir')
    report = work / (label + '.json')
    run([circt / 'circt-opt', source, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-plan,'
         'chipyard-state-partition-materialize{graph-schedule=true min-ops=4 '
         'max-ops=64 max-input-words=4 report-file=' + str(report) + '})',
         '-o', output], work / (label + '.log'))
    return output, json.loads(report.read_text())


def dependency_faults(circt, cxx, source, work):
    """Omit actual dependence construction and require a native oracle failure."""
    from partition import plugin
    original = (SIM / 'compiler/StatePartition.cpp').read_text()
    war = ('edge(read, i, WAR); // Mutation witness: preserve the read\'s old version.\n'
           '          overwrittenReads[i].push_back(read);')
    variants = [
        ('RAW', 'edge(write->second, i, RAW);', '(void)identity;'),
        ('WAW', 'edge(previous->second, i, WAW);', '(void)identity;'),
        ('WAR-analysis', war,
         '(void)read; // Deliberately omit WAR edge and its derived priority metadata.')]
    reports = []
    for label, needle, replacement in variants:
        assert original.count(needle) == 1, (label, 'dependency mutation is stale')
        bad = work / ('StatePartition_missing_' + label.replace('-', '_') + '.cpp')
        bad.write_text(original.replace(needle, replacement))
        library = plugin(circt / 'circt-opt', cxx, work / 'mutation-plugin-cache', bad)
        transformed, details = schedule(circt, source, library, work, 'missing-' + label)
        assert details['graph_reordered_operations'] > 0, 'fault did not exercise scheduling'
        llvm = lower(circt, transformed, work, 'missing-' + label)
        result = execute(circt, cxx, llvm, work, 'missing-' + label, True)
        reports.append(dict(omitted_actual_dependency=label, result=result,
                            note=('WAR also omits the priority metadata derived from that edge; '
                                  'the priority heuristic can otherwise mask a missing edge.')
                            if label == 'WAR-analysis' else 'Actual edge construction removed.'))
    return reports


def graph_fallbacks(circt, cxx, source, library, work):
    from partition import plugin
    original = (SIM / 'compiler/StatePartition.cpp').read_text()
    normalized = work / 'graph-normalized.mlir'
    run([circt / 'circt-opt', source, '-o', normalized])
    control_text = normalized.read_bytes()
    needle = ('auto remaining = remainingUses(nodes, indices);\n'
              '    std::set<unsigned> ready;')
    assert original.count(needle) == 1
    bad = work / 'StatePartition_cycle.cpp'
    bad.write_text(original.replace(
        needle, 'if (nodes.size() > 1) {\n'
        '      edge(0, nodes.size() - 1, SSA);\n'
        '      edge(nodes.size() - 1, 0, SSA);\n'
        '    }\n    ' + needle))
    cycle_library = plugin(circt / 'circt-opt', cxx, work / 'mutation-plugin-cache', bad)
    output, cycle = schedule(circt, source, cycle_library, work, 'cycle-fallback')
    assert cycle['graph_fallback_cycles'] > 0 and cycle['helpers'] == 0
    assert cycle['graph_reordered_operations'] == 0
    assert output.read_bytes() == control_text, 'cycle fallback partially changed original IR'

    # Force cancellation at the real cost-acceptance point. Contiguous fallback
    # may still outline, but no executable operation may retain a tentative reorder.
    needle = 'bool accept = moved && improved &&'
    assert original.count(needle) == 1
    bad = work / 'StatePartition_cancel_schedule.cpp'
    bad.write_text(original.replace(needle, 'bool accept = false && moved && improved &&'))
    cancel_library = plugin(circt / 'circt-opt', cxx, work / 'mutation-plugin-cache', bad)
    output, canceled = schedule(circt, source, cancel_library, work, 'canceled-schedule')
    assert canceled['graph_reordered_operations'] == 0
    assert canceled['graph_rejected_cost'] > 0
    for region in canceled['regions']:
        positions = region['original_operation_ordinals']
        assert positions == sorted(positions), 'canceled order leaked into an outlined helper'
    llvm = lower(circt, output, work, 'canceled-schedule')
    canceled_results = execute(circt, cxx, llvm, work, 'canceled-schedule')

    planned = work / 'graph-planned.mlir'
    run([circt / 'circt-opt', source, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-plan)', '-o', planned])
    certificates = []
    for label, modified in [
            ('plan-width', re.sub(r'(chipyard.state_partition.plan = \d+) : i64',
                                  r'\1 : i128', planned.read_text())),
            ('negative-object', re.sub(r'chipyard.state_partition.object = \d+ : i64',
                                      'chipyard.state_partition.object = -1 : i64',
                                      planned.read_text()))]:
        assert modified != planned.read_text()
        before = work / ('bad-certificate-' + label + '.mlir')
        after = work / ('certificate-fallback-' + label + '.mlir')
        report = work / ('certificate-fallback-' + label + '.json')
        before.write_text(modified)
        run([circt / 'circt-opt', before, '--load-pass-plugin=' + str(library),
             '--pass-pipeline=builtin.module(chipyard-state-partition-materialize{'
             'graph-schedule=true min-ops=4 max-ops=64 max-input-words=4 report-file='
             + str(report) + '})', '-o', after])
        details = json.loads(report.read_text())
        assert details['helpers'] == 0 and details['graph_reordered_operations'] == 0
        assert after.read_bytes() == control_text
        certificates.append(dict(case=label, original_IR_exact=True))
    return dict(cycle=cycle, cycle_original_IR_exact=True,
                cancellation=canceled, cancellation_oracle=canceled_results,
                invalid_certificates=certificates)


def parallel_oracle(circt, cxx, llvm, work, expected):
    from codegen import compile_model
    obj = work / 'graph-parallel.o'
    report = work / 'graph-parallel.json'
    compile_model(llvm, obj, circt / 'opt', circt / 'llc', work / 'plugin-cache',
                  cxx=cxx, opt_level='3', llc_opt_level='3', jobs=2,
                  report=report, abi_buffers=False, pipeline='stock', parallel_stock=True)
    details = json.loads(report.read_text())
    assert details['jobs'] == 2 and len(details['partitions']) == 2
    assert obj.with_suffix('.bc').read_bytes() == (work / 'graph-candidate-O3.bc').read_bytes()
    exe = obj.with_suffix('.exe')
    run([*shlex.split(cxx), '-O2', '-std=c++17', SIM / 'tests/state_partition_graph.cpp',
         obj, '-o', exe], work / 'graph-parallel-link.log')
    result = run([exe], work / 'graph-parallel-execute.log')
    assert result.stdout.strip() == expected['output']
    return dict(parts=2, pre_codegen_stock_bitcode_exact=True,
                oracle=result.stdout.strip())


def legacy_graph_oracle(circt, cxx, source, baseline, library, work):
    from state_partition import execute as execute_legacy, check_clocks
    output, details = schedule(circt, source, library, work, 'legacy-graph')
    llvm = lower(circt, output, work, 'legacy-graph')
    result = execute_legacy(circt, cxx, llvm, work, 'legacy-graph')
    assert result == baseline, 'graph changed the stage-one independent oracle'
    clocks = check_clocks(circt, cxx, library, work / 'graph-clocks', graph_schedule=True)
    return dict(original_stage_one_oracle=result, graph=details,
                derived_clock_graph=clocks)


def thin_oracle(circt, cxx, llvm, work, expected):
    """Exercise the actual state-partition ThinLTO path with visible link state."""
    from codegen import compile_model
    source = work / 'graph-link-probes.ll'
    source.write_text(llvm.read_text() + '''
@state_partition_graph_private = internal constant [4 x i64] [i64 17, i64 4886718345, i64 9223372036854775807, i64 11400714819323198485]
@state_partition_graph_mutable = global i64 0
@state_partition_graph_published = constant i64 324508639
define i64 @state_partition_graph_link_probe(i64 %x) {
  %index = and i64 %x, 3
  %pointer = getelementptr [4 x i64], ptr @state_partition_graph_private, i64 0, i64 %index
  %salt = load i64, ptr %pointer
  %old = load i64, ptr @state_partition_graph_mutable
  %sum = add i64 %old, %x
  %new = xor i64 %sum, %salt
  store i64 %new, ptr @state_partition_graph_mutable
  %public = load i64, ptr @state_partition_graph_published
  %r = xor i64 %new, %public
  ret i64 %r
}
define hidden i64 @state_partition_graph_hidden_probe(i64 %x) {
  %r = xor i64 %x, 610839776
  ret i64 %r
}
''')
    target = run([circt / 'llvm-config', '--host-target']).stdout.strip()
    assert target
    reports = []
    for level in ('0', '3'):
        obj = work / ('graph-thin-O' + level + '.o')
        report = work / ('graph-thin-O' + level + '.json')
        compile_model(source, obj, circt / 'opt', circt / 'llc', work / 'plugin-cache',
                      cxx=cxx, opt_level=level, llc_opt_level=level, jobs=2,
                      report=report, abi_buffers=False, pipeline='stock',
                      state_partitioning=True, opt_flags='-mtriple=' + target)
        details = json.loads(report.read_text())
        assert details['state_partitioning'] is True and details['jobs'] == 2
        assert details['optimization_strategy'] == 'thinlto-summary-parallel'
        assert details['bitcode_stage'] == 'verified-pre-partition-seed'
        assert len(details['partitions']) == 2
        exe = obj.with_suffix('.exe')
        run([*shlex.split(cxx), '-O2', '-std=c++17', '-DGRAPH_LINK_PROBES=1',
             SIM / 'tests/state_partition_graph.cpp', obj, '-o', exe],
            work / ('graph-thin-link-O' + level + '.log'))
        result = run([exe], work / ('graph-thin-execute-O' + level + '.log'))
        assert result.stdout.splitlines()[0] == expected[int(level == '3')]['output']
        assert 'PASS link_checks=2048' in result.stdout
        reports.append(dict(level=level, parts=2,
                            optimization_strategy=details['optimization_strategy'],
                            bitcode_stage=details['bitcode_stage'],
                            oracle=result.stdout.strip()))
    return reports


def small_oracle(circt, cxx, llvm, work, label):
    results = []
    for level in ('0', '3'):
        obj = work / (label + '-O' + level + '.o')
        bc = obj.with_suffix('.bc')
        exe = obj.with_suffix('.exe')
        run([circt / 'opt', '-passes=default<O' + level + '>,verify', llvm, '-o', bc])
        run([circt / 'llc', '-O' + level, '-relocation-model=pic', '-filetype=obj',
             bc, '-o', obj])
        run([*shlex.split(cxx), '-O2', '-std=c++17',
             SIM / 'tests/state_partition_graph_lowfrontier.cpp', obj, '-o', exe])
        result = run([exe], work / (label + '-execute-O' + level + '.log'))
        results.append(dict(level=level, output=result.stdout.strip()))
    return results


def check_low_frontier(circt, cxx, library, work):
    source = SIM / 'tests/state_partition_graph_lowfrontier.mlir'
    baseline_llvm = lower(circt, source, work, 'low-frontier-baseline')
    baseline = small_oracle(circt, cxx, baseline_llvm, work, 'low-frontier-baseline')
    text = source.read_text()
    before_effect = text.split('func.call @state_partition_low_frontier_event', 1)[0]
    window_ops = re.findall(r'^\s*(?:%[^\n]+ = (?:arc.state_read|comb\.)|arc.state_write)',
                            before_effect, re.M)
    assert len(window_ops) == 16, 'the full-window witness must contain exactly 16 operations'
    after_effect = text.split('func.call @state_partition_low_frontier_event', 1)[1]
    for n in range(5):
        assert f'arc.state_write %out{n} = %v{n}' in after_effect
    output = work / 'low-frontier-materialized.mlir'
    report_file = work / 'low-frontier-materialized.json'
    run([circt / 'circt-opt', source, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-plan,'
         'chipyard-state-partition-materialize{graph-schedule=true min-ops=4 '
         'max-ops=16 max-input-words=4 report-file=' + str(report_file) + '})',
         '-o', output], work / 'low-frontier-materialized.log')
    details = json.loads(report_file.read_text())
    assert details['partition_selection'] == 'costed-low-frontier'
    assert details['partition_prefix_candidates'] > 1
    assert details['partition_admissible_candidates'] > 0
    materialized = output.read_text()
    original_fields = re.findall(r'name = "([^\"]+)", offset = (\d+)', text)
    candidate_fields = re.findall(r'name = "([^\"]+)", offset = (\d+)', materialized)
    assert candidate_fields == original_fields
    assert materialized.count('arc.alloc_state ') == text.count('arc.alloc_state ')
    assert 'storageBytes 128' in materialized
    helpers = re.findall(r'  func.func private @([^\s(]+)[^\n]*\{\n(.*?)\n  \}',
                         materialized, re.S)
    prefix_helpers = [name for name, body in helpers if 'test.node = "prefix_write"' in body]
    assert len(prefix_helpers) == 1
    prefix = next(region for region in details['regions']
                  if region['helper'] == prefix_helpers[0])
    assert prefix['selection'] == 'costed-prefix'
    assert prefix['candidate_window_operations'] == 16
    assert prefix['selected_prefix_operations'] < 16
    assert prefix['selected_input_words'] <= 4 and prefix['selected_output_words'] <= 4
    assert prefix['operations'] == prefix['selected_prefix_operations']
    candidate_llvm = lower(circt, output, work, 'low-frontier-candidate')
    candidate = small_oracle(circt, cxx, candidate_llvm, work, 'low-frontier-candidate')
    assert candidate == baseline
    rejected = work / 'low-frontier-rejected.mlir'
    rejected_report = work / 'low-frontier-rejected.json'
    normalized = work / 'low-frontier-normalized.mlir'
    run([circt / 'circt-opt', source, '-o', normalized])
    run([circt / 'circt-opt', source, '--load-pass-plugin=' + str(library),
         '--pass-pipeline=builtin.module(chipyard-state-partition-plan,'
         'chipyard-state-partition-materialize{graph-schedule=true min-ops=4 '
         'max-ops=16 max-input-words=1 report-file=' + str(rejected_report) + '})',
         '-o', rejected], work / 'low-frontier-rejected.log')
    no_prefix = json.loads(rejected_report.read_text())
    assert no_prefix['helpers'] == 0
    assert no_prefix['partition_no_admissible_windows'] > 0
    assert rejected.read_bytes() == normalized.read_bytes(), \
        'no-admissible-prefix fallback changed original IR'
    return dict(full_window_operations=16, full_window_required_output_words=5,
                interface_limit_words=4, low_prefix=prefix,
                baseline=baseline, candidate=candidate,
                physical_descriptors_exact=True, selection_report=details,
                no_admissible_window_original_IR_exact=True,
                no_admissible_window_report=no_prefix)


def check(circt, cxx, work, library=None, baseline_only=False,
          legacy_source=None, legacy_baseline=None):
    work.mkdir(parents=True, exist_ok=True)
    source = work / 'graph-source.mlir'
    source.write_text(fixture())
    llvm = lower(circt, source, work, 'graph-baseline')
    baseline = execute(circt, cxx, llvm, work, 'graph-baseline')
    assert baseline[0]['output'] == baseline[1]['output']
    report = dict(status='baseline_passed', baseline=baseline)
    if baseline_only:
        (work / 'graph-result.json').write_text(json.dumps(report, indent=2) + '\n')
        return report
    # The graph implementation supplies the materializer contract; never treat
    # the contiguous stage-one pass as a positive graph-scheduling result.
    if library is None:
        from partition import plugin
        library = plugin(circt / 'circt-opt', cxx, work / 'plugin-cache',
                         SIM / 'compiler/StatePartition.cpp')
    implementation = (SIM / 'compiler/StatePartition.cpp').read_bytes()
    transformed, details = schedule(circt, source, library, work, 'graph-transformed')
    assert details['schema'] == 2 and details['algorithm'] == 'state-dependence-graph'
    assert details['helpers'] > 1
    assert details['graph_reordered_operations'] > 0
    assert details['graph_interface_words_after'] < details['graph_interface_words_before']
    assert details['graph_frontier_words_after'] < details['graph_frontier_words_before']
    assert details['graph_old_consumers_before_overwrite'] > 0
    assert any(any(b <= a for a, b in zip(region['original_operation_ordinals'],
                                        region['original_operation_ordinals'][1:]))
               for region in details['regions']), 'no noncontiguous original operations were co-located'
    text = transformed.read_text()
    assert descriptor_signature(text) == descriptor_signature(source.read_text())
    assert text.count('test.node = "shared_producer"') == 1, 'shared producer duplicated'
    assert text.count('test.node = "freeze_boundary"') == 1, 'freeze cloned or removed'
    assert text.count('test.node = "trap_boundary"') == 1, 'trap cloned or removed'
    llvm = lower(circt, transformed, work, 'graph-candidate')
    candidate = execute(circt, cxx, llvm, work, 'graph-candidate')
    assert candidate == baseline
    report.update(status='passed', candidate=candidate, graph=details,
                  physical_descriptors_exact=True,
                  stock_parallel=parallel_oracle(circt, cxx, llvm, work, candidate[1]),
                  summary_parallel=thin_oracle(circt, cxx, llvm, work, candidate),
                  actual_dependency_faults=dependency_faults(circt, cxx, source, work),
                  fallback=graph_fallbacks(circt, cxx, source, library, work),
                  costed_prefix=check_low_frontier(circt, cxx, library, work),
                  implementation_sha256=hashlib.sha256(implementation).hexdigest(),
                  transformed_sha256=hashlib.sha256(transformed.read_bytes()).hexdigest())
    if legacy_source is not None:
        report['stage_one_fixtures_with_graph'] = legacy_graph_oracle(
            circt, cxx, legacy_source, legacy_baseline, library, work)
    assert implementation == (SIM / 'compiler/StatePartition.cpp').read_bytes(), \
        'implementation changed during graph qualification'
    (work / 'graph-result.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--work', type=Path, default=SIM / 'generated-src/state-partition-graph-test')
    parser.add_argument('--baseline-only', action='store_true')
    args = parser.parse_args()
    if args.circt_bin is None:
        parser.error('set CIRCT_BIN or supply --circt-bin')
    print(json.dumps(check(args.circt_bin.resolve(), args.cxx, args.work.resolve(),
                           baseline_only=args.baseline_only), indent=2))
