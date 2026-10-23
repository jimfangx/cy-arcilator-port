#!/usr/bin/env python3
"""Check convergence certificates, readback fallbacks, and binding feedback."""
from pathlib import Path
import argparse
import json
import os
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-bin', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=SIM / 'generated-src/convergence-test')
args = parser.parse_args()
sdk = args.circt_bin.resolve().parent
work = args.work.resolve()
out = work / 'compiler'
out.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(SIM))
from compiler.partition import plugin

sim = SIM
cxx = os.environ.get('CXX', 'c++')
lib = plugin(sdk / 'bin/circt-opt', cxx, SIM / 'generated-src/compiler',
             SIM / 'compiler/Convergence.cpp')
base = (SIM / 'tests/convergence_layout.mlir').read_text()
cases = {
    'direct-write': (base, True),
    'alias-write': (base.replace(
        '  arc.state_write %y = %one',
        '  %alias=arc.storage.get %storage[24] : !arc.storage -> !arc.state<i8>\n  arc.state_write %alias = %one'
    ), True),
    'direct-read':
    (base.replace('  %one=',
                  '  %old=arc.state_read %y : <i8>\n  %one='), False),
    'alias-read': (base.replace(
        '  %one=',
        '  %alias=arc.storage.get %storage[24] : !arc.storage -> !arc.state<i8>\n  %old=arc.state_read %alias : <i8>\n  %one='
    ), False),
    'private-overlap': (base.replace(
        '  %one=',
        '  %private=arc.alloc_state %storage {offset=24:i32}:(!arc.storage)->!arc.state<i8>\n  %one='
    ), False),
    'partial-alias': (base.replace(
        '  %one=',
        '  %alias=arc.storage.get %storage[24] : !arc.storage -> !arc.state<i16>\n  %one='
    ), False),
    'storage-escape': (base.replace(
        ' arc.model',
        ' func.func private @escape(!arc.storage)\n arc.model').replace(
            '  %one=',
            '  func.call @escape(%storage):(!arc.storage)->()\n  %one='),
                       False),
    'context-escape': (base.replace(
        ' arc.model', ' func.func private @escape(!arc.context)\n arc.model'
    ).replace(
        '  %one=',
        '  %context=arc.as_context %storage:!arc.storage\n  func.call @escape(%context):(!arc.context)->()\n  %one='
    ), False),
    'bounded-context': (base.replace(
        '  %one=',
        '  %context=arc.as_context %storage:!arc.storage\n  %time=arc.current_time %context\n  %one='
    ), True),
    'reserved-prefix': (base.replace('offset=24:i32', 'offset=16:i32'), False),
    'out-of-bounds': (base.replace('offset=24:i32', 'offset=64:i32'), False),
    'missing-source-proof': (base.replace(
        ' attributes {chipyard.output_readback_safe=true,chipyard.source_time_dependence=false}',
        ''), False),
    'unsafe-source-proof': (base.replace('output_readback_safe=true',
                                         'output_readback_safe=false'), False),
    'time-dependent': (base.replace('source_time_dependence=false',
                                    'source_time_dependence=true'), False),
    'missing-time-proof':
    (base.replace(',chipyard.source_time_dependence=false', ''), False),
    'disabled': (base, False),
    'input-alias': (base.replace(
        '!hw.modty<output y:i8>', '!hw.modty<input x:i8,output y:i8>'
    ).replace(
        '  %one=',
        '  %input=arc.root_input "x", %storage {offset=24:i32}:(!arc.storage)->!arc.state<i8>\n  %one='
    ), False),
}
report = dict(status='running', structural=[], effects=[])
for name, (text, expected) in cases.items():
    src = out / (name + '.mlir')
    dst = out / (name + '-after.mlir')
    src.write_text(text)
    p = subprocess.run([
        str(sdk / 'bin/circt-opt'),
        str(src), '--load-pass-plugin=' + str(lib),
        '--pass-pipeline=builtin.module(chipyard-certify-write-only-outputs' +
        ('{enabled=false}' if name == 'disabled' else '') + ')', '-o',
        str(dst)
    ],
                       capture_output=True,
                       text=True)
    assert p.returncode == 0, (name, p.stderr)
    actual = 'chipyard.write_only_outputs = ["y"]' in dst.read_text()
    assert actual == expected, (name, p.stderr, dst.read_text())
    report['structural'].append(dict(name=name, certified=actual))
