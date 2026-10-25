"""Check once-only native DPI declaration initialization and startup ordering."""
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


def check(sdk, frontend, work, cache, cxx, context=False, writable=False):
    fixture = "native_writable_declaration_dpi" if writable else (
        "native_context_declaration_dpi" if context else "native_declaration_dpi")
    sim = Path(__file__).resolve().parents[1]
    interface = sim / ('tests/native_writable_declaration_dpi.mlir' if writable else
                       'tests/native_declaration_dpi.mlir')
    work.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p): sha(p) for p in [sim / ('tests/' + fixture + '.' + ext) for ext in ['sv', 'cpp']] + [interface]}
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
        if (code == 0) != success:
            raise RuntimeError(name + ': ' + log.read_text()[-5000:])
        print('PASS ' + name, flush=True)
        return log.read_text()

    def pipeline(folder, fault=None):
        folder.mkdir(parents=True, exist_ok=True)
        body = (sim / ('tests/' + fixture + '.sv')).read_text()
        if fault == 'duplicate':
            body = body.replace('snapshot = declaration_seed(TAG);',
                                'snapshot = declaration_seed(TAG) + declaration_seed(TAG);')
        elif fault == 'observer':
            body = body.replace('observed = next_snapshot + shared_snapshot;',
                                "observed = next_snapshot + shared_snapshot + 64'd1;")
        source = folder / 'declarations.sv'
        source.write_text(body)
        filelist = folder / 'blackboxes.f'
        filelist.write_text(shlex.quote(str(source)) + '\n')
        tag = folder.name
        run(tag + '-native-HW', [py, sim / 'native_hw.py', '--input', interface,
            '--output', folder / 'imported.mlir', '--filelist', filelist, '--work', folder / 'rtl',
            '--circt-verilog', frontend, '--circt-opt', tool / 'circt-opt', '--cxx', cxx, '--jobs=1'])
        run(tag + '-partition', [py, sim / 'partition.py', '--input', folder / 'imported.mlir',
            '--output', folder, '--top', 'DeclarationDPIProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        run(tag + '-outline', [py, sim / 'compiler/partition.py', '--input', folder / 'partitioned.mlir',
            '--output', folder / 'outlined.mlirbc', '--circt-opt', tool / 'circt-opt',
            '--cache', cache, '--cxx', cxx, '--max-ops', '16', '--emit-bytecode', '--native-feedback'])
        run(tag + '-canonicalize', [tool / 'circt-opt', folder / 'outlined.mlirbc',
            '--symbol-dce', '--canonicalize', '-o', folder / 'model.mlir'])
        if fault in ('store', 'resample'):
            import re
            source = folder / 'model.mlir'
            output, active, seen, aliases, changed = [], False, {}, {}, 0
            for line in source.read_text().splitlines(True):
                if re.search(r'= seq\.initial\(', line):
                    active, seen, aliases = True, {}, {}
                if active:
                    for old, new in aliases.items():
                        line = re.sub(re.escape(old) + r'(?![A-Za-z0-9_$.-])', new, line)
                    if fault == 'store' and re.search(r'sim\.func\.dpi\.call @[^ (]*chipyard_arc_signal_write', line):
                        changed += 1
                        continue
                    match = re.match(r'\s*(%[^ ]+) = sim\.func\.dpi\.call (@[^ (]*chipyard_arc_signal_read[^ (]*)\(([^)]*)\)', line)
                    if fault == 'resample' and match:
                        result, callee, operands = match.groups()
                        key = (callee, operands)
                        if key in seen:
                            aliases[result] = seen[key]
                            changed += 1
                            continue
                        seen[key] = result
                    if line.lstrip().startswith('} {chipyard.declaration_startup}'):
                        active = False
                output.append(line)
            assert changed >= 2, 'writable initializer fault did not reach both instances'
            source.write_text(''.join(output))
        run(tag + '-model', [py, sim / 'compiler/model.py', '--input', folder / 'model.mlir',
            '--output', folder / 'model.ll', '--state-file', folder / 'model.json',
            '--arcilator', tool / 'arcilator', '--circt-opt', tool / 'circt-opt',
            '--mlir-translate', tool / 'mlir-translate', '--cache', cache, '--cxx', cxx,
            '--max-ops', '16', '--partition', folder / 'partition.json', '--', '--split-funcs-threshold=8192'])
        layout, = json.loads((folder / 'model.json').read_text())
        states = {v['name']: v for v in layout['states']}
        fields = {'clock': 1, 'reset': 1, 'data': 32}
        for prefix in ['first', 'second']:
            fields.update({prefix + suffix: bits for suffix, bits in
                           [('', 64), ('_derived', 64), ('_shared', 64), ('_observed', 64), ('_live', 32)]})
        if writable:
            fields.update(first_wide=129, second_wide=129)
        assert all(states[n]['numBits'] == bits for n, bits in fields.items())
        run(tag + '-LLVM', [py, sim / 'compiler/llvm.py', folder / 'model.ll', '--llvm-opt', tool / 'opt',
            '--cache', cache, '--cxx', cxx])
        run(tag + '-factory', [py, sim / 'native.py', folder / 'model.json', folder / 'partition.json',
            folder / 'model.cpp', '--filelist=' + str(filelist)])
        if fault == 'scope':
            # Swap real initialized scope bindings in the generated factory,
            # retaining the compiled model and all port widths.
            part = json.loads((folder / 'partition.json').read_text())
            a, b = [i['ports'][0]['wire'] for i in part['instances']
                    if i['module'] == 'ArcDpiScope'][:2]
            source = folder / 'model.cpp'
            text = source.read_text()
            first, second = '{"scope", "' + a + '"}', '{"scope", "' + b + '"}'
            assert text.count(first) == text.count(second) == 1
            text = text.replace(first, 'SCOPE_WIRING_FAULT_PLACEHOLDER').replace(second, first)
            source.write_text(text.replace('SCOPE_WIRING_FAULT_PLACEHOLDER', second))
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
    def oracle():
        result = bytearray()
        live = [0, 0]
        counters = [(0xfedcba9800000000 | (tag * 0x10203)) ^ 0x0100000100000100
                    for tag in [17, 29]]
        wides = [(1 << 128) | (value << 64) | (value ^ 0xfedcba9876543210)
                 for value in counters]
        previous = False
        for sample in range(24):
            clock, reset = bool(sample & 1), 5 <= sample < 10
            data = (0xf1234567 + sample * 0x10203) & 0xffffffff
            if clock and not previous and not reset:
                live = [data ^ tag for tag in [17, 29]]
            if writable and clock and not previous:
                for i, tag in enumerate([17, 29]):
                    counters[i] = (0x8000000100000055 + tag if reset else
                                   counters[i] + data) & ((1 << 64) - 1)
            if writable and clock and not previous:
                for i, tag in enumerate([17, 29]):
                    wides[i] = ((0x123456789abcdef00123456789abcdef01 ^ tag) if reset else
                                ((wides[i] << 1) ^ data)) & ((1 << 129) - 1)
            row = bytearray()
            held_row = bytearray()
            for i, (tag, current) in enumerate(zip([17, 29], live)):
                seed = 0xfedcba9800000000 | (tag * 0x10203)
                derived = seed ^ 0x8000000100000011 ^ tag
                initial = seed ^ 0x0100000100000100 if writable else seed
                shared = (initial + 0x100000001) & ((1 << 64) - 1)
                observed = (derived + shared) & ((1 << 64) - 1)
                value = counters[i] if writable else seed
                row += struct.pack('<QQQQI', value, derived, shared, observed, current)
                if writable: row += wides[i].to_bytes(17, 'little')
                if writable and sample == 4 and i == 1:
                    counters[i] = value = 0x8000000100000055 + tag
                held_row += struct.pack('<QQQQI', value, derived, shared, observed, current)
                if writable: held_row += wides[i].to_bytes(17, 'little')
            result += row + held_row
            previous = clock
        return bytes(result * 2)

    try:
        if frontend.name == 'circt-verilog':
            text = run('select-patched-frontend', [py, sim / 'compiler/verilog/build.py',
                '--circt-verilog', frontend, '--cache', cache / 'verilog', '--jobs=1'])
            frontend = Path(text.splitlines()[-1]).resolve()
            report.update(frontend=str(frontend), frontend_sha256=sha(frontend))
            save()
        folder = work / 'correct'
        filelist = pipeline(folder)
        expected = oracle()
        (work / 'expected.bin').write_bytes(expected)
        for level in ['0', '3']:
            obj = codegen(folder, level)
            exe = work / ('O' + level)
            if level == '0':
                text = run('native-CLI', [py, sim / 'native_build.py', '--filelist=' + str(filelist),
                    '--main=' + str(sim / ('tests/' + fixture + '.cpp')), '--model=' + str(obj),
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
            trace = work / ('O' + level + '.bin')
            text = run('execute-O' + level, [exe, trace])
            assert text == ('PASS native DPI declarations: once-only startup, dependencies, '
                            'time-zero observers, clocked calls and recreation\n')
            assert trace.read_bytes() == expected, 'complete declaration trace mismatch'
            report['models'].append(dict(optimization=level, recreations=2, samples=96,
                trace=str(trace), trace_bytes=len(expected), trace_sha256=sha(trace), all_trace_bytes_match=True))
        for fault in ['duplicate', 'observer'] + (['scope'] if context else []) + (['store', 'resample'] if writable else []):
            bad = work / ('wrong-' + fault)
            pipeline(bad, fault)
            obj = codegen(bad, '3')
            factory = bad / 'factory.o'
            run('compile-' + fault + '-factory', compiler + ['-O3', '-std=c++17', *flags,
                '-c', bad / 'model.cpp', '-o', factory])
            command = link.copy()
            command[command.index(objects[str(folder / 'model.cpp')])] = str(factory)
            command[command.index(str(folder / 'model-O0.o'))] = str(obj)
            command[command.index('-o') + 1] = str(bad / 'test')
            run('link-' + fault, command)
            trace = bad / 'trace.bin'
            text = run('reject-' + fault, [bad / 'test', trace], success=fault in ('observer', 'resample'))
            if fault == 'store':
                assert 'dependent initializer ran before prior declaration assignment' in text
            elif fault == 'scope':
                assert 'context scope mismatch' in text
            elif fault == 'duplicate':
                assert 'declaration seed repeated or wrong startup order' in text
            else:
                assert trace.read_bytes() != expected, 'time-zero observer fault escaped full-byte oracle'
            report['faults'].append(dict(name=fault, detected=True))
        assert all(sha(Path(path)) == value for path, value in hashes.items())
        report.update(status='passed', generated_root_fields_checked=15 if writable else 13,
                      once_only_startup_and_ordinary_callbacks_checked=True,
                      unused_initializer_effects_and_chandle_ABI_checked=True,
                      independent_complete_trace_oracle=True, contextual_startup_checked=context,
                      writable_startup_checked=writable)
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
    parser.add_argument('--context', action='store_true')
    parser.add_argument('--writable', action='store_true')
    parser.add_argument('--build', type=Path)
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/compiler')
    args = parser.parse_args()
    sdk = args.circt_bin.resolve().parent
    build = args.build or Path(__file__).resolve().parents[1] / 'generated-src' / (
        'native-writable-declaration-dpi-test' if args.writable else (
        'native-context-declaration-dpi-test' if args.context else 'native-declaration-dpi-test'))
    check(sdk, (args.frontend or sdk / 'bin/circt-verilog').resolve(), build.resolve(),
          args.cache.resolve(), os.environ.get('CXX', 'c++'), args.context or args.writable, args.writable)
    print('Native DPI declarations passed: O0/O3 startup ordering, shared dependencies, '
          'full output traces, clock/reset/held evaluations and model recreation')
