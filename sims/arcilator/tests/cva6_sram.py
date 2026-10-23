#!/usr/bin/env python3
"""Execute the original CVA6 SRAM with native reference storage and registers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(HERE))
from coverage import implementation_hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--target',
                        choices=('xilinx', 'altera'),
                        default='xilinx')
    parser.add_argument('--build', type=Path)
    parser.add_argument(
        '--rtl-flags',
        default='+define+VERILATOR',
        help="source compatibility guards used by CVA6's preprocessed RTL")
    args = parser.parse_args()
    sdk = args.circt_bin.resolve()
    work = (args.build
            or SIM / ('generated-src/cva6-sram-' + args.target)).resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = SIM.parent.parent / 'generators/cva6/src/main/resources/cva6/vsrc/cva6/src/fpga-support/rtl/SyncSpRamBeNx64.sv'
    paths = [
        HERE / ('cva6_sram.' + suffix) for suffix in ('cpp', 'mlir', 'py')
    ] + [source]
    hashes = {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in paths
    }
    before = implementation_hashes('native')
    report = dict(status='running',
                  target=args.target,
                  rtl_flags=args.rtl_flags,
                  implementation_sha256=before,
                  source_sha256=hashes)
    path = work / 'report.json'

    def run(*command, **kwargs):
        subprocess.run([str(p) for p in command], check=True, **kwargs)

    def save():
        path.write_text(json.dumps(report, indent=2) + '\n')

    save()
    try:
        (work / 'blackboxes.f').write_text(str(source) + '\n')
        flags = ('+define+FPGA_TARGET_ALTERA '
                 if args.target == 'altera' else '') + args.rtl_flags
        run(sys.executable, SIM / 'native_hw.py', '--input',
            HERE / 'cva6_sram.mlir', '--output', work / 'design.mlir',
            '--filelist', work / 'blackboxes.f', '--work', work / 'rtl',
            '--circt-verilog', sdk / 'circt-verilog', '--circt-opt',
            sdk / 'circt-opt', '--flags=' + flags)
        run(sys.executable, SIM / 'partition.py', '--input',
            work / 'design.mlir', '--output', work, '--top', 'CVA6SRAMHarness',
            '--circt-opt', sdk / 'circt-opt')
        run(sys.executable, SIM / 'compiler/model.py', '--input',
            work / 'partitioned.mlir', '--output', work / 'model.ll',
            '--state-file', work / 'model.json', '--arcilator',
            sdk / 'arcilator', '--circt-opt', sdk / 'circt-opt',
            '--mlir-translate', sdk / 'mlir-translate', '--cache',
            SIM / 'generated-src/compiler')
        run(sdk / 'llc', '-O2', '-relocation-model=pic', '-filetype=obj',
            work / 'model.ll', '-o', work / 'model.o')
        run(sys.executable, SIM / 'native.py', work / 'model.json',
            work / 'partition.json', work / 'model.cpp')
        (work / 'drivers.f').write_text(str(work / 'model.cpp') + '\n')
        support = shlex.split(
            subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                                    text=True))
        link = shlex.join([
            '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime', *support,
            '-lz', '-lpthread', '-ldl'
        ])
        flags = shlex.join(
            ['-DARC_NATIVE_NO_MAIN', '-I' + str(sdk.parent / 'include')])
        run(sys.executable, SIM / 'native_build.py', '--filelist',
            work / 'drivers.f', '--main', HERE / 'cva6_sram.cpp', '--model',
            work / 'model.o', '--build',
            work / 'objects', '--output', work / 'test', '--cxx',
            os.environ.get('CXX',
                           'c++'), '--cxxflags=' + flags, '--ldflags=' + link)
        with (work / 'run.log').open('w') as log:
            run(work / 'test', stdout=log, stderr=subprocess.STDOUT)
        if 'PASS: 16384 original CVA6 SRAM comparisons' not in (
                work / 'run.log').read_text():
            raise RuntimeError(
                'SRAM fixture exited before completing all comparisons')
        if implementation_hashes('native') != before or any(
                hashlib.sha256(Path(p).read_bytes()).hexdigest() != h
                for p, h in hashes.items()):
            raise RuntimeError('sources changed during SRAM validation')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        save()
        print('Report:', path, flush=True)


if __name__ == '__main__':
    main()
