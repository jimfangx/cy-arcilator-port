#!/usr/bin/env python3
"""Keep the simulation testbench out of native RTL boundary discovery."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/rtl-source-scope-test')
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    (work / 'report.json').write_text('{"status":"incomplete"}\n')
    source = work / 'input.mlir'
    source.write_text('''module {
  hw.module.extern private @UsesNative(in %value: i8, out observed: i8)
  hw.module private @NativeHelper(in %value: i8, out observed: i8) {
    %mask = hw.constant 90 : i8
    %result = comb.xor %value, %mask : i8
    hw.output %result : i8
  }
  hw.module @TestHarness(in %clock: i1, in %reset: i1, in %pad: !hw.inout<i1>, in %value: i8, out observed: i8) {
    %result = hw.instance "foreign" @UsesNative(value: %value: i8) -> (observed: i8)
    hw.output %result : i8
  }
}
''')
    rtl = work / 'UsesNative.sv'
    rtl.write_text(
        '''module UsesNative(input [7:0] value, output [7:0] observed);
  NativeHelper native(.value(value), .observed(observed));
endmodule
''')
    bench = work / 'TestDriver.sv'
    bench.write_text('''module TestDriver;
  tri pad;
  TestHarness dut(.clock(1'b0), .reset(1'b0), .pad(pad), .value(8'd0), .observed());
  initial $fatal(1, "The testbench must not be imported as foreign RTL");
endmodule
''')
    filelist = work / 'blackboxes.f'
    filelist.write_text(str(rtl) + '\n' + str(bench) + '\n')
    output = work / 'design.mlir'
    subprocess.run([
        sys.executable, SIM / 'native_hw.py', '--input', source, '--output',
        output, '--filelist', filelist, '--work', work / 'rtl',
        '--circt-verilog', args.circt_bin / 'circt-verilog', '--circt-opt',
        args.circt_bin / 'circt-opt', '--cxx',
        os.environ.get('CXX', 'c++')
    ],
                   check=True)
    subprocess.run(
        [args.circt_bin / 'circt-opt', output, '-o', work / 'verified.mlir'],
        check=True)
    boundary = (work / 'rtl/native-boundaries.sv').read_text()
    assert 'NativeHelper' in boundary
    assert 'TestHarness' not in boundary
    text = output.read_text()
    assert 'in %pad: !hw.inout<i1>' in text or 'in %pad : !hw.inout<i1>' in text
    imported = json.loads((work / 'rtl.json').read_text())
    assert len(imported) == 1 and imported[0]['module'] == 'UsesNative'
    assert imported[0]['native_modules'] == ['NativeHelper']
    assert imported[0]['boundary_source'] == str(work / 'rtl/ArcRTL_0-boundary.sv')
    paths = [
        source, rtl, bench, filelist, output, SIM / 'native_hw.py',
        Path(__file__)
    ]
    report = dict(status='passed',
                  sha256={
                      str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in paths
                  })
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(
        'PASS: testbench harness with analog ports excluded; actual RTL-to-Chisel '
        'native boundary preserved',
        flush=True)


if __name__ == '__main__':
    main()
