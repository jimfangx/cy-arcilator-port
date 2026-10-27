#!/usr/bin/env python3
"""Rebuild the native Arcilator compiler toolchain from pinned source and fresh profiles."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
DEFAULT_WORK = SIM / 'generated-src/toolchain'
SETTINGS = dict(
    ARC_RUNTIME='native', ARC_CODEGEN_PIPELINE='stock',
    ARC_OPT_LEVEL='3', ARC_LLC_OPT_LEVEL='3', ARC_STATE_PARTITIONING='0',
    ARC_MODEL_OPS='0', ARC_SHARED_PROCEDURES='0', ARC_ABI_BUFFERS='0',
    ARC_EARLY_INLINE='1', ARC_LATE_WIDE_ARITHMETIC='1',
    ARC_OUTPUT_CONVERGENCE='1', ARC_IDLE_CLOCKS='1', ARC_STABLE_EVALUATOR='1',
    ARC_MLIR_BYTECODE='1', ARC_CONVERSION_BYTECODE='1', ARC_FUSED_PIPELINE='1',
    ARC_SPARSE_PREPARATION='1', ARC_NATIVE_PREPARATION='1',
    ARC_LLC_FLAGS='-parallel-virtreg-liveness-jobs=4',
    ARC_DISTRIBUTE_WIDE_STORES='0', ARC_VECTOR_PACKING='0', ARCILATOR_FLAGS='',
    EXTRA_SIM_CXXFLAGS='-march=native')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def load(path):
    return json.loads(Path(path).read_text())


def dump(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + '.new')
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n')
    temporary.replace(path)


def record(path):
    path = Path(path).resolve(strict=True)
    return dict(path=str(path), sha256=sha(path))


def check_record(value):
    require(sha(value['path']) == value['sha256'], 'File changed: ' + value['path'])


def output(argv):
    return subprocess.check_output([str(x) for x in argv], text=True).strip()


def tool(value, names):
    found = str(value) if value else next((shutil.which(n) for n in names if shutil.which(n)), None)
    require(found is not None, 'Missing host tool: ' + '/'.join(names))
    if '/' not in found:
        found = shutil.which(found)
    require(found and os.access(found, os.X_OK), 'Host tool is not executable: ' + str(found))
    # Keep clang++/ld.lld argv spelling, which determines driver behavior.
    return Path(found).absolute()


def sdk_path(explicit=None):
    if explicit:
        return explicit.resolve(strict=True)
    if os.environ.get('CIRCT_ROOT'):
        return Path(os.environ['CIRCT_ROOT']).resolve(strict=True)
    if os.environ.get('CIRCT_BIN'):
        return Path(os.environ['CIRCT_BIN']).resolve(strict=True).parent
    path = shutil.which('firtool')
    require(path, 'Source Chipyard env.sh first, or supply --sdk')
    return Path(path).resolve(strict=True).parent.parent


def preflight(args):
    require(sys.platform.startswith('linux'), 'The source workflow is qualified on Linux')
    sdk = sdk_path(args.sdk)
    version = output([sdk / 'bin/firtool', '--version'])
    require(re.search(r'^CIRCT firtool-1\.162\.0$', version, re.M),
            'Expected firtool-1.162.0 after env.sh; rerun build-setup step 10 or supply --sdk.\n' + version)
    require(output([sdk / 'bin/llvm-config', '--version']) == '24.0.0git', 'Expected the matching LLVM24 SDK')
    tools = {key: tool(getattr(args, key), names) for key, names in (
        ('cc', ('clang-18', 'clang')), ('cxx', ('clang++-18', 'clang++')),
        ('linker', ('ld.lld-18', 'ld.lld')), ('profdata', ('llvm-profdata-18', 'llvm-profdata')),
        ('cmake', ('cmake',)), ('ninja', ('ninja',)))}
    for name in ('cc', 'cxx'):
        require(re.search(r'clang version 18\.', output([tools[name], '--version'])),
                'This recipe requires Clang18: ' + str(tools[name]))
    require(re.search(r'LLD 18\.', output([tools['linker'], '--version'])), 'This recipe requires LLD18')
    require(re.search(r'LLVM version 18\.', output([tools['profdata'], '--version'])),
            'Use llvm-profdata18 matching the instrumenting Clang')
    tools['host_cc'] = tool(args.host_cc or os.environ.get('CC'), ('cc',))
    tools['host_cxx'] = tool(args.host_cxx or os.environ.get('CXX'), ('c++',))
    tool(None, ('git',))
    return sdk, tools


def freeze_sources():
    paths = {SIM / name for name in ('Makefile', 'partition.py', 'native_build.py')}
    paths.update(path for directory in ('compiler', 'native') for path in (SIM / directory).rglob('*')
                 if path.is_file() and '__pycache__' not in path.parts)
    return {str(path): sha(path) for path in sorted(paths) if path.is_file()}


def guard_sources(frozen):
    for path, expected in frozen.items():
        require(sha(path) == expected, 'Port source changed during setup: ' + path)


def run(argv, log, work):
    from full_toolchain import clean_environment, owned_processes, process_identity, terminate_owned
    argv = [str(x) for x in argv]
    with Path(log).open('w') as stream:
        proc = subprocess.Popen(argv, stdout=stream, stderr=subprocess.STDOUT,
                                env=clean_environment(), start_new_session=True)
        owner, known = process_identity(proc.pid), []
        remaining = []
        entry = dict(pid=proc.pid, identity=owner, argv=argv, log=str(log), status='running')
        dump(work / 'active-command.json', entry)
        try:
            while True:
                known = owned_processes(owner, known)
                entry['owned'] = known
                dump(work / 'active-command.json', entry)
                try:
                    code = proc.wait(timeout=0.2)
                    break
                except subprocess.TimeoutExpired:
                    pass
            survivors = [p for p in owned_processes(owner, known) if p['pid'] != proc.pid]
            if survivors:
                remaining = terminate_owned(proc, owner, survivors)
                raise ValueError('Stage exited with live descendants; retained log: ' + str(log))
        except BaseException:
            remaining = terminate_owned(proc, owner, known)
            raise
        finally:
            dump(work / 'active-command.json', dict(entry, status='cleanup_failed' if remaining else 'terminal',
                                                   remaining_owned=remaining, returncode=proc.poll()))
    require(code == 0, 'Stage failed; retained log: ' + str(log))


def bindings(result):
    """Bind stage metadata, selected tools, and every declared compiler input/reference."""
    result_files = []
    for key, value in result.items():
        if not isinstance(value, str) or not Path(value).is_file():
            continue
        path = Path(value)
        result_files.append(record(path))
        if key in ('training_manifest', 'qualification_manifest'):
            manifest = load(path)
            result_files.extend(manifest.get('inputs', []))
            result_files.extend(out['reference'] for case in manifest['cases'] for out in case['outputs'])
        if key == 'api_cases':
            result_files.extend(value for case in load(path)['cases'] for value in case.values()
                                if isinstance(value, dict) and 'path' in value and 'sha256' in value)
        if key == 'manifest':
            manifest = load(path)
            result_files.extend(manifest.get('outputs', {}).values())
            result_files.extend(manifest['configuration_identity'][name] for name in
                                ('source_record', 'compatibility_manifest'))
        if key == 'record':
            metadata = load(path)
            result_files.extend(metadata.get('raw_profiles', []))
            result_files.extend(metadata.get('inputs_and_references', []))
            if isinstance(metadata.get('merged'), dict):
                result_files.append(metadata['merged'])
        if key == 'source_record':
            result_files.append(load(path)['supplied_source_provenance'])
    for value in result_files:
        check_record(value)
    return result_files


def api_qualify(api_path, prefix, sdk, work, host_cxx, jobs):
    from codegen import compile_model
    # A real fresh port plugin/API check complements all-model opaque pipeline
    # qualification. Choose the first data case; names never select algorithms.
    case = load(api_path)['cases'][0]
    directory = work / 'production-api'
    directory.mkdir()
    check_record(case['input'])
    check_record(case['reference_object'])
    check_record(case['reference_bitcode'])
    obj, bc = directory / 'model.o', directory / 'model.bc'
    compile_model(Path(case['input']['path']), obj, prefix / 'bin/opt', prefix / 'bin/llc',
                  directory / 'plugins', cxx=str(host_cxx), bitcode=bc, jobs=jobs,
                  report=directory / 'codegen-report.json', pipeline=case['pipeline'],
                  abi_buffers=case['abi_buffers'], native_preparation=case['native_preparation'],
                  early_inline=case['early_inline'], share_procedures=case['share_procedures'],
                  opt_level=case['opt_level'], llc_opt_level=case['llc_opt_level'],
                  opt_flags=case['opt_flags'],
                  distribute_wide_stores=case.get('distribute_wide_stores', False),
                  vector_packing=case.get('vector_packing', False),
                  llc_flags=case['llc_flags'])
    require(sha(obj) == case['reference_object']['sha256'], 'Fresh production API model object differs')
    # The standalone qualifier already checks full canonical IR on every case.
    # Repeat the API case through the same decoder, ignoring only ModuleID.
    for label, path in (('reference', Path(case['reference_bitcode']['path'])), ('candidate', bc)):
        text = output([sdk / 'bin/llvm-dis', path, '-o', '-'])
        (directory / (label + '.ll')).write_text(''.join(
            line + '\n' for line in text.splitlines() if not line.startswith('; ModuleID = ')))
    require((directory / 'reference.ll').read_bytes() == (directory / 'candidate.ll').read_bytes(),
            'Fresh production API canonical IR differs')
    result = dict(status='passed', case=case['id'], object=record(obj), bitcode=record(bc),
                  fresh_native_plugin=True, exact_object=True, exact_canonical_IR=True)
    dump(directory / 'result.json', result)
    return dict(record=str(directory / 'result.json'))


def worker(request):
    """Run helper APIs in an owned child tree, including their subprocesses."""
    data = load(request)
    values = data['arguments']
    if data['kind'] == 'prepare':
        from toolchain_source import prepare
        for key in ('work', 'sdk', 'cc', 'cxx', 'cmake', 'ninja', 'source'):
            if values.get(key) is not None:
                values[key] = Path(values[key])
        result = prepare(**values)
    elif data['kind'] == 'cases':
        from toolchain_cases import prepare_cases
        for key in ('work', 'sdk', 'arcilator', 'recipe'):
            values[key] = Path(values[key])
        result = prepare_cases(**values)
    else:
        require(data['kind'] == 'api', 'Unknown setup worker')
        for key in ('api_path', 'prefix', 'sdk', 'work', 'host_cxx'):
            values[key] = Path(values[key])
        result = api_qualify(**values)
    dump(data['result'], {key: str(value) for key, value in result.items()})


def activate(args):
    work = args.work.resolve(strict=True)
    state = load(work / 'setup.json')
    require(state['status'] == 'complete', 'Toolchain setup is incomplete')
    require(Path(state['selected']['path']).resolve() == work / 'selected.json', 'Selection path differs')
    check_record(state['selected'])
    selected = load(work / 'selected.json')
    require(selected['status'] == 'source_built_and_output_qualified', 'Toolchain setup is incomplete')
    for value in selected['tools'].values():
        check_record(value)
    for value in selected['qualification_records']:
        check_record(value)
    for phase in state['phases'].values():
        require(phase['status'] == 'complete', 'Toolchain phase is incomplete')
        for value in phase['files']:
            check_record(value)
    check_record(selected['source_record'])
    check_record(selected['profile'])
    sdk = Path(selected['sdk'])
    for value in selected['sdk_activation_files']:
        check_record(value)
    values = dict(selected['settings'], CIRCT_ROOT=str(sdk), CIRCT_BIN=str(sdk / 'bin'),
                  LLVM_OPT=selected['tools']['opt']['path'], LLC=selected['tools']['llc']['path'],
                  ARCILATOR=selected['tools']['arcilator']['path'])
    for name, value in sorted(values.items()):
        require(re.fullmatch(r'[A-Z][A-Z0-9_]*', name), 'Invalid activation name')
        print('export ' + name + '=' + shlex.quote(value))


def build(args):
    from full_toolchain import same_process
    sdk, tools = preflight(args)
    require(args.jobs > 0 and args.link_jobs > 0, 'Job counts must be positive')
    lto_jobs = args.lto_jobs or args.jobs
    case_jobs = args.training_jobs or max(1, min(3, args.jobs // 4))
    require(lto_jobs > 0 and case_jobs > 0, 'Worker counts must be positive')
    work = args.work.absolute()
    existed = work.exists()
    require(not existed or args.resume, 'Use a fresh --work directory, or explicit --resume')
    work.mkdir(parents=True, exist_ok=True)
    with (work / 'controller.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        active_path = work / 'active-command.json'
        if active_path.exists():
            active = load(active_path)
            owners = [active.get('identity'), *active.get('owned', []), *active.get('remaining_owned', [])]
            require(not any(value and same_process(value) for value in owners),
                    'Prior setup descendants are still active; retain work and stop them before resume')
        frozen = freeze_sources()
        identity = dict(sources=frozen, sdk=str(sdk), tools={k: dict(argv=str(v), **record(v)) for k, v in tools.items()},
                        configs=record(args.configs), source=str(args.source.resolve()) if args.source else None,
                        jobs=args.jobs, link_jobs=args.link_jobs, lto_jobs=lto_jobs, training_jobs=case_jobs)
        state_path = work / 'setup.json'
        if existed:
            state = load(state_path)
            require(state['identity'] == identity, 'Resume inputs/source/tools differ; choose a fresh --work')
            require(state['status'] != 'complete', 'Completed setup is immutable; source its env.sh')
        else:
            state = dict(identity=identity, status='running', phases={})
            dump(state_path, state)
        phase_name = None
        def preserve_incomplete(directory):
            if not directory.exists():
                return
            archive = work / 'interrupted-attempts'
            archive.mkdir(exist_ok=True)
            target = archive / (directory.name + '-' + str(time.time_ns()))
            directory.rename(target)
            print('Preserved incomplete setup attempt: ' + str(target), flush=True)

        def invoke(name, kind, arguments):
            requests = work / 'requests'
            requests.mkdir(exist_ok=True)
            request, result = requests / (name + '.json'), requests / (name + '-result.json')
            result.unlink(missing_ok=True)
            dump(request, dict(kind=kind, arguments=arguments, result=str(result)))
            run([sys.executable, Path(__file__).resolve(), '_worker', '--request', request],
                work / (name + '.log'), work)
            return load(result)

        def phase(name, action):
            nonlocal phase_name
            phase_name = name
            guard_sources(frozen)
            previous = state['phases'].get(name)
            if previous and previous['status'] == 'complete':
                for value in previous['files']:
                    check_record(value)
                return previous['result']
            print('Arcilator toolchain: ' + name, flush=True)
            state['phases'][name] = dict(status='running')
            state['status'] = 'running'
            dump(state_path, state)
            started = time.monotonic()
            result = action()
            guard_sources(frozen)
            state['phases'][name] = dict(status='complete', result=result, files=bindings(result),
                                         wall_seconds=time.monotonic() - started)
            dump(state_path, state)
            return result
        try:
            def prepare_local():
                preserve_incomplete(work / 'preparation')
                return invoke('source-and-driver', 'prepare', dict(work=str(work / 'preparation'),
                    sdk=str(sdk), jobs=args.jobs, **{k: str(tools[k]) for k in ('cc', 'cxx', 'cmake', 'ninja')},
                    source=str(args.source.resolve()) if args.source else None))
            prepared = phase('source-and-driver', prepare_local)
            sdk = Path(prepared.get('sdk', sdk))
            flags = dict(SETTINGS, ARC_OPT_FLAGS='-mtriple=' + output([sdk / 'bin/llvm-config', '--host-target']) + ' -mcpu=native')
            def prepare_models():
                preserve_incomplete(work / 'models')
                return invoke('fresh-model-inputs', 'cases', dict(work=str(work / 'models'), sdk=str(sdk),
                    arcilator=prepared['arcilator'], settings=flags, jobs=args.jobs,
                    cxx=str(tools['host_cxx']), cc=str(tools['host_cc']), recipe=str(args.configs.resolve())))
            cases = phase('fresh-model-inputs', prepare_models)
            helper = HERE / 'full_toolchain.py'
            common = [sys.executable, helper, 'build', '--source', prepared['source'],
                      '--source-record', prepared['source_record'], '--compatibility-manifest', prepared['compatibility'],
                      '--sdk', sdk, '--cc', tools['cc'], '--cxx', tools['cxx'], '--linker', tools['linker'],
                      '--cmake', tools['cmake'], '--ninja', tools['ninja'], '--lto', 'thin',
                      '--lto-jobs', str(lto_jobs), '--jobs', str(args.jobs), '--link-jobs', str(args.link_jobs),
                      '--profile-kind', 'frontend', '--profile-update', 'atomic',
                      '--profile-list', HERE / 'frontend-implementation.profile-list', '--unprofiled-warning', 'warning']
            def construct(mode):
                stage = work / mode
                manifest = stage / 'build-manifest.json'
                # A successful full_toolchain publication before a controller
                # interruption remains immutable and is checked by subsequent guards.
                if not manifest.exists():
                    run([*common, '--mode', mode, '--work', stage,
                         *(['--profile-record', work / 'training/profile.json'] if mode == 'pgo' else []),
                         *(['--resume'] if stage.exists() else [])], work / (mode + '.log'), work)
                require(load(stage / 'stage.json')['status'] == 'complete', 'Compiler construction is incomplete')
                return dict(manifest=str(manifest))
            phase('instrumented-compiler', lambda: construct('instrument'))
            instrument = work / 'instrument/prefix'
            def train():
                preserve_incomplete(work / 'training')
                run([sys.executable, helper, 'train', '--manifest', cases['training_manifest'],
                     '--work', work / 'training', '--opt', instrument / 'bin/opt', '--llc', instrument / 'bin/llc',
                     '--llvm-dis', sdk / 'bin/llvm-dis', '--profdata', tools['profdata'], '--jobs', str(case_jobs)],
                    work / 'training.log', work)
                return dict(record=str(work / 'training/profile.json'))
            phase('fresh-profile-training', train)
            phase('profiled-compiler', lambda: construct('pgo'))
            prefix = work / 'pgo/prefix'
            def qualify():
                preserve_incomplete(work / 'qualification')
                run([sys.executable, helper, 'qualify', '--manifest', cases['qualification_manifest'],
                     '--work', work / 'qualification', '--opt', prefix / 'bin/opt', '--llc', prefix / 'bin/llc',
                     '--llvm-dis', sdk / 'bin/llvm-dis', '--jobs', str(case_jobs)], work / 'qualification.log', work)
                return dict(record=str(work / 'qualification/result.json'))
            qualification = phase('all-model-output-qualification', qualify)
            def qualify_api():
                preserve_incomplete(work / 'production-api')
                return invoke('fresh-production-API', 'api', dict(api_path=cases['api_cases'],
                    prefix=str(prefix), sdk=str(sdk), work=str(work), host_cxx=str(tools['host_cxx']), jobs=args.jobs))
            api = phase('fresh-production-API', qualify_api)
            guard_sources(frozen)
            selected = dict(status='source_built_and_output_qualified', sdk=str(sdk), settings=flags,
                tools={name: record(prefix / 'bin' / name) for name in ('opt', 'llc')},
                qualification_records=[record(qualification['record']), record(api['record'])],
                sdk_activation_files=[record(sdk / name) for name in
                    ('bin/firtool', 'bin/llvm-config', 'include/llvm/Config/llvm-config.h', 'include/llvm/Config/abi-breaking.h')],
                source_record=record(prepared['source_record']), profile=record(work / 'training/profile.json'),
                setup_identity=identity, scope='Locally reconstructed recipe and exact compiler output/API qualification; not a new simulator performance measurement.')
            selected['tools']['arcilator'] = record(prepared['arcilator'])
            dump(work / 'selected.json', selected)
            # Copy only the source activation command, not any former toolchain binary.
            (work / 'toolchain.py').write_bytes(Path(__file__).read_bytes())
            (work / 'env.sh').write_text(
                '# Source after Chipyard env.sh. Generated by the checked-in source build command.\n'
                '_arc_toolchain_work="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"\n'
                '_arc_toolchain_exports="$(python3 "$_arc_toolchain_work/toolchain.py" env '
                '--work "$_arc_toolchain_work")" || return\n'
                'eval "$_arc_toolchain_exports"\n'
                'unset _arc_toolchain_exports _arc_toolchain_work\n')
            state.update(status='complete', selected=record(work / 'selected.json'),
                         completed_at_UTC=datetime.datetime.now(datetime.timezone.utc).isoformat())
            dump(state_path, state)
            print('Toolchain built and output-qualified. Activate:\nsource ' + shlex.quote(str(work / 'env.sh')))
        except BaseException as error:
            state.update(status='failed', failed_phase=phase_name, error=str(error))
            dump(state_path, state)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    for action in ('build', 'check'):
        p = sub.add_parser(action, help='Build from source' if action == 'build' else 'Check local prerequisites')
        p.add_argument('--work', type=Path, default=DEFAULT_WORK)
        p.add_argument('--sdk', type=Path)
        p.add_argument('--source', type=Path, help='Optional pristine pinned LLVM source checkout')
        p.add_argument('--configs', type=Path, default=HERE / 'toolchain-configs.json')
        p.add_argument('--jobs', type=int, default=len(os.sched_getaffinity(0)))
        p.add_argument('--lto-jobs', type=int)
        p.add_argument('--link-jobs', type=int, default=1)
        p.add_argument('--training-jobs', type=int)
        p.add_argument('--resume', action='store_true')
        for name in ('cc', 'cxx', 'linker', 'profdata', 'cmake', 'ninja', 'host-cc', 'host-cxx'):
            p.add_argument('--' + name, type=Path)
    p = sub.add_parser('env', help='Print exports from a completed local source build')
    p.add_argument('--work', type=Path, default=DEFAULT_WORK)
    p = sub.add_parser('_worker', help=argparse.SUPPRESS)
    p.add_argument('--request', type=Path, required=True)
    args = parser.parse_args()
    def interrupted(number, frame):
        raise KeyboardInterrupt('Received signal ' + str(number))
    signal.signal(signal.SIGTERM, interrupted)
    try:
        if args.action == '_worker':
            worker(args.request)
        elif args.action == 'env':
            activate(args)
        elif args.action == 'check':
            sdk, tools = preflight(args)
            print(json.dumps(dict(sdk=str(sdk), tools={k: str(v) for k, v in tools.items()}), indent=2))
        else:
            build(args)
    except (ValueError, OSError, KeyError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        parser.exit(1, 'Arcilator source toolchain: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
