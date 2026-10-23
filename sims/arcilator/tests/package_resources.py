#!/usr/bin/env python3
"""Check native package selection and the original Radiance FPnew resources."""
import argparse
from pathlib import Path
import re
import shlex
import subprocess

SIM = Path(__file__).resolve().parent.parent
ROOT = SIM.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/package-resource-tests')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    rules = args.work / 'flags.mk'
    rules.write_text(
        'include ' + str(ROOT / 'generators/radiance/chipyard.mk') +
        '\n.PHONY: flags\nflags:\n'
        '\t$(file >$(flags_out),$(EXTRA_SIM_PREPROC_DEFINES))\n\t@:\n')
    profiles = {}
    for name, simulator, runtime in (('native', 'arcilator', 'native'),
                                     ('hybrid', 'arcilator', 'hybrid'),
                                     ('verilator', 'verilator', '')):
        target = args.work / (name + '.flags')
        subprocess.run([
            'make', '--no-print-directory', '-f', rules, 'flags',
            'base_dir=' + str(ROOT), 'sim_name=' + simulator,
            'ARC_RUNTIME=' + runtime, 'CONFIG=PackageResourceProbeConfig',
            'RADIANCE_EXTERNAL_SRCS=', 'flags_out=' + str(target)
        ],
                       check=True)
        profiles[name] = shlex.split(target.read_text())
    native = profiles['native']
    assert native and all(flag.startswith('+define+')
                          for flag in native), native
    assert '+define+FPU_FPNEW' in native
    packages = [
        ROOT /
        'generators/radiance/src/main/resources/vsrc/vortex/third_party/fpnew/src'
        / name for name in ('fpnew_pkg.sv',
                            'fpu_div_sqrt_mvp/hdl/defs_div_sqrt_mvp.sv')
    ]
    for name in ('hybrid', 'verilator'):
        assert [f for f in profiles[name]
                if f.startswith('+define+')] == native
        assert {f
                for f in profiles[name]
                if f.endswith('.sv')} == set(map(str, packages))

    # The selected source can use package names before their source files in
    # the command, as native_hw does for a parameter-specialized top wrapper.
    # Exercise the real resources and their constant-function/type definitions.
    source = args.work / 'probe.sv'
    source.write_text('''module PackageResourceProbe(
      input fpnew_pkg::fp_format_e unused_format,
      output [31:0] single_width, double_width, half_width, tiny_width,
                    alt_width, tiny_exponent, sqrt_exponent);
      localparam int SINGLE = fpnew_pkg::fp_width(fpnew_pkg::FP32);
      localparam int DOUBLE = fpnew_pkg::fp_width(fpnew_pkg::FP64);
      localparam int HALF = fpnew_pkg::fp_width(fpnew_pkg::FP16);
      localparam int TINY = fpnew_pkg::fp_width(fpnew_pkg::FP8);
      localparam int ALT = fpnew_pkg::fp_width(fpnew_pkg::FP16ALT);
      assign single_width = SINGLE;
      assign double_width = DOUBLE;
      assign half_width = HALF;
      assign tiny_width = TINY;
      assign alt_width = ALT;
      assign tiny_exponent = fpnew_pkg::FP_ENCODINGS[fpnew_pkg::FP8].exp_bits;
      assign sqrt_exponent = defs_div_sqrt_mvp::C_EXP_FP64;
    endmodule
    ''')
    imported, folded = args.work / 'imported.mlir', args.work / 'folded.mlir'
    defines = [
        '-D' + value for flag in native
        for value in flag.removeprefix('+define+').split('+') if value
    ]
    subprocess.run([
        args.circt_bin / 'circt-verilog', '--single-unit',
        '--top=PackageResourceProbe', '--ir-hw', '--infer-clock-ports=false',
        *defines, source, *packages, '-o', imported
    ],
                   check=True)
    subprocess.run([
        args.circt_bin / 'circt-opt', imported, '--canonicalize',
        '--symbol-dce', '-o', folded
    ],
                   check=True)
    text = folded.read_text()
    constants = dict(re.findall(r'(%[\w.$-]+) = hw.constant (\d+) : i32',
                                text))
    outputs = re.search(r'hw.output\s+(.+?)\s*:\s*i32', text, re.S)
    assert outputs, text
    values = [int(constants[value.strip()]) for value in outputs[1].split(',')]
    assert values == [32, 64, 16, 8, 16, 5, 11], (values, text)
    print(
        'PASS: native selected-package inputs, retained FPU defines, unchanged '
        'legacy package flags and original FPnew format/exponent constants',
        flush=True)


if __name__ == '__main__':
    main()