effects = {
    'pure': ('module {func.func @empty(){return}}', True),
    'unknown-import': ('module {func.func private @unknown()}', False),
    'contracted-helper':
    ('module {func.func private @helper() attributes {chipyard.no_output_readback}}',
     True),
    'export-frame':
    ('module {func.func @exported() attributes {chipyard.lowered_dpi_export} {return}}',
     False),
    'raw-memory':
    ('module {func.func @read(%p:!llvm.ptr)->i8 {%v=llvm.load %p : !llvm.ptr -> i8\nreturn %v:i8}}',
     False),
    'low-level-pure':
    ('module {func.func @constant()->i8 {%v=llvm.mlir.constant(1:i8):i8\nreturn %v:i8}}',
     False),
    'indirect-call':
    ('module {func.func @apply(%f:(i8)->i8,%x:i8)->i8 {%v=func.call_indirect %f(%x):(i8)->i8\nreturn %v:i8}}',
     False),
    'contracted-dpi-call':
    ('module {func.func private @helper(i8,i8)->i8 attributes {chipyard.no_output_readback}\nhw.module @H(in %x:i8,out y:i8){%one=hw.constant 1:i8\n%y=sim.func.dpi.call @helper(%x,%one):(i8,i8)->i8\nhw.output %y:i8}}',
     True),
    'uncontracted-dpi-call':
    ('module {func.func private @helper(i8,i8)->i8\nhw.module @H(in %x:i8,out y:i8){%one=hw.constant 1:i8\n%y=sim.func.dpi.call @helper(%x,%one):(i8,i8)->i8\nhw.output %y:i8}}',
     False),
    'dpi-imports': ((sim / 'tests/sensitized_dpi.mlir').read_text(), False),
    'clocked-controls': ((sim / 'tests/native_control.mlir').read_text(), True),
    'clocked-unknown-import': (
        (sim / 'tests/native_control.mlir').read_text().replace(
            'module {', 'module { func.func private @unknown()'), False),
}
for name, (text, expected) in effects.items():
    src = out / (name + '.mlir')
    dst = out / (name + '-after.mlir')
    src.write_text(text)
    p = subprocess.run([
        str(sdk / 'bin/circt-opt'),
        str(src), '--load-pass-plugin=' + str(lib),
        '--pass-pipeline=builtin.module(chipyard-record-output-effects)', '-o',
        str(dst)
    ],
                       capture_output=True,
                       text=True)
    assert p.returncode == 0, (name, p.stderr)
    actual = 'chipyard.output_readback_safe = true' in dst.read_text()
    assert actual == expected, (name, p.stderr)
    report['effects'].append(dict(name=name, safe=actual))

# Validate the metadata boundary independently of the MLIR pass.
from native import generate
metadata_work = work / 'metadata'
metadata_work.mkdir(exist_ok=True)
partition = metadata_work / 'partition.json'
partition.write_text(json.dumps(dict(top='Test', instances=[], phase_clocks=[])))
layout = metadata_work / 'model.json'
factory = metadata_work / 'factory.cpp'
model = dict(name='ArcModel', states=[
    dict(name='x', type='input', offset=24, numBits=8),
    dict(name='y', type='output', offset=25, numBits=8)])
schemas = [
    ('absent', None, True, False),
    ('empty', dict(version=1, names=[]), True, False),
    ('valid', dict(version=1, names=['y']), True, True),
    ('not-object', [], False, False),
    ('missing-version', dict(names=['y']), False, False),
    ('future-version', dict(version=2, names=['y']), False, False),
    ('boolean-version', dict(version=True, names=['y']), False, False),
    ('missing-names', dict(version=1), False, False),
    ('not-list', dict(version=1, names='y'), False, False),
    ('not-string', dict(version=1, names=[1]), False, False),
    ('unknown', dict(version=1, names=['unknown']), False, False),
    ('input', dict(version=1, names=['x']), False, False),
    ('duplicate', dict(version=1, names=['y', 'y']), False, False),
]
report['metadata'] = []
for name, certificate, expected, registered in schemas:
    case = dict(model)
    if name != 'absent':
        case['convergenceOutputs'] = certificate
    layout.write_text(json.dumps([case]))
    try:
        generate(layout, partition, factory)
        accepted = True
    except ValueError:
        accepted = False
    assert accepted == expected, name
    if accepted:
        assert ('runtime.add_write_only_output("y");' in
                factory.read_text()) == registered, name
    report['metadata'].append(dict(name=name, accepted=accepted))

