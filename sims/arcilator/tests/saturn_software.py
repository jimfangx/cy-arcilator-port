#!/usr/bin/env python3
"""Build original Saturn vector workloads without modifying their checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess

SIM = Path(__file__).resolve().parent.parent
ROOT = SIM.parent.parent
BENCHMARKS = ('architecture', 'vec-mixed_width_mask', 'vec-transpose-load', 'vec-sgemm')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(riscv, work, benchmark, check_reference=False):
    if benchmark not in BENCHMARKS:
        raise ValueError('unknown Saturn workload: ' + benchmark)
    work.mkdir(parents=True, exist_ok=True)
    work = work.resolve()
    original = ROOT / 'generators/saturn/benchmarks'
    tools = riscv.resolve() / 'bin'
    env = dict(os.environ, PATH=str(tools) + ':' + os.environ['PATH'])
    for name in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'):
        env.pop(name, None)
    make = ['make', '-B', '-f', str(original / 'Makefile'),
            'src_dir=' + str(original),
            'RISCV_PREFIX=' + str(tools / 'riscv64-unknown-elf-')]
    selected = 'vec-mixed_width_mask' if benchmark == 'architecture' else benchmark
    inputs = [original / 'Makefile']
    inputs += [path for directory in ('common', 'env', selected)
               for path in (original / directory).rglob('*')
               if path.is_file() and path.suffix in ('.c', '.cc', '.h', '.S', '.ld')]
    if benchmark == 'architecture':
        inputs.append(SIM / 'tests/saturn_architecture.c')
    source_hashes = {str(path): digest(path) for path in sorted(set(inputs))}

    def run(command, log, success=True):
        with log.open('w') as stream:
            result = subprocess.run([str(arg) for arg in command], cwd=work,
                                    env=env, stdout=stream, stderr=subprocess.STDOUT)
        if success:
            result.check_returncode()
        return result.returncode

    # Record the upstream command, including its complete original include
    # search and startup sources. No dataset generator or checkout write runs.
    dry = subprocess.check_output([*make, '-n', selected + '.riscv'], cwd=work,
                                  env=env, text=True)
    line, = [line for line in dry.splitlines()
             if line.startswith(str(tools / 'riscv64-unknown-elf-gcc'))]
    command = shlex.split(line)
    binary = work / (benchmark + '.riscv')
    if benchmark == 'architecture':
        c = str(original / selected / (selected + '_main.c'))
        assembly = str(original / selected / (selected + '.S'))
        if c not in command or assembly not in command:
            raise ValueError('unrecognized upstream Saturn build command')
        command = [str(SIM / 'tests/saturn_architecture.c') if token == c else token
                   for token in command if token != assembly]
        # Explicit intrinsics still execute RVV; the diagnostic's checker must
        # use scalar loads and precomputed host bit patterns.
        command.append('-fno-tree-vectorize')
        command[command.index('-o') + 1] = str(binary)
        run(command, work / 'compile.log')
    else:
        run([*make, benchmark + '.riscv'], work / 'compile.log')
    report = dict(benchmark=benchmark, binary=str(binary), binary_sha256=digest(binary),
                  source_sha256=source_hashes, upstream_command=command,
                  original_data_and_startup_retained=True, reference_checked=False)
    if check_reference:
        # This Spike SDK derives VLEN from Zvl, rather than the removed --varch
        # option. The CSR diagnostic also checks the reported length directly.
        reference = [tools / 'spike', '--isa=rv64gcv_zfh_zvfh_zvl256b',
                     '-p1', '-m0x80000000:0x10000000']
        run([*reference, binary], work / 'spike-positive.log')
        if benchmark == 'architecture':
            if 'SATURN_ARCH_OK vlenb=32 fp64=74 i64=37 checks=111' not in (work / 'spike-positive.log').read_text():
                raise ValueError('architecture diagnostic did not finish all checks')
            negative_command = [*command, '-DARC_SATURN_INJECT_FP64_FAULT']
            expected_failure = 111
        elif benchmark in ('vec-mixed_width_mask', 'vec-sgemm'):
            assembly = original / benchmark / (benchmark + '.S')
            before, after, expected_failure = (
                ('vle32.v v4, (a3), v0.t', 'vle32.v v4, (a3)', 2)
                if benchmark == 'vec-mixed_width_mask'
                else ('vfmacc.vf', 'vfnmacc.vf', 1))
            text = assembly.read_text()
            if before not in text or str(assembly) not in command:
                raise ValueError('unrecognized original vector kernel')
            fault = work / (benchmark + '-fault.S')
            fault.write_text(text.replace(before, after))
            negative_command = [str(fault) if token == str(assembly) else token
                                for token in command]
        else:
            negative_command = None
        if negative_command:
            negative = work / (benchmark + '-fault.riscv')
            negative_command[negative_command.index('-o') + 1] = str(negative)
            run(negative_command, work / 'compile-negative.log')
            code = run([*reference, negative], work / 'spike-negative.log', success=False)
            if code != expected_failure:
                raise ValueError(f'wrong injected-fault result: {code}, expected {expected_failure}')
            report['negative_control'] = dict(status='rejected_as_expected', returncode=code,
                                              binary_sha256=digest(negative))
        report.update(reference_checked=True, reference_isa='rv64gcv_zfh_zvfh_zvl256b',
                      reference_status='passed')
    if any(digest(Path(name)) != value for name, value in source_hashes.items()):
        raise ValueError('original Saturn workload source changed during compilation')
    if digest(binary) != report['binary_sha256']:
        raise ValueError('positive workload changed during negative control')
    report['status'] = 'passed'
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--riscv', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--benchmark', choices=BENCHMARKS, required=True)
    parser.add_argument('--check-reference', action='store_true')
    args = parser.parse_args()
    try:
        report = build(args.riscv, args.work, args.benchmark, args.check_reference)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Saturn vector workload: {error}\n')
    print(json.dumps(dict(status=report['status'], benchmark=report['benchmark'],
                         binary=report['binary'], reference_checked=report['reference_checked'])))


if __name__ == '__main__':
    main()
