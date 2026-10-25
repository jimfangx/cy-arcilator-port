"""Compile and execute shared package DPI initialization without Verilator."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import resource
import shlex
import signal
import struct
import subprocess
import sys


def check(sdk, frontend, work, cache, cxx):
    sim = Path(__file__).resolve().parents[1]
    fixture = sim / 'tests/native_global_declaration_dpi'
    work.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
    hashes = {str(fixture.with_suffix('.' + ext)): sha(fixture.with_suffix('.' + ext))
              for ext in ['sv', 'mlir', 'cpp']}
    report = dict(status='running', steps=[], models=[], faults=[],
                  original_sources_sha256=hashes, frontend=str(frontend), frontend_sha256=sha(frontend))
    output = work / 'report.json'
    py, tool = sys.executable, sdk / 'bin'
    compiler = shlex.split(cxx)

    def save():
        output.write_text(json.dumps(report, indent=2) + '\n')

    def limits():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3, 4 * 1024**3))

    def run(name, command, success=True):
        command = list(map(str, command))
        log = work / (name + '.log')
        row = dict(name=name, command=command, log=str(log))
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
        source = folder / 'globals.sv'
        body = fixture.with_suffix('.sv').read_text()
        if fault == 'observer':
            body = body.replace('value ^ local_snapshot;', "value ^ local_snapshot ^ 64'd1;")
        source.write_text(body)
        filelist = folder / 'blackboxes.f'
        filelist.write_text(shlex.quote(str(source)) + '\n')
        tag = folder.name
        run(tag + '-native-HW', [py, sim / 'native_hw.py', '--input', fixture.with_suffix('.mlir'),
            '--output', folder / 'imported.mlir', '--filelist', filelist, '--work', folder / 'rtl',
            '--circt-verilog', frontend, '--circt-opt', tool / 'circt-opt', '--cxx', cxx, '--jobs=1'])
        assert len(json.loads((folder / 'rtl.json').read_text())) == 2
        run(tag + '-partition', [py, sim / 'partition.py', '--input', folder / 'imported.mlir',
            '--output', folder, '--top', 'GlobalDeclarationDPIProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        run(tag + '-outline', [py, sim / 'compiler/partition.py', '--input', folder / 'partitioned.mlir',
            '--output', folder / 'outlined.mlirbc', '--circt-opt', tool / 'circt-opt',
            '--cache', cache, '--cxx', cxx, '--max-ops', '16', '--emit-bytecode', '--native-feedback'])
        run(tag + '-canonicalize', [tool / 'circt-opt', folder / 'outlined.mlirbc',
            '--symbol-dce', '--canonicalize', '-o', folder / 'model.mlir'])
        model = folder / 'model.mlir'
        if fault == 'duplicate':
            text = model.read_text()
            assert 'chipyard_arc_signal_claim_startup' in text
            model.write_text(text.replace('chipyard_arc_signal_claim_startup',
                                           'chipyard_arc_signal_claim_startup_fault'))
        if fault in ('store', 'tail'):
            active, constants, changed, lines = False, {}, 0, []
            for line in model.read_text().splitlines(True):
                if re.search(r'\bseq\.initial\(', line):
                    active, constants = True, {}
                if active:
                    const = re.match(r'\s*(%[^ ]+) = hw.constant (-?\d+) : i64', line)
                    if const:
                        constants[const[1]] = int(const[2])
                    call = re.search(r'sim\.func\.dpi\.call @[^ (]*chipyard_arc_signal_write[^ (]*\(([^)]*)\)', line)
                    if call and (fault == 'store' or constants.get(call[1].split(', ')[4]) == 1):
                        changed += 1
                        continue
                    if line.lstrip().startswith('} {chipyard.declaration_startup}'):
                        active = False
                lines.append(line)
            assert changed >= 2, 'startup write fault did not reach both specializations'
            model.write_text(''.join(lines))
        run(tag + '-model', [py, sim / 'compiler/model.py', '--input', model,
            '--output', folder / 'model.ll', '--state-file', folder / 'model.json',
            '--arcilator', tool / 'arcilator', '--circt-opt', tool / 'circt-opt',
            '--mlir-translate', tool / 'mlir-translate', '--cache', cache, '--cxx', cxx,
            '--max-ops', '16', '--partition', folder / 'partition.json', '--', '--split-funcs-threshold=8192'])
        states = {x['name']: x for x in json.loads((folder / 'model.json').read_text())[0]['states']}
        fields = dict(clock=1, reset=1, write_a=1, write_b=1, data=32)
        for prefix in ['first', 'second', 'third']:
            fields.update({prefix + suffix: bits for suffix, bits in
                [('', 64), ('_derived', 64), ('_local', 64), ('_observed', 64),
                 ('_metric', 64), ('_wide', 129), ('_small', 32)]})
        assert all(states[name]['numBits'] == bits for name, bits in fields.items())
        run(tag + '-LLVM', [py, sim / 'compiler/llvm.py', folder / 'model.ll', '--llvm-opt', tool / 'opt',
            '--cache', cache, '--cxx', cxx])
        partition = folder / 'partition.json'
        if fault in ('sharing', 'contract'):
            data = json.loads(partition.read_text())
            changed = False
            for instance in data['instances']:
                if instance['module'] != 'ArcSignalMemory' or changed:
                    continue
                if not any(p.startswith('KEY:') and p.endswith('"global_declaration_pkg::value"') for p in instance['parameters']):
                    continue
                for index, param in enumerate(instance['parameters']):
                    if param.startswith('KEY:' if fault == 'sharing' else 'STARTUP:'):
                        instance['parameters'][index] = param.split(' = ', 1)[0] + ' = "fault-shared-global"'
                        changed = True
                        break
            assert changed, 'factory fault did not reach a shared dynamic allocation'
            partition = folder / 'fault-partition.json'
            partition.write_text(json.dumps(data, indent=2) + '\n')
        run(tag + '-factory', [py, sim / 'native.py', folder / 'model.json', partition,
            folder / 'model.cpp', '--filelist', filelist])
        filelist.write_text(filelist.read_text() + str(folder / 'model.cpp') + '\n')
        return filelist

    def codegen(folder, level):
        obj = folder / ('model-O' + level + '.o')
        run(folder.name + '-codegen-O' + level, [py, sim / 'compiler/codegen.py', '--input', folder / 'model.ll',
            '--output', obj, '--llvm-opt', tool / 'opt', '--llc', tool / 'llc', '--cache', cache,
            '--cxx', cxx, '--opt-level=' + level, '--llc-opt-level=' + level, '--jobs=1'])
        return obj

    def oracle():
        seed = value = 0xfedcba9876543201
        derived = seed ^ 0x8100010000000077
        wide = (1 << 128) | (seed << 64) | derived
        trace, previous = bytearray(), False
        for sample in range(24):
            clock, reset, write_a = bool(sample & 1), 5 <= sample < 10, sample % 4 < 2
            data = (0xf1234567 + sample * 0x10203) & 0xffffffff
            if clock and not previous and not reset:
                value = (value + data + (17 if write_a else 29)) & ((1 << 64) - 1)
                wide = ((wide << 1) ^ data) & ((1 << 129) - 1)
            row = bytearray()
            for tag in [17, 29, 17]:
                local = derived ^ (tag * 0x10203)
                row += struct.pack('<QQQQd', value, derived, local, seed ^ local,
                                   -8.5)
                row += wide.to_bytes(17, 'little')
                row += struct.pack('<f', float.fromhex('-0x1.23456p-10'))
            trace += row * 2
            previous = clock
        return bytes(trace * 2)

    flags = ['-O3', '-std=c++17', '-DARC_NATIVE_NO_MAIN', '-I' + str(sim),
             '-I' + str(sim / 'native/include'), '-I' + str(sdk / 'include')]
    libraries = ['-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport',
                 '-lLLVMDemangle', '-lz', '-lpthread', '-ldl']
    try:
        if frontend.name == 'circt-verilog':
            text = run('select-patched-frontend', [py, sim / 'compiler/verilog/build.py',
                '--circt-verilog', frontend, '--cache', cache / 'verilog', '--jobs=1'])
            frontend = Path(text.splitlines()[-1]).resolve()
            report.update(frontend=str(frontend), frontend_sha256=sha(frontend))
            save()
        sys.path.insert(0, str(sim))
        from compiler.partition import plugin
        state = plugin(tool / 'circt-opt', cxx, cache, sim / 'compiler/SignalState.cpp')
        memory = plugin(tool / 'circt-opt', cxx, cache, sim / 'compiler/SignalMemory.cpp')
        abi = ' attributes {chipyard.dpi_decl = {context = false, task = false}}'
        body = '%tag = hw.constant 43 : i64\n    %v = sim.func.dpi.call @seed(%tag) : (i64) -> i64'
        def global_ir(initializer, attributes=abi, second=''):
            return ('module {\n  func.func private @effect() -> i64\n'
                '  sim.func.dpi private @seed(in %tag: i64, return return: i64)' + attributes + '\n'
                '  llhd.global_signal @shared : i64 init {\n    ' + initializer + '\n    llhd.yield %v : i64\n  }\n' + second +
                '  hw.module @GlobalGuard(out q: i64) {\n'
                '    %ref = llhd.get_global_signal @shared : <i64>\n'
                '    %q = llhd.prb %ref : i64\n    hw.output %q : i64\n  }\n}\n')
        cycle_second = ('  llhd.global_signal @later : i64 init {\n'
            '    %r = llhd.get_global_signal @shared : <i64>\n'
            '    %v = llhd.prb %r : i64\n    llhd.yield %v : i64\n  }\n')
        later_seed = ('  llhd.global_signal @later : i64 init {\n    ' + body + '\n    llhd.yield %v : i64\n  }\n')
        cases = [
            ('closed', global_ir(body), None),
            ('context', global_ir(body, abi.replace('context = false', 'context = true')), 'requires a bound context-free DPI function'),
            ('task', global_ir(body, abi.replace('task = false', 'task = true')), 'requires a bound context-free DPI function'),
            ('unknown-ABI', global_ir(body, ''), 'requires a bound context-free DPI function'),
            ('enabled', global_ir(body.replace('%v =', '%false = hw.constant false\n    %v =').replace('@seed(%tag) :', '@seed(%tag) enable %false :')), 'requires a bound context-free DPI function'),
            ('effect', global_ir('%v = func.call @effect() : () -> i64'), 'unsupported effect in native global startup'),
            ('unused-effect', global_ir('%unused = func.call @effect() : () -> i64\n    %v = hw.constant 0 : i64'), 'unsupported effect in native global startup'),
            ('cycle', global_ir('%r = llhd.get_global_signal @later : <i64>\n    %v = llhd.prb %r : i64', second=cycle_second), 'cyclic native global startup dependencies'),
            ('effect-order', global_ir('%r = llhd.get_global_signal @later : <i64>\n    %tag = llhd.prb %r : i64\n    %v = sim.func.dpi.call @seed(%tag) : (i64) -> i64', second=later_seed), 'cannot reorder initializer callbacks'),
        ]
        report['initializer_guards'] = []
        for name, text, diagnostic in cases:
            source = work / ('guard-' + name + '.mlir')
            source.write_text(text)
            result = run('guard-' + name, [tool / 'circt-opt', source,
                '--load-pass-plugin=' + str(state), '--load-pass-plugin=' + str(memory),
                '--pass-pipeline=builtin.module(chipyard-snapshot-global-signal-initializers{defer-runtime=true},chipyard-native-signal-memory{global-initializers=true},chipyard-snapshot-global-signal-initializers)'], success=diagnostic is None)
            if diagnostic:
                assert diagnostic in result, result
            else:
                assert 'seq.initial(' in result and 'chipyard_arc_signal_claim_startup' in result
                assert 'llhd.global_signal' not in result
            report['initializer_guards'].append(dict(name=name, passed=True))
        folder = work / 'correct'
        filelist = pipeline(folder)
        expected = oracle()
        (work / 'expected.bin').write_bytes(expected)
        for level in ['0', '3']:
            obj = codegen(folder, level)
            exe = work / ('O' + level)
            if level == '0':
                text = run('native-CLI', [py, sim / 'native_build.py', '--filelist=' + str(filelist),
                    '--main=' + str(fixture.with_suffix('.cpp')), '--model=' + str(obj),
                    '--output=' + str(exe), '--build=' + str(work / 'objects'), '--cxx=' + cxx,
                    '--cxxflags=' + shlex.join(flags), '--ldflags=' + shlex.join(libraries), '--jobs=1'])
                link = shlex.split(text.splitlines()[-1])
            else:
                command = link.copy()
                command[command.index(str(folder / 'model-O0.o'))] = str(obj)
                command[command.index('-o') + 1] = str(exe)
                run('link-O3', command)
            trace = work / ('O' + level + '.bin')
            run('execute-O' + level, [exe, trace])
            assert trace.read_bytes() == expected, 'complete shared global declaration trace mismatch'
            report['models'].append(dict(optimization=level, recreations=2, samples=96,
                trace=str(trace), trace_bytes=len(expected), trace_sha256=sha(trace), all_trace_bytes_match=True))
        for fault in ['duplicate', 'store', 'tail', 'sharing', 'contract', 'observer']:
            bad = work / ('wrong-' + fault)
            pipeline(bad, fault)
            obj = codegen(bad, '3')
            run('compile-' + fault + '-factory', [*compiler, *flags, '-c', bad / 'model.cpp', '-o', bad / 'factory.o'])
            command = link.copy()
            command[command.index(str(folder / 'model-O0.o'))] = str(obj)
            factory_obj = next(x for x in command if x.startswith(str(work / 'objects')) and x.endswith('-model.o'))
            command[command.index(factory_obj)] = str(bad / 'factory.o')
            command[command.index('-o') + 1] = str(bad / 'test')
            run('link-' + fault, command)
            trace = bad / 'trace.bin'
            text = run('reject-' + fault, [bad / 'test', trace], success=fault in ('tail', 'observer'))
            if fault in ('tail', 'observer'):
                assert trace.read_bytes() != expected, 'corrupt startup/output data escaped complete trace oracle'
            else:
                diagnostic = {'duplicate': 'global initializer repeated', 'sharing': 'global initializer repeated',
                              'store': 'global dependent initializer saw an uninitialized value',
                              'contract': 'conflicting native global signal layout'}[fault]
                assert diagnostic in text, text
            report['faults'].append(dict(name=fault, detected=True))
        assert all(sha(path) == value for path, value in hashes.items())
        report.update(status='passed', generated_root_fields_checked=26, imported_RTL_specializations=2,
            physical_instances=3, once_only_shared_global_initialization_checked=True,
            module_startup_after_global_startup_checked=True, original_global_unused_effects_checked=True,
            integral_real_shortreal_and_129bit_storage_checked=True, independent_complete_trace_oracle=True)
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
    parser.add_argument('--build', type=Path)
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1] / 'generated-src/compiler')
    args = parser.parse_args()
    sdk = args.circt_bin.resolve().parent
    build = args.build or Path(__file__).resolve().parents[1] / 'generated-src/native-global-declaration-dpi-test'
    check(sdk, (args.frontend or sdk / 'bin/circt-verilog').resolve(), build.resolve(),
          args.cache.resolve(), os.environ.get('CXX', 'c++'))
    print('Native global DPI initialization passed: O0/O3, complete traces and six semantic fault controls')