# Exercise the real frontend-to-JSON path, including observation and opt-out.
cli_work = work / 'cli'
cli_work.mkdir(exist_ok=True)
source = cli_work / 'input.mlir'
source.write_text('module { hw.module @ArcModel(in %x:i8, out y:i8) {'
                  '%one=hw.constant 1:i8\n'
                  '%y=comb.xor %x,%one:i8\nhw.output %y:i8} }')
report['cli'] = []
normal_ir = None
for name, options, flags, expected in [
        ('normal', [], [], True),
        ('disabled', ['--output-convergence=false'], [], False),
        ('observed', [], ['--observe-registers'], False),
        ('trace-taps', [], ['--trace-taps'], False)]:
    dst = cli_work / name
    dst.mkdir(exist_ok=True)
    command = [sys.executable, SIM / 'compiler/model.py', '--input', source,
               '--output', dst / 'model.ll', '--state-file', dst / 'model.json',
               '--arcilator', sdk / 'bin/arcilator', '--circt-opt',
               sdk / 'bin/circt-opt', '--mlir-translate',
               sdk / 'bin/mlir-translate', '--cache',
               SIM / 'generated-src/compiler', '--cxx', cxx, *options,
               '--', *flags]
    with (dst / 'compile.log').open('w') as stream:
        subprocess.run(list(map(str, command)), stdout=stream,
                       stderr=subprocess.STDOUT, check=True)
    result, = json.loads((dst / 'model.json').read_text())
    assert bool(result.get('convergenceOutputs', {}).get('names')) == expected, name
    if expected:
        assert result['convergenceOutputs'] == dict(version=1, names=['y'])
    ir = '\n'.join(line for line in (dst / 'model.ll').read_text().splitlines()
                   if not line.startswith(('; ModuleID =', 'source_filename =')))
    if name == 'normal':
        normal_ir = ir
    elif name == 'disabled':
        assert normal_ir == ir, 'certificate changed generated model code'
    report['cli'].append(dict(name=name, certified=expected))

# Each invocation owns these objects; other simulator builds cannot replace them.
runtime_work = work / 'runtime'
runtime_work.mkdir(exist_ok=True)
objects = []
with (runtime_work / 'compile.log').open('w') as log:
    for source in sorted((SIM / 'native').glob('*.cpp')):
        obj = runtime_work / (source.stem + '.o')
        subprocess.run([
            *shlex.split(cxx), '-O2', '-std=c++17', '-DARC_NATIVE_NO_MAIN',
            *(['-fno-rtti'] if source.name == 'files.cpp' else []),
            '-I' + str(sdk / 'include'), '-I' + str(SIM),
            '-I' + str(SIM / 'native/include'), '-c',
            str(source), '-o',
            str(obj)
        ],
                       stdout=log,
                       stderr=subprocess.STDOUT,
                       check=True)
        objects.append(obj)
    libs = shlex.split(
        subprocess.check_output([sdk / 'bin/llvm-config', '--libs', 'support'],
                                text=True))
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(sdk / 'include'),
        '-I' + str(SIM), SIM / 'tests/native_convergence.cpp', *objects,
        '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', *libs, '-lz',
        '-lpthread', '-ldl', '-o', runtime_work / 'test'
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
subprocess.run([runtime_work / 'test'], check=True)
report.update(status='passed', runtime_cases=20)
(work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(
    f"PASS: {len(report['structural'])} allocated layout/readback guards, {len(report['effects'])} source-effect guards, {len(report['metadata'])} metadata guards, {len(report['cli'])} CLI modes and 20 runtime ownership/feedback/fallback cases"
)
