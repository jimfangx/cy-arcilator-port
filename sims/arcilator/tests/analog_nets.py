#!/usr/bin/env python3
"""Check native analog resolution and generated bindings with the real SDK."""
import argparse
import json
import os
from pathlib import Path
import resource
import shlex
import signal
import subprocess
import sys
import tempfile


def partition_fixture(generator, sdk, fixture, bits, run, literal=-1, parameter_bits=32):
    """Verify real HW syntax and preserve its analog boundary and parameters."""
    source = fixture / 'input.mlir'
    source.write_text(
        'module {\n'
        '  hw.module.extern @AnalogConst<CONST: i' + str(parameter_bits) + ', WIDTH: i32>'
        '(in %io: !hw.inout<i' + str(bits) + '>)\n'
        '  hw.module @WideAnalogUnit(in %clock: i1, in %pad: !hw.inout<i' +
        str(bits) + '>) {\n'
        '    hw.instance "constant" @AnalogConst<CONST: i' + str(parameter_bits) +
        ' = ' + str(literal) + ', WIDTH: i32 = ' +
        str(bits) + '>(io: %pad: !hw.inout<i' + str(bits) + '>) -> ()\n'
        '    hw.output\n  }\n}\n')
    verified = fixture / 'verified.mlir'
    run('verify-HW-' + str(bits),
        [sdk / 'bin/circt-opt', source, '-o', verified])
    sys.path.insert(0, str(generator))
    from partition import partition_flat
    partition_flat(verified, fixture, 'WideAnalogUnit',
                   {'AnalogConst': 'AnalogConst'})
    part = json.loads((fixture / 'partition.json').read_text())
    if part['analog_nets'] != [dict(wire='pad', bits=bits)]:
        raise RuntimeError('partition lost analog width')
    instance, = part['instances']
    if (instance['module'] != 'AnalogConst' or instance['ports'] != [
            dict(name='io', direction='inout', wire='pad', bits=bits)] or
            ('CONST: i' + str(parameter_bits) + ' = ' + str(literal)) not in instance['parameters'] or
            ('WIDTH: i32 = ' + str(bits)) not in instance['parameters']):
        raise RuntimeError('partition lost analog ports or signed parameters')
    return part


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path)
    parser.add_argument('--generator-root', type=Path)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    sim = here.parent
    generator = (args.generator_root or sim).resolve()
    sdk = args.circt_bin.resolve().parent
    temporary = None
    if args.work is None:
        temporary = tempfile.TemporaryDirectory(prefix='native-analog-test-')
        work = Path(temporary.name)
    else:
        work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'steps': []}
    cxx = shlex.split(os.environ.get('CXX', 'c++'))
    includes = ['-I' + str(sim), '-I' + str(sim / 'native'),
                '-I' + str(sim / 'native/include'), '-I' + str(sdk / 'include'),
                '-I' + str(work)]
    libraries = ['-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport',
                 '-lLLVMDemangle', '-lz', '-lpthread', '-ldl']

    def limits():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3, 4 * 1024**3))

    def run(name, command):
        command = list(map(str, command))
        log = work / (name + '.log')
        row = dict(name=name, command=command, log=str(log))
        report['steps'].append(row)
        with log.open('w') as output:
            process = subprocess.Popen(command, cwd=work, stdout=output,
                                       stderr=subprocess.STDOUT,
                                       start_new_session=True, preexec_fn=limits)
            try:
                code = process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise RuntimeError(name + ' exceeded its 180-second deadline')
        row['returncode'] = code
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        if code:
            raise RuntimeError(name + ' failed:\n' + log.read_text()[-6000:])
        print('PASS: ' + name, flush=True)

    try:
        # Python arbitrary precision integers provide independent constant
        # expectations, including signed 32-bit Chisel CONST values.
        literals = ['0', '1', '-1', '-2147483648', '2147483647', '+42', '0777',
                    '0x80', '-0X80000000',
                    '1234567890123456789012345678901234567890',
                    '-1234567890123456789012345678901234567890',
                    '0xffffffffffffffffffffffffffffffffffffffffffffffff',
                    '-0x1000000000000000000000000000000000000000000000000']
        lines = ['struct AnalogCase { unsigned bits; const char *literal;',
                 'std::vector<uint8_t> expected; };',
                 'static const AnalogCase analog_cases[] = {']
        for bits in [7, 64, 65, 73, 127, 128, 129, 257, 1027]:
            for literal in literals:
                digits = literal.lstrip('+-')
                base = 16 if digits.lower().startswith('0x') else (
                    8 if digits.startswith('0') else 10)
                value = int(literal, base) % (1 << bits)
                expected = value.to_bytes((bits + 7) // 8, 'little')
                lines.append('{' + str(bits) + ', ' + json.dumps(literal) +
                             ', {' + ','.join(map(str, expected)) + '}},')
        lines.append('};')
        (work / 'analog_cases.h').write_text('\n'.join(lines) + '\n')
        objects = {}
        for source in sorted((sim / 'native').glob('*.cpp')):
            obj = work / (source.stem + '-O3.o')
            flags = ['-fno-rtti'] if source.name == 'files.cpp' else []
            run('compile-' + source.stem, cxx + ['-O3', '-std=c++17'] +
                includes + flags + ['-c', source, '-o', obj])
            objects[source.stem] = obj
        for level in ['3', '0']:
            selected = dict(objects)
            if level == '0':
                for name in ['analog', 'analog_nets', 'runtime']:
                    obj = work / (name + '-O0.o')
                    run('compile-' + name + '-O0', cxx + ['-O0', '-std=c++17'] +
                        includes + ['-c', sim / 'native' / (name + '.cpp'), '-o', obj])
                    selected[name] = obj
            executable = work / ('analog-O' + level)
            run('link-O' + level, cxx + ['-O' + level, '-std=c++17'] +
                includes + [here / 'analog_nets.cpp', *selected.values(),
                            *libraries, '-o', executable])
            run('execute-O' + level, [executable])
        # Exercise the original generator and real runtime together; no new
        # hardware config or firmware is introduced by these unit factories.
        for bits, literal, parameter_bits in [(65, -1, 32), (73, -1, 32),
                (257, -1, 32), (7, (1 << 100) + 5, 128),
                (64, -((1 << 100) + 0x0123456789abcdef), 128)]:
            fixture = work / ('factory-' + str(bits))
            fixture.mkdir(exist_ok=True)
            layout = [{'name': 'ArcModel', 'numStateBytes': 64, 'states': [
                dict(name='clock', type='input', offset=0, numBits=1)]}]
            part = partition_fixture(generator, sdk, fixture, bits, run, literal, parameter_bits)
            (fixture / 'model.json').write_text(json.dumps(layout))
            (fixture / 'partition.json').write_text(json.dumps(part))
            factory = fixture / 'factory.cpp'
            run('generate-' + str(bits), [sys.executable, generator / 'native.py',
                fixture / 'model.json', fixture / 'partition.json', factory])
            executable = fixture / 'factory-test'
            run('factory-link-' + str(bits), cxx + ['-O3', '-std=c++17',
                '-DARC_NATIVE_NO_MAIN'] + includes + [factory,
                here / 'analog_factory.cpp', *objects.values(), *libraries,
                '-o', executable])
            wave = fixture / 'wave.vcd'
            expected = literal % (1 << bits)
            oracle = fixture / 'expected.bin'
            oracle.write_bytes(expected.to_bytes((bits + 7) // 8, 'little'))
            run('factory-execute-' + str(bits), [executable, '+vcdfile=' + str(wave),
                '+analog-expected-file=' + str(oracle)])
            body = wave.read_text()
            if ('$var wire ' + str(bits)) not in body or ('b' + format(expected, '0' + str(bits) + 'b') + ' ') not in body:
                raise RuntimeError('analog waveform lost width or value')
        # Reuse existing tests against these same newly compiled objects.
        for name, source, flags in [
            ('vpi', 'native_vpi_lifecycle.cpp',
             ['-Wl,--wrap=arcRuntimeOnInitialized', '-Wl,--wrap=arcRuntimeOnEval']),
            ('run-loop', 'native_run_loop.cpp', [])]:
            executable = work / name
            run(name + '-link', cxx + ['-O3', '-std=c++17'] + includes +
                [generator / 'tests' / source, *objects.values(), *flags,
                 *libraries, '-o', executable])
            command = [executable]
            if name == 'run-loop':
                waves = work / 'waves'
                waves.mkdir(exist_ok=True)
                command.append(waves)
            run(name + '-execute', command)
        sys.path.insert(0, str(generator / 'tests'))
        from run_loop import check
        check(waves)
        report.update(status='passed', constant_cases=len(literals) * 9,
                      narrow_constant_cases=len(literals) * 2, generated_factories=5,
                      generated_narrow_wide_parameter_factories=2)
    except BaseException as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        raise
    finally:
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        if temporary is not None:
            temporary.cleanup()


if __name__ == '__main__':
    main()
