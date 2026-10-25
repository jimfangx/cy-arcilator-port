#!/usr/bin/env python3
"""Exercise native object Make rules, real headers and atomic link behavior."""
import os
from pathlib import Path
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]


def check(circt, cxx, cc, directory):
    directory.mkdir(parents=True, exist_ok=True)
    header_dir = directory / 'include files'
    header_dir.mkdir(exist_ok=True)
    header = header_dir / "value #$'header.h"
    header.write_text('#define FIXTURE_VALUE 17\n')
    (directory / 'driver.c').write_text("""#ifdef __cplusplus
#error requires_C_compiler
#endif
#include "value #$'header.h"
#if SHARED_VALUE != 23
#error shared_define_lost
#endif
#ifdef REMOVE_ME
#error shared_undef_lost
#endif
int c_value(void){return FIXTURE_VALUE;}
""")
    (directory / 'driver cpp.cpp').write_text(
        'extern "C" int cpp_value(){return 29;}\n')
    (directory / 'main.cpp').write_text(
        '#include <cstdlib>\nextern "C" int c_value(void);'
        'extern "C" int cpp_value();'
        'int main(int argc,char**argv){return argc==2 && '
        'c_value()==std::atoi(argv[1]) && cpp_value()==29 ? 0:1;}\n')
    (directory / 'marker.c').write_text('int fixture_model_marker;\n')
    (directory / 'drivers.f').write_text(
        'driver.c\n"driver cpp.cpp"\n"driver cpp.cpp"\n')
    sdk = circt.resolve().parent
    jobs = (len(os.sched_getaffinity(0)) if hasattr(os, 'sched_getaffinity')
            else os.cpu_count() or 1)
    objects = directory / 'objects'
    rules = directory / 'native.mk'
    executable = directory / 'simulator'
    stamp = objects / 'objects.stamp'
    flags = shlex.join(['-I' + str(sdk / 'include'), '-I', str(header_dir),
                        '-D', 'SHARED_VALUE=23', '-U', 'REMOVE_ME'])
    common = ['--filelist=' + str(directory / 'drivers.f'),
              '--main=' + str(directory / 'main.cpp'),
              '--model=' + str(directory / 'marker.o'),
              '--output=' + str(executable), '--build=' + str(objects),
              '--cxx=' + cxx, '--cc=' + cc, '--cxxflags=' + flags,
              '--cflags=-DREMOVE_ME=1',
              '--ldflags=-L' + str(sdk / 'lib') +
              ' -lCIRCTArcRuntime -lLLVMSupport -lLLVMDemangle -lz -lpthread -ldl',
              '--jobs=' + str(jobs)]
    command = [sys.executable, str(SIM / 'native_build.py')]
    checks = []

    def run(label, argv, success=True):
        result = subprocess.run(list(map(str, argv)), cwd=directory,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=120)
        (directory / (label + '.log')).write_text(result.stdout)
        assert (result.returncode == 0) == success, result.stdout
        return result.stdout

    def timestamps():
        return {p.name: p.stat().st_mtime_ns for p in objects.glob('*.o')}

    run('emit', command + common + ['--makefile=' + str(rules)])
    assert not executable.exists() and not (directory / 'marker.o').exists()
    run('objects', ['make', '-j' + str(jobs), '-f', rules, stamp])
    assert not (directory / 'marker.o').exists()
    assert len(list(objects.glob('*driver cpp.o'))) == 1
    driver_object = next(objects.glob('*-driver.o')).name
    checks.append('compile_before_model_with_original_C_CXX_flags_and_deduplication')
    before = timestamps()
    run('model', shlex.split(cc) + ['-c', directory / 'marker.c',
                                  '-o', directory / 'marker.o'])
    run('link', command + common + ['--link-only'])
    assert before == timestamps()
    run('execute', [executable, '17'])
    reference = directory / 'one-shot-reference'
    original = [('--output=' + str(reference) if arg.startswith('--output=')
                 else '--build=' + str(directory / 'reference-objects')
                 if arg.startswith('--build=') else arg) for arg in common]
    run('original', command + original)
    assert reference.read_bytes() == executable.read_bytes()
    checks.append('whole_executable_identical_to_original_one_shot')

    sys.path.insert(0, str(SIM))
    from native_build import make_path
    recipe = lambda argv: '\t' + shlex.join(list(map(str, argv))).replace('$', '$$') + '\n'
    parent = ('.PHONY: FORCE\nFORCE:\n' + make_path(stamp) + ': ' +
              make_path(rules) + ' FORCE\n' +
              '\t+$(MAKE) -f ' + shlex.quote(str(rules)).replace('$', '$$') +
              ' ' + shlex.quote(str(stamp)).replace('$', '$$') + '\n' +
              make_path(executable) + ': ' + make_path(stamp) + ' ' +
              make_path(directory / 'marker.o') + '\n' +
              recipe(command + common + ['--link-only']))
    parent_file = directory / 'parent.mk'
    parent_file.write_text(parent)
    build = ['make', '-j' + str(jobs), '-f', parent_file, executable]
    before = timestamps()
    old_times = (stamp.stat().st_mtime_ns, executable.stat().st_mtime_ns)
    run('noop', build)
    assert before == timestamps()
    assert old_times == (stamp.stat().st_mtime_ns, executable.stat().st_mtime_ns)
    checks.append('recursive_jobserver_and_FORCE_preserve_cached_objects_and_executable')
    header.write_text('#define FIXTURE_VALUE 19\n')
    run('header', build)
    changed = [name for name, time in timestamps().items() if time != before[name]]
    assert changed == [driver_object], changed
    run('execute-header', [executable, '19'])
    checks.append('quoted_space_hash_dollar_header_dependency_rebuilds_only_affected_source')
    before = timestamps()
    (directory / 'driver.c').write_text((directory / 'driver.c').read_text() + '// source edit\n')
    run('source', build)
    assert [n for n,t in timestamps().items() if t != before[n]] == [driver_object]
    before = timestamps()
    run('new-model', shlex.split(cc) + ['-c', directory / 'marker.c',
                                      '-o', directory / 'marker.o'])
    assert '--link-only' in run('model-only', build) and before == timestamps()
    checks.append('source_change_rebuilds_one_object_and_model_change_only_relinks')
    old_time = rules.stat().st_mtime_ns
    run('same-plan', command + common + ['--makefile=' + str(rules)])
    assert old_time == rules.stat().st_mtime_ns
    alternate = [arg + ' -DUNUSED_NEW_OPTION=1' if arg.startswith('--cxxflags=')
                 else arg for arg in common]
    run('new-flags', command + alternate + ['--makefile=' + str(rules)])
    before = timestamps()
    run('flags-rebuild', build)
    assert all(t != before[n] for n,t in timestamps().items())
    run('execute-flags', [executable, '19'])
    checks.append('compiler_flag_changes_rebuild_all_objects')
    saved = executable.read_bytes()
    header.write_text('#error expected_dependency_failure\n')
    assert 'expected_dependency_failure' in run('failure', build, success=False)
    assert saved == executable.read_bytes()
    checks.append('failed_compilation_preserves_original_diagnostic_and_executable')
    return checks
