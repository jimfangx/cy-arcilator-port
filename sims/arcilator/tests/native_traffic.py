"""Compile native TrafficEval with original C++/C drivers and packet oracles."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import resource
import shlex
import signal
import struct
import subprocess
import sys
import time


def check(sdk, work, cache, cxx, cc):
    sim = Path(__file__).resolve().parents[1]
    for ancestor in sim.parents:
        root = ancestor / 'generators/constellation/src/main/resources'
        if (root / 'csrc/TrafficEval.cpp').is_file():
            break
    else:
        raise FileNotFoundError('cannot locate original Constellation TrafficEval drivers')
    originals = [root / p for p in ('csrc/TrafficEval.cpp', 'csrc/netrace/netrace.c',
        'csrc/netrace/queue.c', 'csrc/TrafficEval.h', 'csrc/netrace/netrace.h',
        'csrc/netrace/queue.h', 'vsrc/TrafficEval.v')]
    work.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p): sha(p) for p in originals}
    report = {'status': 'running', 'steps': [], 'models': [], 'faults': [],
              'original_sources_sha256': hashes}
    output = work / 'report.json'
    compiler, py, tool = shlex.split(cxx), sys.executable, sdk / 'bin'

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
            process = subprocess.Popen(command, cwd=work, stdin=subprocess.DEVNULL,
                stdout=stream, stderr=subprocess.STDOUT, start_new_session=True,
                preexec_fn=limits)
            report['child_pid'] = process.pid
            save()
            try:
                code = process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise RuntimeError(name + ' exceeded its actual180s deadline')
        report.pop('child_pid', None)
        row.update(returncode=code, build_or_test_seconds=time.monotonic() - start)
        save()
        if code != (0 if success else 1):
            raise RuntimeError(name + ': ' + log.read_text()[-5000:])
        print('PASS ' + name, flush=True)
        return log.read_text()

    def trace_bytes(mode):
        if mode == 'timeout':
            return b''
        flits = 2 if mode == 'default' else 3
        return b''.join(struct.pack('<QQQBB', cycle, ident, 0,
            ident % flits == 0, ident % flits == flits - 1)
            for ident, cycle in enumerate([0, 2, *range(4, 14)]))

    try:
        run('partition', [py, sim / 'partition.py', '--input', sim / 'tests/native_traffic.mlir',
            '--output', work, '--top', 'TrafficModelProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        part = json.loads((work / 'partition.json').read_text())
        instances = {p['name']: p for p in part['instances']}
        assert len(instances) == 2
        common = {'clock': 1, 'reset': 1, 'current_cycle': 64}
        contracts = {
            'ingress': ('TrafficEvalIngress', {**common, 'flit_out_ready': 1,
                'flit_out_valid': 1, 'flit_out_head': 1, 'flit_out_tail': 1,
                'flit_out_egress_id': 64, 'flit_out_unique_id': 64}),
            'egress': ('TrafficEvalEgress', {**common, 'flit_in_ready': 1,
                'flit_in_valid': 1, 'flit_in_head': 1, 'flit_in_tail': 1,
                'flit_in_ingress_id': 64, 'flit_in_unique_id': 64, 'success': 1, 'fatal': 1})}
        for name, (module, ports) in contracts.items():
            assert instances[name]['module'] == module
            assert {p['name']: p['bits'] for p in instances[name]['ports']} == ports
        run('outline', [py, sim / 'compiler/partition.py', '--input', work / 'partitioned.mlir',
            '--output', work / 'outlined.mlirbc', '--circt-opt', tool / 'circt-opt',
            '--cache', cache, '--cxx', cxx, '--max-ops', '16', '--emit-bytecode', '--native-feedback'])
        run('canonicalize', [tool / 'circt-opt', work / 'outlined.mlirbc',
            '--symbol-dce', '--canonicalize', '-o', work / 'model.mlir'])
        run('model', [py, sim / 'compiler/model.py', '--input', work / 'model.mlir',
            '--output', work / 'model.ll', '--state-file', work / 'model.json',
            '--arcilator', tool / 'arcilator', '--circt-opt', tool / 'circt-opt',
            '--mlir-translate', tool / 'mlir-translate', '--cache', cache, '--cxx', cxx,
            '--max-ops', '16', '--partition', work / 'partition.json', '--', '--split-funcs-threshold=8192'])
        fields = {'clock': 1, 'reset': 1, 'current_cycle': 64, 'tx_ready': 1,
                  'tx_valid': 1, 'tx_head': 1, 'tx_tail': 1, 'tx_egress': 64, 'tx_unique': 64,
                  'rx_ready': 1, 'rx_valid': 1, 'rx_head': 1, 'rx_tail': 1,
                  'rx_ingress': 64, 'rx_unique': 64, 'success': 1, 'fatal': 1}
        layout, = json.loads((work / 'model.json').read_text())
        states = {p['name']: p for p in layout['states']}
        assert all(states[name]['numBits'] == width for name, width in fields.items())
        run('LLVM', [py, sim / 'compiler/llvm.py', work / 'model.ll', '--llvm-opt', tool / 'opt',
            '--cache', cache, '--cxx', cxx])
        (work / 'originals.f').write_text(''.join(shlex.quote(str(p)) + '\n'
            for p in [*originals[:3], originals[-1]]))
        run('native-factory', [py, sim / 'native.py', work / 'model.json', work / 'partition.json',
            work / 'model.cpp', '--filelist=' + str(work / 'originals.f')])
        (work / 'native.f').write_text((work / 'originals.f').read_text() +
                                       shlex.quote(str(work / 'model.cpp')) + '\n')
        flags = ['-DARC_NATIVE_NO_MAIN', '-I' + str(sim), '-I' + str(sim / 'native/include'),
                 '-I' + str(sdk / 'include'), '-I' + str(root / 'csrc/netrace')]
        libraries = ['-Wl,--wrap=ingress_tick', '-Wl,--wrap=egress_tick',
            '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle',
            '-lz', '-lpthread', '-ldl', '-lm']
        params = {}
        for mode in ('default', 'override', 'throughput', 'timeout'):
            params[mode] = work / (mode + ' traffic parameters.txt')
            params[mode].write_text('warmup 0\nmeasurement 4\ndrain 32\n'
                'flits_per_packet 3\nflow 0 0 3.0\nrequired_throughput ' +
                ('2.0' if mode == 'throughput' else '0.0') + '\n')
        for level in ('0', '3'):
            obj = work / ('model-O' + level + '.o')
            run('codegen-O' + level, [py, sim / 'compiler/codegen.py', '--input', work / 'model.ll',
                '--output', obj, '--llvm-opt', tool / 'opt', '--llc', tool / 'llc',
                '--cache', cache, '--cxx', cxx, '--opt-level=' + level,
                '--llc-opt-level=' + level, '--jobs=1'])
            exe = work / ('O' + level)
            if level == '0':
                text = run('native-CLI', [py, sim / 'native_build.py',
                    '--filelist=' + str(work / 'native.f'), '--main=' + str(sim / 'tests/native_traffic.cpp'),
                    '--model=' + str(obj), '--output=' + str(exe), '--build=' + str(work / 'objects'),
                    '--cxx=' + cxx, '--cc=' + cc, '--cxxflags=' + shlex.join(flags),
                    '--ldflags=' + shlex.join(libraries), '--jobs=1'])
                link = shlex.split(text.splitlines()[-1])
                objects = {}
                for line in text.splitlines():
                    words = shlex.split(line)
                    if '-c' in words and '-o' in words:
                        objects[words[words.index('-c') + 1]] = words[words.index('-o') + 1]
                assert all(str(p) in objects for p in originals[:3])
            else:
                command = link.copy()
                command[command.index(str(work / 'model-O0.o'))] = str(obj)
                command[command.index('-o') + 1] = str(exe)
                run('link-O' + level, command)
            for mode in params:
                trace_file = work / ('O' + level + '-' + mode + '.bin')
                text = run('execute-O' + level + '-' + mode, [exe, mode, trace_file, params[mode]])
                timeout = mode == 'timeout'
                failure = timeout or mode == 'throughput'
                expected = ('PASS original TrafficEval: mode=' + mode + ' cycles=' +
                    ('1 flits=0' if timeout else '15 flits=12') +
                    ' success=' + str(int(not failure)) + ' fatal=' + str(int(failure)) + '\n')
                assert text.endswith(expected), text
                assert trace_file.read_bytes() == trace_bytes(mode), 'complete TrafficEval flit trace mismatch'
                if timeout:
                    assert 'Error, traffic eval timed out\n' in text
                else:
                    stats = '3, 12, 0.250000, 1, 4' if mode == 'default' else '2, 12, 0.166667, 1, 3'
                    assert '\n0, 0, ' + stats + '\n' in text, text
                if mode in ('override', 'throughput'):
                    assert 'Constructing params from ' + str(params[mode]) + '\n' in text
                else:
                    assert 'No traffic matrix plusarg found. Falling back on default config str.\n' in text
                report['models'].append({'optimization': level, 'mode': mode,
                    'trace': str(trace_file), 'trace_bytes': trace_file.stat().st_size,
                    'trace_sha256': sha(trace_file), 'all_trace_bytes_match': True,
                    'original_completion_and_diagnostics_checked': True})
        original = (sim / 'native/cosim.cpp').read_text()
        start = original.index('class Traffic : public Binding')
        end = original.index('template <class T> void add', start)
        body = original[start:end]
        for name, mode, old, new, count, diagnostic in [
            ('corrupt-unique', 'default', 'stage("flit_out_unique_id", unique);',
             'stage("flit_out_unique_id", unique ^ 1);', 1, 'flit identity/head/tail mapping changed'),
            ('swap-completion', 'default', 'stage("success", success);',
             'stage("success", fatal);', 1, 'completion success/fatal mapping changed'),
            ('truncate-cycle', 'timeout', 'get("current_cycle")',
             'uint32_t(get("current_cycle"))', 2, '64-bit cycle ABI changed'),
            ('clear-reset-payload', 'default', 'stage("flit_out_valid", 0);',
             'stage("flit_out_valid", 0); stage("flit_out_tail", 0);', 1,
             'reset must retain ingress payload')]:
            assert body.count(old) == count
            source = work / (name + '.cpp')
            source.write_text(original[:start] + body.replace(old, new) + original[end:])
            obj = work / (name + '.o')
            run('compile-' + name, compiler + ['-O3', '-std=c++17', *flags,
                '-I' + str(sim / 'native'), '-c', source, '-o', obj])
            command = link.copy()
            command[command.index(objects[str(sim / 'native/cosim.cpp')])] = str(obj)
            command[command.index(str(work / 'model-O0.o'))] = str(work / 'model-O3.o')
            command[command.index('-o') + 1] = str(work / name)
            run('link-' + name, command)
            text = run('reject-' + name, [work / name, mode, work / (name + '.bin'), params[mode]], success=False)
            assert diagnostic in text
            report['faults'].append({'name': name, 'detected': True})
        assert all(sha(Path(p)) == h for p, h in hashes.items())
        report.update(status='passed', original_sources_unchanged=True,
                      generated_root_fields_checked=17, generated_binding_ports_checked=20,
                      original_C_and_CXX_drivers_linked=True, netrace_file_playback_tested=False)
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
                        / 'generated-src/native-traffic-test')
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/compiler')
    args = parser.parse_args()
    check(args.circt_bin.resolve().parent, args.build.resolve(), args.cache.resolve(),
          os.environ.get('CXX', 'c++'), os.environ.get('CC', 'cc'))
    print('Compiled native TrafficEval passed: original C++/C drivers, O0/O3, '
          'flit traces, default/file parameters, reset, success/failure and four rejected faults')
