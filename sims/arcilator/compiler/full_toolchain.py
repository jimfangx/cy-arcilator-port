#!/usr/bin/env python3
"""Construct and profile complete private LLVM compilers with explicit provenance.

No simulator outputs are reused as build results. Case IDs are opaque labels;
all compiler pipelines, plugins, inputs, and expected outputs come from JSON.
"""
import argparse
import concurrent.futures
import datetime
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
import tempfile
import threading
import time

SCHEMA = 2
PRODUCER_BYTES = Path(__file__).read_bytes()
PRODUCER_SHA256 = hashlib.sha256(PRODUCER_BYTES).hexdigest()
COMPONENTS = ('llvm', 'cmake', 'third-party', 'libc')
LOCK = threading.Lock()
CANCELLED = threading.Event()


class Cancelled(ValueError):
    pass


class CommandError(ValueError):
    def __init__(self, message, record):
        super().__init__(message)
        self.record = record


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def load(path):
    return json.loads(Path(path).read_text())


def dump(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.new')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    os.replace(temporary, path)


def absolute(path):
    # Preserve clang++/ld.lld argv[0] spellings; resolving these symlinks can
    # change the selected compiler language or linker flavor.
    return str(Path(path).absolute())


def checked_file(path, expected=None):
    path = Path(path)
    require(path.is_file(), 'Missing file: ' + str(path))
    actual = sha(path)
    require(expected is None or actual == expected, 'File hash changed: ' + str(path))
    return dict(path=str(path.resolve()), sha256=actual)


def clean_environment():
    env = dict(os.environ)
    for key in list(env):
        if key in ('LD_PRELOAD', 'LD_AUDIT', 'LD_DEBUG', 'LD_DEBUG_OUTPUT',
                   'ARC_COMPILER_SAMPLE_PATH') or key.startswith(('LLVM_PROFILE_', 'CPUPROFILE', 'GCOV_')):
            env.pop(key, None)
    env.update(LC_ALL='C', LANG='C', TZ='UTC')
    return env


def macro_definitions(path):
    result = {}
    for line in Path(path).read_text().splitlines():
        match = re.match(r'^#define (LLVM_\w+)(.*)', line)
        if match:
            result[match[1]] = match[2].strip()
    return result


def source_inventory(source):
    """Bind every relevant checked-in source/build-support byte, not just edits."""
    source = Path(source).resolve()
    result = {}
    for component in COMPONENTS:
        require((source / component).is_dir(), 'Missing required source component: ' + component)
    for path in sorted(source.rglob('*')):
        if '__pycache__' in path.parts or '.git' in path.parts:
            continue
        if path.is_symlink():
            require(path.resolve().is_relative_to(source), 'Source link escapes checkout: ' + str(path))
            result[str(path.relative_to(source))] = hashlib.sha256(
                b'symlink\0' + os.readlink(path).encode()).hexdigest()
        elif path.is_file():
            result[str(path.relative_to(source))] = sha(path)
    return dict(files=result, tree_sha256=fingerprint(result))


def sdk_inventory(sdk):
    """All bytes borrowed by the prefix, including SDK plugin headers/libraries."""
    sdk = Path(sdk).resolve()
    files, links = {}, {}
    for name in ('bin', 'include', 'lib'):
        root = sdk / name
        require(root.is_dir() and not root.is_symlink(), 'SDK root must be a regular directory: ' + str(root))
        for path in sorted(root.rglob('*')):
            relative = str(path.relative_to(sdk))
            if path.is_symlink():
                require(path.exists(), 'Dangling SDK link: ' + str(path))
                require(not path.is_dir(), 'SDK directory symlinks need an explicit materialized SDK: ' + str(path))
                links[relative] = os.readlink(path)
            if path.is_file():
                files[relative] = sha(path)
    return dict(files=files, symlinks=links, tree_sha256=fingerprint(dict(files=files, symlinks=links)))


def executable_identity(path):
    return dict(argv_path=absolute(path), **checked_file(path))


def check_executable(record):
    actual = executable_identity(record['argv_path'])
    require(actual == record, 'Executable or argv-path target changed: ' + record['argv_path'])


def sdk_configuration(sdk):
    sdk = Path(sdk).resolve()
    tool = sdk / 'bin/llvm-config'
    require((sdk / 'include/mlir/Tools/Plugins/PassPlugin.h').is_file(),
            'The matching CIRCT SDK plugin headers are required')
    query = lambda arg: subprocess.check_output([str(tool), arg], text=True).strip()
    macros = macro_definitions(sdk / 'include/llvm/Config/llvm-config.h')
    abi = macro_definitions(sdk / 'include/llvm/Config/abi-breaking.h')
    flags = query('--cxxflags')
    package_config = sdk / 'lib/cmake/llvm/LLVMConfig.cmake'
    package_text = package_config.read_text()
    def package_boolean(name, fallback):
        match = re.search(r'set\(' + re.escape(name) + r'\s+"?(ON|OFF|TRUE|FALSE|1|0)"?\s*\)', package_text, re.IGNORECASE)
        return match[1].upper() in ('ON', 'TRUE', '1') if match else fallback
    system_libraries = shlex.split(query('--system-libs'))
    uses_library = lambda name: any(token == '-l' + name or Path(token).name.startswith('lib' + name + '.') for token in system_libraries)
    match = re.search(r'_GLIBCXX_USE_CXX11_ABI=(\d)', flags)
    require(match is not None, 'SDK must declare its libstdc++ CXX11 ABI')
    return dict(path=str(sdk), version=query('--version'), targets=query('--targets-built').split(),
                assertion_mode=query('--assertion-mode'), cxxflags=flags,
                cxx11_abi=int(match[1]), rtti=query('--has-rtti').upper() in ('YES', 'ON', '1'),
                exceptions=package_boolean('LLVM_ENABLE_EH', '-fno-exceptions' not in flags),
                z3=package_boolean('LLVM_ENABLE_Z3_SOLVER', uses_library('z3')),
                libedit=package_boolean('LLVM_ENABLE_LIBEDIT', uses_library('edit')),
                libxml2=package_boolean('LLVM_ENABLE_LIBXML2', uses_library('xml2')),
                package_config=checked_file(package_config), llvm_config=checked_file(tool),
                system_libraries=system_libraries, macros=macros, abi=abi,
                config=checked_file(sdk / 'include/llvm/Config/llvm-config.h'),
                abi_config=checked_file(sdk / 'include/llvm/Config/abi-breaking.h'))


def process_identity(pid):
    """Linux PID identity; start ticks prevent signalling a recycled PID."""
    try:
        raw = Path('/proc') / str(pid) / 'stat'
        fields = raw.read_text().rsplit(')', 1)[1].split()
        return dict(pid=pid, state=fields[0], ppid=int(fields[1]),
                    process_group=int(fields[2]), session=int(fields[3]), start_ticks=int(fields[19]))
    except (OSError, ValueError, IndexError):
        return None


def same_process(record):
    actual = process_identity(record['pid'])
    return actual is not None and actual['start_ticks'] == record['start_ticks'] and actual['state'] != 'Z'


def owned_processes(root, known=()):
    if not Path('/proc').is_dir():
        return [root] if root and same_process(root) else []
    rows = []
    for path in Path('/proc').iterdir():
        if path.name.isdecimal():
            info = process_identity(int(path.name))
            if info and info['state'] != 'Z':
                rows.append(info)
    seeds = {row['pid'] for row in known if same_process(row)}
    root_alive = root is not None and same_process(root)
    if root_alive:
        seeds.add(root['pid'])
    # Child processes retain this private session even if Ninja makes separate
    # process groups. The session PID cannot be recycled while members exist.
    session = root['session'] if root and (root_alive or seeds) else None
    if session is not None:
        seeds.update(row['pid'] for row in rows if row['session'] == session)
    changed = True
    while changed:
        changed = False
        for row in rows:
            if row['ppid'] in seeds and row['pid'] not in seeds:
                seeds.add(row['pid']); changed = True
    return [row for row in rows if row['pid'] in seeds]


def signal_owned(record, number):
    if same_process(record):
        try:
            os.kill(record['pid'], number)
        except ProcessLookupError:
            pass


def terminate_owned(proc, root, known):
    """Quiesce spawning, capture child groups, then terminate only owned IDs."""
    if root:
        signal_owned(root, signal.SIGSTOP)
    captured = {row['pid']: row for row in known}
    for _ in range(4):
        rows = owned_processes(root, captured.values())
        before = set(captured)
        for row in rows:
            captured[row['pid']] = row
            signal_owned(row, signal.SIGSTOP)
        if set(captured) == before:
            break
        time.sleep(0.02)
    for row in captured.values():
        signal_owned(row, signal.SIGTERM)
        signal_owned(row, signal.SIGCONT)
    if not captured and proc.poll() is None:
        # Popen owns the unreaped child PID even where procfs is unavailable.
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline and any(same_process(row) for row in captured.values()):
        time.sleep(0.02)
    for row in captured.values():
        signal_owned(row, signal.SIGKILL)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait()
    return [row for row in captured.values() if same_process(row)]


def ensure_no_active_jobs(path):
    if not Path(path).exists():
        return
    active = load(path)
    for row in active.values():
        require(not owned_processes(row.get('identity'), row.get('owned', [])),
                'Prior owned processes still exist; construction cannot resume: ' + str(path))


def run(argv, log, env, active=None, name='child', cwd=None):
    require(not CANCELLED.is_set(), 'Stage cancelled before child launch')
    Path(log).parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    started_UTC = datetime.datetime.now(datetime.timezone.utc).isoformat()
    with Path(log).open('wb') as stream:
        proc = subprocess.Popen(list(map(str, argv)), stdout=stream, stderr=subprocess.STDOUT,
                                env=env, cwd=cwd, start_new_session=True)
        identity = process_identity(proc.pid)
        known = []
        entry = dict(pid=proc.pid, process_group=proc.pid, identity=identity, owned=[],
                     argv=list(map(str, argv)), cwd=str(Path(cwd or os.getcwd()).resolve()),
                     log=str(log), started_UTC=started_UTC)
        if active is not None:
            with LOCK:
                active['jobs'][name] = entry
                dump(active['path'], active['jobs'])
        try:
            while True:
                if CANCELLED.is_set():
                    raise Cancelled('Stage cancelled')
                known = owned_processes(identity, known)
                entry['owned'] = known
                if active is not None:
                    with LOCK:
                        dump(active['path'], active['jobs'])
                try:
                    returncode = proc.wait(timeout=0.2)
                    break
                except subprocess.TimeoutExpired:
                    pass
            survivors = [row for row in owned_processes(identity, known) if row['pid'] != proc.pid]
            if survivors:
                terminate_owned(proc, identity, survivors)
                raise ValueError('Child returned while owned descendants remained active')
        except BaseException:
            survivors = terminate_owned(proc, identity, known)
            entry.update(returncode=proc.returncode, aborted=True, remaining_owned=survivors)
            raise
        finally:
            if active is not None:
                with LOCK:
                    if not entry.get('remaining_owned'):
                        active['jobs'].pop(name, None)
                    dump(active['path'], active['jobs'])
    entry.update(returncode=returncode, diagnostic_seconds=time.monotonic() - started,
                 log_sha256=sha(log))
    if returncode != 0:
        raise CommandError('Command failed; retained log: ' + str(log), entry)
    text = Path(log).read_text(errors='replace')
    require('LLVM Profile Error' not in text and 'LLVM Profile Warning' not in text,
            'Profile runtime diagnostic; retained log: ' + str(log))
    return entry


def verify_profile(record_path, identity):
    record = load(record_path)
    require(record.get('schema') == SCHEMA and record.get('status') == 'passed' and record.get('training') is True,
            'Profile construction did not finish with this workflow schema')
    require(record.get('profile_kind') == identity['profile_kind'] and
            record.get('profile_update') == identity['profile_update'], 'Profile kind/update policy differs')
    require(record['configuration_fingerprint'] == fingerprint(identity), 'Profile/source/config/LTO binding differs')
    checked_file(record['merged']['path'], record['merged']['sha256'])
    checked_file(record['argv_manifest']['path'], record['argv_manifest']['sha256'])
    checked_file(record['instrumented_manifest']['path'], record['instrumented_manifest']['sha256'])
    instrumented = load(record['instrumented_manifest']['path'])
    require(instrumented['mode'] == 'instrument', 'Profile producer was not instrumented')
    require(instrumented['configuration_identity'] == identity, 'Instrumented construction identity differs')
    require(set(record['instrumented_tools']) == {'opt', 'llc'}, 'Profile omits compiler tools')
    for tool, value in record['instrumented_tools'].items():
        check_executable(value)
        require(value['sha256'] == instrumented['outputs'][tool]['sha256'], 'Profile tool was replaced')
    for value in record['auxiliary_tools'].values():
        check_executable(value)
    for raw in record['raw_profiles']:
        checked_file(raw['path'], raw['sha256'])
    for artifact in record['inputs_and_references']:
        checked_file(artifact['path'], artifact['sha256'])
    for result in record['results']:
        for output in result['outputs']:
            checked_file(output['path'], output['sha256'])
    return Path(record['merged']['path'])


def compatible_pair(record_path, source_revision, tree_sha256, sdk):
    """Require an explicitly validated exact source/SDK pair, not version24 alone."""
    record = load(record_path)
    require(record.get('status') == 'validated-pinned-sdk-source-pair',
            'A validated pinned compatibility manifest is required')
    require(record['llvm_revision'] == source_revision, 'Source revision is not SDK-qualified')
    require(record['source_tree_sha256'] == tree_sha256, 'This patched source tree is not SDK-qualified')
    require(record.get('public_header_layout_changes') is False,
            'This workflow only supports the validated SDK-compatible public ABI')
    required = {'bin/llvm-config', 'bin/opt', 'bin/llc', 'bin/firtool',
                'include/llvm/Config/llvm-config.h', 'include/llvm/Config/abi-breaking.h',
                'lib/cmake/llvm/LLVMConfig.cmake'}
    require(required <= set(record['sdk_files']), 'Compatibility manifest omits SDK identity inputs')
    for relative in record['sdk_files']:
        path = Path(relative)
        require(not path.is_absolute() and '..' not in path.parts, 'Unsafe compatibility SDK path')
    actual = sdk_inventory(sdk)
    require(record['sdk_files'] == actual['files'], 'Complete borrowed SDK byte inventory differs')
    require(record.get('sdk_symlinks', {}) == actual['symlinks'], 'Borrowed SDK symlink inventory differs')
    require(record.get('sdk_tree_sha256') == actual['tree_sha256'], 'SDK inventory fingerprint differs')
    return checked_file(record_path)


def audit_profile_warnings(work, mode, policy):
    """Keep even failed attempts' warnings visible, source-counted and immutable."""
    log = Path(work) / 'build.log'
    warnings = []
    for line in log.read_text(errors='replace').splitlines():
        if 'warning:' not in line:
            continue
        unprofiled = (mode == 'pgo' and policy['unprofiled_missing_file'] == 'warning'
                      and '[-Wprofile-instr-unprofiled]' in line)
        # LLD does not implement Clang's -Wno-pass-failed. Keep this exact
        # missed transformation visible; profile and unknown warnings still
        # reject publication in the audit below.
        plain = re.sub(r'\x1b\[[0-9;]*m', '', line)
        missed_unroll = (mode == 'pgo' and policy.get('lto_missed_loop_unroll') == 'warning'
                         and re.fullmatch(
                             r'(?:.*[/\\])?ld\.lld(?:-\d+)?: warning: (?:.*?:\d+:\d+: )?'
                             r'loop not unrolled: the optimizer was unable to perform the requested '
                             r'transformation; the transformation might be disabled or specified as part '
                             r'of an unsupported transformation ordering', plain) is not None)
        allowed = unprofiled or missed_unroll
        match = re.search(r"no profile data available for file\s*:?\s*['\"]([^'\"]+)", line)
        source_name = match[1] if match else None
        diagnostic = 'profile-instr-unprofiled' if unprofiled else 'lto-missed-loop-unroll' if missed_unroll else 'other'
        warnings.append(dict(source=source_name, diagnostic=diagnostic,
                             visible_line=line, permitted=allowed))
    counts = {}
    for warning in warnings:
        source_name = warning['source'] or '<source not emitted>'
        counts[source_name] = counts.get(source_name, 0) + 1
    audit = dict(policy=policy, count=len(warnings), by_source=counts, warnings=warnings, build_log_sha256=sha(log))
    dump(Path(work) / 'profile-warning-audit.json', audit)
    return audit


def build(args):
    args._stage_owned = False
    try:
        return build_stage(args)
    except BaseException as error:
        if args._stage_owned:
            state = dict(schema=SCHEMA, status='failed', error=repr(error),
                         producer_sha256=PRODUCER_SHA256)
            if isinstance(error, CommandError):
                state['command'] = error.record
            dump(args.work.absolute() / 'stage.json', state)
        raise


def build_stage(args):
    source, work = args.source.resolve(), args.work.absolute()
    require(not work.resolve().is_relative_to(source), 'Build work must be outside source tree')
    pin = load(args.source_record)
    reserved_flags = ('-fprofile', '-fno-profile', '-fcs-profile', '-fcoverage',
                      '-flto', '-fno-lto', '--ld-path', '-fuse-ld',
                      '-include', '-imacros', '-isystem', '-idirafter', '-I', '@',
                      '-Wno-profile', '-Wno-error', '-w', '-mllvm', '-Xclang')
    require(not any(token.startswith(reserved_flags) for token in shlex.split(args.compiler_flags)),
            'Instrumentation/LTO/linker/include flags have dedicated guarded workflow inputs')
    require(re.fullmatch(r'[0-9a-f]{40}', pin['revision']) is not None, 'Exact source revision is required')
    inventory = source_inventory(source)
    require(inventory['tree_sha256'] == pin['tree_sha256'], 'Complete source inventory differs from source record')
    compatibility = compatible_pair(args.compatibility_manifest, pin['revision'], inventory['tree_sha256'], args.sdk)
    compatibility_record = load(args.compatibility_manifest)
    require(not compatibility_record.get('requires_atomic_profile_updates') or args.profile_update == 'atomic',
            'This source/parallel-analysis policy requires atomic instrumentation updates')
    require(compatibility_record.get('compiler_flags', []) == shlex.split(args.compiler_flags),
            'Compiler flags are not covered by the validated SDK ABI review')
    sdk = sdk_configuration(args.sdk)
    identity = dict(schema=SCHEMA, revision=pin['revision'], source=str(source),
                    source_record=checked_file(args.source_record), compatibility_manifest=compatibility, source_tree_sha256=inventory['tree_sha256'],
                    sdk=sdk, cc=executable_identity(args.cc), cxx=executable_identity(args.cxx),
                    linker=executable_identity(args.linker), cmake=executable_identity(args.cmake),
                    ninja=executable_identity(args.ninja), compiler_flags=shlex.split(args.compiler_flags),
                    build_type='Release', lto=args.lto,
                    lto_backend_jobs=(args.lto_jobs or args.jobs) if args.lto == 'thin' else None,
                    profile_kind=args.profile_kind, profile_update=args.profile_update,
                    profile_list=checked_file(args.profile_list) if args.profile_list else None,
                    exports_for_plugins=True, helper_sha256=PRODUCER_SHA256)
    common_fingerprint = fingerprint(identity)
    consumer_diagnostics = dict(applies_to='pgo-consumer-only',
        unprofiled_missing_file=args.unprofiled_warning,
        lto_missed_loop_unroll='warning' if args.lto == 'thin' else 'fatal',
        hash_out_of_date_COMDAT_and_other_warnings='fatal', warnings_suppressed=False)
    # Never replace a prior stage's immutable publication or changed recipe.
    if work.exists():
        require(args.resume, 'Fresh work directory required (or explicit same-identity --resume)')
        require(not (work / 'build-manifest.json').exists(), 'Completed stage is immutable; choose new work')
        previous = load(work / 'recipe.json')
        require(previous['configuration_fingerprint'] == common_fingerprint and previous['mode'] == args.mode,
                'Resume source/configuration differs; choose new work')
        require(previous['producer']['sha256'] == PRODUCER_SHA256, 'Resume helper changed; choose new work')
        require(previous.get('consumer_diagnostics') == consumer_diagnostics,
                'Resume consumer diagnostic policy differs; preserve this stage and choose fresh work')
        requested_profile = checked_file(args.profile_record) if args.profile_record else None
        require(previous['profile_record'] == requested_profile, 'Resume profile record changed; choose new work')
        ensure_no_active_jobs(work / 'active-jobs.json')
    else:
        work.mkdir(parents=True)
    require(args.mode != 'pgo' or args.profile_record is not None, 'PGO requires a validated profile record')
    profile = verify_profile(args.profile_record, identity) if args.mode == 'pgo' else None
    consumed_profile = artifact_identity(load(args.profile_record)['merged']) if profile is not None else None
    require(args.mode == 'pgo' or args.profile_record is None, 'Profile is only valid for PGO mode')
    cpp_tokens = ['-D_GLIBCXX_USE_CXX11_ABI=' + str(sdk['cxx11_abi']), *shlex.split(args.compiler_flags)]
    options = []
    linker_flags = shlex.quote('--ld-path=' + absolute(args.linker))
    if args.mode == 'instrument':
        options += ['-DLLVM_BUILD_INSTRUMENTED=' + ('Frontend' if args.profile_kind == 'frontend' else 'IR'),
                    '-DLLVM_PROFILE_DATA_DIR=' + str(work / 'build-profiles')]
    if args.mode == 'pgo':
        cpp_tokens.append('-Werror')
        if args.unprofiled_warning == 'warning':
            cpp_tokens.append('-Wno-error=profile-instr-unprofiled')
        options += ['-DLLVM_PROFDATA_FILE=' + str(profile)]
    compile_options = []
    if args.mode == 'instrument':
        compile_options.append('"$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-update=' + args.profile_update + '>"')
    if args.profile_list and args.mode in ('instrument', 'pgo'):
        value = '$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-list=' + str(args.profile_list.resolve()) + '>'
        compile_options.append('"' + value.replace('\\', '\\\\').replace('"', '\\"').replace(';', '\\;') + '"')
    if args.mode == 'pgo':
        compile_options += ['"$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -no-pgo-warn-mismatch=false>"',
                            '"$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -no-pgo-warn-mismatch-comdat-weak=false>"']
    if compile_options:
        hook = work / 'profile-compile-options.cmake'
        hook.write_text('add_compile_options(\n  ' + '\n  '.join(compile_options) + ')\n')
        options += ['-DCMAKE_PROJECT_LLVM_INCLUDE=' + str(hook)]
    if args.lto == 'thin':
        linker_flags += ' -Wl,--thinlto-jobs=' + str(identity['lto_backend_jobs'])
        if args.mode == 'pgo':
            linker_flags += (' -Wl,-mllvm,-no-pgo-warn-mismatch=false'
                             ' -Wl,-mllvm,-no-pgo-warn-mismatch-comdat-weak=false -Wl,--no-fatal-warnings')
    cpp = ' '.join(shlex.quote(token) for token in cpp_tokens)
    boolean = lambda value: 'ON' if value else 'OFF'
    macros, abi = sdk['macros'], sdk['abi']
    flags = dict(CMAKE_BUILD_TYPE='Release', CMAKE_C_COMPILER=absolute(args.cc),
                 CMAKE_CXX_COMPILER=absolute(args.cxx), CMAKE_MAKE_PROGRAM=absolute(args.ninja),
                 CMAKE_C_FLAGS=cpp, CMAKE_CXX_FLAGS=cpp, CMAKE_EXE_LINKER_FLAGS=linker_flags,
                 CMAKE_SHARED_LINKER_FLAGS=linker_flags, CMAKE_LINKER=absolute(args.linker),
                 CMAKE_EXPORT_COMPILE_COMMANDS='ON', LLVM_USE_LINKER='',
                 LLVM_ENABLE_LTO='Thin' if args.lto == 'thin' else 'OFF',
                 LLVM_TARGETS_TO_BUILD=';'.join(sdk['targets']),
                 LLVM_HOST_TRIPLE=macros['LLVM_HOST_TRIPLE'].strip('"'),
                 LLVM_DEFAULT_TARGET_TRIPLE=macros['LLVM_DEFAULT_TARGET_TRIPLE'].strip('"'),
                 LLVM_ENABLE_PROJECTS='', LLVM_ENABLE_RUNTIMES='', LLVM_ENABLE_ASSERTIONS=sdk['assertion_mode'],
                 LLVM_ABI_BREAKING_CHECKS='FORCE_ON' if abi['LLVM_ENABLE_ABI_BREAKING_CHECKS'] == '1' else 'FORCE_OFF',
                 LLVM_ENABLE_REVERSE_ITERATION=boolean(abi['LLVM_ENABLE_REVERSE_ITERATION'] == '1'),
                 LLVM_ENABLE_RTTI=boolean(sdk['rtti']), LLVM_ENABLE_EH=boolean(sdk['exceptions']),
                 LLVM_ENABLE_PIC='ON', LLVM_ENABLE_THREADS=boolean(macros['LLVM_ENABLE_THREADS'] == '1'),
                 LLVM_ENABLE_LLVM_EXPORT_ANNOTATIONS=boolean('LLVM_ENABLE_LLVM_EXPORT_ANNOTATIONS' in macros),
                 LLVM_ENABLE_Z3_SOLVER=boolean(sdk['z3']), LLVM_ENABLE_LIBEDIT=boolean(sdk['libedit']),
                 LLVM_FORCE_ENABLE_STATS=boolean(macros['LLVM_FORCE_ENABLE_STATS'] == '1'),
                 LLVM_ENABLE_LZMA=boolean(macros['LLVM_ENABLE_LZMA'] == '1'),
                 LLVM_ENABLE_ZLIB='FORCE_ON' if macros['LLVM_ENABLE_ZLIB'] == '1' else 'OFF',
                 LLVM_ENABLE_ZSTD=boolean(macros['LLVM_ENABLE_ZSTD'] == '1'),
                 LLVM_ENABLE_LIBXML2=boolean(sdk['libxml2']),
                 LLVM_EXPORT_SYMBOLS_FOR_PLUGINS='ON', LLVM_BUILD_LLVM_DYLIB='OFF',
                 LLVM_LINK_LLVM_DYLIB='OFF', LLVM_BUILD_LLVM_C_DYLIB='OFF', BUILD_SHARED_LIBS='OFF',
                 LLVM_INCLUDE_TESTS='OFF', LLVM_INCLUDE_BENCHMARKS='OFF', LLVM_INCLUDE_EXAMPLES='OFF',
                 LLVM_INCLUDE_DOCS='OFF', LLVM_APPEND_VC_REV='OFF', LLVM_PARALLEL_LINK_JOBS=str(args.link_jobs))
    command = [absolute(args.cmake), '-S', str(source / 'llvm'), '-B', str(work / 'build'), '-G', 'Ninja',
               *['-D' + key + '=' + value for key, value in flags.items()], *options]
    recipe = dict(schema=SCHEMA, mode=args.mode, configuration_identity=identity,
                  consumer_diagnostics=consumer_diagnostics,
                  configuration_fingerprint=common_fingerprint, configure_argv=command,
                  profile_record=checked_file(args.profile_record) if args.profile_record else None,
                  producer=dict(path=str(work / 'producer.py'), sha256=PRODUCER_SHA256, original_path=absolute(__file__)), affinity=sorted(os.sched_getaffinity(0)), jobs=args.jobs,
                  link_jobs=args.link_jobs, compiler_construction_only=True)
    dump(work / 'recipe.json', recipe)
    args._stage_owned = True
    dump(work / 'stage.json', dict(schema=SCHEMA, status='running', phase='configure',
                                   configuration_fingerprint=common_fingerprint, resumed=bool(args.resume)))
    (work / 'producer.py').write_bytes(PRODUCER_BYTES)
    dump(work / 'source-inventory.json', inventory)
    env, active = clean_environment(), dict(path=work / 'active-jobs.json', jobs={})
    def guard(full_sdk=False):
        checked_file(args.source_record, identity['source_record']['sha256'])
        checked_file(args.compatibility_manifest, identity['compatibility_manifest']['sha256'])
        if identity['profile_list']:
            checked_file(identity['profile_list']['path'], identity['profile_list']['sha256'])
        for name in ('cc', 'cxx', 'linker', 'cmake', 'ninja'):
            check_executable(identity[name])
        require(sdk_configuration(args.sdk) == sdk, 'SDK configuration changed')
        if full_sdk:
            compatible_pair(args.compatibility_manifest, pin['revision'], inventory['tree_sha256'], args.sdk)
        if args.profile_record:
            checked_file(args.profile_record, recipe['profile_record']['sha256'])
            check_artifact(consumed_profile)
    prefix = work / 'prefix'
    if prefix.exists():
        require(args.resume and (prefix / 'build-manifest.json').is_file(), 'Unknown partial publication; preserve and use fresh work')
        published = load(prefix / 'build-manifest.json')
        require(published['configuration_fingerprint'] == common_fingerprint and published['mode'] == args.mode,
                'Published prefix does not belong to this recipe')
        require(published.get('schema') == SCHEMA and published.get('status') == 'built_requires_output_qualification' and
                published.get('configuration_identity') == identity, 'Partial publication metadata is inconsistent')
        require(published['configure']['returncode'] == published['construction']['returncode'] == 0,
                'Partial publication did not finish construction')
        for operation in (published['configure'], published['construction'], *published['versions'].values()):
            checked_file(operation['log'], operation['log_sha256'])
        for value in published['outputs'].values():
            checked_file(value['path'], value['sha256'])
        guard(full_sdk=True)
        dump(work / 'build-manifest.json', published)
        dump(work / 'stage.json', dict(schema=SCHEMA, status='complete', recovered_publication=True))
        print(json.dumps(dict(status=published['status'], prefix=str(prefix), recovered_publication=True)))
        return
    guard()
    configure = run(command, work / 'configure.log', env, active, 'configure')
    audit = {}
    for filename in ('llvm-config.h', 'abi-breaking.h'):
        expected = macro_definitions(Path(sdk['path']) / 'include/llvm/Config' / filename)
        actual = macro_definitions(work / 'build/include/llvm/Config' / filename)
        audit[filename] = {key: dict(sdk=expected.get(key), candidate=actual.get(key))
                           for key in expected.keys() | actual.keys() if expected.get(key) != actual.get(key)}
    dump(work / 'generated-config-audit.json', audit)
    require(not any(audit.values()), 'SDK public generated configuration mismatch; compilation refused')
    commands = subprocess.check_output([absolute(args.ninja), '-C', str(work / 'build'), '-t', 'commands',
                                        'opt', 'llc'], text=True, env=env)
    (work / 'all-build-commands.txt').write_text(commands)
    require('--thinlto-cache-dir' not in commands, 'Unexpected incremental LTO cache')
    compiles = [line for line in commands.splitlines() if ' -c ' in line and str(source) in line]
    require(compiles, 'No LLVM implementation compile commands were generated')
    profiled_compiles, assembly_compiles = [], []
    for line in compiles:
        words = shlex.split(line)
        input_path = words[words.index('-c') + 1]
        if Path(input_path).suffix.lower() in ('.s', '.asm'):
            assembly_compiles.append(line)
        else:
            profiled_compiles.append(line)
    require(profiled_compiles, 'No C/C++ implementation compile commands were generated')
    dump(work / 'profile-command-audit.json', dict(profile_kind=args.profile_kind, profile_update=args.profile_update,
         C_CXX_compile_count=len(profiled_compiles), assembly_nonconsumer_count=len(assembly_compiles),
         assembly_nonconsumers=assembly_compiles, all_commands_sha256=sha(work / 'all-build-commands.txt')))
    if args.mode == 'instrument':
        generation = '-fprofile-instr-generate=' if args.profile_kind == 'frontend' else '-fprofile-generate='
        require(all(generation in line and '-fprofile-update=' + args.profile_update in line for line in profiled_compiles),
                'Actual compile commands omit the declared instrumentation kind/update policy')
        links = [line for line in commands.splitlines() if '-o bin/opt ' in line or '-o bin/llc ' in line]
        require(all('-fprofile-update=' not in line for line in links), 'Profile update option leaked into link flags')
    if args.profile_list and args.mode in ('instrument', 'pgo'):
        require(all('-fprofile-list=' in line for line in profiled_compiles), 'Actual compile commands omit profile filtering')
    if args.mode == 'pgo':
        require(all('-Werror' in line and '-fprofile-instr-use=' in line and '-no-pgo-warn-mismatch=false' in line and
                    '-no-pgo-warn-mismatch-comdat-weak=false' in line for line in profiled_compiles),
                'Strict mismatch flags are missing from actual compile commands')
        if args.unprofiled_warning == 'warning':
            require(all('-Wno-error=profile-instr-unprofiled' in line for line in profiled_compiles),
                    'Visible nonfatal missing-file policy is absent from actual PGO commands')
        if args.lto == 'thin':
            links = [line for line in commands.splitlines() if '-o bin/opt ' in line or '-o bin/llc ' in line]
            require(len(links) == 2 and all('--no-fatal-warnings' in line and '--fatal-warnings' not in line and
                        '-no-pgo-warn-mismatch-comdat-weak=false' in line for line in links),
                    'Strict diagnostics are missing from LTO backend commands')
    guard()
    dump(work / 'stage.json', dict(schema=SCHEMA, status='running', phase='construction', resumed=bool(args.resume)))
    try:
        construction = run([absolute(args.ninja), '-C', str(work / 'build'), '-j' + str(args.jobs),
                            'opt', 'llc'], work / 'build.log', env, active, 'build')
    except BaseException:
        if (work / 'build.log').is_file():
            audit_profile_warnings(work, args.mode, consumer_diagnostics)
        raise
    warning_audit = audit_profile_warnings(work, args.mode, consumer_diagnostics)
    if args.mode == 'pgo':
        require(all(warning['permitted'] for warning in warning_audit['warnings']),
                'A nonpermitted diagnostic occurred; retained profile-warning-audit.json')
    require(source_inventory(source)['tree_sha256'] == inventory['tree_sha256'], 'Source changed during construction')
    guard()
    staging = work / '.prefix.pending'
    owner = dict(configuration_fingerprint=common_fingerprint, mode=args.mode, producer_sha256=PRODUCER_SHA256)
    if staging.exists():
        require(args.resume and not staging.is_symlink() and (staging / '.owner.json').is_file(),
                'Unknown staging prefix; preserve and use fresh work')
        require(load(staging / '.owner.json') == owner, 'Staging prefix belongs to another recipe')
        shutil.rmtree(staging)
    staging.mkdir()
    dump(staging / '.owner.json', owner)
    for name in ('include', 'lib'):
        (staging / name).symlink_to(Path(sdk['path']) / name, target_is_directory=True)
    (staging / 'bin').mkdir()
    for tool in (Path(sdk['path']) / 'bin').iterdir():
        if tool.name in ('opt', 'llc'):
            shutil.copy2(work / 'build/bin' / tool.name, staging / 'bin' / tool.name)
        else:
            (staging / 'bin' / tool.name).symlink_to(tool, target_is_directory=tool.is_dir())
    outputs = {tool: dict(path=str(prefix / 'bin' / tool), sha256=sha(staging / 'bin' / tool)) for tool in ('opt', 'llc')}
    versions = {}
    (work / 'startup-profiles').mkdir(exist_ok=True)
    for tool in ('opt', 'llc'):
        smoke_env = dict(env, LLVM_PROFILE_FILE=str(work / 'startup-profiles' / (tool + '-%p-%m.profraw')))
        versions[tool] = run([str(staging / 'bin' / tool), '--version'], work / (tool + '-version.log'),
                             smoke_env, active, tool + '-version', cwd=work)
        require('LLVM version ' + sdk['version'] in (work / (tool + '-version.log')).read_text(),
                'Built tool does not report the SDK LLVM version')
    manifest = dict(recipe, status='built_requires_output_qualification', configure=configure,
                    construction=construction, versions=versions, outputs=outputs, source_inventory=checked_file(work / 'source-inventory.json'),
                    profile_warning_audit=checked_file(work / 'profile-warning-audit.json'),
                    SDK_configuration_rechecked=sdk_configuration(args.sdk) == sdk,
                    compiler_construction_seconds_excluded_from_simulator_timings=True)
    require(manifest['SDK_configuration_rechecked'], 'SDK configuration changed during construction')
    guard(full_sdk=True)
    require(source_inventory(source)['tree_sha256'] == inventory['tree_sha256'], 'Source changed before publication')
    dump(staging / 'build-manifest.json', manifest)
    require(not prefix.exists(), 'Publication destination unexpectedly exists')
    os.replace(staging, prefix)
    dump(work / 'build-manifest.json', manifest)
    dump(work / 'stage.json', dict(schema=SCHEMA, status='complete', resumed=bool(args.resume)))
    print(json.dumps(dict(status=manifest['status'], prefix=str(prefix), outputs=outputs), indent=2))


def record_source(args):
    source = args.source.resolve()
    require(not args.output.resolve().is_relative_to(source), 'Source record must be outside the immutable checkout')
    require(re.fullmatch(r'[0-9a-f]{40}', args.revision) is not None, 'A complete revision is required')
    provenance = checked_file(args.provenance)
    git_head = None
    if (source / '.git').exists():
        git_head = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        require(git_head == args.revision, 'Git source revision differs')
    inventory = source_inventory(source)
    compatible_pair(args.compatibility_manifest, args.revision, inventory['tree_sha256'], args.sdk)
    require(not args.output.exists(), 'Source record must be fresh')
    dump(args.output, dict(schema=SCHEMA, revision=args.revision, git_head_verified=git_head,
                          supplied_source_provenance=provenance, source=str(source),
                          tree_sha256=inventory['tree_sha256'], inventory=inventory,
                          scope='Records an already verified private checkout/archive and applied patches; does not fetch or authenticate a supplied archive.'))


def artifact_identity(value):
    path = Path(value['path'])
    require(path.is_absolute(), 'Immutable input/reference paths must be absolute: ' + str(path))
    info = checked_file(path, value['sha256'])
    return dict(path=absolute(path), resolved_path=info['path'], sha256=info['sha256'])


def check_artifact(value):
    checked_file(value['path'], value['sha256'])
    require(str(Path(value['path']).resolve()) == value['resolved_path'], 'Input symlink target changed: ' + value['path'])


def case_output_path(value, directory):
    """Reject collisions before a compiler can overwrite helper evidence."""
    output = Path(value)
    output = (output if output.is_absolute() else directory / output).resolve()
    require(output.is_relative_to(directory), 'Output escapes fresh case work')
    relative = output.relative_to(directory)
    require(relative.parts, 'A case output must be a file path')
    first = relative.parts[0]
    require(first not in ('result.json', 'result.json.new') and
            re.fullmatch(r'[0-9]+\.log(?:\.new)?', first) is None and
            not first.startswith('.canonical-'), 'Output collides with helper-owned case evidence: ' + str(output))
    return output


def validate_argv(argv, directory, immutable):
    protected = {value['resolved_path'] for value in immutable}
    output_positions = set()
    for index, value in enumerate(argv[1:], 1):
        require(not value.startswith('@'), 'Response files must be expanded into the pinned argv manifest')
        if value in ('-o', '--o'):
            require(index + 1 < len(argv), 'Missing output argument')
            output_positions.add(index + 1)
            case_output_path(argv[index + 1], directory)
        elif value.startswith(('-o=', '--o=')):
            output_positions.add(index)
            case_output_path(value.split('=', 1)[1], directory)
    for index, value in enumerate(argv[1:], 1):
        if index in output_positions:
            continue
        candidates = [value]
        if value.startswith('-') and '=' in value:
            candidates.append(value.split('=', 1)[1])
        for candidate in candidates:
            path = Path(candidate)
            path = path if path.is_absolute() else directory / path
            try:
                exists = path.is_file()
            except OSError:
                exists = False
            if exists and not path.resolve().is_relative_to(directory):
                require(str(path.resolve()) in protected,
                        'Compiler input/plugin missing from immutable manifest: ' + str(path))


def canonical_ir(path, destination, tool, env, active, name, cwd):
    operation = run([tool['argv_path'], str(path), '-o', str(destination)],
                    destination.with_suffix('.log'), env, active, name, cwd=cwd)
    h = hashlib.sha256()
    with destination.open('rb') as stream:
        for line in stream:
            if not line.startswith(b'; ModuleID = '):
                h.update(line)
    return h.hexdigest(), operation


def execute_cases(args, training):
    work = args.work.absolute()
    require(not work.exists(), 'A fresh case/profile directory is required')
    supplied_manifest = checked_file(args.manifest)
    manifest = load(args.manifest)
    cases = manifest['cases']
    require(cases and isinstance(cases, list), 'A nonempty opaque case list is required')
    identifiers = [case['id'] for case in cases]
    require(len(set(identifiers)) == len(identifiers), 'Case IDs must be distinct')
    require(all(re.fullmatch(r'[A-Za-z0-9_-][A-Za-z0-9_.-]*', item) for item in identifiers), 'Unsafe case ID')
    tools = {key: executable_identity(path) for key, path in [('opt', args.opt), ('llc', args.llc)]}
    origins = [Path(value['argv_path']).parent.parent / 'build-manifest.json' for value in tools.values()]
    origins_checked = [checked_file(path) for path in origins]
    require(origins_checked[0]['sha256'] == origins_checked[1]['sha256'], 'Tools come from different construction stages')
    construction = load(origins[0])
    require(construction.get('schema') == SCHEMA and construction['status'] == 'built_requires_output_qualification',
            'Incomplete/incompatible compiler publication')
    construction_work = origins[0].parent.parent
    checked_file(construction_work / 'build-manifest.json', origins_checked[0]['sha256'])
    require(load(construction_work / 'stage.json')['status'] == 'complete', 'Construction publication did not finish')
    if training:
        require(construction['mode'] == 'instrument', 'Training requires an instrumented compiler')
        require(construction['configuration_identity']['profile_update'] == 'atomic' or
                manifest.get('instrumented_tools_single_threaded') is True,
                'Single-update training requires an explicit single-threaded-tools manifest declaration')
    for tool, value in tools.items():
        require(value['sha256'] == construction['outputs'][tool]['sha256'], 'Compiler publication changed')
        require(not Path(value['argv_path']).is_symlink(), 'Compiler publication must be a regular immutable file')
    identity = construction['configuration_identity']
    require(identity.get('helper_sha256') == PRODUCER_SHA256,
            'Construction producer differs; use its frozen producer.py or fresh matching construction')
    auxiliary = {'llvm_dis': executable_identity(args.llvm_dis)}
    sdk_pair = load(identity['compatibility_manifest']['path'])
    require(auxiliary['llvm_dis']['sha256'] == sdk_pair['sdk_files']['bin/llvm-dis'],
            'Canonical IR decoder must match the pinned SDK llvm-dis')
    if training:
        auxiliary['profdata'] = executable_identity(args.profdata)
    immutable = [artifact_identity(value) for value in manifest.get('inputs', [])]
    for case in cases:
        require(isinstance(case.get('steps'), list) and case['steps'], 'Each case requires a nonempty compiler pipeline')
        require(case.get('outputs'), 'Each case must declare validated outputs')
        for output in case['outputs']:
            require(output['comparison'] in ('bytes', 'llvm-ir'), 'Unknown comparison mode')
            immutable.append(artifact_identity(output['reference']))
    by_path = {}
    for artifact in immutable:
        key = artifact['resolved_path']
        require(key not in by_path or by_path[key] == artifact['sha256'], 'Conflicting hashes for the same input')
        by_path[key] = artifact['sha256']
    work.mkdir(parents=True)
    (work / 'profiles').mkdir()
    (work / 'cases').mkdir()
    shutil.copy2(args.manifest, work / 'argv-manifest.json')
    (work / 'producer.py').write_bytes(PRODUCER_BYTES)
    env = clean_environment()
    active = dict(path=work / 'active-jobs.json', jobs={})
    dump(active['path'], {})
    record = dict(schema=SCHEMA, status='running', training=training, case_ids=identifiers,
                  profile_kind=identity['profile_kind'], profile_update=identity['profile_update'],
                  configuration_fingerprint=construction['configuration_fingerprint'], results=[],
                  argv_manifest=checked_file(work / 'argv-manifest.json'), argv_manifest_as_supplied=supplied_manifest,
                  instrumented_manifest=origins_checked[0], instrumented_tools=tools,
                  auxiliary_tools=auxiliary, inputs_and_references=immutable,
                  producer=checked_file(work / 'producer.py'),
                  timing_scope='Concurrent diagnostic/training; no performance claim')
    dump(work / 'result.json', record)

    def guard(full=False):
        checked_file(args.manifest, supplied_manifest['sha256'])
        checked_file(work / 'argv-manifest.json', record['argv_manifest']['sha256'])
        for path, value in zip(origins, origins_checked):
            checked_file(path, value['sha256'])
        checked_file(construction_work / 'build-manifest.json', origins_checked[0]['sha256'])
        require(load(construction_work / 'stage.json')['status'] == 'complete', 'Construction state changed')
        for value in list(tools.values()) + list(auxiliary.values()):
            check_executable(value)
        for artifact in immutable:
            check_artifact(artifact)
        if identity.get('profile_list'):
            checked_file(identity['profile_list']['path'], identity['profile_list']['sha256'])
        if full:
            checked_file(identity['source_record']['path'], identity['source_record']['sha256'])
            checked_file(identity['compatibility_manifest']['path'], identity['compatibility_manifest']['sha256'])
            require(source_inventory(identity['source'])['tree_sha256'] == identity['source_tree_sha256'],
                    'Compiler source tree changed since construction')
            compatible_pair(identity['compatibility_manifest']['path'], identity['revision'],
                            identity['source_tree_sha256'], identity['sdk']['path'])
            require(sdk_configuration(identity['sdk']['path']) == identity['sdk'], 'SDK configuration changed')

    def one(case):
        require(not CANCELLED.is_set(), 'Case cancelled before execution')
        directory = (work / 'cases' / case['id']).resolve()
        directory.mkdir()
        substitutions = {'{opt}': tools['opt']['argv_path'], '{llc}': tools['llc']['argv_path'], '{work}': str(directory)}
        def expand(value):
            for old, new in substitutions.items():
                value = value.replace(old, new)
            return value
        result = dict(id=case['id'], status='running', steps=[], outputs=[])
        try:
            for output in case['outputs']:
                case_output_path(expand(output['path']), directory)
            local_env = dict(env)
            for index, step in enumerate(case['steps']):
                guard()
                require(step['tool'] in tools, 'Only declared compiler tools may produce profiles')
                argv = [tools[step['tool']]['argv_path'], *[expand(value) for value in step['args']]]
                validate_argv(argv, directory, immutable)
                kind = 'train' if training else 'excluded-validation'
                local_env['LLVM_PROFILE_FILE'] = str(work / 'profiles' / (kind + '-' + case['id'] + '-' + str(index) + '-%p-%m.profraw'))
                result['steps'].append(run(argv, directory / (str(index) + '.log'), local_env,
                                           active, case['id'] + ':' + str(index), cwd=directory))
                validate_argv(argv, directory, immutable)
                guard()
            declared = []
            for index, output in enumerate(case['outputs']):
                actual = Path(expand(output['path']))
                actual = (actual if actual.is_absolute() else directory / actual).resolve()
                require(actual.is_relative_to(directory), 'Declared output escapes case work')
                require(actual not in declared, 'Duplicate declared output')
                declared.append(actual)
                reference = artifact_identity(output['reference'])
                proof = dict(path=str(actual), sha256=sha(actual), reference=reference,
                             comparison=output['comparison'], byte_identical=sha(actual) == reference['sha256'])
                if output['comparison'] == 'bytes':
                    proof['passed'] = proof['byte_identical']
                else:
                    scratch = Path(tempfile.mkdtemp(prefix='.canonical-', dir=directory))
                    values, operations = [], []
                    for label, path in [('reference', reference['path']), ('candidate', actual)]:
                        guard()
                        value, operation = canonical_ir(path, scratch / (label + '.ll'), auxiliary['llvm_dis'], env,
                                                        active, case['id'] + ':decode:' + str(index) + ':' + label, directory)
                        values.append(value); operations.append(operation)
                    proof.update(canonical_IR_sha256=values[1], passed=values[0] == values[1],
                                 decoder_operations=operations, normalization='Only the llvm-dis ModuleID comment is excluded')
                checked_file(actual, proof['sha256'])
                result['outputs'].append(proof)
                require(proof['passed'], 'Output differs; retained evidence: ' + str(directory))
            guard()
            for output in result['outputs']:
                checked_file(output['path'], output['sha256'])
            result['status'] = 'passed'
            dump(directory / 'result.json', result)
            return result
        except BaseException as error:
            result.update(status='failed', error=repr(error))
            dump(directory / 'result.json', result)
            raise

    pool = None
    try:
        guard(full=True)
        pool = concurrent.futures.ThreadPoolExecutor(max_workers=min(args.jobs, len(cases)))
        pending = {pool.submit(one, case): case['id'] for case in cases}
        completed = {}
        for future in concurrent.futures.as_completed(pending):
            value = future.result()
            completed[value['id']] = value
            record['results'] = [completed[key] for key in identifiers if key in completed]
            dump(work / 'result.json', record)
        pool.shutdown(wait=True); pool = None
        guard(full=True)
        record['status'] = 'outputs_verified_profile_merge_pending' if training else 'passed'
        dump(work / 'result.json', record)
        if training:
            profiles = sorted((work / 'profiles').glob('train-*.profraw'))
            require(profiles, 'No profiles were produced')
            merged = work / 'compiler.profdata'
            guard()
            merge = run([auxiliary['profdata']['argv_path'], 'merge', '-o', str(merged), *map(str, profiles)],
                        work / 'merge.log', env, active, 'profile-merge', cwd=work)
            require('warning:' not in (work / 'merge.log').read_text().lower(), 'Profile merge warning retained; refusing profile')
            guard(full=True)
            record.update(status='passed', merged=checked_file(merged), merge=merge,
                          raw_profiles=[checked_file(path) for path in profiles],
                          profile_producer=auxiliary['profdata'], prior_profile_reused=False,
                          held_out_policy='Only cases present in this supplied training manifest were merged')
        for result in record['results']:
            for output in result['outputs']:
                checked_file(output['path'], output['sha256'])
        dump(work / 'result.json', record)
        if training:
            # This is the final publication, after successful merge/guards and
            # complete result metadata. Failed stages never publish profile.json.
            dump(work / 'profile.json', record)
    except BaseException as error:
        CANCELLED.set()
        if pool is not None:
            pool.shutdown(wait=True, cancel_futures=True)
        record.update(status='failed', error=repr(error))
        dump(work / 'result.json', record)
        raise
    print(json.dumps(dict(status=record['status'], training=training, case_ids=identifiers, work=str(work)), indent=2))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    source = sub.add_parser('record-source', help='Bind an already pinned/verified private checkout and patch provenance')
    source.add_argument('--source', type=Path, required=True)
    source.add_argument('--revision', required=True)
    source.add_argument('--provenance', type=Path, required=True)
    source.add_argument('--compatibility-manifest', type=Path, required=True)
    source.add_argument('--sdk', type=Path, required=True)
    source.add_argument('--output', type=Path, required=True)
    construction = sub.add_parser('build')
    construction.add_argument('--source', type=Path, required=True)
    construction.add_argument('--source-record', type=Path, required=True)
    construction.add_argument('--compatibility-manifest', type=Path, required=True)
    construction.add_argument('--work', type=Path, required=True)
    construction.add_argument('--sdk', type=Path, required=True)
    construction.add_argument('--cc', type=Path, required=True)
    construction.add_argument('--cxx', type=Path, required=True)
    construction.add_argument('--linker', type=Path, required=True)
    construction.add_argument('--cmake', type=Path, required=True)
    construction.add_argument('--ninja', type=Path, required=True)
    construction.add_argument('--mode', choices=('release', 'instrument', 'pgo'), required=True)
    construction.add_argument('--lto', choices=('none', 'thin'), default='none')
    construction.add_argument('--lto-jobs', type=int, help='fixed ThinLTO backend count; bind this across instrument/PGO when build jobs change')
    construction.add_argument('--profile-kind', choices=('frontend', 'ir'), default='frontend')
    construction.add_argument('--profile-update', choices=('atomic', 'single'), default='atomic')
    construction.add_argument('--profile-list', type=Path, help='immutable compiler instrumentation filter, bound in generation/use identity')
    construction.add_argument('--unprofiled-warning', choices=('warning', 'error'), default='warning',
                              help='PGO consumer-only missing-file severity; warnings stay visible, all mismatches remain fatal')
    construction.add_argument('--profile-record', type=Path)
    construction.add_argument('--compiler-flags', default='')
    construction.add_argument('--resume', action='store_true')
    construction.add_argument('--link-jobs', type=int, default=1)
    for action in ('train', 'qualify'):
        command = sub.add_parser(action)
        command.add_argument('--manifest', type=Path, required=True)
        command.add_argument('--work', type=Path, required=True)
        command.add_argument('--opt', type=Path, required=True)
        command.add_argument('--llc', type=Path, required=True)
        command.add_argument('--llvm-dis', type=Path, required=True)
        if action == 'train':
            command.add_argument('--profdata', type=Path, required=True)
    default_jobs = len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity') else os.cpu_count() or 1
    for command in (construction, sub.choices['train'], sub.choices['qualify']):
        command.add_argument('--jobs', type=int, default=default_jobs)
    args = parser.parse_args()
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, lambda signum, frame: CANCELLED.set())
    if hasattr(args, 'jobs'):
        require(args.jobs > 0, 'Jobs must be positive')
    if args.action == 'record-source':
        record_source(args)
    elif args.action == 'build':
        require(args.link_jobs > 0, 'Link jobs must be positive')
        require(args.lto_jobs is None or args.lto_jobs > 0, 'ThinLTO jobs must be positive')
        build(args)
    else:
        execute_cases(args, args.action == 'train')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print('Full LLVM construction refused: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
