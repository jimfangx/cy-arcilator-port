#!/usr/bin/env python3
"""Generate local SDK reference models and opaque full-toolchain cases.

References are always built in a new directory on the destination machine.
The SDK backend receives no private LLVM flags. The source-built instrumented
and final compilers replay the same optimization pipeline with the requested
backend worker flag. Config-specific choices live in recipe data only.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys


PORT = Path(__file__).resolve().parent.parent
DEFAULT_RECIPE = Path(__file__).with_name('toolchain-configs.json')
REQUIRED = dict(ARC_RUNTIME='native', ARC_CODEGEN_PIPELINE='stock',
                ARC_OPT_LEVEL='3', ARC_LLC_OPT_LEVEL='3',
                ARC_STATE_PARTITIONING='0', ARC_MODEL_OPS='0',
                ARC_SHARED_PROCEDURES='0', ARC_ABI_BUFFERS='0',
                ARC_EARLY_INLINE='1', ARC_NATIVE_PREPARATION='1')
# These affect elaboration or model lowering, not the compiler replay algorithm.
RECIPE_SETTINGS = {'ARCILATOR_FLAGS', 'ARC_DISTRIBUTE_WIDE_STORES',
                   'ARC_VECTOR_PACKING', 'ARC_OPT_FLAGS', 'CONFIG_PACKAGE',
                   'MODEL_PACKAGE', 'MODEL', 'VLOG_MODEL', 'TOP', 'TOP_PACKAGE',
                   'ARC_PARTITION_OPS', 'ARC_PARTITION_PORTS'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def identity(path):
    path = Path(path).resolve(strict=True)
    return dict(path=str(path), sha256=sha(path))


def dump(path, data):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def clean_environment():
    env = dict(os.environ)
    for key in list(env):
        if key in ('LD_PRELOAD', 'LD_AUDIT', 'ARC_COMPILER_SAMPLE_PATH',
                   'MAKEFLAGS', 'MAKEOVERRIDES', 'MFLAGS') or key.startswith(
                       ('LLVM_PROFILE_', 'CPUPROFILE', 'GCOV_')):
            env.pop(key, None)
    env['LC_ALL'] = 'C'
    return env


def run(argv, log, cwd, env):
    """Keep failed logs; cancel the complete Make/compiler process group."""
    argv = list(map(str, argv))
    with Path(log).open('w') as stream:
        stream.write(shlex.join(argv) + '\n')
        stream.flush()
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=stream,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        try:
            returncode = process.wait()
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=10)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            raise
    require(returncode == 0, 'Reference generation failed; see ' + str(log))


def read_recipe(recipe=None):
    path = Path(recipe or DEFAULT_RECIPE).resolve(strict=True)
    data = json.loads(path.read_text())
    require(data.get('schema') == 1 and isinstance(data.get('cases'), list)
            and data['cases'], 'Expected schema 1 with nonempty cases')
    ids = set()
    for case in data['cases']:
        require(isinstance(case, dict), 'Each recipe case must be an object')
        require(set(case) <= {'id', 'config', 'training', 'settings'},
                'Unknown case fields')
        require(isinstance(case.get('config'), str) and re.fullmatch(
            r'[A-Za-z_][A-Za-z0-9_.]*', case['config']), 'Unsafe Scala config name')
        case_id = case.get('id', case['config'])
        require(isinstance(case_id, str) and re.fullmatch(
            r'[A-Za-z0-9_][A-Za-z0-9_.-]*', case_id), 'Unsafe case ID')
        require(case_id not in ids, 'Duplicate case ID: ' + case_id)
        ids.add(case_id)
        require(type(case.get('training')) is bool, 'Explicit boolean training is required')
        values = case.get('settings', {})
        require(isinstance(values, dict) and set(values) <= RECIPE_SETTINGS,
                'Unsupported case settings: ' + str(values))
        validate_settings(values)
    require(any(c['training'] for c in data['cases']), 'At least one training case is required')
    return path, data


def validate_settings(values):
    require(isinstance(values, dict), 'Settings must be an object')
    for key, value in values.items():
        require(isinstance(key, str) and re.fullmatch(r'[A-Z][A-Z0-9_]*', key),
                'Invalid Make variable name')
        require(isinstance(value, str) and not any(c in value for c in '\n\r\0$`'),
                'Settings must be literal strings, without Make/shell substitutions: ' + key)


def pipeline(settings, libraries):
    """Same whole-module native preparation/order as production codegen.py."""
    for key, value in REQUIRED.items():
        require(settings.get(key) == value, 'Reference pipeline requires ' + key + '=' + value)
    for key in ('ARC_DISTRIBUTE_WIDE_STORES', 'ARC_VECTOR_PACKING'):
        require(settings.get(key, '0') in ('0', '1'), key + ' must be 0 or 1')
    preparation = 'require<profile-summary>,cgscc(inline),chipyard-native-arithmetic,verify'
    post = ['default<O3>']
    if settings.get('ARC_DISTRIBUTE_WIDE_STORES') == '1':
        post.append('chipyard-distribute-wide-stores')
    if settings.get('ARC_VECTOR_PACKING') == '1':
        post.append('chipyard-vector-packing')
    post.append('verify')
    return (['-load-pass-plugin=' + str(p) for p in libraries], preparation,
            ','.join(post), shlex.split(settings.get('ARC_OPT_FLAGS', '')))


def prepare_cases(work, sdk, arcilator, settings, jobs, cxx, cc, recipe=None):
    """Return training_manifest, qualification_manifest, api_cases and record paths.

    ``settings`` is the selected common native Make profile. ``recipe`` is an
    optional schema-1 JSON file; arbitrary config names use the same algorithm.
    This function executes fresh reference builds, not simulations or timings.
    Existing work directories are refused, including partially failed attempts.
    """
    work = Path(work).resolve()
    sdk = Path(sdk).resolve(strict=True)
    arcilator = Path(arcilator).resolve(strict=True)
    require(type(jobs) is int and jobs > 0, 'jobs must be positive')
    validate_settings(settings)
    recipe_path, recipe_data = read_recipe(recipe)
    require(not work.exists(), 'Refusing to replace reference work: ' + str(work))
    # Native Make rules do not quote their generated target paths.
    for path in (work, sdk, arcilator, PORT):
        require(not any(c.isspace() or c in '$#:%;\\' for c in str(path)),
                'Native Make requires simple paths: ' + str(path))
    for tool in ('opt', 'llc', 'llvm-config', 'firtool', 'circt-opt', 'mlir-translate'):
        require((sdk / 'bin' / tool).is_file(), 'SDK tool missing: ' + tool)
    settings = dict(settings)
    for key, value in REQUIRED.items():
        require(settings.get(key) == value, 'Unsupported common setting: ' + key)
    forbidden = {'CIRCT_BIN', 'CIRCT_ROOT', 'LLVM_OPT', 'LLC', 'ARCILATOR',
                 'CONFIG', 'CXX', 'CC', 'PYTHON', 'FIRTOOL', 'ARC_BUILD_JOBS'}
    require(not set(settings) & forbidden, 'Pass tools/config/jobs through the API')
    work.mkdir(parents=True)
    env = clean_environment()
    tools = {name: identity(sdk / 'bin' / name) for name in
             ('opt', 'llc', 'llvm-config', 'firtool', 'circt-opt', 'mlir-translate')}
    tools['arcilator'] = identity(arcilator)
    sources = [Path(__file__), recipe_path, PORT / 'Makefile',
               PORT / 'compiler/codegen.py', PORT / 'compiler/partition.py',
               *[PORT / 'compiler' / name for name in
                 ('NativeArithmetic.cpp', 'WideStores.cpp', 'VectorPacking.cpp')]]
    immutable = [identity(p) for p in sources] + list(tools.values())
    def guard():
        for item in immutable:
            require(sha(item['path']) == item['sha256'], 'Preparation input changed: ' + item['path'])
    record = dict(schema=1, status='running', recipe=identity(recipe_path),
                  settings=settings, tools=tools, sources=[identity(p) for p in sources],
                  cases=[], scope='Fresh destination-local SDK references; setup only, no timing claim')
    result_path = work / 'preparation.json'
    dump(result_path, record)
    manifests = []
    inputs = {}
    api = []
    try:
        # The directory starts empty: existing installation/checkout plugins are
        # never used as reference libraries. Make and this call share its SDK key.
        from partition import plugin
        cache = work / 'generated/compiler'
        for case in recipe_data['cases']:
            guard()
            case_id = case.get('id', case['config'])
            values = dict(settings, **case.get('settings', {}))
            pipeline(values, [])
            directory = work / 'references' / case_id
            directory.mkdir(parents=True)
            generated = directory / 'generated'
            native = generated / 'arcilator-native'
            target = native / 'model.o'
            overrides = dict(values, ARC_LLC_FLAGS='', CONFIG=case['config'],
                             gen_dir=str(work / 'generated'), build_dir=str(generated),
                             output_dir=str(directory / 'output'),
                             CIRCT_BIN=str(sdk / 'bin'), CIRCT_ROOT=str(sdk),
                             ARCILATOR=str(arcilator), LLVM_OPT=str(sdk / 'bin/opt'),
                             LLC=str(sdk / 'bin/llc'), LLVM_CONFIG=str(sdk / 'bin/llvm-config'),
                             FIRTOOL=str(sdk / 'bin/firtool'),
                             ARC_FIRTOOL=str(sdk / 'bin/firtool'),
                             CIRCT_OPT=str(sdk / 'bin/circt-opt'),
                             MLIR_TRANSLATE=str(sdk / 'bin/mlir-translate'),
                             CXX=str(cxx), CC=str(cc), PYTHON=sys.executable,
                             ARC_BUILD_JOBS=str(jobs))
            argv = ['make', '-C', str(PORT), '-j' + str(jobs),
                    *[key + '=' + value for key, value in sorted(overrides.items())], str(target)]
            log = directory / 'make.log'
            run(argv, log, PORT, env)
            for name in ('model.ll', 'model.bc', 'model.o', 'model.json', 'codegen-report.json'):
                require((native / name).is_file(), 'Missing fresh reference: ' + str(native / name))
            names = ['NativeArithmetic.cpp']
            if values.get('ARC_DISTRIBUTE_WIDE_STORES') == '1':
                names.append('WideStores.cpp')
            if values.get('ARC_VECTOR_PACKING') == '1':
                names.append('VectorPacking.cpp')
            libraries = [plugin(sdk / 'bin/opt', str(cxx), cache, PORT / 'compiler' / name)
                         for name in names]
            loaded, preparation, optimizer, opt_flags = pipeline(values, libraries)
            raw = identity(native / 'model.ll')
            for item in [raw, *map(identity, libraries)]:
                inputs[item['path']] = item
                immutable.append(item)
            outputs = [dict(path='{work}/' + name, comparison=mode,
                            reference=identity(native / name)) for name, mode in
                       [('model.bc', 'llvm-ir'), ('model.o', 'bytes')]]
            immutable.extend(output['reference'] for output in outputs)
            steps = [dict(tool='opt', args=[*loaded, '-passes=' + preparation,
                         raw['path'], '-o', '{work}/prepared.bc']),
                     dict(tool='opt', args=[*loaded, '-passes=' + optimizer, *opt_flags,
                         '{work}/prepared.bc', '-o', '{work}/model.bc']),
                     dict(tool='llc', args=['-O3', '-relocation-model=pic', '-filetype=obj',
                         *shlex.split(values.get('ARC_LLC_FLAGS', '')),
                         '{work}/model.bc', '-o', '{work}/model.o'])]
            manifests.append(dict(id=case_id, steps=steps, outputs=outputs,
                                  role='training and qualification' if case['training'] else 'held-out qualification'))
            api.append(dict(id=case_id, input=raw,
                            reference_object=outputs[1]['reference'],
                            reference_bitcode=outputs[0]['reference'],
                            metadata=identity(native / 'model.json'),
                            opt_flags=values.get('ARC_OPT_FLAGS', ''),
                            distribute_wide_stores=values.get('ARC_DISTRIBUTE_WIDE_STORES') == '1',
                            vector_packing=values.get('ARC_VECTOR_PACKING') == '1',
                            native_preparation=True, early_inline=True,
                            share_procedures=False, abi_buffers=False,
                            pipeline='stock', opt_level='3', llc_opt_level='3',
                            llc_flags=values.get('ARC_LLC_FLAGS', '')))
            record['cases'].append(dict(id=case_id, config=case['config'], training=case['training'],
                                        settings=values, argv=argv, log=identity(log),
                                        metadata=identity(native / 'model.json'),
                                        raw_LLVM=raw, outputs=outputs))
            guard()
            dump(result_path, record)
        selected = {c.get('id', c['config']) for c in recipe_data['cases'] if c['training']}
        paths = dict(training_manifest=work / 'training-cases.json',
                     qualification_manifest=work / 'qualification-cases.json',
                     api_cases=work / 'api-cases.json', record=result_path)
        for key, cases in [('training_manifest', [c for c in manifests if c['id'] in selected]),
                           ('qualification_manifest', manifests)]:
            used = {arg.split('=', 1)[1] if arg.startswith('-load-pass-plugin=') else arg
                    for case in cases for step in case['steps'] for arg in step['args']}
            dump(paths[key], dict(schema=1, cases=cases,
                                 inputs=[item for path, item in sorted(inputs.items()) if path in used]))
        dump(paths['api_cases'], dict(schema=1, cases=api))
        guard()
        record.update(status='passed', manifests={k: identity(v) for k, v in paths.items() if k != 'record'})
        dump(result_path, record)
        return {key: str(path) for key, path in paths.items()}
    except BaseException as error:
        record.update(status='failed', error=repr(error))
        dump(result_path, record)
        raise
