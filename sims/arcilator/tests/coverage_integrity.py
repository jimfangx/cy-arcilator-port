#!/usr/bin/env python3
"""Exercise the coverage CLI with isolated, explicitly synthetic evidence."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--coverage', type=Path,
                        default=Path(__file__).with_name('coverage.py'))
    parser.add_argument('--work', type=Path)
    args = parser.parse_args()
    work = args.work or Path(tempfile.mkdtemp(prefix='coverage-integrity-'))
    work.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('coverage_under_test', args.coverage)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    hashes = module.implementation_hashes('native')
    configs = sorted(module.config_catalog())
    assert len(configs) >= 10
    extra = dict(hashes, **{'synthetic/changed-config.scala': 'a' * 64})
    missing = dict(hashes)
    missing.pop(next(iter(missing)))
    changed = dict(hashes)
    changed[next(iter(changed))] = 'b' * 64
    report = dict(status='running', scope='Synthetic analyzer fixtures only; '
                  'these records are not simulator or hardware evidence.', cases=[])

    def save():
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    def invoke(name, runtime_cases, build_cases, valid=True):
        directory = work / name
        runtime = directory / 'synthetic-runtime' / 'fixture'
        builds = directory / 'synthetic-builds' / 'fixture'
        runtime.mkdir(parents=True)
        builds.mkdir(parents=True)
        for target, cases in ((runtime, runtime_cases), (builds, build_cases)):
            (target / 'report.json').write_text(json.dumps(dict(
                runtime='native', stage='simulator', synthetic=True,
                source_sha256=hashes, cases=cases), indent=2) + '\n')
        output = directory / 'analysis.json'
        result = subprocess.run([sys.executable, str(args.coverage), '--runtime', 'native',
                                 '--reports', str(runtime.parent), '--build-reports', str(builds.parent),
                                 '--output', str(output)], capture_output=True, text=True)
        (directory / 'stdout.log').write_text(result.stdout)
        (directory / 'stderr.log').write_text(result.stderr)
        if valid:
            assert result.returncode == 0, result.stderr
            return json.loads(output.read_text())
        assert result.returncode != 0 and 'incomplete ' in result.stderr, result.stderr
        assert not output.exists(), 'rejected evidence must not produce a success report'

    def runtime(index, fingerprint):
        case = dict(config=configs[index], case=f'synthetic-{index}', status='passed',
                    completed_at='2000-01-01T00:00:00Z', cycles=1,
                    simulator_sha256='c' * 64, partition_sha256='d' * 64)
        if fingerprint is not None:
            case['implementation_sha256'] = fingerprint
        return case

    def build(index, fingerprint):
        return dict(config=configs[index], status='compiled',
                    completed_at='2000-01-01T00:00:00Z', artifact_sha256='e' * 64,
                    partition_sha256='f' * 64, source_sha256=fingerprint)

    save()
    runtime_cases = [runtime(i, fp) for i, fp in enumerate((hashes, extra, missing, changed, None))]
    failed = runtime(5, hashes)
    failed['status'] = 'failed'
    runtime_cases.append(failed)
    build_cases = [build(i + 6, fp) for i, fp in enumerate((hashes, extra, missing, changed))]
    result = invoke('classification', runtime_cases, build_cases)
    entries = {entry['config']: entry for entry in result['configs']}
    for index, status in enumerate(('runtime_tested_current', 'runtime_tested_historical',
                                    'runtime_tested_historical', 'runtime_tested_historical',
                                    'runtime_tested_current', 'unverified')):
        assert entries[configs[index]]['status'] == status, (configs[index], entries[configs[index]])
    for index, current in enumerate((True, False, False, False), 6):
        assert entries[configs[index]]['build_evidence'][0]['current_implementation'] is current
    assert result['summary'] == dict(named_configs=len(configs), runtime_tested_current=2,
        runtime_tested_historical=3, unverified=len(configs) - 5,
        simulator_compiled_current=3, simulator_compiled_historical=6), result['summary']
    report['cases'].append(dict(case='exact_extra_missing_changed_fallback_failed', status='passed'))
    save()
    for kind in ('runtime', 'build'):
        for key in ('completed_at', 'partition_sha256',
                    'simulator_sha256' if kind == 'runtime' else 'artifact_sha256'):
            record = runtime(0, hashes) if kind == 'runtime' else build(0, hashes)
            record[key] = '' if key == 'completed_at' else 'not-a-sha256'
            invoke(f'incomplete-{kind}-{key}', [record] if kind == 'runtime' else [],
                   [record] if kind == 'build' else [], valid=False)
            report['cases'].append(dict(case=f'reject-{kind}-{key}', status='passed'))
            save()
    report.update(status='passed', catalog_configs=len(configs), implementation_files=len(hashes))
    save()
    print('Coverage integrity report:', work / 'report.json', flush=True)


if __name__ == '__main__':
    main()
