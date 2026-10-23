#!/usr/bin/env python3
"""Compile original Gemmini workloads with an explicitly selected hardware header."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
CASES = ('mvin_mvout', 'matmul_os', 'matmul_ws')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(riscv, work, parameters, cases=CASES):
    source = ROOT / 'generators/gemmini/software/gemmini-rocc-tests'
    parameters = parameters.resolve(strict=True)
    if not parameters.is_file():
        raise ValueError(
            'select the gemmini_params.h generated for the hardware')
    if not cases or len(set(cases)) != len(cases) or set(cases) - set(CASES):
        raise ValueError('select distinct supported Gemmini workloads')
    original_headers = {
        str(p): digest(p)
        for p in (source / 'include').rglob('*') if p.is_file()
    }
    if not original_headers:
        raise ValueError('the Gemmini software source checkout is required')
    parameter_hash = digest(parameters)
    sources = {name: source / 'bareMetalC' / (name + '.c') for name in cases}
    source_hashes = {name: digest(p) for name, p in sources.items()}
    workaround = ROOT / 'tests/atexit-workaround.c'
    workaround_hash = digest(workaround)
    # A fresh directory prevents mixing software or parameters from two configs.
    work.mkdir(parents=True)
    report = dict(
        status='building',
        scope='Compile unchanged upstream Gemmini bare-metal workloads with a '
        'private include snapshot and the explicitly supplied hardware parameter '
        'header. No FAST selection. Compilation does not establish hardware '
        'execution or a match between the supplied header and a simulator.',
        parameter_source=str(parameters),
        parameter_sha256=parameter_hash,
        source_headers_sha256=original_headers,
        workaround_sha256=workaround_hash,
        cases=[])

    def save():
        report['updated_at'] = datetime.now(timezone.utc).isoformat()
        temporary = work / 'report.tmp'
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(work / 'report.json')

    try:
        save()
        shutil.copytree(source / 'include', work / 'include')
        shutil.copyfile(parameters, work / 'include/gemmini_params.h')
        for name, program in sources.items():
            binary = work / (name + '.riscv')
            command = [
                riscv / 'bin/riscv64-unknown-elf-gcc', '-O2', '-std=gnu11',
                '-DBAREMETAL=1', '-mcmodel=medany', '-march=rv64gc',
                '-mabi=lp64d', '-static', '-specs=htif_nano.specs', '-T',
                'htif.ld', '-Wl,--wrap=atexit', '-I' + str(work),
                '-I' + str(source), program, workaround, '-lm', '-o', binary
            ]
            with (work / (name + '-build.log')).open('w') as log:
                subprocess.run(command,
                               check=True,
                               stdout=log,
                               stderr=subprocess.STDOUT)
            report['cases'].append(
                dict(name=name,
                     status='compiled',
                     command=list(map(str, command)),
                     source=str(program),
                     source_sha256=source_hashes[name],
                     binary_sha256=digest(binary)))
            save()
        if (any(
                digest(Path(p)) != value
                for p, value in original_headers.items())
                or digest(parameters) != parameter_hash or any(
                    digest(sources[name]) != value
                    for name, value in source_hashes.items())
                or digest(workaround) != workaround_hash):
            raise RuntimeError(
                'Gemmini software inputs changed during compilation')
        report['status'] = 'compiled'
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        raise
    finally:
        report['header_sha256'] = {
            str(p.relative_to(work)): digest(p)
            for p in (work / 'include').rglob('*') if p.is_file()
        }
        save()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--riscv', type=Path, default=os.environ.get('RISCV'))
    parser.add_argument('--work',
                        type=Path,
                        required=True,
                        help='new directory for private headers and binaries')
    parser.add_argument(
        '--parameters',
        type=Path,
        required=True,
        help='gemmini_params.h generated for the selected hardware')
    parser.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    args = parser.parse_args()
    if not args.riscv:
        parser.error('set RISCV or pass --riscv')
    build(args.riscv.resolve(), args.work.resolve(), args.parameters,
          args.cases)
    print('Compiled Gemmini software; report: ' +
          str(args.work / 'report.json'))


if __name__ == '__main__':
    main()
