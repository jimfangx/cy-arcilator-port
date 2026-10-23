#!/usr/bin/env python3
"""Bound actual payload work without expanding aggregate SSA updates."""
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


def aggregate_ir():
    container = '[4096 x i64]'
    lines = ['define void @aggregate_update(ptr %out, ptr %in, i64 %seed, i1 %enabled) {',
             f'  %old = load {container}, ptr %in, align 8']
    value = '%old'
    for i in range(96):
        index = (i * 37 + 13) % 4096
        constant = ((i + 1) * 0x9e3779b97f4a7c15) & ((1 << 64) - 1)
        lines += [f'  %field.{i} = extractvalue {container} %old, {index}',
                  f'  %data.{i} = xor i64 %seed, {constant}',
                  f'  %select.{i} = select i1 %enabled, i64 %data.{i}, i64 %field.{i}',
                  f'  %value.{i} = insertvalue {container} {value}, i64 %select.{i}, {index}']
        value = f'%value.{i}'
    lines += [f'  store {container} {value}, ptr %out, align 8', '  ret void', '}']
    return '\n'.join(lines) + '\n'


def wide_ir():
    lines = ['define void @wide(ptr %out, ptr %in) {',
             '  %old = load i32768, ptr %in, align 1']
    value = '%old'
    for i in range(96):
        lines.append(f'  %value.{i} = add i32768 {value}, 17')
        value = f'%value.{i}'
    lines += [f'  store i32768 {value}, ptr %out, align 1', '  ret void', '}']
    return '\n'.join(lines) + '\n'


def check(circt, cxx, build):
    build.mkdir(parents=True, exist_ok=True)
    cache = sim / 'generated-src/compiler'
    library = plugin(circt / 'opt', cxx, cache, sim / 'compiler/Codegen.cpp')
    counts = {}
    for name, text in [('aggregate', aggregate_ir()), ('wide', wide_ir())]:
        source = build / (name + '.ll')
        source.write_text(text)
        transformed = subprocess.check_output([
            circt / 'opt', '-load-pass-plugin=' + str(library),
            '-passes=chipyard-bound-codegen-blocks,verify', source,
            '-S', '-o', '-'], text=True)
        (build / (name + '.bounded.ll')).write_text(transformed)
        counts[name] = len(re.findall(r'^chipyard\.codegen\.chunk[^:]*:', transformed, re.M))
    assert counts['aggregate'] < 16, counts
    assert counts['aggregate'] > 0, 'large aggregate loads/stores retain their actual payload cost'
    assert counts['wide'] > 32, 'wide integer arithmetic must retain bounded code generation'
    results = []
    for level in ('0', '3'):
        obj = build / ('aggregate-O' + level + '.o')
        compile_model(build / 'aggregate.ll', obj, circt / 'opt', circt / 'llc',
                      cache, cxx, level, level)
        executable = build / ('aggregate-O' + level)
        subprocess.run([*shlex.split(cxx), '-O2', '-std=c++17',
                        sim / 'tests/aggregate_codegen.cpp', obj,
                        '-o', executable], check=True)
        output = subprocess.check_output([executable], text=True)
        assert 'PASS: 1048576 aggregate lane checks' in output, output
        results.append(dict(level=level, lane_checks=1048576))
    result = dict(status='passed', boundary_counts=counts, levels=results,
                  total_lane_checks=2097152, aliasing_and_disabled_updates=True)
    (build / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: aggregate codegen, 2097152 independent lane checks, wide payload guards')
    return result


if __name__ == '__main__':
    check(Path(os.environ['CIRCT_BIN']).resolve(), os.environ.get('CXX', 'c++'),
          sim / 'generated-src/aggregate-codegen-test')
