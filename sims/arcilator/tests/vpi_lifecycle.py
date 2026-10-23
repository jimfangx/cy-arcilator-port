#!/usr/bin/env python3
"""Check the native VPI C ABI and real SDK Runtime without a hardware build."""
import argparse
import json
import os
from pathlib import Path
import resource
import shlex
import shutil
import signal
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin',
                        type=Path,
                        default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--work', type=Path)
    args = parser.parse_args()
    if args.circt_bin is None:
        arcilator = shutil.which('arcilator')
        if arcilator is None:
            parser.error(
                'set CIRCT_BIN or --circt-bin to the matching SDK bin directory'
            )
        args.circt_bin = Path(arcilator).resolve().parent
    sdk = args.circt_bin.resolve().parent
    here = Path(__file__).resolve().parent
    sim = here.parent
    cxx = shlex.split(os.environ.get('CXX', 'c++'))
    cc = shlex.split(os.environ.get('CC', 'cc'))
    includes = [
        '-I' + str(sim), '-I' + str(sim / 'native'),
        '-I' + str(sim / 'native/include'), '-I' + str(sdk / 'include')
    ]
    temporary = None
    if args.work is None:
        temporary = tempfile.TemporaryDirectory(prefix='native-vpi-test-')
        work = Path(temporary.name)
    else:
        work = args.work.resolve()
        work.mkdir(parents=True, exist_ok=True)
    report = {'status': 'running', 'steps': []}

    def limits():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (4 * 1024**3, 4 * 1024**3))

    def run(name, command):
        command = list(map(str, command))
        log = work / (name + '.log')
        row = {'name': name, 'command': command, 'log': str(log)}
        report['steps'].append(row)
        with log.open('w') as output:
            process = subprocess.Popen(command,
                                       cwd=work,
                                       stdin=subprocess.DEVNULL,
                                       stdout=output,
                                       stderr=subprocess.STDOUT,
                                       start_new_session=True,
                                       preexec_fn=limits)
            try:
                code = process.wait(timeout=180)
            except subprocess.TimeoutExpired:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise RuntimeError(name + ' exceeded its 180-second deadline')
        row['returncode'] = code
        if code:
            raise RuntimeError(name + ' failed:\n' + log.read_text()[-6000:])

    try:
        # A C client and a C++ client share the public header and real registry.
        for level in ('0', '3'):
            c_obj = work / ('client-O' + level + '.o')
            run(
                'C-client-O' + level, cc + ['-O' + level, '-std=c11'] +
                includes + ['-c', here / 'vpi_lifecycle_c.c', '-o', c_obj])
            executable = work / ('core-O' + level)
            run(
                'core-link-O' + level,
                cxx + ['-O' + level, '-std=c++17'] + includes + [
                    here / 'vpi_lifecycle_core.cpp', sim / 'native/compat.cpp',
                    sim / 'native/vpi_lifecycle.cpp', c_obj, '-lpthread', '-o',
                    executable
                ])
            run('core-execute-O' + level, [executable])

        # Compile every maintained runtime source, as in run_native.py. Weak
        # driver imports remain unused by these unit models; no RTL is replaced.
        objects = []
        for source in sorted((sim / 'native').glob('*.cpp')):
            obj = work / (source.stem + '.o')
            flags = ['-fno-rtti'] if source.name == 'files.cpp' else []
            run(
                'runtime-' + source.stem,
                cxx + ['-O2', '-std=c++17', '-DARC_NATIVE_NO_MAIN'] + flags +
                includes + ['-c', source, '-o', obj])
            objects.append(obj)
        libraries = [
            '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport',
            '-lLLVMDemangle', '-lz', '-lpthread', '-ldl'
        ]
        runtime_test = work / 'runtime-test'
        run(
            'runtime-link', cxx + ['-O2', '-std=c++17'] + includes + [
                here / 'native_vpi_lifecycle.cpp', *objects,
                '-Wl,--wrap=arcRuntimeOnInitialized',
                '-Wl,--wrap=arcRuntimeOnEval', *libraries, '-o', runtime_test
            ])
        run('runtime-execute', [runtime_test])
        # Link the original UART driver: its vpi_get_vlog_info query must work
        # for the embedding API's valid Runtime(0, nullptr, ...) constructor.
        for ancestor in sim.parents:
            drivers = ancestor / 'generators/testchipip/src/main/resources/testchipip/csrc'
            if (drivers / 'SimUART.cc').is_file():
                break
        else:
            raise FileNotFoundError('cannot locate original SimUART.cc/uart.cc drivers')
        uart_test = work / 'uart-empty-args'
        run('uart-empty-args-link', cxx + ['-O2', '-std=c++17'] + includes + [
            here / 'native_uart_empty_args.cpp', *objects,
            drivers / 'SimUART.cc', drivers / 'uart.cc', *libraries,
            '-o', uart_test])
        for mode in ('query', 'uart'):
            run('uart-empty-args-' + mode, [uart_test, mode])
        # Exercise unchanged clock/reset/control/waveform and recreation paths
        # with the same freshly compiled runtime, with VPI unused.
        run_loop = work / 'run-loop'
        run(
            'run-loop-link', cxx + ['-O2', '-std=c++17'] + includes + [
                here / 'native_run_loop.cpp', *objects, *libraries, '-o',
                run_loop
            ])
        waves = work / 'waves'
        waves.mkdir(exist_ok=True)
        run('run-loop-execute', [run_loop, waves])
        from run_loop import check
        check(waves)
        report['status'] = 'passed'
        print(
            'PASS: native VPI C/C++ ABI at O0/O3, empty argv with original UART, real SDK lifecycle and all18 run-loop waveforms',
            flush=True)
    except BaseException as error:
        report.update(status='failed', error=str(error))
        raise
    finally:
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        if temporary is not None:
            temporary.cleanup()


if __name__ == '__main__':
    main()
