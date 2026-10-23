"""Check native SimNetwork with original drivers and local switch loopback.

The original empty-device path opens no TAP interface. This standalone MLIR
fixture uses the real FESVR contexts, without firmware or a hardware config.
"""
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
import time


def check(sdk, riscv, work, cache, cxx):
    sim = Path(__file__).resolve().parents[1]
    for ancestor in sim.parents:
        root = ancestor / 'generators/icenet/src/main/resources'
        if (root / 'csrc/SimNetwork.cc').is_file():
            break
    else:
        raise FileNotFoundError('cannot locate original icenet drivers')
    if riscv is None:
        candidates = ([Path(os.environ['RISCV'])] if os.environ.get('RISCV') else [])
        candidates += [p / '.conda-env/riscv-tools' for p in sim.parents]
        riscv = next((p for p in candidates if (p / 'include/fesvr/context.h').is_file()), None)
    if riscv is None or not (riscv / 'include/fesvr/context.h').is_file():
        raise FileNotFoundError('set RISCV or --riscv to the matching FESVR installation')
    riscv = riscv.resolve()
    library = next((p for p in (riscv / 'lib/libfesvr.so', riscv / 'lib/libfesvr.a')
                    if p.is_file()), None)
    if library is None:
        raise FileNotFoundError('matching FESVR library is missing')
    originals = [root / p for p in ('csrc/SimNetwork.cc', 'csrc/device.cc',
        'csrc/switch.cc', 'csrc/device.h', 'csrc/switch.h', 'csrc/packet.h', 'vsrc/SimNetwork.v')]
    work.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    hashes = {str(p): sha(p) for p in originals}
    fesvr_hashes = {str(p.resolve()): sha(p) for p in [riscv / 'include/fesvr/context.h', library]}
    report = {'status': 'running', 'steps': [], 'models': [], 'faults': [],
              'original_sources_sha256': hashes, 'fesvr_inputs_sha256': fesvr_hashes}
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

    def trace_bytes(mac):
        # Construct bytes independently of the C++ test's shifted-word headers.
        mask = (1 << 64) - 1
        result = bytearray()
        for count, pad, etype in [(8, b'\xa7\x39', b'\x88\xb5'),
                                  (11, b'\x7c\xa9', b'\x08\x00')]:
            packet = bytearray(pad + mac.to_bytes(6, 'little') * 2 + etype)
            for i in range(2, count):
                word = (0xfedcba9800000000 ^ (i * 0x0102040810204081)) if count == 8 else (
                    0x8123456700000000 ^ ((i + 17) * 0x0011223344556677))
                packet += struct.pack('<Q', word & mask)
            for i in range(count):
                result += packet[i * 8:(i + 1) * 8] + bytes([i + 1 == count])
        return bytes(result)

    try:
        run('partition', [py, sim / 'partition.py', '--input', sim / 'tests/native_network.mlir',
            '--output', work, '--top', 'NetworkModelProbe', '--circt-opt', tool / 'circt-opt',
            '--native-sparse-memories', '--native-combinational-models'])
        part = json.loads((work / 'partition.json').read_text())
        assert len(part['instances']) == 1
        instance = part['instances'][0]
        assert instance['name'] == 'nic' and instance['module'] == 'SimNetwork'
        ports = {p['name']: p for p in instance['ports']}
        assert len(ports) == 17 and ports['net_macAddr']['bits'] == 48
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
        layout, = json.loads((work / 'model.json').read_text())
        states = {p['name']: p for p in layout['states']}
        for name, port in ports.items():
            assert states[name]['numBits'] == port['bits']
        run('LLVM', [py, sim / 'compiler/llvm.py', work / 'model.ll', '--llvm-opt', tool / 'opt',
            '--cache', cache, '--cxx', cxx])
        (work / 'originals.f').write_text(''.join(shlex.quote(str(p)) + '\n'
            for p in [*originals[:3], originals[-1]]))
        run('native-factory', [py, sim / 'native.py', work / 'model.json', work / 'partition.json',
            work / 'model.cpp', '--filelist=' + str(work / 'originals.f')])
        (work / 'native.f').write_text((work / 'originals.f').read_text() +
                                       shlex.quote(str(work / 'model.cpp')) + '\n')
        flags = ['-DARC_NATIVE_NO_MAIN', '-I' + str(sim), '-I' + str(sim / 'native/include'),
                 '-I' + str(sdk / 'include'), '-I' + str(riscv / 'include')]
        libraries = ['-Wl,--wrap=network_init', '-Wl,--wrap=network_tick',
            '-L' + str(riscv / 'lib'), '-Wl,-rpath,' + str(riscv / 'lib'), '-lfesvr',
            '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle',
            '-lz', '-lpthread', '-ldl']
        for level in ('0', '3'):
            obj = work / ('model-O' + level + '.o')
            run('codegen-O' + level, [py, sim / 'compiler/codegen.py', '--input', work / 'model.ll',
                '--output', obj, '--llvm-opt', tool / 'opt', '--llc', tool / 'llc',
                '--cache', cache, '--cxx', cxx, '--opt-level=' + level,
                '--llc-opt-level=' + level, '--jobs=1'])
            exe = work / ('O' + level)
            if level == '0':
                text = run('native-CLI', [py, sim / 'native_build.py',
                    '--filelist=' + str(work / 'native.f'), '--main=' + str(sim / 'tests/native_network.cpp'),
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
            for mode in ('default', 'override'):
                trace = work / ('O' + level + '-' + mode + '.bin')
                text = run('execute-O' + level + '-' + mode, [exe, mode, trace])
                match = re.fullmatch(r'PASS original network loopback: mode=' + mode +
                    r' mac=([0-9a-f]{12}) packets=2 flits=19\n', text)
                assert match, text
                mac = int(match[1], 16)
                assert trace.read_bytes() == trace_bytes(mac), 'complete network data/last trace mismatch'
                report['models'].append({'optimization': level, 'mode': mode,
                    'observed_original_mac': mac, 'trace': str(trace), 'trace_bytes': trace.stat().st_size,
                    'trace_sha256': sha(trace), 'all_packet_words_and_last_bits_match': True})
        original = (sim / 'native/peripherals.cpp').read_text()
        start = original.index('class Network : public Binding')
        end = original.index('class BranchPredictor : public Binding', start)
        body = original[start:end]
        for name, old, new, diagnostic in [
            ('truncate-TX', 'get("net_out_bits_data")', 'uint32_t(get("net_out_bits_data"))',
             '64-bit TX/valid/last ABI changed'),
            ('reset-callback', 'if (get("reset"))', 'if (false)',
             'driver ran during reset or wrong edge'),
            ('corrupt-MAC', 'stage("net_macAddr", uint64_t(mac));',
             'stage("net_macAddr", uint64_t(mac) ^ 1);', '48-bit MAC mapping changed')]:
            assert body.count(old) == 1
            source = work / (name + '.cpp')
            source.write_text(original[:start] + body.replace(old, new) + original[end:])
            obj = work / (name + '.o')
            run('compile-' + name, compiler + ['-O3', '-std=c++17', *flags,
                '-I' + str(sim / 'native'), '-c', source, '-o', obj])
            command = link.copy()
            command[command.index(objects[str(sim / 'native/peripherals.cpp')])] = str(obj)
            command[command.index(str(work / 'model-O0.o'))] = str(work / 'model-O3.o')
            command[command.index('-o') + 1] = str(work / name)
            run('link-' + name, command)
            text = run('reject-' + name, [work / name, 'default', work / (name + '.bin')], success=False)
            assert diagnostic in text
            report['faults'].append({'name': name, 'detected': True})
        assert all(sha(Path(p)) == h for p, h in {**hashes, **fesvr_hashes}.items())
        report.update(status='passed', original_sources_unchanged=True,
                      all_generated_widths_checked=True, external_device_name='')
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
                        / 'generated-src/native-network-test')
    parser.add_argument('--cache', type=Path, default=Path(__file__).resolve().parents[1]
                        / 'generated-src/compiler')
    args = parser.parse_args()
    check(args.circt_bin.resolve().parent, args.riscv, args.build.resolve(),
          args.cache.resolve(), os.environ.get('CXX', 'c++'))
    print('Compiled native SimNetwork passed: original local switch/FESVR, O0/O3, '
          'defaults/overrides, complete packet traces and three rejected faults')
