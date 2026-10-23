"""Check native predictor bindings against the unchanged BOOM C++ drivers.

Uses the SDK Runtime directly; no hardware configuration or firmware build.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import subprocess
import sys


def check(sdk, work, cxx):
    sim = Path(__file__).resolve().parents[1]
    for ancestor in sim.parents:
        boom = ancestor / 'generators/boom/src/main/resources'
        if (boom / 'csrc/predictor_sw.cc').is_file():
            break
    else:
        raise FileNotFoundError('cannot locate original BOOM predictor drivers')
    work.mkdir(parents=True, exist_ok=True)
    drivers = [boom / 'csrc/predictor_sw.cc', boom / 'csrc/btb_sw.cc']
    originals = drivers + [boom / 'vsrc/predictor_harness.v', boom / 'vsrc/btb_harness.v']
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p): sha(p) for p in originals}
    report = {'status': 'running', 'steps': [], 'variants': [], 'faults': [],
              'original_sources_sha256': hashes}
    output = work / 'report.json'
    compiler = shlex.split(cxx)
    includes = ['-I' + str(sim), '-I' + str(sim / 'native'),
                '-I' + str(sim / 'native/include'), '-I' + str(sdk / 'include')]
    functions = ['initialize_branch_predictor', 'predict_branch', 'update_branch',
                 'initialize_btb', 'predict_target', 'update_btb']
    libraries = ['-Wl,--wrap=' + name for name in functions] + [
        '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport',
        '-lLLVMDemangle', '-lz', '-lpthread', '-ldl']
    fixture = sim / 'tests/native_predictors.cpp'
    peripheral = sim / 'native/peripherals.cpp'

    def save():
        output.write_text(json.dumps(report, indent=2) + '\n')

    def run(name, command, success=True):
        command = list(map(str, command))
        log = work / (name + '.log')
        row = {'name': name, 'command': command, 'log': str(log)}
        report['steps'].append(row)
        save()
        with log.open('w') as stream:
            process = subprocess.run(command, cwd=work, stdin=subprocess.DEVNULL,
                                     stdout=stream, stderr=subprocess.STDOUT)
        row['returncode'] = process.returncode
        save()
        if (process.returncode == 0) != success:
            raise RuntimeError(name + ': ' + log.read_text()[-3000:])
        return log.read_text()

    try:
        (work / 'drivers.f').write_text(''.join(str(p) + '\n' for p in drivers))
        (work / 'marker.cpp').write_text('int native_predictor_model_marker;\n')
        run('marker', compiler + ['-c', work / 'marker.cpp', '-o', work / 'marker.o'])
        exe = work / 'O3'
        text = run('native-CLI', [sys.executable, sim / 'native_build.py',
            '--filelist=' + str(work / 'drivers.f'), '--main=' + str(fixture),
            '--model=' + str(work / 'marker.o'), '--output=' + str(exe),
            '--build=' + str(work / 'objects'), '--cxx=' + cxx,
            '--cxxflags=' + shlex.join(includes),
            '--ldflags=' + shlex.join(libraries), '--jobs=1'])
        objects = {}
        for line in text.splitlines():
            words = shlex.split(line)
            if '-c' in words and '-o' in words:
                objects[Path(words[words.index('-c') + 1])] = words[words.index('-o') + 1]
        link = shlex.split(text.splitlines()[-1])
        for label, flags in [('O3', []), ('O0', ['-O0']),
                             ('ubsan', ['-O2', '-fsanitize=undefined',
                                        '-fno-sanitize-recover=all'])]:
            if label != 'O3':
                command = link.copy()
                for source in [fixture, peripheral, *drivers]:
                    obj = work / (label + '-' + source.stem + '.o')
                    run('compile-' + label + '-' + source.stem, compiler + [
                        *flags, '-std=c++17', *includes, '-c', source, '-o', obj])
                    command[command.index(objects[source])] = str(obj)
                exe = work / label
                command[1:1] = flags
                command[command.index('-o') + 1] = str(exe)
                run('link-' + label, command)
            text = run('execute-' + label, [exe])
            assert text.startswith('PASS original BOOM predictors:') and 'recreations=2' in text
            report['variants'].append({'name': label, 'model_recreations': 2,
                                       'predictor_instances_per_model': 2,
                                       'rising_edges_per_model': 20})
        original = peripheral.read_text()
        predict = ('      if (get("req_valid"))\n'
                   '        predict_branch(get("req_pc"), get("req_hist"), &taken);\n')
        update = ('      if (get("update_valid"))\n'
                  '        update_branch(get("update_pc"), get("update_hist"),\n'
                  '                      get("update_taken"));\n')
        faults = [
            ('update-first', predict + update, update + predict,
             'predict/update callback order'),
            ('truncate-PC', 'predict_target(get("req_pc"), get("req_hist"), &valid,',
             'predict_target(uint32_t(get("req_pc")), get("req_hist"), &valid,',
             '64-bit PC/history ABI changed'),
            ('early-commit', 'stage("req_taken", taken);',
             'port("req_taken").set(taken);', 'outputs committed before sampling ended')]
        for name, old, new, diagnostic in faults:
            assert original.count(old) == 1, name
            source = work / (name + '.cpp')
            source.write_text(original.replace(old, new))
            obj = work / (name + '.o')
            run('compile-fault-' + name, compiler + ['-O3', '-std=c++17',
                *includes, '-c', source, '-o', obj])
            command = link.copy()
            command[command.index(objects[peripheral])] = str(obj)
            command[command.index('-o') + 1] = str(work / name)
            run('link-fault-' + name, command)
            text = run('reject-' + name, [work / name], success=False)
            assert diagnostic in text, text
            report['faults'].append({'name': name, 'detected': True})
        assert all(sha(Path(p)) == h for p, h in hashes.items())
        report.update(status='passed', original_sources_unchanged=True)
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
                        / 'generated-src/native-predictor-test')
    args = parser.parse_args()
    check(args.circt_bin.resolve().parent, args.build.resolve(),
          os.environ.get('CXX', 'c++'))
    print('Original BOOM native predictor bindings passed at O0/O3/UBSAN '
          'with callback-order, 64-bit ABI and early-commit fault controls')
