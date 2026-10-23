#!/usr/bin/env python3
"""Build named Chipyard configs and record compilation evidence and failures."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import resource
import subprocess
import tempfile
import time

from coverage import SIM, ROOT, config_catalog, digest, implementation_hashes
from integration import CASES, MAKE_OVERRIDES, execute, make_settings, timestamp


def case_config(name):
    config = CASES[name][0]
    if '.' in config:
        return config
    overrides = MAKE_OVERRIDES.get(name, {})
    package = overrides.get('CONFIG_PACKAGE')
    if package is None and overrides.get('SUB_PROJECT', 'chipyard') == 'chipyard':
        package = 'chipyard'
    return package + '.' + config if package else None


def config_defaults(config):
    matches = [MAKE_OVERRIDES.get(name, {}) for name in CASES if case_config(name) == config]
    if matches and any(settings != matches[0] for settings in matches):
        raise ValueError('integration cases disagree on build defaults for ' + config)
    return dict(matches[0]) if matches else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--configs', nargs='+', help='named configs; defaults to the full main config catalog')
    parser.add_argument('--skip-integration', action='store_true', help='leave configs in the integration suite to that runner')
    parser.add_argument('--stage', choices=('hw', 'llvm', 'simulator'), default='simulator')
    parser.add_argument('--runtime', choices=('native', 'hybrid'), default='native')
    parser.add_argument('--circt-bin', default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--riscv', default=os.environ.get('RISCV'))
    parser.add_argument('--timeout', type=float, default=3600,
                        help='wall seconds per command; 0 disables the wall timeout')
    parser.add_argument('--make-setting', action='append', default=[], metavar='NAME=VALUE',
                        help='repeatable ARC_* build option or compiler tool override, recorded with each case')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not args.circt_bin or not args.riscv:
        parser.error('set CIRCT_BIN and RISCV, or supply --circt-bin and --riscv')
    if not math.isfinite(args.timeout) or args.timeout < 0:
        parser.error('--timeout must be finite and nonnegative')
    try:
        extra_settings = make_settings(args.make_setting)
    except ValueError as error:
        parser.error(str(error))
    timeout = args.timeout or None
    configs = config_catalog()
    selected = ['chipyard.' + name if '.' not in name else name for name in args.configs] if args.configs else list(configs)
    if unknown := set(selected) - configs.keys():
        parser.error(f'configs not in the no-argument main catalog: {sorted(unknown)}')
    if args.skip_integration:
        integration_configs = {case_config(name) for name in CASES}
        selected = [name for name in selected if name not in integration_configs]
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    circt, riscv = Path(args.circt_bin).resolve(), Path(args.riscv).resolve()
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=True)
    else:
        parent = SIM / 'output/config-sweep'
        parent.mkdir(parents=True, exist_ok=True)
        output = Path(tempfile.mkdtemp(
            prefix=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ-'), dir=parent))
    env = dict(os.environ, RISCV=str(riscv), PATH=str(riscv / 'bin') + os.pathsep + os.environ['PATH'])
    for key in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'):
        env.pop(key, None)
    base = ['make', 'SUB_PROJECT=chipyard', 'MODEL_PACKAGE=chipyard.harness', 'MODEL=TestHarness',
            f'CIRCT_BIN={circt}', f'ARC_RUNTIME={args.runtime}',
            *(['VERILATOR=/not-installed/verilator'] if args.runtime == 'native' else [])]
    report = dict(started_at=timestamp(), runtime=args.runtime, stage=args.stage, status='running',
                  wall_timeout_seconds=timeout, make_settings=extra_settings,
                  interpretation='Compilation results do not establish runtime correctness.',
                  arcilator_version=subprocess.check_output([circt / 'arcilator', '--version'], text=True),
                  cases=[dict(config=name, status='queued', source=configs[name]['source'],
                              make_overrides=dict(config_defaults(name), **extra_settings), wall_timeout_seconds=timeout,
                              config_source_sha256=configs[name]['source_sha256']) for name in selected])
    report_path = output / 'report.json'
    def save():
        temporary = report_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(report_path)
    save()
    print(f'Report: {report_path}', flush=True)
    try:
        # Build shared prerequisites once before any config. Each make then
        # uses the same assembled generator and memory-model library.
        execute([*base, *(f'{name}={value}' for name, value in extra_settings.items()),
                 'CONFIG_PACKAGE=chipyard', 'CONFIG=RocketConfig', ROOT / '.classpath_cache/chipyard.jar',
                 ROOT / '.classpath_cache/tapeout.jar', ROOT / 'tools/DRAMSim2/libdramsim.a'],
                output / 'prepare.log', timeout, env)
        for index, case in enumerate(report['cases'], 1):
            if (output / 'STOP').exists():
                report['status'] = 'stopped'
                break
            package, name = case['config'].rsplit('.', 1)
            directory = output / case['config'].removeprefix('chipyard.')
            directory.mkdir(exist_ok=True)
            overrides = case['make_overrides']
            model_package = overrides.get('MODEL_PACKAGE', 'chipyard.harness')
            model = overrides.get('MODEL', 'TestHarness')
            build = SIM / 'generated-src' / f'{model_package}.{model}.{name}' / ('arcilator-native' if args.runtime == 'native' else 'arcilator')
            prefix = 'simulator-native' if args.runtime == 'native' else 'simulator'
            simulator = SIM / f'{prefix}-{model_package}-{name}'
            target = {'hw': build / 'model.mlir', 'llvm': build / 'model.ll', 'simulator': simulator}[args.stage]
            before = implementation_hashes(args.runtime)
            case.update(status='running', started_at=timestamp(), source_sha256=before)
            save()
            print(f'[{index}/{len(selected)}] {name}: building {args.stage}', flush=True)
            started = time.monotonic()
            try:
                execute([*base, f'CONFIG_PACKAGE={package}', f'CONFIG={name}',
                         *(f'{key}={value}' for key, value in overrides.items()), target],
                        directory / 'build.log', timeout, env)
                if implementation_hashes(args.runtime) != before or digest(ROOT / case['source']) != case['config_source_sha256']:
                    case.update(status='source_changed', error='sources changed during this build; remaining builds stopped')
                    report['status'] = 'source_changed'
                    break
                if not target.is_file() or not target.stat().st_size:
                    raise RuntimeError(f'missing or empty compiler output: {target}')
                if args.stage == 'simulator':
                    with target.open('rb') as stream:
                        if stream.read(4) != b'\x7fELF':
                            raise RuntimeError('simulator output is not an ELF executable')
                partition = json.loads((build / 'partition.json').read_text())
                case.update(status='compiled', artifact=str(target), artifact_sha256=digest(target),
                            partition_sha256=digest(build / 'partition.json'),
                            foreign_modules=dict(Counter(inst['module'] for inst in partition['instances'])),
                            phase_clocks=len(partition['phase_clocks']), analog_nets=len(partition['analog_nets']))
            except KeyboardInterrupt:
                case.update(status='interrupted', error='interrupted before compilation completed')
                raise
            except Exception as error:
                case.update(status='failed', error=str(error))
            finally:
                case.update(completed_at=timestamp(), elapsed_seconds=round(time.monotonic() - started, 3))
                save()
            print(f'{name}: {case["status"]}' + (': ' + case['error'] if 'error' in case else ''), flush=True)
        else:
            report['status'] = 'completed'
    except KeyboardInterrupt:
        report['status'] = 'interrupted'
        raise
    except Exception as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        report['completed_at'] = timestamp()
        save()
    return int(report['status'] != 'completed' or any(case['status'] != 'compiled' for case in report['cases']))


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
