"""Compile predictor port wiring through CIRCT and run the original drivers.

This is an MLIR/SDK fixture; it elaborates no Chipyard hardware configuration
and builds no firmware. Each compiler/execution stage has its own deadline.
"""
from pathlib import Path
import argparse
import copy
import hashlib
import json
import os
import resource
import shlex
import signal
import subprocess
import sys
import time


def check(sdk, work, cache, cxx):
    sim = Path(__file__).resolve().parents[1]
    for ancestor in sim.parents:
        boom = ancestor / 'generators/boom/src/main/resources'
        if (boom / 'csrc/predictor_sw.cc').is_file():
            break
    else:
        raise FileNotFoundError('cannot locate original BOOM predictor drivers')
    originals = [boom / name for name in ('csrc/predictor_sw.cc', 'csrc/btb_sw.cc',
                  'vsrc/predictor_harness.v', 'vsrc/btb_harness.v')]
    work.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p): sha(p) for p in originals}
    report = {'status': 'running', 'steps': [], 'models': [],
              'original_sources_sha256': hashes}
    output = work / 'report.json'
    compiler = shlex.split(cxx)
    tool = sdk / 'bin'
    py = sys.executable

    def save():
        output.write_text(json.dumps(report, indent=2) + '\n')

    def limits():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3, 4 * 1024**3))

    def run(name, command, success=True):
        command = list(map(str, command))
        log = work / (name + '.log')
        row = {'name': name, 'command': command, 'log': str(log)}
        report['steps'].append(row)
        save()
        start = time.monotonic()
        with log.open('w') as stream:
            process = subprocess.Popen(command, cwd=work,
                stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                start_new_session=True, preexec_fn=limits)
            report['child_pid'] = process.pid
            save()
            try:
                code = process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise RuntimeError(name + ' exceeded its actual 180-second deadline')
        report.pop('child_pid', None)
        row.update(returncode=code, build_or_test_seconds=time.monotonic() - start)
        save()
        if code != (0 if success else 1):
            raise RuntimeError(name + ': ' + log.read_text()[-5000:])
        print('PASS ' + name, flush=True)
        return log.read_text()

    try:
        run('partition', [py, sim / 'partition.py', '--input',
            sim / 'tests/native_predictors.mlir', '--output', work,
            '--top', 'PredictorModelProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        part = json.loads((work / 'partition.json').read_text())
        instances = {i['name']: i for i in part['instances']}
        assert {n: i['module'] for n, i in instances.items()} == {
            'bp': 'BranchPredictorHarness', 'btb': 'BTBHarness'}
        for instance in instances.values():
            for port in instance['ports']:
                if port['name'].endswith(('_pc', '_hist', '_target')):
                    assert port['bits'] == 64
        run('outline', [py, sim / 'compiler/partition.py', '--input',
            work / 'partitioned.mlir', '--output', work / 'outlined.mlirbc',
            '--circt-opt', tool / 'circt-opt', '--cache', cache,
            '--cxx', cxx, '--max-ops', '16', '--emit-bytecode', '--native-feedback'])
        run('canonicalize', [tool / 'circt-opt', work / 'outlined.mlirbc',
            '--symbol-dce', '--canonicalize', '-o', work / 'model.mlir'])
        run('model', [py, sim / 'compiler/model.py', '--input', work / 'model.mlir',
            '--output', work / 'model.ll', '--state-file', work / 'model.json',
            '--arcilator', tool / 'arcilator', '--circt-opt', tool / 'circt-opt',
            '--mlir-translate', tool / 'mlir-translate', '--cache', cache,
            '--cxx', cxx, '--max-ops', '16', '--partition', work / 'partition.json',
            '--', '--split-funcs-threshold=8192'])
        layout, = json.loads((work / 'model.json').read_text())
        states = {p['name']: p for p in layout['states']}
        # The driver observers compare with root inputs, so certify that the
        # generated root metadata itself preserves the original 64-bit ports.
        for prefix, names in [('bp', ['req_pc', 'req_hist', 'update_pc', 'update_hist']),
                              ('btb', ['req_pc', 'req_hist', 'req_target_pc',
                                       'update_pc', 'update_hist', 'update_target'])]:
            for name in names:
                assert states[prefix + '_' + name]['numBits'] == 64
        run('LLVM', [py, sim / 'compiler/llvm.py', work / 'model.ll',
            '--llvm-opt', tool / 'opt', '--cache', cache, '--cxx', cxx])
        (work / 'originals.f').write_text(''.join(str(p) + '\n' for p in originals))
        run('native-factory', [py, sim / 'native.py', work / 'model.json',
            work / 'partition.json', work / 'model.cpp',
            '--filelist=' + str(work / 'originals.f')])
        (work / 'native.f').write_text((work / 'originals.f').read_text() +
                                       str(work / 'model.cpp') + '\n')
        flags = ['-DARC_NATIVE_NO_MAIN', '-DARC_PREDICTOR_COMPILED_MODEL',
                 '-I' + str(sim), '-I' + str(sim / 'native/include'),
                 '-I' + str(sdk / 'include')]
        functions = ['initialize_branch_predictor', 'predict_branch', 'update_branch',
                     'initialize_btb', 'predict_target', 'update_btb']
        libraries = ['-Wl,--wrap=' + name for name in functions] + [
            '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport',
            '-lLLVMDemangle', '-lz', '-lpthread', '-ldl']
        for level in ('0', '3'):
            obj = work / ('model-O' + level + '.o')
            run('codegen-O' + level, [py, sim / 'compiler/codegen.py', '--input',
                work / 'model.ll', '--output', obj, '--llvm-opt', tool / 'opt',
                '--llc', tool / 'llc', '--cache', cache, '--cxx', cxx,
                '--opt-level=' + level, '--llc-opt-level=' + level, '--jobs=1'])
            exe = work / ('O' + level)
            if level == '0':
                text = run('native-CLI', [py, sim / 'native_build.py',
                    '--filelist=' + str(work / 'native.f'),
                    '--main=' + str(sim / 'tests/native_predictors.cpp'),
                    '--model=' + str(obj), '--output=' + str(exe),
                    '--build=' + str(work / 'objects'), '--cxx=' + cxx,
                    '--cxxflags=' + shlex.join(flags),
                    '--ldflags=' + shlex.join(libraries), '--jobs=1'])
                link = shlex.split(text.splitlines()[-1])
                objects = {}
                for line in text.splitlines():
                    words = shlex.split(line)
                    if '-c' in words and '-o' in words:
                        objects[words[words.index('-c') + 1]] = words[words.index('-o') + 1]
            else:
                command = link.copy()
                command[command.index(str(work / 'model-O0.o'))] = str(obj)
                command[command.index('-o') + 1] = str(exe)
                run('link-O' + level, command)
            text = run('execute-O' + level, [exe])
            assert text.startswith('PASS original BOOM predictors:') and 'recreations=2' in text
            report['models'].append({'optimization': level, 'object_sha256': sha(obj),
                'executable_sha256': sha(exe), 'model_recreations': 2})
        # An incorrect generated port map must fail the same driver oracle.
        # This also proves that the fixture used create_model(), not its direct
        # Runtime constructor path.
        fault = copy.deepcopy(part)
        ports = {p['name']: p for p in next(i for i in fault['instances']
                                          if i['name'] == 'btb')['ports']}
        ports['req_pc']['wire'], ports['req_hist']['wire'] = (
            ports['req_hist']['wire'], ports['req_pc']['wire'])
        (work / 'miswired.json').write_text(json.dumps(fault))
        run('miswired-factory', [py, sim / 'native.py', work / 'model.json',
            work / 'miswired.json', work / 'miswired.cpp',
            '--filelist=' + str(work / 'originals.f')])
        run('miswired-compile', compiler + ['-O3', '-std=c++17', *flags,
            '-c', work / 'miswired.cpp', '-o', work / 'miswired.o'])
        command = link.copy()
        command[command.index(objects[str(work / 'model.cpp')])] = str(work / 'miswired.o')
        command[command.index(str(work / 'model-O0.o'))] = str(work / 'model-O3.o')
        command[command.index('-o') + 1] = str(work / 'miswired')
        run('miswired-link', command)
        text = run('reject-miswired-model', [work / 'miswired'], success=False)
        assert '64-bit PC/history ABI changed' in text
        assert all(sha(Path(p)) == h for p, h in hashes.items())
        report.update(status='passed', generated_port_map_fault_detected=True,
                      generated_64_bit_interfaces_checked=True,
                      original_sources_unchanged=True, instances=part['instances'])
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
                        / 'generated-src/native-predictor-model-test')
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/compiler')
    args = parser.parse_args()
    check(args.circt_bin.resolve().parent, args.build.resolve(), args.cache.resolve(),
          os.environ.get('CXX', 'c++'))
    print('Compiled CIRCT predictor wiring passed at O0/O3 with original BOOM '
          'drivers and a rejected generated-port-map fault')
