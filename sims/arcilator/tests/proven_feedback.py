#!/usr/bin/env python3
"""Prove combinational feedback rewrites and preserve genuine state/oscillation."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/proven-feedback-guards')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    library = plugin(args.circt_bin / 'circt-opt',
                     os.environ.get('CXX',
                                    'c++'), SIM / 'generated-src/compiler')

    def lower(name, text, enabled=True):
        source, target = args.work / (name + '.mlir'), args.work / (
            name + '-lowered.mlir')
        source.write_text(text)
        result = subprocess.run([
            args.circt_bin / 'circt-opt', source, '--load-pass-plugin=' +
            str(library), '--pass-pipeline=builtin.module(' +
            ('chipyard-resolve-feedback,' if enabled else '') +
            'canonicalize)', '-o', target
        ],
                                capture_output=True,
                                text=True)
        assert result.returncode == 0, result.stderr
        return target.read_text(), result.stderr

    text = (HERE / 'native_proven_feedback.mlir').read_text()
    positive, log = lower('selected-words', text)
    assert 'Proved combinational SCC' in log, log
    assert lower('idempotent', positive)[0] == positive
    positive_file = args.work / 'selected-words-lowered.mlir'
    result = subprocess.run([
        args.circt_bin / 'arcilator', positive_file, '--until-after=arc-conv',
        '--emit-mlir', '-o', args.work / 'arc.mlir'
    ],
                            capture_output=True,
                            text=True)
    assert result.returncode == 0, result.stderr
    lower('disabled', text, enabled=False)
    result = subprocess.run([
        args.circt_bin / 'arcilator', args.work / 'disabled-lowered.mlir',
        '--until-after=arc-conv', '--emit-mlir', '-o',
        args.work / 'disabled-arc.mlir'
    ],
                            capture_output=True,
                            text=True)
    assert result.returncode and 'combinational loop detected' in result.stderr, result.stderr
    # The feedback bits can depend on slices of a much wider state/input
    # vector. Unused bits remain unconstrained in the proof.
    wide = text.replace('in %a: i13, in %b: i13', 'in %wide: i1019')
    wide = wide.replace(
        '    %zero =',
        '    %a = comb.extract %wide from 0 : (i1019) -> i13\n    %b = comb.extract %wide from 500 : (i1019) -> i13\n    %zero ='
    )
    _, wide_log = lower('wide-state-input', wide)
    assert 'Proved combinational SCC' in wide_log, wide_log
    for name, body in {
            'retained': '%q = comb.and %a, %q : i1',
            'oscillating': '%q = comb.xor %q, %true : i1',
            'unsupported': '%q = comb.divu %a, %q : i1',
    }.items():
        source = f'''module {{
          hw.module @ArcModel(in %a: i1, out q: i1) {{
            %true = hw.constant true
            {body}
            hw.output %q : i1
          }}
        }}'''
        candidate, log = lower(name, source)
        assert 'Proved combinational SCC' not in log, (name, log)
        baseline, _ = lower(name + '-baseline', source, enabled=False)
        assert candidate == baseline, name
    print(
        'PASS: solver-proved 13-bit feedback, fixed point and initial-state independence, idempotence and disabled-pass failure; retained state, oscillation and unsupported operators unchanged',
        flush=True)


if __name__ == '__main__':
    main()
