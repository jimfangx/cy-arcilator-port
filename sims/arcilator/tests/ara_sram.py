#!/usr/bin/env python3
"""Execute the original Ara SRAM through its unmodified exports and hardware ports."""
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
    parser.add_argument('--build', type=Path)
    args = parser.parse_args()
    sdk = args.circt_bin.resolve()
    work = (args.build or SIM / 'generated-src/ara-sram').resolve()
    work.mkdir(parents=True, exist_ok=True)
    source = SIM.parent.parent / 'generators/ara/ara/hardware/deps/tech_cells_generic/src/rtl/tc_sram.sv'
    paths = [
        HERE / ('ara_sram.' + suffix) for suffix in ('cpp', 'mlir', 'sv', 'py')
    ] + [source]
    hashes = {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in paths
    }
    before = implementation_hashes('native')
    report = dict(status='running',
                  implementation_sha256=before,
                  source_sha256=hashes)
    path = work / 'report.json'

    def run(*command, **kwargs):
        subprocess.run([str(p) for p in command], check=True, **kwargs)

    def save():
        path.write_text(json.dumps(report, indent=2) + '\n')

    save()
    try:
        (work / 'blackboxes.f'
         ).write_text(str(HERE / 'ara_sram.sv') + '\n' + str(source) + '\n')
        run(sys.executable, SIM / 'native_hw.py', '--input',
            HERE / 'ara_sram.mlir', '--output', work / 'design.mlir',
            '--filelist', work / 'blackboxes.f', '--work', work / 'rtl',
            '--circt-verilog', sdk / 'circt-verilog', '--circt-opt',
            sdk / 'circt-opt', '--flags=+define+ARCILATOR')
        run(sys.executable, SIM / 'partition.py', '--input',
            work / 'design.mlir', '--output', work, '--top',
            'OriginalSramHarness', '--circt-opt', sdk / 'circt-opt')
        run(sys.executable, SIM / 'compiler/partition.py', '--input',
            work / 'partitioned.mlir', '--output', work / 'outlined.mlirbc',
            '--circt-opt', sdk / 'circt-opt', '--cache',
            SIM / 'generated-src/compiler', '--max-ops', '4096',
            '--emit-bytecode', '--native-feedback')
        run(sdk / 'circt-opt', work / 'outlined.mlirbc', '--symbol-dce',
            '--canonicalize', '-o', work / 'model.mlir')
        run(sys.executable, SIM / 'compiler/model.py', '--input',
            work / 'model.mlir', '--output', work / 'model.ll', '--state-file',
            work / 'model.json', '--arcilator', sdk / 'arcilator',
            '--circt-opt', sdk / 'circt-opt', '--mlir-translate',
            sdk / 'mlir-translate', '--cache', SIM / 'generated-src/compiler')
        run(sys.executable, SIM / 'native.py', work / 'model.json',
            work / 'partition.json', work / 'model.cpp', '--filelist',
            work / 'blackboxes.f')
        (work / 'drivers.f').write_text(str(work / 'model.cpp') + '\n')
        support = shlex.split(
            subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                                    text=True))
        link = shlex.join([
            '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime', *support,
            '-lz', '-lpthread', '-ldl'
        ])
        report['levels'] = []
        for level in ('0', '2'):
            run(sdk / 'opt', '-O' + level, work / 'model.ll', '-o',
                work / ('model-O' + level + '.bc'))
            run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
                '-filetype=obj', work / ('model-O' + level + '.bc'), '-o',
                work / ('model-O' + level + '.o'))
            executable = work / ('test-O' + level)
            flags = shlex.join([
                '-O' + level, '-DARC_NATIVE_NO_MAIN',
                '-I' + str(sdk.parent / 'include')
            ])
            run(sys.executable, SIM / 'native_build.py', '--filelist',
                work / 'drivers.f', '--main', HERE / 'ara_sram.cpp', '--model',
                work / ('model-O' + level + '.o'), '--build',
                work / ('objects-O' + level), '--output', executable, '--cxx',
                os.environ.get('CXX', 'c++'), '--cxxflags=' + flags,
                '--ldflags=' + link)
            result = subprocess.run([executable],
                                    capture_output=True,
                                    text=True)
            (work / ('run-O' + level + '.log')).write_text(result.stdout +
                                                           result.stderr)
            result.check_returncode()
            if 'PASS: 16896 original Ara SRAM comparisons' not in result.stdout:
                raise RuntimeError(
                    'Original Ara SRAM fixture exited before completing all comparisons'
                )
            assert 'Verilated' not in subprocess.check_output(
                ['nm', '-C', executable], text=True)
            report['levels'].append(
                dict(optimization=level,
                     comparisons=16896,
                     executable_sha256=hashlib.sha256(
                         executable.read_bytes()).hexdigest(),
                     verilated_symbols_absent=True))
            print(result.stdout, end='', flush=True)
            save()
        if implementation_hashes('native') != before or any(
                hashlib.sha256(Path(p).read_bytes()).hexdigest() != h
                for p, h in hashes.items()):
            raise RuntimeError('sources changed during SRAM validation')
        report['status'] = 'passed'
        report['comparisons'] = 33792
        report['comparisons_per_level'] = 16896
        report[
            'scope'] = 'Original tc_sram memory and exports; full Ara CPU workload remains unverified'
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        save()
        print('Report:', path, flush=True)


if __name__ == '__main__':
    main()
