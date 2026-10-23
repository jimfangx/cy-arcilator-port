#!/usr/bin/env python3
"""Check time provenance before coroutine scheduler reads are introduced."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path,
                        default=SIM / 'generated-src/time-dependency-guards')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    opt = args.circt_bin / 'circt-opt'
    library = plugin(opt, os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler', SIM / 'compiler/Model.cpp')

    def transform(name, source, pipeline):
        before, after = (args.work / (name + suffix)
                         for suffix in ('.mlir', '-after.mlir'))
        before.write_text(source)
        subprocess.run([str(opt), str(before), '--load-pass-plugin=' + str(library),
                        '--pass-pipeline=builtin.module(' + pipeline + ')',
                        '-o', str(after)], check=True)
        return after.read_text()

    def record(name, source, expected):
        actual = transform(name, source, 'chipyard-record-time-dependencies')
        assert ('chipyard.source_time_dependence = ' + str(expected).lower()) in actual, name

    # The scheduling contract's last yielded value is an absolute i64 wakeup;
    # only UINT64_MAX proves that no future timestamp has been requested.
    def coroutine(wakeup):
        return '''module {
          arc.coroutine.define @worker(%a: i8, %dynamic: i64) -> (i8, i2, i64) {
            %mask = hw.constant 3 : i2
            %time = hw.constant WAKEUP : i64
            arc.coroutine.yield (%a, %mask, %time : i8, i2, i64), ^again
          ^again(%next: i8, %next_dynamic: i64):
            %next_mask = hw.constant 3 : i2
            %next_time = hw.constant WAKEUP : i64
            arc.coroutine.yield (%next, %next_mask, %next_time : i8, i2, i64), ^again
          }
          hw.module @A(in %a: i8, in %time: i64, out value: i8) {
            %result = arc.coroutine.instance @worker(%a, %time) sensitive [true, true] : (i8, i64) -> i8
            hw.output %result : i8
          }
        }'''.replace('WAKEUP', str(wakeup))

    pure = coroutine(-1)
    record('input-events', pure, False)
    for time in (0, 1, 1500, -2):
        record('finite-' + str(time), coroutine(time), True)
    dynamic = pure.replace('%time : i8, i2, i64', '%dynamic : i8, i2, i64')
    dynamic = dynamic.replace('%next_time : i8, i2, i64', '%next_dynamic : i8, i2, i64')
    record('unknown-wakeup', dynamic, True)
    record('arc-time', '''module {func.func @time() -> i64 {
      %context = arc.inferred_context
      %time = arc.current_time %context
      return %time : i64
    }}''', True)
    record('llhd-time', '''module {func.func @time() -> !llhd.time {
      %time = llhd.current_time
      return %time : !llhd.time
    }}''', True)
    record('print-time', '''module {func.func @time() -> !sim.fstring {
      %time = sim.fmt.current_time
      return %time : !sim.fstring
    }}''', True)
    nested = pure[:-1] + '''
      func.func @nested(%state: !arc.coroutine_state<@worker>,
                        %pc: !arc.coroutine_pc<@worker>, %a: i8, %time: i64) {
        %s, %p, %r, %mask, %wake = arc.coroutine.call @worker(%state, %pc, %a, %time) :
          (!arc.coroutine_state<@worker>, !arc.coroutine_pc<@worker>, i8, i64) ->
          (!arc.coroutine_state<@worker>, !arc.coroutine_pc<@worker>, i8, i2, i64)
        return
      }
    }'''
    record('nested-coroutine', nested, True)

    # Run the real state lowering and exporter, proving that a scheduling-only
    # read survives in IR without reclassifying the design as time-dependent.
    lower = 'arc-lower-state,arc-lower-coroutines,arc-lower-arcs-to-funcs,arc.model(arc-allocate-state),arc-lower-clocks-to-funcs'
    reports = []
    for name, prefix, source, expected in (
            ('legacy', '', pure, True),
            ('proven', 'chipyard-record-time-dependencies,', pure, False),
            ('timed', 'chipyard-record-time-dependencies,', coroutine(1500), True)):
        path = (args.work / (name + '.json')).resolve()
        text = transform(name, source, prefix + lower +
                         ',chipyard-export-model-info{output-file=' + json.dumps(str(path)) + '}')
        assert 'arc.current_time' in text
        model, = json.loads(path.read_text())
        assert model['hasTimeReaders'] is expected, name
        reports.append(dict(case=name, hasTimeReaders=model['hasTimeReaders']))
    (args.work / 'report.json').write_text(json.dumps(dict(status='passed',
        cases=reports, provenance_cases=10), indent=2) + '\n')
    print('PASS: input-only, finite/unknown wakeups, explicit Arc/LLHD/print time, '
          'real scheduler lowering and conservative legacy metadata')


if __name__ == '__main__':
    main()
