#!/usr/bin/env python3
"""Check implicit combinational dependencies without turning state into wires."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--circt-bin', type=Path, required=True)
    p.add_argument('--work',
                   type=Path,
                   default=SIM / 'generated-src/procedural-feedback-guards')
    a = p.parse_args()
    a.work.mkdir(parents=True, exist_ok=True)
    library = plugin(a.circt_bin / 'circt-opt', os.environ.get('CXX', 'c++'),
                     SIM / 'generated-src/compiler',
                     SIM / 'compiler/ProceduralFeedback.cpp')

    def lower(name, text, enabled=True):
        source, output = a.work / (name + '.mlir'), a.work / (name +
                                                              '-out.mlir')
        source.write_text(text)
        command = [a.circt_bin / 'circt-opt', source, '-o', output]
        if enabled:
            command += [
                '--load-pass-plugin=' + str(library),
                '--pass-pipeline=builtin.module(chipyard-bind-procedural-feedback)'
            ]
        subprocess.run(command, check=True)
        return output.read_text()

    source = (SIM /
              'tests/native_aggregate_feedback.mlir').read_text().replace(
                  '@NativeAggregateHarness', '@ArcModel')
    first = lower('aggregate', source)
    assert 'bits = 117 : i64' in first and 'chipyard.feedback_wires' in first
    assert first == lower('idempotent', first)
    # Actual storage and clock boundaries must keep their state semantics.
    guards = {
        'pure-loop':
        '''module { hw.module @ArcModel(in %a:i1,out q:i1) {
          %q=comb.xor %a,%q:i1
          hw.output %q:i1
        }}''',
        'register-loop':
        '''module { hw.module @ArcModel(in %clock:i1,out q:i1) {
          %clk=seq.to_clock %clock
          %next=llhd.combinational -> i1 { llhd.yield %q:i1 }
          %q=seq.firreg %next clock %clk:i1
          hw.output %q:i1
        }}''',
        'over-budget':
        '''module { hw.module @ArcModel(in %data:i65537,out q:i65537) {
          %q=llhd.combinational -> i65537 {
            %next=comb.xor %data,%feedback:i65537
            llhd.yield %next:i65537
          }
          %feedback=comb.xor %q,%data:i65537
          hw.output %q:i65537
        }}''',
    }
    for nested in (False, True):
        call = '%q = sim.func.dpi.call @record(%q) clock %clk : (i32) -> i32'
        if nested:
            call = '%q = llhd.combinational -> i32 { %v = sim.func.dpi.call @record(%q) clock %clk : (i32) -> i32\nllhd.yield %v : i32 }'
        guards['clocked-nested-' + str(nested)] = (
            'module { sim.func.dpi private @record(in %data: i32, return result: i32)\n'
            'hw.module @ArcModel(in %clock: i1, out result: i32) {\n'
            '%clk = seq.to_clock %clock\n' + call + '\nhw.output %q : i32\n}}')
    for name, text in guards.items():
        assert lower(name, text) == lower(name + '-baseline', text,
                                          False), name
    print(
        'PASS: 117-bit aggregate feedback binding and idempotence; pure feedback, registers and unsupported widths retain semantics',
        flush=True)


if __name__ == '__main__':
    main()
