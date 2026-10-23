#!/usr/bin/env python3
"""Compare named Chipyard configs with recorded runtime integration evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re

SIM = Path(__file__).resolve().parent.parent
ROOT = SIM.parent.parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def implementation_hashes(runtime='hybrid'):
    # These files determine the compiled simulator. Changes to the test-case
    # selection alone do not invalidate a previously completed hardware test.
    implementation = [ROOT / 'generators/chipyard/src/main/scala/example/CustomChipTop.scala', ROOT / 'common.mk', ROOT / 'generators/rocket-chip/src/main/resources/csrc/debug_rob.cc',
                      ROOT / 'generators/radiance/chipyard.mk',
                      ROOT / 'scripts/insert-includes.py',
                      ROOT / 'generators/caliptra-aes-acc/src/main/resources/vsrc/aes/aes_cipher_core/Makefile']
    implementation += [SIM / name for name in ('Makefile', 'frontend.py', 'partition.py', 'settings.py')]
    bootrom = ROOT / 'generators/testchipip/src/main/resources/testchipip/bootrom'
    implementation += [bootrom / name for name in (
        'bootrom.S', 'linker.ld', 'Makefile', 'bootrom.rv32.img', 'bootrom.rv64.img')]
    implementation += sorted(p for p in (SIM / 'compiler').glob('*') if p.suffix in ('.py', '.cpp', '.h'))
    if runtime == 'native':
        implementation += [SIM / 'native.py', SIM / 'native_exports.py', SIM / 'native_build.py', SIM / 'native_hw.py', SIM / 'rtl_libraries.py']
        implementation += sorted(p for p in (SIM / 'compiler/verilog').iterdir()
                                 if p.suffix in ('.py', '.patch', '.json') or p.name == 'CMakeLists.txt')
        implementation += [ROOT / 'generators/radiance/src/main/resources/vsrc/vortex/hw/rtl/VX_platform.vh']
        implementation += sorted(p for p in (SIM / 'native').rglob('*') if p.suffix in ('.h', '.cpp', '.def'))
    else:
        implementation += [SIM / name for name in ('adapter.py', 'trace.h', 'c_sources.py', 'native/wide.cpp', 'native/format.cpp', 'native/evaluation.cpp', 'native/evaluation.h')]
    return {str(path.relative_to(ROOT)): digest(path) for path in implementation}


def config_catalog():
    configs = {}
    paths = set((ROOT / 'generators/chipyard/src/main/scala/config').glob('*.scala'))
    # Several user-facing configs moved into the generator plugins. They are
    # still ordinary CLI Chipyard configs and must not disappear from coverage.
    paths.update((ROOT / 'generators').glob('*/chipyard/*Config*.scala'))
    for path in sorted(paths):
        source = path.read_text()
        package = re.search(r'^package\s+([\w.]+)', source, re.M)[1]
        # CLI configs have a public no-argument constructor. Config fragments
        # and classes requiring constructor arguments are outside this catalog.
        for match in re.finditer(r'^class\s+(\w+Config)\s+extends\b', source, re.M):
            name = package + '.' + match[1]
            if name in configs:
                raise ValueError(f'duplicate named config {name}: {path}')
            configs[name] = dict(config=name, source=str(path.relative_to(ROOT)),
                                 source_sha256=digest(path), evidence=[], build_evidence=[], reported_running=[])
    return configs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reports', type=Path, default=SIM / 'output/integration')
    parser.add_argument('--build-reports', type=Path, default=SIM / 'output/config-sweep')
    parser.add_argument('--output', type=Path, default=SIM / 'output/config-coverage.json')
    parser.add_argument('--runtime', choices=('native', 'hybrid'), default='native')
    args = parser.parse_args()
    hashes, configs = implementation_hashes(args.runtime), config_catalog()
    other = []
    for path in sorted(args.reports.glob('*/report.json')):
        report = json.loads(path.read_text())
        if report.get('runtime', 'hybrid') != args.runtime:
            continue
        for case in report.get('cases', []):
            case_hashes = case.get('implementation_sha256', report.get('source_sha256', {}))
            # Private candidates can fingerprint additional modified sources.
            # A superset is different evidence even when every main file matches.
            current = case_hashes == hashes
            overrides = case.get('make_overrides', {})
            package = overrides.get('CONFIG_PACKAGE',
                                    'chipyard' if overrides.get('SUB_PROJECT', 'chipyard') == 'chipyard' else None)
            config = case['config'] if '.' in case['config'] or package is None else package + '.' + case['config']
            entry = dict(report=str(path.resolve()), case=case['case'], status=case['status'],
                         make_overrides=overrides,
                         current_implementation=current, completed_at=case.get('completed_at'),
                         cycles=case.get('cycles'))
            if case['status'] == 'passed':
                # A selection/manifest alone does not establish runtime success.
                if not case.get('completed_at') or not all(re.fullmatch(r'[0-9a-f]{64}', case.get(key, ''))
                        for key in ('simulator_sha256', 'partition_sha256')):
                    raise ValueError(f'incomplete runtime evidence in {path}: {case["case"]}')
                if config in configs:
                    configs[config]['evidence'].append(entry)
                    configs[config]['build_evidence'].append(dict(stage='simulator', **entry))
                else:
                    other.append(dict(config=config, **entry))
            elif case['status'] == 'running' and config in configs:
                configs[config]['reported_running'].append(entry)
    for path in sorted(args.build_reports.glob('*/report.json')):
        report = json.loads(path.read_text())
        if report.get('runtime', 'hybrid') != args.runtime:
            continue
        for case in report.get('cases', []):
            if case.get('status') != 'compiled' or case['config'] not in configs:
                continue
            if not case.get('completed_at') or not all(re.fullmatch(r'[0-9a-f]{64}', case.get(key, ''))
                    for key in ('artifact_sha256', 'partition_sha256')):
                raise ValueError(f'incomplete compilation evidence in {path}: {case["config"]}')
            current = case.get('source_sha256', {}) == hashes
            configs[case['config']]['build_evidence'].append(dict(report=str(path.resolve()), stage=report['stage'],
                current_implementation=current, completed_at=case['completed_at'], artifact_sha256=case['artifact_sha256']))
    counts = dict(runtime_tested_current=0, runtime_tested_historical=0, unverified=0)
    for item in configs.values():
        item['status'] = ('runtime_tested_current' if any(e['current_implementation'] for e in item['evidence']) else
                          'runtime_tested_historical' if item['evidence'] else 'unverified')
        counts[item['status']] += 1
    for current, label in ((True, 'simulator_compiled_current'), (False, 'simulator_compiled_historical')):
        counts[label] = sum(any(e['stage'] == 'simulator' and e['current_implementation'] is current
                                for e in item['build_evidence']) for item in configs.values())
    result = dict(runtime=args.runtime, implementation_sha256=hashes, summary=dict(named_configs=len(configs), **counts),
                  configs=list(configs.values()), other_runtime_tests=other,
                  interpretation=('Runtime tests cover the recorded workloads; they do not prove all configurations or behaviors. '
                                  'Running entries are report states, not process liveness checks.'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['summary']))
    print(f'Coverage report: {args.output.resolve()}')


if __name__ == '__main__':
    main()
