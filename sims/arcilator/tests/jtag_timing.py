"""Check native SimJTAG timing against the original wrapper's assignments.

Uses the real SDK/runtime and jtag_tick ABI, with scripted driver pin values.
No RTL simulator, hardware configuration, socket server or firmware is needed.
"""
from pathlib import Path
import argparse
import json
import os
import shlex
import subprocess
import sys


def check(sdk, work, cxx):
    sim = Path(__file__).resolve().parents[1]
    work.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'steps': [], 'variants': [], 'faults': []}
    output = work / 'report.json'
    compiler = shlex.split(cxx)
    includes = ['-I' + str(sim), '-I' + str(sim / 'native/include'),
                '-I' + str(sdk / 'include')]
    libraries = ['-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime',
                 '-lLLVMSupport', '-lLLVMDemangle', '-lz', '-lpthread', '-ldl']
    fixture = sim / 'tests/jtag_timing.cpp'
    cases = ['repeat', 'independent', 'fractional-output', 'fractional-tdo',
             'tdo-pulse', 'reset-divider']

    def save():
        output.write_text(json.dumps(report, indent=2) + '\n')

    def run(name, command, success=True):
        command = list(map(str, command))
        log = work / (name + '.log')
        row = {'name': name, 'command': command, 'log': str(log)}
        report['steps'].append(row)
        save()
        with log.open('w') as stream:
            process = subprocess.run(command, cwd=work, stdout=stream,
                                     stderr=subprocess.STDOUT)
        row['returncode'] = process.returncode
        save()
        if (process.returncode == 0) != success:
            raise RuntimeError(name + ': ' + log.read_text()[-3000:])
        return log.read_text()

    try:
        # Build through the maintained native driver; reuse its unchanged
        # runtime objects when checking optimization levels and fault controls.
        (work / 'empty.f').write_text('')
        (work / 'marker.cpp').write_text('int jtag_timing_model_marker;\n')
        run('marker', compiler + ['-c', work / 'marker.cpp', '-o', work / 'marker.o'])
        executable = work / 'O3'
        build_log = run('native-build', [sys.executable, sim / 'native_build.py',
            '--filelist=' + str(work / 'empty.f'), '--main=' + str(fixture),
            '--model=' + str(work / 'marker.o'), '--output=' + str(executable),
            '--build=' + str(work / 'objects'), '--cxx=' + cxx,
            '--cxxflags=' + shlex.join(includes),
            '--ldflags=' + shlex.join(libraries), '--jobs=1'])
        objects = {}
        for line in build_log.splitlines():
            words = shlex.split(line)
            if '-c' in words and '-o' in words:
                objects[Path(words[words.index('-c') + 1])] = words[words.index('-o') + 1]
        assert fixture in objects and sim / 'native/bindings.cpp' in objects
        binding_object = objects[sim / 'native/bindings.cpp']
        fixture_object = objects[fixture]
        rest = [v for v in objects.values() if v not in (binding_object, fixture_object)]

        def link(name, binding, test, flags=()):
            exe = work / name
            run('link-' + name, compiler + [*flags, test, binding, *rest,
                                           *libraries, '-o', exe])
            return exe

        for name, flags in [('O3', []), ('O0', ['-O0']),
                             ('ubsan', ['-O2', '-fsanitize=undefined',
                                        '-fno-sanitize-recover=all'])]:
            if name != 'O3':
                binding = work / (name + '-bindings.o')
                test = work / (name + '-fixture.o')
                for label, source, obj in [('bindings', sim / 'native/bindings.cpp', binding),
                                           ('fixture', fixture, test)]:
                    run('compile-' + name + '-' + label, compiler + [*flags,
                        '-std=c++17', *includes, '-c', source, '-o', obj])
                executable = link(name, binding, test, flags)
            for case in cases:
                text = run(name + '-' + case, [executable, case])
                assert text == 'PASS ' + case + ' recreations=2\n', text
            report['variants'].append({'name': name, 'cases': cases,
                                       'model_recreations_per_case': 2})
            save()

        original = (sim / 'native/bindings.cpp').read_text()
        for name, old, new, case, diagnostic in [
            ('shared-deadline', '!outputs_observed || values[i] != output.source',
             'true', 'repeat', 'unchanged pins keep their original deadline'),
            ('rounded-origin', 'return runtime.time_fs() + delay_fs;',
             'return runtime.time() * 1000 + delay_fs;', 'fractional-output',
             'fractional output must not arrive early')
        ]:
            assert original.count(old) == 1
            source = work / (name + '.cpp')
            source.write_text(original.replace(old, new))
            obj = work / (name + '.o')
            run('compile-' + name, compiler + ['-O3', '-std=c++17', *includes,
                '-I' + str(sim / 'native'), '-c', source, '-o', obj])
            executable = link(name, obj, fixture_object)
            text = run('reject-' + name, [executable, case], success=False)
            assert diagnostic in text, text
            report['faults'].append({'name': name, 'detected': True})
        report['status'] = 'passed'
        save()
        return report
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        save()
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/jtag-timing-test')
    args = parser.parse_args()
    check(args.circt_bin.resolve().parent, args.build.resolve(),
          os.environ.get('CXX', 'c++'))
    print('Native JTAG timing passed: independent inertial outputs, fractional '
          'delays, reset/divider/sticky enable, DPI scopes and model recreation')
