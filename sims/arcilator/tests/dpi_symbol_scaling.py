#!/usr/bin/env python3
"""Check DPI lowering with large symbol tables and shadowed local symbols."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def worker(wakeup):
    return f'''arc.coroutine.define @worker(%a: i8) -> (i8, i1, i64) {{
      %mask = hw.constant 1 : i1
      %time = hw.constant {wakeup} : i64
      arc.coroutine.yield (%a, %mask, %time : i8, i1, i64), ^again
    ^again(%next: i8):
      %next_mask = hw.constant 1 : i1
      %next_time = hw.constant {wakeup} : i64
      arc.coroutine.yield (%next, %next_mask, %next_time : i8, i1, i64), ^again
    }}'''


def source(symbols=16384, calls=16384, outer_wakeup=-1, inner_wakeup=1):
    lines = ['module {']
    lines += [f'func.func private @padding_{index}(i8) -> i8'
              for index in range(symbols)]
    lines += ['''
      func.func private @external(i1, i8, i13, i65) -> (i1, i8, i13, i65)
      func.func @defined(%a: i1) -> i1 { return %a : i1 }
      func.func @wrapper(%a: i1, %b: i8, %c: i13, %d: i65) -> (i1, i8, i13, i65) {
        %bit = func.call @defined(%a) : (i1) -> i1
        %r:4 = func.call @external(%bit, %b, %c, %d) : (i1, i8, i13, i65) -> (i1, i8, i13, i65)
        return %r#0, %r#1, %r#2, %r#3 : i1, i8, i13, i65
      }
      func.func @driver(%a: i1, %b: i8, %c: i13, %d: i65) -> (i1, i8, i13, i65) {
    ''']
    previous = '%a, %b, %c, %d'
    for index in range(calls):
        lines.append(f'%r{index}:4 = sim.func.dpi.call @wrapper({previous}) '
                     f'{{chipyard.test_order = {index} : i32}} : '
                     '(i1, i8, i13, i65) -> (i1, i8, i13, i65)')
        previous = ', '.join(f'%r{index}#{result}' for result in range(4))
    lines += [f'return {previous} : i1, i8, i13, i65', '}', '''
      func.func @gated(%a: i1, %b: i8, %c: i13, %d: i65) -> (i1, i8, i13, i65) {
        %clk = seq.to_clock %a
        %r:4 = sim.func.dpi.call @wrapper(%a, %b, %c, %d) clock %clk enable %a :
          (i1, i8, i13, i65) -> (i1, i8, i13, i65)
        %s:4 = sim.func.dpi.call @wrapper(%r#0, %r#1, %r#2, %r#3) enable %a :
          (i1, i8, i13, i65) -> (i1, i8, i13, i65)
        return %s#0, %s#1, %s#2, %s#3 : i1, i8, i13, i65
      }
    ''', worker(outer_wakeup)]
    if outer_wakeup == -1:
        lines.append('hw.module @ManyObservers(in %a: i8, out value: i8) {')
        previous = '%a'
        for index in range(calls):
            lines.append(f'%coro{index} = arc.coroutine.instance @worker({previous}) '
                         'sensitive [true] : (i8) -> i8')
            previous = f'%coro{index}'
        lines += [f'hw.output {previous} : i8', '}']
    lines += ['''
      module @nested {
        func.func private @external(i8) -> i8
        func.func @wrapper(%a: i8) -> i8 {
          %r = func.call @external(%a) : (i8) -> i8
          return %r : i8
        }
        func.func @driver(%a: i8) -> i8 {
          %r = sim.func.dpi.call @wrapper(%a) : (i8) -> i8
          return %r : i8
        }
    ''', worker(inner_wakeup), '''
        hw.module @Observer(in %a: i8, out value: i8) {
          %r = arc.coroutine.instance @worker(%a) sensitive [true] : (i8) -> i8
          hw.output %r : i8
        }
      }
    }''']
    return '\n'.join(lines) + '\n'


def check(circt_bin, cxx, build):
    build.mkdir(parents=True, exist_ok=True)
    opt = circt_bin / 'circt-opt'
    library = plugin(opt, cxx, SIM / 'generated-src/compiler',
                     SIM / 'compiler/Model.cpp')
    reports = []
    for name, count, outer, inner, expected in (
            ('large', 16384, -1, 1, True),
            ('shadowed-passive', 32, 1500, -1, False)):
        before, after = build / (name + '.mlir'), build / (name + '-after.mlir')
        before.write_text(source(count, count, outer, inner))
        start = time.monotonic()
        subprocess.run([str(opt), str(before), '--load-pass-plugin=' + str(library),
                        '--pass-pipeline=builtin.module(chipyard-lower-procedural-dpi,chipyard-record-time-dependencies)',
                        '-o', str(after)], check=True)
        actual = after.read_text()
        # Every data-dependent call and its order marker survives unchanged.
        order = [int(value) for value in re.findall(r'chipyard.test_order = (\d+) : i32', actual)]
        assert order == list(range(count)), name
        assert actual.count('sim.func.dpi.call') == 2, name
        assert actual.count('call @wrapper(') == count + 3, name
        external = re.findall(r'func.func private @external\([^\n]+', actual)
        assert external == [
            'func.func private @external(i1 {llvm.zeroext}, i8, i13, i65) -> (i1 {llvm.zeroext}, i8, i13, i65)',
            'func.func private @external(i8) -> i8'], external
        assert re.search(r'func.func @defined\(%\w+: i1\) -> i1 \{', actual)
        assert ('chipyard.source_time_dependence = ' + str(expected).lower()) in actual
        reports.append(dict(case=name,unrelated_symbols=count,ordered_DPI_calls=count,
                            repeated_coroutine_instances=count if outer == -1 else 0,
                            source_time_dependence=expected,elapsed_seconds=time.monotonic()-start))
    (build / 'report.json').write_text(json.dumps(dict(status='passed', cases=reports), indent=2) + '\n')
    print('PASS: 16416 ordered DPI calls, large scoped symbol tables, exact scalar ABI attributes, '
          'wide results, gated calls and shadowed coroutine wakeups')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--work', type=Path, default=SIM / 'generated-src/dpi-symbol-scaling')
    args = parser.parse_args()
    if args.circt_bin is None:
        parser.error('set CIRCT_BIN or supply --circt-bin')
    check(args.circt_bin, os.environ.get('CXX', 'c++'), args.work)
