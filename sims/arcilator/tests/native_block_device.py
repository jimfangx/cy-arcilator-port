"""Check compiled native block-device bindings with the original file driver.

Uses a 2 KiB temporary image and the installed FESVR context implementation;
no firmware or Chipyard hardware configuration is built.
"""
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


def check(sdk, riscv, work, cache, cxx):
    sim = Path(__file__).resolve().parents[1]
    for ancestor in sim.parents:
        drivers = ancestor / 'generators/testchipip/src/main/resources/testchipip/csrc'
        if (drivers / 'SimBlockDevice.cc').is_file():
            break
    else:
        raise FileNotFoundError('cannot locate original block-device drivers')
    if riscv is None:
        candidates = ([Path(os.environ['RISCV'])] if os.environ.get('RISCV') else [])
        candidates += [p / '.conda-env/riscv-tools' for p in sim.parents]
        riscv = next((p for p in candidates if (p / 'include/fesvr/context.h').is_file()
                      and (p / 'lib').is_dir()), None)
    if riscv is None or not (riscv / 'include/fesvr/context.h').is_file():
        raise FileNotFoundError('set RISCV or --riscv to the matching FESVR installation')
    riscv = riscv.resolve()
    fesvr_library = next((p for p in (riscv / 'lib/libfesvr.so',
                                      riscv / 'lib/libfesvr.a') if p.is_file()), None)
    if fesvr_library is None:
        raise FileNotFoundError('matching FESVR library is missing')
    work.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    originals = [drivers / 'SimBlockDevice.cc', drivers / 'blkdev.cc',
                 drivers / 'blkdev.h', drivers.parent / 'vsrc/SimBlockDevice.v']
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p): sha(p) for p in originals}
    report = {'status': 'running', 'steps': [], 'models': [], 'faults': [],
              'original_sources_sha256': hashes, 'fesvr_prefix': str(riscv)}
    fesvr_hashes = {str(p.resolve()): sha(p) for p in
                   [riscv / 'include/fesvr/context.h', fesvr_library]}
    report['fesvr_inputs_sha256'] = fesvr_hashes
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

    mask = (1 << 64) - 1
    initial = b''.join(struct.pack('<Q', (0x8123456700000000 ^
                     (i * 0x0101010101010101)) & mask) for i in range(256))
    expected = bytearray(initial)
    for tag, sector in [(1, 1), (2, 2)]:
        words = b''.join(struct.pack('<Q', ((0xfedcba9800000000 if tag == 1 else
            0x8765432100000000) ^ (i * 0x0010203040506071)) & mask) for i in range(64))
        expected[sector * 512:(sector + 1) * 512] = words
    try:
        run('partition', [py, sim / 'partition.py', '--input',
            sim / 'tests/native_block_device.mlir', '--output', work,
            '--top', 'BlockDeviceModelProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        part = json.loads((work / 'partition.json').read_text())
        assert len(part['instances']) == 1
        instance = part['instances'][0]
        assert instance['name'] == 'disk' and instance['module'] == 'SimBlockDevice'
        ports = {p['name']: p for p in instance['ports']}
        assert len(ports) == 18
        for name, bits in [('bdev_req_bits_offset', 32), ('bdev_req_bits_len', 32),
                           ('bdev_data_bits_data', 64), ('bdev_resp_bits_data', 64),
                           ('bdev_req_bits_tag', 2), ('bdev_data_bits_tag', 2),
                           ('bdev_resp_bits_tag', 2)]:
            assert ports[name]['bits'] == bits
        run('outline', [py, sim / 'compiler/partition.py', '--input',
            work / 'partitioned.mlir', '--output', work / 'outlined.mlirbc',
            '--circt-opt', tool / 'circt-opt', '--cache', cache, '--cxx', cxx,
            '--max-ops', '16', '--emit-bytecode', '--native-feedback'])
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
        for name, port in ports.items():
            assert states[name]['numBits'] == port['bits']
        run('LLVM', [py, sim / 'compiler/llvm.py', work / 'model.ll',
            '--llvm-opt', tool / 'opt', '--cache', cache, '--cxx', cxx])
        (work / 'originals.f').write_text(''.join(shlex.quote(str(p)) + '\n'
            for p in [originals[0], originals[1], originals[3]]))
        run('native-factory', [py, sim / 'native.py', work / 'model.json',
            work / 'partition.json', work / 'model.cpp',
            '--filelist=' + str(work / 'originals.f')])
        (work / 'native.f').write_text((work / 'originals.f').read_text() +
                                       shlex.quote(str(work / 'model.cpp')) + '\n')
        flags = ['-DARC_NATIVE_NO_MAIN', '-I' + str(sim),
                 '-I' + str(sim / 'native/include'), '-I' + str(sdk / 'include'),
                 '-I' + str(riscv / 'include')]
        libraries = ['-Wl,--wrap=block_device_init', '-Wl,--wrap=block_device_tick',
            '-L' + str(riscv / 'lib'), '-Wl,-rpath,' + str(riscv / 'lib'), '-lfesvr',
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
                    '--main=' + str(sim / 'tests/native_block_device.cpp'),
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
            disk = work / ('O' + level + ' disk image.raw')
            disk.write_bytes(initial)
            text = run('execute-O' + level, [exe, disk])
            assert text.startswith('PASS original block device:')
            assert disk.read_bytes() == expected, 'complete disk image mismatch'
            text = run('disabled-O' + level, [exe, 'disabled'])
            assert text.startswith('PASS original block device disabled:')
            report['models'].append({'optimization': level, 'disk': str(disk),
                'complete_image_bytes': len(expected), 'final_image_sha256': sha(disk),
                'disabled_original_driver_passed': True})
        original = (sim / 'native/peripherals.cpp').read_text()
        for name, old, new, diagnostic in [
            ('truncate-data', 'get("bdev_data_bits_data")',
             'uint32_t(get("bdev_data_bits_data"))', '64-bit data ABI changed'),
            ('swap-tags', 'get("bdev_data_bits_tag")',
             'get("bdev_req_bits_tag")', 'tag ABI changed'),
            ('consume-stalled', 'get("bdev_resp_ready")',
             '1', 'response-ready ABI changed')]:
            assert original.count(old) == 1
            source = work / (name + '.cpp')
            source.write_text(original.replace(old, new))
            obj = work / (name + '.o')
            run('compile-' + name, compiler + ['-O3', '-std=c++17', *flags,
                '-I' + str(sim / 'native'), '-c', source, '-o', obj])
            command = link.copy()
            command[command.index(objects[str(sim / 'native/peripherals.cpp')])] = str(obj)
            command[command.index(str(work / 'model-O0.o'))] = str(work / 'model-O3.o')
            command[command.index('-o') + 1] = str(work / name)
            run('link-' + name, command)
            disk = work / (name + ' disk.raw')
            disk.write_bytes(initial)
            text = run('reject-' + name, [work / name, disk], success=False)
            assert diagnostic in text
            report['faults'].append({'name': name, 'detected': True})
        assert all(sha(Path(p)) == h for p, h in hashes.items())
        assert all(sha(Path(p)) == h for p, h in fesvr_hashes.items())
        report.update(status='passed', original_sources_unchanged=True,
                      all_generated_port_widths_checked=True,
                      parameter_and_original_context_verified=True)
        save()
        return report
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        save()
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--riscv', type=Path)
    parser.add_argument('--build', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/native-block-device-test')
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/compiler')
    args = parser.parse_args()
    check(args.circt_bin.resolve().parent, args.riscv, args.build.resolve(),
          args.cache.resolve(), os.environ.get('CXX', 'c++'))
    print('Compiled native block-device bindings passed: original driver/FESVR, '
          'O0/O3, complete disk images, disabled mode and three rejected ABI faults')
