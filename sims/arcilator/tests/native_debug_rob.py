"""Compile native DebugROB bindings with the original mixed-edge DPI driver."""
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


def check(sdk, work, cache, cxx):
    sim = Path(__file__).resolve().parents[1]
    for ancestor in sim.parents:
        root = ancestor / 'generators/rocket-chip/src/main/resources'
        if (root / 'csrc/debug_rob.cc').is_file():
            break
    else:
        raise FileNotFoundError('cannot locate original Rocket Chip DebugROB driver')
    originals = [root / 'csrc/debug_rob.cc', root / 'vsrc/debug_rob.v']
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

    def trace_bytes():
        # Handwritten publication order, including retained values across reset.
        # Tuples are instruction identity and the independently supplied WB data.
        order = [None, None, None, None, None, None, None,
                 (3, 3), (1, 10), (2, 2), None, None,
                 (6, 11), (6, 11), (6, 11), None, None,
                 (4, 4), (5, 5), None, None, None, None, None,
                 (7, 14), (8, 13), None]
        result = bytearray()
        mask = (1 << 64) - 1
        for event in order:
            if event is None:
                result += bytes(100)
                continue
            ident, data = event
            words = [(0x8123456700000000 ^ (data * 0x0101010101010101) ^
                      (lane * 0x0102040810204081)) & mask for lane in range(8)]
            result += struct.pack('<BQQBBBQQ8Q', 1,
                0x8000000000100000 + ident * 4,
                0xfedcba9800000000 ^ (ident * 0x10001), ident & 7,
                ident == 4, ident == 5, 0x8000000000000000 | ident,
                0xfeed000000000000 ^ (ident * 0x10203), *words)
        assert len(result) == 2700
        return bytes(result)

    try:
        run('partition', [py, sim / 'partition.py', '--input', sim / 'tests/native_debug_rob.mlir',
            '--output', work, '--top', 'DebugROBModelProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        part = json.loads((work / 'partition.json').read_text())
        instances = {p['name']: p for p in part['instances']}
        assert len(instances) == 3
        trace = {'trace_valid': 1, 'trace_iaddr': 64, 'trace_insn': 64, 'trace_priv': 3,
                 'trace_exception': 1, 'trace_interrupt': 1, 'trace_cause': 64,
                 'trace_tval': 64, 'trace_wdata': 512}
        common = {'clock': 1, 'reset': 1, 'hartid': 32}
        contracts = {
            'push': ('DebugROBPushTrace', {**common, 'should_wb': 1, 'has_wb': 1,
                                         'wb_tag': 64, **trace}),
            'wb': ('DebugROBPushWb', {**common, 'valid': 1, 'wb_tag': 64, 'wb_data': 512}),
            'pop': ('DebugROBPopTrace', {**common, **trace})}
        for name, (module, ports) in contracts.items():
            instance = instances[name]
            assert instance['module'] == module
            assert {p['name']: p['bits'] for p in instance['ports']} == ports
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
        fields = {'clock': 1, 'reset': 1, 'pop_hartid': 32,
                  'push_hartid': 32, 'push_should_wb': 1, 'push_has_wb': 1, 'push_wb_tag': 64,
                  'wb_hartid': 32, 'wb_valid': 1, 'wb_tag': 64, 'wb_data': 512}
        for name, width in trace.items():
            fields['push_' + name.removeprefix('trace_')] = width
            fields['out_' + name.removeprefix('trace_')] = width
        layout, = json.loads((work / 'model.json').read_text())
        states = {p['name']: p for p in layout['states']}
        assert len(fields) == 29
        assert all(states[name]['numBits'] == width for name, width in fields.items())
        run('LLVM', [py, sim / 'compiler/llvm.py', work / 'model.ll', '--llvm-opt', tool / 'opt',
            '--cache', cache, '--cxx', cxx])
        (work / 'originals.f').write_text(''.join(shlex.quote(str(p)) + '\n' for p in originals))
        run('native-factory', [py, sim / 'native.py', work / 'model.json', work / 'partition.json',
            work / 'model.cpp', '--filelist=' + str(work / 'originals.f')])
        (work / 'native.f').write_text((work / 'originals.f').read_text() +
                                       shlex.quote(str(work / 'model.cpp')) + '\n')
        flags = ['-DARC_NATIVE_NO_MAIN', '-I' + str(sim), '-I' + str(sim / 'native/include'),
                 '-I' + str(sdk / 'include')]
        libraries = ['-Wl,--wrap=debug_rob_push_trace', '-Wl,--wrap=debug_rob_push_wb',
            '-Wl,--wrap=debug_rob_pop_trace', '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime',
            '-lLLVMSupport', '-lLLVMDemangle', '-lz', '-lpthread', '-ldl']
        expected = trace_bytes()
        for level in ('0', '3'):
            obj = work / ('model-O' + level + '.o')
            run('codegen-O' + level, [py, sim / 'compiler/codegen.py', '--input', work / 'model.ll',
                '--output', obj, '--llvm-opt', tool / 'opt', '--llc', tool / 'llc',
                '--cache', cache, '--cxx', cxx, '--opt-level=' + level,
                '--llc-opt-level=' + level, '--jobs=1'])
            exe = work / ('O' + level)
            if level == '0':
                text = run('native-CLI', [py, sim / 'native_build.py',
                    '--filelist=' + str(work / 'native.f'), '--main=' + str(sim / 'tests/native_debug_rob.cpp'),
                    '--model=' + str(obj), '--output=' + str(exe), '--build=' + str(work / 'objects'),
                    '--cxx=' + cxx, '--cxxflags=' + shlex.join(flags),
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
            trace_file = work / ('O' + level + '.bin')
            text = run('execute-O' + level, [exe, trace_file])
            assert text == 'PASS original DebugROB: cycles=27 push=24 wb=24 pop=21 harts=2 words=8\n'
            assert trace_file.read_bytes() == expected, 'complete DebugROB publication trace mismatch'
            report['models'].append({'optimization': level, 'trace': str(trace_file),
                'trace_bytes': trace_file.stat().st_size, 'trace_sha256': sha(trace_file),
                'all_trace_bytes_match': True})
        original = (sim / 'native/cosim.cpp').read_text()
        start = original.index('class DebugROB : public Binding')
        end = original.index('class Traffic : public Binding', start)
        body = original[start:end]
        for name, old, new, diagnostic in [
            ('truncate-payload', 'std::memcpy(data.data(), value.data(), sizeof(data));',
             'std::memcpy(data.data(), value.data(), sizeof(long long));',
             '512-bit push payload ABI changed'),
            ('wrong-pop-edge', 'if (falling) {', 'if (rising) {',
             'pop callback edge/reset gate changed'),
            ('skip-reset-history', 'if (!reset && !previous_reset)', 'if (!reset)',
             'pop callback edge/reset gate changed'),
            ('truncate-writeback-tag', 'get("valid"), get("wb_tag"),',
             'get("valid"), uint32_t(get("wb_tag")),', '64-bit writeback tag ABI changed')]:
            assert body.count(old) == 1
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
            text = run('reject-' + name, [work / name, work / (name + '.bin')], success=False)
            assert diagnostic in text
            report['faults'].append({'name': name, 'detected': True})
        assert all(sha(Path(p)) == h for p, h in hashes.items())
        report.update(status='passed', original_sources_unchanged=True,
                      generated_root_fields_checked=29, generated_binding_ports_checked=33)
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
                        / 'generated-src/native-debug-rob-test')
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/compiler')
    args = parser.parse_args()
    check(args.circt_bin.resolve().parent, args.build.resolve(), args.cache.resolve(),
          os.environ.get('CXX', 'c++'))
    print('Compiled native DebugROB passed: original driver, O0/O3, full 512-bit '
          'payloads, mixed edges, reset history, two harts and four rejected faults')
