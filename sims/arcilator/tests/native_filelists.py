#!/usr/bin/env python3
"""Check native filelist expansion, input invalidation and real driver linking."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from filelists import source_paths, input_manifests, _update
from native_build import c_preprocessor_flags


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def preprocessor_checks():
    cases = [
        (['-Ijoined path', '-DNAME=17', '-UOLD', '-std=c++17', '-O0'],
         ['-Ijoined path', '-DNAME=17', '-UOLD']),
        (['-I', 'include path', '-D', 'NAME=(17 + 0)', '-U', 'OLD'],
         ['-I', 'include path', '-D', 'NAME=(17 + 0)', '-U', 'OLD']),
        (['-std=c++17', '-DNAME=1', '-U', 'NAME', '-D', 'NAME=2',
          '-I', '-directory', '-Isecond'],
         ['-DNAME=1', '-U', 'NAME', '-D', 'NAME=2',
          '-I', '-directory', '-Isecond']),
        ([], []),
    ]
    for tokens, expected in cases:
        before = list(tokens)
        check(c_preprocessor_flags(tokens) == expected and tokens == before,
              'shared C option operands/order changed')
    for flag in ('-I', '-D', '-U'):
        try:
            c_preprocessor_flags(['-O3', flag])
        except ValueError as error:
            check(flag in str(error) and 'requires an operand' in str(error),
                  'missing shared C operand diagnostic differs')
        else:
            raise AssertionError('missing C preprocessor operand accepted')
    return ['joined_separated_C_preprocessor_operands_order_and_missing_errors']


def parse_checks(directory):
    cases = []
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'lists/sub').mkdir(parents=True, exist_ok=True)
    (directory / 'root.f').write_text('first.c\n-F lists/outer.f\nlast.h\n')
    (directory / 'lists/outer.f').write_text('local.sv\n-f cwd.f\n-F sub/inner.f\n')
    (directory / 'lists/cwd.f').write_text('cwd-driver.c\n')
    (directory / 'lists/sub/inner.f').write_text('"last driver.cpp"\n')
    expected = [directory / p for p in ('first.c', 'lists/local.sv', 'cwd-driver.c', 'lists/sub/last driver.cpp', 'last.h')]
    for p in expected:
        p.write_text('// original source\n')
    check(list(source_paths(directory / 'root.f', directory)) == expected,
          'nested -f invocation base and -F list base/order differ')
    cases.append('mixed_f_F_bases_quoted_paths_order')
    (directory / 'quoted.f').write_text(r'''# ignored /* missing.cpp
// missing.c
/* missing.cpp
 -F missing.f */
"hash#name.c" 'star/*/name.cpp' "slash//name.c" escaped\ name.c
+incdir+ignored.cpp +define+IGNORED=missing.sv
''')
    quoted = [directory / p for p in ('hash#name.c', 'star/*/name.cpp', 'slash//name.c', 'escaped name.c')]
    check(list(source_paths(directory / 'quoted.f', directory)) == quoted,
          'comments escaped or quoted source paths were misparsed')
    cases.append('hash_C_comments_escaped_and_quoted_comment_markers_options')
    (directory / 'duplicate.f').write_text('-F lists/sub/inner.f\n-F lists/sub/inner.f\n')
    check(list(source_paths(directory / 'duplicate.f', directory)) == [expected[3]] * 2,
          'nonrecursive repeated filelist was rejected or reordered')
    cases.append('repeated_nonrecursive_list')
    for name, text in [('self.f', '-F self.f\n'), ('mutual.f', '-F lists/back.f\n')]:
        (directory / name).write_text(text)
    (directory / 'lists/back.f').write_text('-F ../mutual.f\n')
    for name in ['self.f', 'mutual.f']:
        try:
            list(source_paths(directory / name, directory))
        except ValueError as error:
            check('recursive native filelist:' in str(error) and name in str(error), 'cycle diagnostic lost include chain')
        else:
            raise AssertionError('recursive list accepted: ' + name)
    cases.append('self_and_mutual_recursion_diagnostics')
    (directory / 'symlink.f').write_text('-F alias.f\n')
    (directory / 'alias.f').symlink_to(directory / 'symlink.f')
    try:
        list(source_paths(directory / 'symlink.f', directory))
    except ValueError as error:
        check('recursive native filelist:' in str(error), 'symlink cycle diagnostic differs')
    else:
        raise AssertionError('symlink recursion accepted')
    cases.append('symlink_recursion_identity')
    for name, text, phrase in [('operand.f', '-F', 'requires a filelist'), ('comment.f', '/* not closed', 'unterminated filelist comment'), ('quote.f', '"not closed', 'No closing quotation')]:
        (directory / name).write_text(text)
        try:
            list(source_paths(directory / name, directory))
        except ValueError as error:
            check(phrase in str(error) and name in str(error), 'malformed list diagnostic lost context')
        else:
            raise AssertionError('malformed list accepted: ' + name)
    cases.append('missing_operand_unterminated_comment_and_quote')
    (directory / 'missing.f').write_text('-F lists/absent.f\n')
    try:
        list(source_paths(directory / 'missing.f', directory))
    except FileNotFoundError as error:
        check('absent.f' in str(error), 'missing nested list diagnostic lost filename')
    else:
        raise AssertionError('missing nested list accepted')
    cases.append('missing_nested_list_fails')
    # Expansion is iterative; a large acyclic chain must not hit Python's
    # recursion limit, and no arbitrary nesting cap changes valid selections.
    deep = directory / 'deep'
    deep.mkdir()
    for index in range(1050):
        (deep / f'{index}.f').write_text(f'-F {index + 1}.f\n' if index < 1049 else '../first.c\n')
    (directory / 'deep-root.f').write_text('-F deep/0.f\n')
    check(list(source_paths(directory / 'deep-root.f', directory)) == [deep / '../first.c'], 'deep acyclic list expansion differs')
    cases.append('1050_acyclic_lists_without_recursive_python_calls')
    hdl, drivers = input_manifests(directory / 'root.f', directory)
    check(set(hdl) == {str(expected[1])} and set(drivers) == {str(expected[i]) for i in (0, 2, 3, 4)}, 'HDL and driver input inventories differ')
    hdl_json, driver_json = directory / 'hdl.json', directory / 'drivers.json'
    _update(hdl_json, hdl)
    _update(driver_json, drivers)
    times = (hdl_json.stat().st_mtime_ns, driver_json.stat().st_mtime_ns)
    _update(hdl_json, hdl)
    _update(driver_json, drivers)
    check(times == (hdl_json.stat().st_mtime_ns, driver_json.stat().st_mtime_ns), 'unchanged input inventory timestamp changed')
    expected[2].write_text('// changed C driver\n')
    hdl2, drivers2 = input_manifests(directory / 'root.f', directory)
    check(hdl2 == hdl and drivers2 != drivers, 'driver edit invalidates wrong inventory')
    expected[1].write_text('// changed HDL\n')
    hdl3, drivers3 = input_manifests(directory / 'root.f', directory)
    check(hdl3 != hdl2 and drivers3 == drivers2, 'HDL edit invalidates wrong inventory')
    cases.append('exact_input_fingerprints_and_unchanged_timestamp')
    return cases


def make_checks(directory):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'blackboxes.f').write_text('-F nested.f\n')
    (directory / 'nested.f').write_text('rtl.sv\ndriver.c\n')
    (directory / 'rtl.sv').write_text('// selected RTL\n')
    (directory / 'driver.c').write_text('int driver(void){return 1;}\n')
    text = (SIM / 'Makefile').read_text()
    begin = text.index('$(arc_dir)/hdl-inputs.json $(arc_dir)/driver-inputs.json &:')
    end = text.index('$(arc_dir)/rtl-libraries.json:', begin)
    inventory_rule = text[begin:end]
    fragment = ('arc_dir := ' + str(directory) + '\nsim_dir := ' + str(SIM) + '\nPYTHON := ' + sys.executable + '\n.PHONY: FORCE\nFORCE:\n' + inventory_rule +
                '\n' + str(directory / 'rtl.stamp') + ': ' + str(directory / 'hdl-inputs.json') + '\n\t@echo rtl >> ' + str(directory / 'build-order.log') + '\n\t@touch $@\n' +
                str(directory / 'model.stamp') + ': ' + str(directory / 'rtl.stamp') + '\n\t@echo model >> ' + str(directory / 'build-order.log') + '\n\t@touch $@\n' +
                str(directory / 'simulator.stamp') + ': ' + str(directory / 'model.stamp') + ' ' + str(directory / 'driver-inputs.json') + '\n\t@echo simulator >> ' + str(directory / 'build-order.log') + '\n\t@touch $@\n')
    (directory / 'Makefile').write_text(fragment)

    def build(label, success=True):
        result = subprocess.run(['make', '--no-print-directory', '-f', directory / 'Makefile', directory / 'simulator.stamp'], cwd=directory, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (directory / (label + '.log')).write_text(result.stdout)
        check((result.returncode == 0) == success, f'{label}: unexpected make status: {result.stdout}')
        return (directory / 'build-order.log').read_text().splitlines()

    first = build('initial')
    check(first == ['rtl', 'model', 'simulator'], 'initial Make dependency order differs')
    check(build('unchanged') == first, 'unchanged inputs rebuilt native stages')
    (directory / 'driver.c').write_text('int driver(void){return 2;}\n')
    after_driver = build('driver-edit')
    check(after_driver == first + ['simulator'], 'driver edit rebuilt RTL/model or failed to relink')
    (directory / 'rtl.sv').write_text('// changed selected RTL\n')
    after_hdl = build('hdl-edit')
    check(after_hdl == after_driver + ['rtl', 'model', 'simulator'], 'HDL edit did not invalidate model and link')
    (directory / 'nested.f').write_text('// comment only\nrtl.sv\ndriver.c\n')
    check(build('comment-edit') == after_hdl, 'nested comment-only edit rebuilt native stages')
    (directory / 'extra.cpp').write_text('int extra(){return 3;}\n')
    (directory / 'nested.f').write_text('rtl.sv\ndriver.c\nextra.cpp\n')
    after_add = build('nested-driver-addition')
    check(after_add == after_hdl + ['simulator'], 'nested C++ selection change failed to relink')
    (directory / 'nested.f').unlink()
    check(build('missing-nested', success=False) == after_add, 'missing nested list reused or rebuilt stale target')
    # The exercised producer must also be connected to the actual consumers.
    check('$(arc_dir)/hdl-inputs.json' in text[text.index('$(arc_input) $(arc_dir)/rtl.json:'):].splitlines()[0], 'native RTL does not depend on selected HDL inventory')
    native_target = next(line for line in text.splitlines() if line.startswith('$(2): $(1)/native-main.cpp'))
    check('$(arc_dir)/driver-inputs.json' in native_target and '$(sim_dir)/filelists.py' in native_target, 'native executable does not depend on driver inventory/parser')
    return ['actual_Make_inventory_rule_initial_unchanged_driver_HDL_comment_membership_and_missing_list']


def native_link(directory, circt, cxx, cc):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'lists/nested').mkdir(parents=True, exist_ok=True)
    (directory / 'drivers.f').write_text('-F lists/outer.f\n')
    (directory / 'lists/outer.f').write_text('-f cwd.f\n-F nested/inner.f\n')
    (directory / 'lists/cwd.f').write_text('driver.c\n')
    (directory / 'lists/nested/inner.f').write_text('"driver cpp.cpp"\n"../nested/driver cpp.cpp"\n')
    header = directory / 'include files'
    header.mkdir(exist_ok=True)
    (header / 'fixture_value.h').write_text('#define ARC_TEST_HEADER_VALUE 17\n')
    (directory / 'driver.c').write_text('#ifdef __cplusplus\n#error requires_C_compiler\n#endif\n#include "fixture_value.h"\n#if ARC_TEST_CPP_VALUE != ARC_TEST_HEADER_VALUE\n#error shared_define_lost\n#endif\n#ifdef ARC_TEST_REMOVE\n#error shared_undef_lost\n#endif\nint nested_c_driver(void){return ARC_TEST_HEADER_VALUE;}\n')
    (directory / 'lists/nested/driver cpp.cpp').write_text('#ifndef __cplusplus\n#error requires_CXX_compiler\n#endif\nextern "C" int nested_cxx_driver(){return 29;}\n')
    (directory / 'main.cpp').write_text('#include <cstdio>\nextern "C" int nested_c_driver(void);\nextern "C" int nested_cxx_driver();\nint main(){if(nested_c_driver()!=17||nested_cxx_driver()!=29)return 1;std::puts("PASS nested C/CXX driver fixtures linked without Verilator");}\n')
    (directory / 'marker.c').write_text('int native_fixture_model_marker = 0;\n')
    trap = directory / 'trap'
    trap.mkdir(exist_ok=True)
    (trap / 'verilator').write_text('#!/bin/sh\necho invoked > ' + shlex.quote(str(directory / 'verilator-invoked')) + '\nexit 99\n')
    (trap / 'verilator').chmod(0o755)
    env = dict(os.environ, VERILATOR='/not-installed/verilator')
    env['PATH'] = str(trap) + os.pathsep + env.get('PATH', '')
    sdk = circt.resolve().parent
    shared_flags = shlex.join(['-I' + str(sdk / 'include'), '-I', str(header),
                               '-D', 'ARC_TEST_CPP_VALUE=(17 + 0)',
                               '-U', 'ARC_TEST_REMOVE', '-std=c++17'])
    commands = [shlex.split(cc) + ['-c', str(directory / 'marker.c'), '-o', str(directory / 'marker.o')],
                [sys.executable, str(SIM / 'native_build.py'), '--filelist=' + str(directory / 'drivers.f'), '--main=' + str(directory / 'main.cpp'), '--model=' + str(directory / 'marker.o'), '--output=' + str(directory / 'simulator'), '--build=' + str(directory / 'objects'), '--cxx=' + cxx, '--cc=' + cc, '--cxxflags=' + shared_flags, '--cflags=-DARC_TEST_REMOVE=1', '--ldflags=-L' + str(sdk / 'lib') + ' -lCIRCTArcRuntime -lLLVMSupport -lLLVMDemangle -lz -lpthread -ldl', '--jobs=1'],
                [str(directory / 'simulator')]]
    for index, command in enumerate(commands):
        with (directory / f'step-{index}.log').open('w') as stream:
            subprocess.run(command, cwd=directory, env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    check('PASS nested C/CXX driver fixtures linked without Verilator' in (directory / 'step-2.log').read_text(), 'nested drivers did not execute')
    check(not (directory / 'verilator-invoked').exists(), 'native build invoked Verilator')
    build = (directory / 'step-1.log').read_text()
    check(build.count(' -c ' + shlex.quote(str(directory / 'lists/nested/driver cpp.cpp')) + ' ') == 1, 'repeated nested C++ driver was compiled more than once')
    c_command = next(shlex.split(line) for line in build.splitlines()
                     if '-std=gnu11' in line and
                     str(directory / 'driver.c') in line)
    check('-std=c++17' not in c_command and str(header) in c_command and
          'ARC_TEST_CPP_VALUE=(17 + 0)' in c_command and
          'ARC_TEST_REMOVE' in c_command,
          'C preprocessor operands or C-only standard changed')
    # Invalid nested selections must fail before any compiler is invoked or
    # an existing output can be replaced.
    for name, listing, phrase in [('cycle', '-F cycle.f\n', 'recursive native filelist'),
                                  ('missing', '-F absent.f\n', 'No such file')]:
        bad = directory / (name + '.f')
        bad.write_text(listing)
        output = directory / (name + '-output')
        output.write_text('existing output')
        invalid = [sys.executable, str(SIM / 'native_build.py'),
                   '--filelist=' + str(bad), '--main=unused.cpp',
                   '--model=unused.o', '--output=' + str(output),
                   '--build=' + str(directory / (name + '-objects')),
                   '--cxx=/not-installed/compiler', '--cc=/not-installed/compiler']
        result = subprocess.run(invalid, cwd=directory, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        (directory / (name + '-error.log')).write_text(result.stdout)
        check(result.returncode == 2 and phrase in result.stdout and
              output.read_text() == 'existing output' and
              not (directory / (name + '-objects')).exists(),
              'invalid nested list reached compilation or replaced output')
    for flag in ('-I', '-D', '-U'):
        output = directory / ('missing-operand-' + flag[1:])
        output.write_text('existing output')
        objects = directory / ('missing-operand-objects-' + flag[1:])
        command = [sys.executable, str(SIM / 'native_build.py'),
                   '--filelist=' + str(directory / 'drivers.f'),
                   '--main=unused.cpp', '--model=unused.o',
                   '--output=' + str(output), '--build=' + str(objects),
                   '--cxx=/not-installed/compiler', '--cc=/not-installed/compiler',
                   '--cxxflags=' + flag]
        result = subprocess.run(command, cwd=directory, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        check(result.returncode == 2 and
              'requires an operand' in result.stdout and
              output.read_text() == 'existing output' and not objects.exists(),
              'missing preprocessing operand reached compilation/output')
    return dict(status='passed',commands=commands,log=str(directory / 'step-2.log'),verilator_not_invoked=True,
                separated_C_include_define_undef_operands_preserved=True,
                CXX_only_standard_not_forwarded_to_C=True,
                missing_C_preprocessor_operands_rejected_before_compilation=True,
                repeated_driver_alias_compiled_once=True,
                invalid_nested_lists_rejected_before_compilation=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--work', type=Path, default=SIM / 'generated-src/filelist-test')
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    report = dict(status='running', cases=[], native_link={'status':'not_requested_requires_CIRCT_BIN'})
    try:
        report['cases'] += preprocessor_checks()
        with tempfile.TemporaryDirectory(prefix='parsing-', dir=work) as temp:
            report['cases'] += parse_checks(Path(temp))
        with tempfile.TemporaryDirectory(prefix='make-', dir=work) as temp:
            report['cases'] += make_checks(Path(temp))
        if args.circt_bin:
            report['native_link'] = native_link(work / 'native', args.circt_bin, os.environ.get('CXX', 'c++'), os.environ.get('CC', 'cc'))
        report['status'] = 'passed'
        print('PASS: native filelist parsing, input invalidation and ' + report['native_link']['status'])
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        raise
    finally:
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
