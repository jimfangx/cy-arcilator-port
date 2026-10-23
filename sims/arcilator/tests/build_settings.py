#!/usr/bin/env python3
"""Verify incremental native builds with real Rocket hardware in a fresh checkout."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess

SIM = Path(__file__).resolve().parent.parent
ROOT = (SIM.parent.parent / 'common.mk').resolve().parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(work, circt, riscv, cxx, before_makefile=None):
    work.mkdir(parents=True)
    checkout = work / 'chipyard'
    sim = checkout / 'sims/arcilator'
    (checkout / 'sims').mkdir(parents=True)
    for name in ('build.sbt', 'common.mk', 'fpga', 'generators', 'scripts',
                 'tests', 'tools', 'variables.mk', 'vlsi'):
        (checkout / name).symlink_to(ROOT / name,
                                     target_is_directory=(ROOT /
                                                          name).is_dir())
    shutil.copytree(SIM,
                    sim,
                    ignore=shutil.ignore_patterns('generated-src', 'output',
                                                  'simulator-*',
                                                  '__pycache__'))
    (sim / 'generated-src').mkdir()
    (sim / 'generated-src/compiler').symlink_to(
        ROOT / 'sims/arcilator/generated-src/compiler',
        target_is_directory=True)
    hashes = runpy.run_path(str(sim /
                                'tests/coverage.py'))['implementation_hashes']
    implementation = hashes('native')
    env = dict(os.environ, RISCV=str(riscv))
    env['PATH'] = str(riscv / 'bin') + os.pathsep + env['PATH']
    for key in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'):
        env.pop(key, None)
    # Pin the test settings rather than inheriting another Make's tuning knobs.
    defaults = dict(base_dir=str(ROOT),
                    CONFIG='RocketConfig',
                    ARC_RUNTIME='native',
                    CIRCT_BIN=str(circt),
                    CXX=str(cxx),
                    ARC_MODEL_OPS='16384',
                    ARC_SHARED_PROCEDURES='1',
                    ARC_OUTPUT_CONVERGENCE='1',
                    ARC_PARTITION_OPS='4096',
                    ARC_PARTITION_PORTS='16384',
                    ARC_OPT_LEVEL='1',
                    ARC_LLC_OPT_LEVEL='0',
                    ARC_BUILD_JOBS='2',
                    ARC_CFLAGS='',
                    VERILATOR='/not-installed/verilator')
    native = sim / 'generated-src/chipyard.harness.TestHarness.RocketConfig/arcilator-native'
    executable = sim / 'simulator-native-chipyard.harness-RocketConfig'
    tracked = ('design.mlir', 'native-design.mlir', 'model.mlir', 'model.ll',
               'model.json', 'model.bc', 'model.o', 'native-main.cpp')
    report = dict(
        status='running',
        runtime='native',
        source_sha256=implementation,
        scope='Real native Rocket builds in a fresh isolated checkout. '
        'Verify option invalidation, unchanged and scheduling-only '
        'invocations, baseline artifact recovery and CPU execution. '
        'Original drivers; Verilator unavailable.',
        steps=[],
        cases=[])

    def save():
        report['updated_at'] = datetime.now(timezone.utc).isoformat()
        p = work / 'report.tmp'
        p.write_text(json.dumps(report, indent=2) + '\n')
        p.replace(work / 'report.json')

    def execute(command, name):
        with (work / (name + '.log')).open('w') as stream:
            stream.write('COMMAND: ' + json.dumps(list(map(str, command))) +
                         '\n')
            stream.flush()
            subprocess.run(command,
                           cwd=sim,
                           env=env,
                           stdout=stream,
                           stderr=subprocess.STDOUT,
                           check=True)

    def snapshot():
        paths = {name: native / name for name in tracked}
        paths['executable'] = executable
        paths.update({p.name: p for p in native.glob('*-settings.json')})
        return {
            name: dict(mtime_ns=p.stat().st_mtime_ns, sha256=digest(p))
            for name, p in paths.items()
        }

    def build(name, changes=None, makefile=None):
        values = dict(defaults, **(changes or {}))
        command = [
            'make', *(['-f', str(makefile)] if makefile else []), 'native',
            *(key + '=' + value for key, value in values.items())
        ]
        report['active_step'] = name
        save()
        execute(command, name)
        state = snapshot()
        report['steps'].append(
            dict(name=name, settings=values, artifacts=state))
        save()
        return state

    def unchanged(before, after, names=None):
        for name in names or before:
            if before[name] != after[name]:
                raise AssertionError('unexpected rebuild: ' + name)

    def changed(before, after, names):
        for name in names:
            if before[name]['mtime_ns'] == after[name]['mtime_ns']:
                raise AssertionError('stale build output: ' + name)

    def boot(name, values):
        binary = work / 'hello.riscv'
        if not binary.exists():
            execute([
                riscv / 'bin/riscv64-unknown-elf-gcc', '-O2', '-std=gnu11',
                '-march=rv64gc', '-mabi=lp64d', '-mcmodel=medany', '-static',
                '-specs=htif_nano.specs', '-T', 'htif.ld', '-Wl,--wrap=atexit',
                ROOT / 'tests/hello.c', ROOT / 'tests/atexit-workaround.c',
                '-o', binary
            ], 'hello-build')
        symbols = subprocess.check_output(['nm', '-C', executable], text=True)
        if re.search(r'\bVerilated\w*::', symbols):
            raise AssertionError(
                'native executable contains Verilated symbols')
        execute([
            executable, '+permissive', '+max-cycles=1000000',
            '+loadmem=' + str(binary), '+dramsim_ini_dir=' +
            str(ROOT /
                'generators/testchipip/src/main/resources/dramsim2_ini'),
            '+dramsim', '+permissive-off', binary
        ], name)
        text = (work / (name + '.log')).read_text(errors='replace')
        match = re.search(r'PASSED.*?after\s+(\d+) simulation cycles', text)
        if not match or 'Hello world from core 0' not in text:
            raise AssertionError('Rocket did not complete hello: ' + name)
        report['cases'].append(
            dict(case=name,
                 config='RocketConfig',
                 status='passed',
                 completed_at=datetime.now(timezone.utc).isoformat(),
                 cycles=int(match[1]),
                 implementation_sha256=implementation,
                 make_overrides=values,
                 simulator_sha256=digest(executable),
                 partition_sha256=digest(native / 'partition.json'),
                 binary_sha256=digest(binary),
                 verilated_symbols_absent=True))
        save()

    try:
        save()
        if before_makefile:
            report['before_makefile_sha256'] = digest(before_makefile)
            before = build('before-baseline', makefile=before_makefile)
            stale = build('before-changed-budget', {'ARC_MODEL_OPS': '16384'},
                          makefile=before_makefile)
            unchanged(before, stale)
            report['before_bug_reproduced'] = True
        baseline = build('baseline')
        repeat = build('identical')
        unchanged(baseline, repeat)
        unchanged(repeat, build('scheduling-only', {'ARC_BUILD_JOBS': '1'}))
        disabled = build('output-convergence-disabled',
                         {'ARC_OUTPUT_CONVERGENCE': '0'})
        unchanged(baseline, disabled, tracked[:3])
        changed(baseline, disabled, tracked[3:] + ('executable', ))
        metadata, = json.loads((native / 'model.json').read_text())
        if 'convergenceOutputs' in metadata:
            raise AssertionError('disabled output certificate was retained')
        unchanged(disabled, build('identical-disabled',
                                  {'ARC_OUTPUT_CONVERGENCE': '0'}))
        boot('convergence-disabled-hello',
             dict(defaults, ARC_OUTPUT_CONVERGENCE='0'))
        enabled = build('output-convergence-restored')
        unchanged(baseline, enabled, tracked[:3])
        changed(disabled, enabled, tracked[3:] + ('executable', ))
        for name in tracked:
            if baseline[name]['sha256'] != enabled[name]['sha256']:
                raise AssertionError('convergence restoration changed ' + name)
        tuned_values = dict(ARC_MODEL_OPS='4096')
        tuned = build('model-budget', tuned_values)
        unchanged(baseline, tuned,
                  ('design.mlir', 'native-design.mlir', 'model.mlir'))
        changed(baseline, tuned, ('model.ll', 'model.json', 'model.bc',
                                  'model.o', 'native-main.cpp', 'executable'))
        if baseline['model.json']['sha256'] != tuned['model.json']['sha256']:
            raise AssertionError(
                'late outlining changed the model state layout')
        if baseline['model.ll']['sha256'] == tuned['model.ll']['sha256']:
            raise AssertionError(
                'late outlining did not change the LLVM model')
        unchanged(tuned, build('identical-tuned', tuned_values))
        boot('tuned-hello', dict(defaults, **tuned_values))
        optimized_values = dict(tuned_values, ARC_OPT_LEVEL='2')
        optimized = build('llvm-level', optimized_values)
        unchanged(tuned, optimized, tracked[:5] + ('native-main.cpp', ))
        changed(tuned, optimized, ('model.bc', 'model.o', 'executable'))
        boot('optimized-hello', dict(defaults, **optimized_values))
        native_flags = build(
            'native-flags',
            dict(optimized_values, ARC_CFLAGS='-DARC_BUILD_SETTINGS_CHECK=1'))
        unchanged(optimized, native_flags, tracked)
        changed(optimized, native_flags, ('executable', ))
        restored = build('restored')
        for name in tracked:
            if baseline[name]['sha256'] != restored[name]['sha256']:
                raise AssertionError('baseline artifact changed: ' + name)
        boot('restored-hello', defaults)
        if hashes('native') != implementation:
            raise AssertionError(
                'simulator sources changed during verification')
        report['status'] = 'passed'
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        raise
    finally:
        save()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--work',
        type=Path,
        required=True,
        help='new directory for the isolated checkout and report')
    parser.add_argument('--circt-bin',
                        type=Path,
                        default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--riscv', type=Path, default=os.environ.get('RISCV'))
    parser.add_argument('--cxx',
                        type=Path,
                        default=os.environ.get('CXX', 'c++'))
    parser.add_argument(
        '--before-makefile',
        type=Path,
        help='optional historical Makefile to reproduce stale options')
    args = parser.parse_args()
    if not args.circt_bin or not args.riscv:
        parser.error('set CIRCT_BIN and RISCV or pass their paths')
    cxx = shutil.which(str(args.cxx))
    if not cxx:
        parser.error('C++ compiler not found: ' + str(args.cxx))
    verify(args.work.resolve(), args.circt_bin.resolve(), args.riscv.resolve(),
           Path(cxx),
           args.before_makefile.resolve() if args.before_makefile else None)
    print('Build settings verified; report: ' + str(args.work / 'report.json'))


if __name__ == '__main__':
    main()
