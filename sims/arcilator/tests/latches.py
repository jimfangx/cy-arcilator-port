#!/usr/bin/env python3
"""Check latch recognition and exclusions before running native RTL tests."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/latch-guards')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    library = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/SignalState.cpp')

    def lower(name, text, feedback=False):
        source, target = args.work / (name + '.mlir'), args.work / (
            name + '-lowered.mlir')
        source.write_text(text)
        subprocess.run([
            args.circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-signal-state' +
            ('{feedback-only=true}' if feedback else '') + ')',
            '-o', target
        ],
                       check=True)
        return target.read_text()

    def source(extra='',
               delay='0ns, 0d, 1e',
               initial='%initial',
               second='',
               data='%data',
               gate='%open'):
        return f'''module {{
          func.func private @effect(i129) -> i129
          func.func private @escape(!llhd.ref<i129>)
          hw.module @Harness(in %clock: !seq.clock, in %open: i1, in %data: i129, out q: i129) {{
            %initial = hw.constant 123456789 : i129
            %time = llhd.constant_time <{delay}>
            %signal = llhd.sig {initial} : <i129>
            {extra}
            llhd.drv %signal, {data} after %time if {gate} : i129
            {second}
            %q = llhd.prb %signal : i129
            hw.output %q : i129
          }}
        }}'''

    blocking = lower('blocking', source())
    assert 'verilogName = "ArcLatch"' in blocking
    assert 'INIT: i129 = 123456789' in blocking, blocking
    assert 'llhd.sig' not in blocking and 'llhd.drv' not in blocking
    assert lower('idempotent', blocking) == blocking
    nba = lower('nba', source(delay='0ns, 1d, 0e'))
    assert 'verilogName = "ArcSignalState"' in nba and '"ArcLatch"' not in nba
    registered = lower(
        'registered-data',
        source(extra='%registered = seq.compreg %data, %clock : i129',
               data='%registered'))
    assert 'verilogName = "ArcLatch"' in registered
    cases = {
        'physical-delay':
        source(delay='1ps, 0d, 0e'),
        'epsilon-delay':
        source(delay='0ns, 0d, 2e'),
        'mixed-delay':
        source(delay='0ns, 1d, 1e'),
        'dynamic-initializer':
        source(initial='%data'),
        'escaped-reference':
        source(extra='func.call @escape(%signal) : (!llhd.ref<i129>) -> ()'),
        'multiple-writers':
        source(second='llhd.drv %signal, %initial after %time : i129'),
        'effectful-data':
        source(extra='%effect = func.call @effect(%data) : (i129) -> i129',
               data='%effect'),
        'effectful-control':
        source(
            extra=
            '%effect = func.call @effect(%data) : (i129) -> i129\n%bit = comb.extract %effect from 0 : (i129) -> i1',
            gate='%bit'),
        'coroutine-data':
        source(extra='''%next = llhd.process -> i129 {
              llhd.wait yield (%data : i129), (%open : i1), ^halt
            ^halt:
              llhd.halt %data : i129
            }''',
               data='%next'),
    }
    for name, text in cases.items():
        lowered = lower(name, text)
        assert 'llhd.sig' in lowered and 'llhd.drv' in lowered, name
        assert '"ArcLatch"' not in lowered, name
    self_feedback = source(extra='%old = llhd.prb %signal : i129\n%next = comb.or %old, %data : i129',
                           data='%next').replace(' if %open', '')
    feedback = lower('feedback-initializer', self_feedback, feedback=True)
    assert 'verilogName = "ArcLatch"' in feedback
    assert 'INIT: i129 = 123456789' in feedback
    assert 'llhd.sig' not in feedback and 'llhd.drv' not in feedback
    assert lower('feedback-idempotent', feedback, feedback=True) == feedback
    feedforward = source().replace(' if %open', '')
    unchanged = lower('feedforward', feedforward, feedback=True)
    assert 'llhd.sig' in unchanged and '"ArcLatch"' not in unchanged
    for name, text in cases.items():
        unchanged = lower('early-' + name, text, feedback=True)
        assert '"ArcLatch"' not in unchanged, name
    print(
        f'PASS: blocking latch recognition, preserved initialization, NBA staging, registered data, idempotence and {len(cases)} delayed/escaped/multiwriter/effectful/coroutine guards',
        flush=True)


if __name__ == '__main__':
    main()
