#!/usr/bin/env python3
"""Check strict provenance and fallbacks at the native wide-arithmetic boundary."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def check(sdk, work, cxx):
    sdk, work = Path(sdk), Path(work)
    work.mkdir(parents=True, exist_ok=True)
    w = work
    lib = str(plugin(sdk / 'circt-opt', cxx, SIM / 'generated-src/compiler',
                     SIM / 'compiler/WideArithmetic.cpp'))
    report = {'status': 'running', 'cases': []}
    def run(name, source, pipeline):
        src = w / (name + '.mlir')
        dst = w / (name + '-after.mlir')
        src.write_text(source)
        command = [str(sdk / 'circt-opt'), str(src), '--load-pass-plugin=' + lib, '--pass-pipeline=builtin.module(' + pipeline + ')', '-o', str(dst)]
        subprocess.run(['/usr/bin/prlimit', '--as=4294967296', '--core=0', '--', *command], check=True, timeout=30, stdout=subprocess.DEVNULL)
        return dst.read_text()
    base = 'module {\n func.func private @chipyard_arc_wide_shift_0_1025(i1025,i1025)->i1025 attributes {chipyard.no_output_readback,chipyard.native_pure_wide_shift}\n hw.module @H(in %d:i1025,in %a:i1025,in %en:i1,in %clk:!seq.clock,out y:i1025) {\n  %y=sim.func.dpi.call @chipyard_arc_wide_shift_0_1025(%d,%a):(i1025,i1025)->i1025\n  hw.output %y:i1025\n }\n}'
    for name, text, yes in [('plain', base, True), ('enabled', base.replace('(%d,%a):', '(%d,%a) enable %en :'), False), ('clocked', base.replace('(%d,%a):', '(%d,%a) clock %clk :'), False), ('clocked-enabled', base.replace('(%d,%a):', '(%d,%a) clock %clk enable %en :'), False), ('mismatched-amount', base.replace('i1025,i1025', 'i1025,i64').replace('in %a:i1025', 'in %a:i64'), False)]:
        after = run(name, text, 'chipyard-restore-wide-arithmetic')
        assert ('comb.shl' in after) == yes and ('sim.func.dpi.call' in after) != yes, name
        report['cases'].append({'name': name, 'status': 'passed', 'restored': yes})
    shift = ' hw.module @H(in %d:i1025,in %a:i1025,out y:i1025) {\n %y=comb.shru %d,%a:i1025\n hw.output %y:i1025\n }\n'
    decl = ' func.func private @chipyard_arc_wide_shift_1_1025(i1025,i1025)->i1025 attributes {chipyard.no_output_readback,chipyard.native_pure_wide_shift}\n'
    for name, d, body, yes in [('fresh', '', shift, True), ('reuse', decl, shift, True), ('missing-markers', decl.replace(' attributes {chipyard.no_output_readback,chipyard.native_pure_wide_shift}', ''), shift, False), ('non-unit', decl.replace('native_pure_wide_shift}', 'native_pure_wide_shift=false}'), shift, False), ('wrong-signature', decl.replace('(i1025,i1025)->i1025', '(i1025,i64)->i1025'), shift, False), ('small', '', shift.replace('1025', '1024'), False), ('defined', decl.replace('private @', '@').replace('(i1025,i1025)', '(%a:i1025,%b:i1025)').rstrip() + ' {return %a:i1025}\n', shift, False)]:
        after = run('lower-' + name, 'module {\n' + d + body + '}\n', 'chipyard-lower-wide-arithmetic')
        assert ('func.call @chipyard_arc_wide_shift_1_1025' in after) == yes, name
        assert ('comb.shru' in after) != yes, name
        report['cases'].append({'name': 'lower-' + name, 'status': 'passed', 'lowered': yes})
    report['status'] = 'passed'
    report['status'] = 'passed'
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path,
                        default=SIM / 'generated-src/wide-arithmetic-guards')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    check(args.circt_bin.resolve(), args.work.resolve(), args.cxx)


if __name__ == '__main__':
    main()
