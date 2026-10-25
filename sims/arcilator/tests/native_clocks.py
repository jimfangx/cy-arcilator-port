"""Check original inline clock wrappers through RTL import and native models."""
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


def check(sdk, frontend, work, cache, cxx):
    sim = Path(__file__).resolve().parents[1]
    for ancestor in sim.parents:
        source = ancestor / 'generators/rocket-chip/src/main/scala/prci/TestClockSource.scala'
        harness = ancestor / 'generators/chipyard/src/main/scala/harness/HarnessClocks.scala'
        if source.is_file() and harness.is_file():
            break
    else:
        raise FileNotFoundError('cannot locate original inline clock sources')
    work.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p): sha(p) for p in [source, harness]}
    report = {'status': 'running', 'steps': [], 'models': [], 'faults': [],
              'original_sources_sha256': hashes, 'frontend': str(frontend),
              'frontend_sha256': sha(frontend)}
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

    def inline(path, marker):
        text = path.read_text()
        start = text.index('      |module ' + marker)
        end = text.index('      |endmodule', start) + len('      |endmodule')
        return '\n'.join(line.split('|', 1)[1] for line in text[start:end].splitlines()) + '\n'

    bodies = {
        'ClockSourceAtFreq': inline(source, 'ClockSourceAtFreq #'),
        'ClockSourceAtFreqMHz': inline(harness, '$moduleName #').replace('$moduleName', 'ClockSourceAtFreqMHz'),
        'ClockSourceAtFreqFromPlusArgUNIT_CLOCK_MHZ': inline(source, '$desiredName (')
            .replace('$desiredName', 'ClockSourceAtFreqFromPlusArgUNIT_CLOCK_MHZ')
            .replace('$plusArgName', 'UNIT_CLOCK_MHZ').replace('$$', '$')}

    def oracle(mode):
        half = [5000000, 5000000, 4000000 if mode == 'override' else 5000000]
        drives = {0: (1, 0), 1250500: (1, 1), 6250250: (1, 0),
                  11250750: (0, 0), 16250500: (1, 0),
                  21250250: (1, 1), 26250750: (1, 0)}
        times = set(drives)
        for period in half:
            for edge in range(period, 30000001, period):
                times.update([edge - 1, edge, edge + 1])
        levels, counts, pending = [0, 0, 0], [0, 0, 0], 1
        lines, binary = [], bytearray()
        power, gate = 1, 0
        for stamp in sorted(times):
            if stamp in drives:
                power, gate = drives[stamp]
            for i in range(3):
                if stamp and stamp % half[i] == 0:
                    old = levels[i]
                    if i == 0:
                        # Intra-assignment delay samples RHS before waiting.
                        levels[i] = pending
                        pending = int(not levels[i] and power and not gate)
                    else:
                        # Delay before assignment samples RHS at its deadline.
                        levels[i] = int(not old and power and not gate)
                    counts[i] += int(not old and levels[i])
            lines.append(' '.join(map(str, [stamp, power, gate, int(stamp in drives), *levels, *counts])))
            binary += struct.pack('<QBBBHHH', stamp, *levels, *counts)
        path = work / (mode + '-expected.txt')
        path.write_text('\n'.join(lines) + '\n')
        return path, bytes(binary), len(lines)

    def pipeline(folder, fault=False):
        folder.mkdir(parents=True, exist_ok=True)
        selected = []
        for module, body in bodies.items():
            if fault and module == 'ClockSourceAtFreq':
                old = 'clk_i = #(PERIOD_PS/2.0) ~clk_i & (power & ~gate);'
                assert body.count(old) == 1
                body = body.replace(old, '#(PERIOD_PS/2.0) clk_i = ~clk_i & (power & ~gate);')
            path = folder / (module + '.v')
            path.write_text(body)
            selected.append(path)
        filelist = folder / 'blackboxes.f'
        filelist.write_text(''.join(shlex.quote(str(p)) + '\n' for p in selected))
        tag = folder.name
        run(tag + '-native-HW', [py, sim / 'native_hw.py', '--input', sim / 'tests/native_clocks.mlir',
            '--output', folder / 'imported.mlir', '--filelist', filelist, '--work', folder / 'rtl',
            '--circt-verilog', frontend, '--circt-opt', tool / 'circt-opt', '--cxx', cxx, '--jobs=1'])
        run(tag + '-partition', [py, sim / 'partition.py', '--input', folder / 'imported.mlir',
            '--output', folder, '--top', 'OriginalClockProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        part = json.loads((folder / 'partition.json').read_text())
        modules = [v['module'] for v in part['instances']]
        assert 'ClockSourceAtFreq' not in modules and 'ArcSignalMemory' in modules
        assert 'ClockSourceAtFreqMHz' in modules and 'ClockSourceAtFreqFromPlusArgUNIT_CLOCK_MHZ' in modules
        run(tag + '-outline', [py, sim / 'compiler/partition.py', '--input', folder / 'partitioned.mlir',
            '--output', folder / 'outlined.mlirbc', '--circt-opt', tool / 'circt-opt',
            '--cache', cache, '--cxx', cxx, '--max-ops', '16', '--emit-bytecode', '--native-feedback'])
        run(tag + '-canonicalize', [tool / 'circt-opt', folder / 'outlined.mlirbc',
            '--symbol-dce', '--canonicalize', '-o', folder / 'model.mlir'])
        run(tag + '-model', [py, sim / 'compiler/model.py', '--input', folder / 'model.mlir',
            '--output', folder / 'model.ll', '--state-file', folder / 'model.json',
            '--arcilator', tool / 'arcilator', '--circt-opt', tool / 'circt-opt',
            '--mlir-translate', tool / 'mlir-translate', '--cache', cache, '--cxx', cxx,
            '--max-ops', '16', '--partition', folder / 'partition.json', '--', '--split-funcs-threshold=8192'])
        layout, = json.loads((folder / 'model.json').read_text())
        states = {p['name']: p for p in layout['states']}
        for name in ('clock', 'reset', 'power', 'gate', 'fixed', 'harness', 'plusarg'):
            assert states[name]['numBits'] == 1
        for name in ('fixed_count', 'harness_count', 'plusarg_count'):
            assert states[name]['numBits'] == 16
        run(tag + '-LLVM', [py, sim / 'compiler/llvm.py', folder / 'model.ll', '--llvm-opt', tool / 'opt',
            '--cache', cache, '--cxx', cxx])
        run(tag + '-factory', [py, sim / 'native.py', folder / 'model.json', folder / 'partition.json',
            folder / 'model.cpp', '--filelist=' + str(filelist)])
        with filelist.open('a') as stream:
            stream.write(shlex.quote(str(folder / 'model.cpp')) + '\n')
        return filelist

    def codegen(folder, level):
        obj = folder / ('model-O' + level + '.o')
        run(folder.name + '-codegen-O' + level, [py, sim / 'compiler/codegen.py', '--input', folder / 'model.ll',
            '--output', obj, '--llvm-opt', tool / 'opt', '--llc', tool / 'llc',
            '--cache', cache, '--cxx', cxx, '--opt-level=' + level, '--llc-opt-level=' + level, '--jobs=1'])
        return obj

    flags = ['-DARC_NATIVE_NO_MAIN', '-I' + str(sim), '-I' + str(sim / 'native/include'),
             '-I' + str(sdk / 'include')]
    libraries = ['-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport',
                 '-lLLVMDemangle', '-lz', '-lpthread', '-ldl']
    try:
        # Resolve the matching patched frontend as a guarded stage so the
        # report identifies the exact binary used by native_hw.py.
        if frontend.name == 'circt-verilog':
            text = run('select-patched-frontend', [py, sim / 'compiler/verilog/build.py',
                '--circt-verilog', frontend, '--cache', cache / 'verilog', '--jobs=1'])
            frontend = Path(text.splitlines()[-1]).resolve()
            report.update(frontend=str(frontend), frontend_sha256=sha(frontend))
            save()
        folder = work / 'correct'
        filelist = pipeline(folder)
        expected = {mode: oracle(mode) for mode in ['default', 'override']}
        for level in ['0', '3']:
            obj = codegen(folder, level)
            exe = work / ('O' + level)
            if level == '0':
                text = run('native-CLI', [py, sim / 'native_build.py', '--filelist=' + str(filelist),
                    '--main=' + str(sim / 'tests/native_clocks.cpp'), '--model=' + str(obj),
                    '--output=' + str(exe), '--build=' + str(work / 'objects'), '--cxx=' + cxx,
                    '--cxxflags=' + shlex.join(flags), '--ldflags=' + shlex.join(libraries), '--jobs=1'])
                link = shlex.split(text.splitlines()[-1])
                objects = {}
                for line in text.splitlines():
                    words = shlex.split(line)
                    if '-c' in words and '-o' in words:
                        objects[words[words.index('-c') + 1]] = words[words.index('-o') + 1]
            else:
                command = link.copy()
                command[command.index(str(folder / 'model-O0.o'))] = str(obj)
                command[command.index('-o') + 1] = str(exe)
                run('link-O' + level, command)
            for mode, (path, binary, rows) in expected.items():
                trace = work / ('O' + level + '-' + mode + '.bin')
                text = run('execute-O' + level + '-' + mode, [exe, mode, path, trace])
                assert text == 'PASS original clocks: mode=' + mode + ' samples=' + str(rows) + '\n'
                assert trace.read_bytes() == binary, 'complete original clock trace mismatch'
                report['models'].append(dict(optimization=level, mode=mode, samples=rows,
                    trace=str(trace), trace_bytes=len(binary), trace_sha256=sha(trace), all_trace_bytes_match=True))
        # Fault 1: move the delay across the original fixed-clock assignment.
        fault = work / 'wrong-sampling'
        pipeline(fault, True)
        obj = codegen(fault, '3')
        factory = fault / 'factory.o'
        run('compile-wrong-sampling-factory', compiler + ['-O3', '-std=c++17', *flags,
            '-c', fault / 'model.cpp', '-o', factory])
        command = link.copy()
        command[command.index(objects[str(folder / 'model.cpp')])] = str(factory)
        command[command.index(str(folder / 'model-O0.o'))] = str(obj)
        command[command.index('-o') + 1] = str(fault / 'test')
        run('link-wrong-sampling', command)
        text = run('reject-wrong-sampling', [fault / 'test', 'default', expected['default'][0],
            fault / 'trace.bin'], False)
        assert 'original clock levels/counters mismatch' in text
        report['faults'].append(dict(name='wrong-RHS-sampling-time', detected=True))
        # Fault 2: ignore gate in the native harness/plusarg clock binding.
        source_text = (sim / 'native/bindings.cpp').read_text()
        begin, end = source_text.index('class ClockSource :'), source_text.index('class ClockFlop :')
        body = source_text[begin:end]
        assert body.count('!get("gate")') == 1
        fault_source = work / 'ignore-gate.cpp'
        fault_source.write_text(source_text[:begin] + body.replace('!get("gate")', 'true') + source_text[end:])
        obj = work / 'ignore-gate.o'
        run('compile-ignore-gate', compiler + ['-O3', '-std=c++17', *flags,
            '-I' + str(sim / 'native'), '-c', fault_source, '-o', obj])
        command = link.copy()
        command[command.index(objects[str(sim / 'native/bindings.cpp')])] = str(obj)
        command[command.index(str(folder / 'model-O0.o'))] = str(folder / 'model-O3.o')
        command[command.index('-o') + 1] = str(work / 'ignore-gate')
        run('link-ignore-gate', command)
        text = run('reject-ignore-gate', [work / 'ignore-gate', 'default', expected['default'][0],
            work / 'ignore-gate.bin'], False)
        assert 'original clock levels/counters mismatch' in text
        report['faults'].append(dict(name='ignored-native-clock-gate', detected=True))
        assert all(sha(Path(p)) == value for p, value in hashes.items())
        report.update(status='passed', original_sources_unchanged=True,
                      generated_root_fields_checked=10, original_fixed_clock_imported_as_RTL=True,
                      original_harness_and_plusarg_native_bindings_selected=True)
        save()
        return report
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        save()
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--frontend', type=Path)
    parser.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/native-clock-test')
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/compiler')
    args = parser.parse_args()
    sdk = args.circt_bin.resolve().parent
    check(sdk, (args.frontend or sdk / 'bin/circt-verilog').resolve(), args.build.resolve(),
          args.cache.resolve(), os.environ.get('CXX', 'c++'))
    print('Original native clocks passed: RTL import, O0/O3, default/plusarg frequency, '
          'power/gate timing, peer counts and two rejected faults')
