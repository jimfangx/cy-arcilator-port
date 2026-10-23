"""Check the optional compiler/native idle-clock metadata boundary."""
from pathlib import Path
import copy
import json
import subprocess
import sys


def check(sdk, work, cxx):
    sim = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(sim))
    from native import generate
    work.mkdir(exist_ok=True)
    layout = {
        'name':
        'ArcModel',
        'states': [{
            'name': 'held',
            'type': 'input',
            'offset': 24,
            'numBits': 1
        }, {
            'name': 'observed',
            'type': 'output',
            'offset': 25,
            'numBits': 1
        }],
        'idleClock': {
            'version': 1,
            'function': 'ArcModel_idle_clock',
            'inputs': ['held']
        }
    }
    part = {
        'top': 'Harness',
        'ports': [],
        'instances': [],
        'phase_clocks': [{
            'held': 'held',
            'observed': 'observed'
        }]
    }
    (work / 'partition.json').write_text(json.dumps(part))
    cases = {
        'valid': (layout, True),
        'legacy': ({
            k: v
            for k, v in layout.items() if k != 'idleClock'
        }, True)
    }
    for name, field, value in [('version-bool', 'version', True),
                               ('version-new', 'version', 2),
                               ('missing-symbol', 'function', None),
                               ('bad-symbol', 'function', 'fn; abort();'),
                               ('wrong-symbol', 'function', 'ArcModel_eval'),
                               ('inputs-empty', 'inputs', []),
                               ('inputs-duplicate', 'inputs', ['held',
                                                               'held']),
                               ('inputs-wrong', 'inputs', ['observed']),
                               ('inputs-unknown', 'inputs', ['absent']),
                               ('inputs-extra', 'inputs', ['held', 'unknown']),
                               ('input-object', 'inputs', [{}])]:
        candidate = copy.deepcopy(layout)
        candidate['idleClock'][field] = value
        cases[name] = (candidate, False)
    for name, field, value in [('time', 'hasTimeReaders', True),
                               ('sampling', 'samplingInput', 'held')]:
        candidate = copy.deepcopy(layout)
        candidate[field] = value
        cases[name] = (candidate, False)
    for name, field, value in [('input-width', 'numBits', 8),
                               ('input-direction', 'type', 'output')]:
        candidate = copy.deepcopy(layout)
        candidate['states'][0][field] = value
        cases[name] = (candidate, False)
    stable_layout = copy.deepcopy(layout)
    stable_layout['idleClock'][
        'stableFunction'] = 'ArcModel_stable_combinational'
    cases['stable'] = (stable_layout, True)
    for name, value in [('stable-null', None), ('stable-bool', True),
                        ('stable-other-symbol', 'ArcModel_eval')]:
        candidate = copy.deepcopy(stable_layout)
        candidate['idleClock']['stableFunction'] = value
        cases[name] = (candidate, False)
    fast_layout = copy.deepcopy(stable_layout)
    fast_layout['idleClock']['stableEvaluator'] = 'ArcModel_stable_eval'
    cases['fast'] = (fast_layout, True)
    for name, value in [('fast-null', None), ('fast-bool', True),
                        ('fast-other-symbol', 'ArcModel_eval')]:
        candidate = copy.deepcopy(fast_layout)
        candidate['idleClock']['stableEvaluator'] = value
        cases[name] = (candidate, False)
    candidate = copy.deepcopy(fast_layout)
    del candidate['idleClock']['stableFunction']
    cases['fast-without-query'] = (candidate, False)
    records = []
    for name, (candidate, valid) in cases.items():
        source = work / (name + '.json')
        target = work / (name + '.cpp')
        source.write_text(json.dumps([candidate]))
        try:
            generate(source, work / 'partition.json', target)
            assert valid, name
            if name == 'valid':
                text = target.read_text()
                assert 'uint32_t ArcModel_idle_clock(void *);' in text
                assert 'runtime.add_idle_clock_model(ArcModel_idle_clock, {"held"});' in text
            if name == 'stable':
                text = target.read_text()
                assert 'uint32_t ArcModel_stable_combinational(void *);' in text
                assert 'runtime.add_idle_clock_model(ArcModel_idle_clock, {"held"}, ArcModel_stable_combinational);' in text
            if name == 'fast':
                text = target.read_text()
                assert 'void ArcModel_stable_eval(void *);' in text
                assert 'ArcModel_stable_combinational, ArcModel_stable_eval);' in text
            if name == 'legacy': assert 'idle_clock' not in target.read_text()
        except ValueError:
            assert not valid, name
        records.append({'name': name, 'accepted': valid})
    source = work / 'input.mlir'
    source.write_text("""module {
 hw.module @ArcModel(in %held:i1,in %data:i8,out y:i8) {
  %clock=seq.to_clock %held
  %q=seq.firreg %data clock %clock : i8
  hw.output %q:i8
 }
}
""")
    cli = []
    reference = None
    for name, options, flags, expected in [
        ('normal', [], [], True),
        ('stable-disabled', ['--stable-evaluator=false'], [], True),
        ('disabled', ['--idle-clocks=false'], [], False),
        ('without-partition', [], [], False),
        ('observed', [], ['--observe-ports'], False),
        ('trace-taps', [], ['--trace-taps'], False)
    ]:
        directory = work / name
        directory.mkdir(exist_ok=True)
        command = [
            sys.executable,
            str(sim / 'compiler/model.py'), '--input=' + str(source),
            '--output=' + str(directory / 'model.ll'),
            '--state-file=' + str(directory / 'model.json'),
            '--arcilator=' + str(sdk / 'bin/arcilator'),
            '--circt-opt=' + str(sdk / 'bin/circt-opt'),
            '--mlir-translate=' + str(sdk / 'bin/mlir-translate'),
            '--cache=' + str(sim / 'generated-src/compiler'), '--cxx=' + cxx,
            *options
        ]
        if name != 'without-partition':
            command += ['--partition=' + str(work / 'partition.json')]
        command += ['--', *flags]
        with (directory / 'compile.log').open('w') as log:
            subprocess.run(command,
                           stdout=log,
                           stderr=subprocess.STDOUT,
                           check=True)
        info, = json.loads((directory / 'model.json').read_text())
        assert ('idleClock' in info) == expected, (name, info)
        if expected:
            assert info['idleClock'] == (stable_layout
                                         if name == 'stable-disabled' else
                                         fast_layout)['idleClock']
            reference = {k: v for k, v in info.items() if k != 'idleClock'}
        elif name == 'disabled':
            assert info == reference, 'clock certificate changed physical layout'
        cli.append({'name': name, 'certified': expected})
    return {'status': 'passed', 'metadata': records, 'compiler_cli': cli}
