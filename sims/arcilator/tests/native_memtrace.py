"""Check native Radiance memory traces with the original C++ implementations."""
from pathlib import Path
import argparse
import copy
import hashlib
import json
import os
import resource
import shlex
import signal
import struct
import subprocess
import sys


def check(sdk, work, cache, cxx):
    sim = Path(__file__).resolve().parents[1]
    for ancestor in sim.parents:
        root = ancestor / 'generators/radiance/src/main/resources'
        if (root / 'csrc/SimMemTrace.cc').is_file():
            break
    else:
        raise FileNotFoundError('cannot locate original Radiance trace drivers')
    originals = [root / name for name in ('csrc/SimMemTrace.cc', 'csrc/SimMemTraceLogger.cc',
        'csrc/SimMemTrace.h', 'vsrc/SimMemTrace.v', 'vsrc/SimMemTraceLogger.v', 'vsrc/SimDefaults.vh')]
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
        with log.open('w') as stream:
            process = subprocess.Popen(command, cwd=work, stdin=subprocess.DEVNULL,
                stdout=stream, stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=limits)
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
        row['returncode'] = code
        save()
        if code != (0 if success else 1):
            raise RuntimeError(name + ': ' + log.read_text()[-5000:])
        print('PASS ' + name, flush=True)
        return log.read_text()

    high = 0x100000001
    # cycle, lane, store, address, data, log2(bytes), optional source column.
    entries = [(0, 0, 1, 0x8000000000001111, 0xfedcba9876543210, 3, 33),
               (0, 2, 0, 0x0123456789abcdef, 0x8877665544332211, 2, 35),
               (1, 1, 1, 0xfffffff001020304, 0x0123456789abcdef, 1, 34),
               (3, 0, 0, 0x8000000100000400, 0x8000000000000001, 4, 52),
               (high, 1, 1, 0xf000000080000123, 0xf00dcafe87654321, 0, 59)]
    # Explicit output order, including stall holds, gaps, reset and retained EOF.
    controls = [(0, 0, 1, 0, 0, [], 0),
                (0, 1, 0, 0, 5, [0, 1], 0),
                (0, 0, 0, 1, 0, [0, 1], 0),
                (1, 1, 0, 2, 2, [2], 0),
                (2, 1, 0, 3, 0, [], 0),
                (3, 1, 0, 4, 7, [3], 0),
                (3, 0, 0, 5, 0, [3], 0),
                (high, 1, 0, 6, 1, [4], 1),
                (high, 0, 0, 7, 0, [4], 1),
                (high, 1, 1, 0, 7, [], 0),
                (high, 1, 0, 0, 4, [], 1),
                (high, 0, 0, 1, 0, [], 1)]
    expected, lines, logger = bytearray(), [], []
    for step, (cycle, ready, reset, logger_cycle, valid, selection, finished) in enumerate(controls):
        addresses, data, sizes = [0] * 3, [0] * 3, [0] * 3
        reads = stores = 0
        for index in selection:
            _, lane, store, address, value, size, _ = entries[index]
            addresses[lane], data[lane], sizes[lane] = address, value, size
            reads |= 1 << lane
            stores |= store << lane
        row = struct.pack('<B3QB3B3QBB', reads, *addresses, stores, *sizes, *data, finished, 1)
        assert len(row) == 55
        expected += row
        lines.append(' '.join(map(str, [cycle, ready, reset, logger_cycle, valid])) + ' ' + row.hex())
        if not reset:
            for lane in range(3):
                if not ((valid >> lane) & 1):
                    continue
                address = 0x8000000000010000 | (step << 8) | lane
                value = 0xfedcba9876500000 ^ (step * 0x10001) ^ (lane * 0x12345)
                source = [33, -7, 0x12345678][lane]
                size = [8, 1, 128][lane]
                action = 'STORE' if (step + lane) % 2 else 'LOAD'
                logger.append(f'{logger_cycle} {action} 0 {lane} {source} 0x{address:x} 0x{value:x} {size}\n')
    expected_log = ''.join(logger).encode()
    oracle = work / 'expected.txt'
    oracle.write_text('\n'.join(lines) + '\n')
    assert len(expected) == 660 and len(logger) == 8

    def input_trace(path, mode):
        text = []
        for cycle, lane, store, address, data, size, source in entries:
            fields = [str(cycle), 'STORE' if store else 'LOAD', '0', str(lane)]
            if mode == 'with-source': fields.append(str(source))
            text.append(' '.join([*fields, f'0x{address:x}', f'0x{data:x}', str(1 << size)]))
        path.write_text('\n'.join(text) + '\n')

    try:
        run('partition', [py, sim / 'partition.py', '--input', sim / 'tests/native_memtrace.mlir',
            '--output', work, '--top', 'MemoryTraceModelProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        part = json.loads((work / 'partition.json').read_text())
        instances = {p['name']: p for p in part['instances']}
        assert len(instances) == 2
        common = {'clock': 1, 'reset': 1}
        contracts = {
            'reader': ('SimMemTrace', {**common, 'trace_read_cycle': 64, 'trace_read_ready': 1,
                'trace_read_valid': 3, 'trace_read_address': 192, 'trace_read_is_store': 3,
                'trace_read_size': 24, 'trace_read_data': 192, 'trace_read_finished': 1}),
            'logger': ('SimMemTraceLogger', {**common, 'trace_log_valid': 3, 'trace_log_source': 96,
                'trace_log_address': 192, 'trace_log_is_store': 3, 'trace_log_size': 24,
                'trace_log_data': 192, 'trace_log_ready': 1})}
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
        fields = {'clock': 1, 'reset': 1, 'trace_cycle': 64, 'trace_ready': 1,
                  'trace_valid': 3, 'trace_address': 192, 'trace_is_store': 3, 'trace_size': 24,
                  'trace_data': 192, 'trace_finished': 1, 'log_valid': 3, 'log_source': 96,
                  'log_address': 192, 'log_is_store': 3, 'log_size': 24, 'log_data': 192, 'log_ready': 1}
        layout, = json.loads((work / 'model.json').read_text())
        states = {p['name']: p for p in layout['states']}
        assert all(states[name]['numBits'] == width for name, width in fields.items())
        run('LLVM', [py, sim / 'compiler/llvm.py', work / 'model.ll', '--llvm-opt', tool / 'opt',
            '--cache', cache, '--cxx', cxx])
        (work / 'originals.f').write_text(''.join(shlex.quote(str(p)) + '\n'
            for p in [*originals[:2], originals[3], originals[4]]))
        flags = ['-DARC_NATIVE_NO_MAIN', '-I' + str(sim), '-I' + str(sim / 'native/include'),
                 '-I' + str(sdk / 'include')]
        libraries = ['-Wl,--wrap=memtrace_init', '-Wl,--wrap=memtrace_query',
            '-Wl,--wrap=memtracelogger_init', '-Wl,--wrap=memtracelogger_log',
            '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle',
            '-lz', '-lpthread', '-ldl', '-lm']
        factories = {}
        for mode in ['without-source', 'with-source']:
            variant = copy.deepcopy(part)
            if mode == 'with-source':
                for instance in variant['instances']:
                    old, new = ('HAS_SOURCE: i32 = 2', 'HAS_SOURCE: i32 = 3') if instance['name'] == 'reader' else ('IS_RESPONSE: i32 = 3', 'IS_RESPONSE: i32 = 2')
                    assert old in instance['parameters']
                    instance['parameters'][instance['parameters'].index(old)] = new
            metadata = work / (mode + '-partition.json')
            metadata.write_text(json.dumps(variant, indent=2) + '\n')
            factory = work / (mode + '-factory.cpp')
            run('factory-' + mode, [py, sim / 'native.py', work / 'model.json', metadata,
                factory, '--filelist=' + str(work / 'originals.f')])
            factories[mode] = factory
        (work / 'native.f').write_text((work / 'originals.f').read_text() +
            shlex.quote(str(factories['without-source'])) + '\n')
        for level in ['0', '3']:
            obj = work / ('model-O' + level + '.o')
            run('codegen-O' + level, [py, sim / 'compiler/codegen.py', '--input', work / 'model.ll',
                '--output', obj, '--llvm-opt', tool / 'opt', '--llc', tool / 'llc',
                '--cache', cache, '--cxx', cxx, '--opt-level=' + level, '--llc-opt-level=' + level, '--jobs=1'])
            if level == '0':
                text = run('native-CLI', [py, sim / 'native_build.py', '--filelist=' + str(work / 'native.f'),
                    '--main=' + str(sim / 'tests/native_memtrace.cpp'), '--model=' + str(obj),
                    '--output=' + str(work / 'O0-without-source'), '--build=' + str(work / 'objects'),
                    '--cxx=' + cxx, '--cxxflags=' + shlex.join(flags), '--ldflags=' + shlex.join(libraries), '--jobs=1'])
                link = shlex.split(text.splitlines()[-1])
                objects = {}
                for line in text.splitlines():
                    words = shlex.split(line)
                    if '-c' in words and '-o' in words:
                        objects[words[words.index('-c') + 1]] = words[words.index('-o') + 1]
                with_factory = work / 'with-source-factory.o'
                run('compile-with-source-factory', compiler + ['-O3', '-std=c++17', *flags,
                    '-c', factories['with-source'], '-o', with_factory])
            for mode in factories:
                exe = work / ('O' + level + '-' + mode)
                if level != '0' or mode != 'without-source':
                    command = link.copy()
                    command[command.index(str(work / 'model-O0.o'))] = str(obj)
                    if mode == 'with-source':
                        command[command.index(objects[str(factories['without-source'])])] = str(with_factory)
                    command[command.index('-o') + 1] = str(exe)
                    run('link-O' + level + '-' + mode, command)
                trace_input = work / ('O' + level + '-' + mode + ' memory trace.txt')
                input_trace(trace_input, mode)
                binary = work / ('O' + level + '-' + mode + '.bin')
                text = run('execute-O' + level + '-' + mode, [exe, mode, trace_input, oracle, binary])
                assert 'PASS original memory trace: mode=' + mode + ' steps=12 queries=18 logs=30\n' in text
                assert binary.read_bytes() == expected, 'complete memory trace output mismatch'
                log = Path(str(trace_input) + '.native-log')
                assert log.read_bytes() == expected_log, 'complete original memory logger file mismatch'
                report['models'].append(dict(optimization=level, mode=mode, trace=str(binary),
                    trace_bytes=len(expected), trace_sha256=sha(binary), log=str(log), log_sha256=sha(log),
                    complete_trace_and_log_bytes_match=True))
        original = (sim / 'native/additional.cpp').read_text()
        begin, end = original.index('class MemoryTrace :'), original.index('class LaneDriver :')
        body = original[begin:end]
        for name, old, new, diagnostic in [
            ('reader-bit', 'number("HAS_SOURCE") & 1', 'number("HAS_SOURCE")', 'failed parsing address..size'),
            ('logger-bit', 'number("IS_RESPONSE") & 1', 'number("IS_RESPONSE")', 'one-bit IS_RESPONSE ABI changed'),
            ('ignore-stall', '(!get("reset") && !get("trace_read_ready"))', 'false', 'reader called while stalled/reset or wrong edge'),
            ('reverse-lanes', 'for (unsigned index = 0; index < lanes; ++index)\n        memtrace_query',
             'for (unsigned index = lanes; index-- > 0;)\n        memtrace_query', 'original memory trace lane/cycle order changed')]:
            assert body.count(old) == 1
            source = work / (name + '.cpp')
            source.write_text(original[:begin] + body.replace(old, new) + original[end:])
            obj = work / (name + '.o')
            run('compile-' + name, compiler + ['-O3', '-std=c++17', *flags,
                '-I' + str(sim / 'native'), '-c', source, '-o', obj])
            command = link.copy()
            command[command.index(objects[str(sim / 'native/additional.cpp')])] = str(obj)
            command[command.index(str(work / 'model-O0.o'))] = str(work / 'model-O3.o')
            command[command.index('-o') + 1] = str(work / name)
            run('link-' + name, command)
            trace_input = work / (name + ' memory trace.txt')
            input_trace(trace_input, 'without-source')
            text = run('reject-' + name, [work / name, 'without-source', trace_input, oracle,
                work / (name + '.bin')], False)
            assert diagnostic in text
            report['faults'].append(dict(name=name, detected=True))
        assert all(sha(Path(p)) == value for p, value in hashes.items())
        report.update(status='passed', original_sources_unchanged=True,
                      generated_root_fields_checked=17, generated_binding_ports_checked=19,
                      original_DPI_bit_parameters_checked=True)
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
                        / 'generated-src/native-memtrace-test')
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/compiler')
    args = parser.parse_args()
    check(args.circt_bin.resolve().parent, args.build.resolve(), args.cache.resolve(),
          os.environ.get('CXX', 'c++'))
    print('Original native memory traces passed: DPI bit parameters, O0/O3, packed lanes, '
          'stall/reset/EOF, full output/log bytes and four rejected faults')
