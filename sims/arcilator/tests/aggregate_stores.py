#!/usr/bin/env python3
"""Check aggregate snapshots, overlapping fields, callbacks and store guards."""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim / 'compiler'))
from codegen import compile_model
from partition import plugin


def source_ir():
    lines = ['%Inner = type { i64, i64 }',
             '%S = type { ' + ', '.join(['i64'] * 128 + ['[4 x %Inner]']) + ' }',
             'declare i64 @external_probe(ptr, ptr, i64)',
             'define void @aggregate_store(ptr %out, ptr %in, ptr %replacement,',
             '                             ptr %snapshot, i64 %seed, i1 %enabled) {',
             '  %old = load %S, ptr %in, align 1',
             '  %tail = load [4 x %Inner], ptr %replacement, align 1']
    value = '%old'
    for i in range(32):
        index = (i * 37 + 13) % 128
        constant = ((i + 1) * 0xd1342543de82ef95) & ((1 << 64) - 1)
        lines += [f'  %old.{i} = extractvalue %S %old, {index}',
                  f'  %data.{i} = xor i64 %seed, {constant}',
                  f'  %choice.{i} = select i1 %enabled, i64 %data.{i}, i64 %old.{i}',
                  f'  %update.{i} = insertvalue %S {value}, i64 %choice.{i}, {index}']
        value = f'%update.{i}'
        if i == 15:
            lines.append(f'  store %S {value}, ptr %snapshot, align 1')
    lines += ['  %token = call i64 @external_probe(ptr %out, ptr %in, i64 %seed)',
              f'  %callback = insertvalue %S {value}, i64 %token, 1',
              '  %nested.first = insertvalue %S %callback, i64 98765, 128, 0, 0',
              '  %parent = insertvalue %S %nested.first, [4 x %Inner] %tail, 128',
              '  %late = add i64 %token, 19',
              '  %nested.last = insertvalue %S %parent, i64 %late, 128, 2, 1',
              '  store %S %nested.last, ptr %out, align 1',
              '  ret void', '}', '''
define void @aggregate_phi(ptr %out, ptr %in, i64 %seed, i1 %keep) {
  %loaded = load %S, ptr %in, align 1
  br i1 %keep, label %left, label %right
left:
  br label %join
right:
  br label %join
join:
  %base = phi %S [%loaded, %left], [zeroinitializer, %right]
''']
    value = '%base'
    for i in range(12):
        lines += [f'  %x.{i} = add i64 %seed, {i}',
                  f'  %v.{i} = insertvalue %S {value}, i64 %x.{i}, {i}']
        value = f'%v.{i}'
    lines += [f'  store %S {value}, ptr %out, align 1', '  ret void', '}']
    return '\n'.join(lines) + '\n'


def guards_ir():
    large = '{ ' + ', '.join(['i64'] * 64) + ' }'
    small = '{ ' + ', '.join(['i64'] * 8) + ' }'
    huge = '{ [4294967296 x i8], ' + ', '.join(['i64'] * 63) + ' }'
    lines = ['%Large = type ' + large, '%Small = type ' + small,
             '%Huge = type ' + huge]
    for name, kind, volatile in [('skip_volatile', 'Large', True),
                                 ('skip_small', 'Small', False)]:
        lines.append(f'define void @{name}(ptr %out, i64 %x) {{')
        value = 'zeroinitializer'
        for i in range(8):
            lines.append(f'  %v.{i} = insertvalue %{kind} {value}, i64 %x, {i}')
            value = f'%v.{i}'
        lines += [f'  store {"volatile " if volatile else ""}%{kind} {value}, ptr %out, align 1',
                  '  ret void', '}']
    lines += ['define void @unsigned_array_index(ptr addrspace(3) %out, i64 %x) {',
              '  %first = insertvalue %Huge zeroinitializer, i8 71, 0, 2147483651']
    value = '%first'
    for i in range(1, 9):
        lines.append(f'  %v.{i} = insertvalue %Huge {value}, i64 %x, {i}')
        value = f'%v.{i}'
    lines += [f'  store %Huge {value}, ptr addrspace(3) %out, align 1',
              '  ret void', '}']
    scalable = '<vscale x 4 x i32>'
    lines += ['%Scalable = type { ' + ', '.join([scalable] * 64) + ' }',
              f'define void @skip_scalable(ptr %out, {scalable} %x) {{']
    value = 'zeroinitializer'
    for i in range(8):
        lines.append(f'  %v.{i} = insertvalue %Scalable {value}, {scalable} %x, {i}')
        value = f'%v.{i}'
    lines += [f'  store %Scalable {value}, ptr %out', '  ret void', '}']
    return '\n'.join(lines) + '\n'


def check(circt, cxx, build):
    build.mkdir(parents=True, exist_ok=True)
    cache = sim / 'generated-src/compiler'
    library = plugin(circt / 'opt', cxx, cache, sim / 'compiler/AggregateStores.cpp')
    transformed = {}
    for name, source in [('semantics', source_ir()), ('guards', guards_ir())]:
        original = build / (name + '.ll')
        original.write_text(source)
        previous = original
        normalized = []
        for step in range(2):
            target = build / (name + f'.step{step}.ll')
            subprocess.run([circt / 'opt', '-load-pass-plugin=' + str(library),
                            '-passes=chipyard-materialize-aggregate-stores,verify',
                            previous, '-S', '-o', target], check=True)
            text = target.read_text()
            normalized.append('\n'.join(x for x in text.splitlines()
                                        if not x.startswith('; ModuleID =')))
            previous = target
        assert normalized[0] == normalized[1], 'aggregate materialization must be idempotent'
        transformed[name] = normalized[0]
    guards = transformed['guards']
    for name in ['skip_volatile', 'skip_small', 'skip_scalable']:
        body = re.search(r'define void @' + name + r'\(.*?\n\}', guards, re.S)[0]
        assert body.count('insertvalue') == 8, name
    assert 'store volatile %Large' in guards
    unsigned = re.search(r'define void @unsigned_array_index\(.*?\n\}', guards, re.S)[0]
    assert 'i64 2147483651' in unsigned and '-2147483645' not in unsigned
    assert 'ptr addrspace(3)' in unsigned and 'insertvalue' not in unsigned
    results = []
    for level in ('0', '3'):
        obj = build / ('semantics-O' + level + '.o')
        compile_model(build / 'semantics.ll', obj, circt / 'opt', circt / 'llc',
                      cache, cxx, level, level)
        executable = build / ('semantics-O' + level)
        subprocess.run([*shlex.split(cxx), '-O2', '-std=c++17',
                        sim / 'tests/aggregate_stores.cpp', obj,
                        '-o', executable], check=True)
        output = subprocess.check_output([executable], text=True)
        assert 'PASS: 243712 aggregate-store lanes, 640 ordered callbacks' in output, output
        results.append(dict(level=level, lane_checks=243712, ordered_callbacks=640))
    result = dict(status='passed', levels=results, total_lane_checks=487424,
                  total_ordered_callbacks=1280, idempotent=True,
                  snapshots_aliases_overlapping_nested_fields_PHI_and_unaligned_buffers=True,
                  volatile_small_struct_scalable_and_unsigned_address_space_guards=True)
    (build / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: aggregate stores, 487424 lane checks and 1280 ordered callbacks')
    return result


if __name__ == '__main__':
    check(Path(os.environ['CIRCT_BIN']).resolve(), os.environ.get('CXX', 'c++'),
          sim / 'generated-src/aggregate-stores-test')
