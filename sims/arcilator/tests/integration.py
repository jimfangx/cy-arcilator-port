#!/usr/bin/env python3
"""Build selected Chipyard configs and exercise their actual peripheral drivers."""
import argparse
from contextlib import nullcontext
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import signal
import struct
import sys
import subprocess
import tempfile
import time
from jtag import exercise as exercise_jtag
from coverage import implementation_hashes
from cva6_software import linker_script as cva6_linker, check_mailboxes
from saturn_software import build as build_saturn_software

SIM = Path(__file__).resolve().parent.parent
ROOT = SIM.parent.parent
# These are regression selections, not a list of configs supported by the backend.
CASES = {
    'custom_io': ('CustomIOChipTopRocketConfig', 'hello', True,
                  ['SimDRAM', 'SimTSI', 'SimUART', 'ArcAsyncRegBank']),
    'custom_io_uart': ('CustomIOChipTopRocketConfig', 'uart', True,
                       ['SimDRAM', 'SimTSI', 'SimUART', 'ArcAsyncRegBank']),
    'custom_io_jtag': ('CustomIOChipTopRocketConfig', 'jtag_idle', True,
                       ['SimDRAM', 'SimTSI', 'SimJTAG', 'ArcAsyncRegBank']),
    'ara_rocket_4096': ('V4096Ara2LaneRocketConfig', 'ara', True,
                        ['AraBlackbox', 'SimDRAM', 'SimTSI']),
    'ara_rocket_8192': ('V8192Ara4LaneRocketConfig', 'ara', True,
                        ['AraBlackbox', 'SimDRAM', 'SimTSI']),
    'ara_shuttle_4096': ('V4096Ara2LaneShuttleConfig', 'ara', True,
                         ['AraBlackbox', 'SimDRAM', 'SimTSI']),
    'ara_shuttle_8192': ('V8192Ara4LaneShuttleConfig', 'ara', True,
                         ['AraBlackbox', 'SimDRAM', 'SimTSI']),
    'aes': ('AES256ECBRocketConfig', 'caliptra_aes', True,
            ['AesCipherCoreWrapper_AES256_ECB_NoMask', 'ArcSignalMemory', 'ArcAsyncRegBank', 'SimDRAM', 'SimTSI']),
    'cva6': ('CVA6Config', 'hello', True, ['SimDRAM', 'SimTSI', 'ArcSignalMemory']),
    'vexii': ('VexiiRiscvConfig', 'hello', True, ['SimDRAM', 'SimTSI', 'VexiiRiscvTilelink']),
    'saturn': ('REFV256D128RocketConfig', 'saturn:architecture', True,
               ['SimDRAM', 'SimTSI', 'ArcAsyncRegBank']),
    'saturn_masked': ('REFV256D128RocketConfig', 'saturn:vec-mixed_width_mask', True,
                      ['SimDRAM', 'SimTSI', 'ArcAsyncRegBank']),
    'saturn_transpose': ('REFV256D128RocketConfig', 'saturn:vec-transpose-load', True,
                         ['SimDRAM', 'SimTSI', 'ArcAsyncRegBank']),
    'saturn_sgemm': ('REFV256D128RocketConfig', 'saturn:vec-sgemm', True,
                     ['SimDRAM', 'SimTSI', 'ArcAsyncRegBank']),
    'memfuzz': ('RadianceFuzzerConfig', 'none', False, ['SimMemFuzzer', 'SimDRAM']),
    'ibex': ('IbexConfig', 'ibex_isa32', False, ['SimDRAM', 'SimTSI', 'ArcLatch']),
    'tiny': ('TinyRocketConfig', 'isa32', False, ['SimTSI']),
    'sodor1': ('Sodor1StageConfig', 'isa32', False, ['SimTSI']),
    'sodor2': ('Sodor2StageConfig', 'isa32', False, ['SimTSI']),
    'sodor3': ('Sodor3StageConfig', 'isa32', False, ['SimTSI']),
    'sodor3_single': ('Sodor3StageSinglePortConfig', 'isa32', False, ['SimTSI']),
    'sodor5': ('Sodor5StageConfig', 'isa32', False, ['SimTSI']),
    'sodor_ucode': ('SodorUCodeConfig', 'isa32', False, ['SimTSI']),
    'rocket': ('RocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'rv32': ('RV32RocketConfig', 'isa32', True, ['SimDRAM', 'SimTSI']),
    'dual': ('DualRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'quadchannel': ('QuadChannelRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'asyncreset': ('AsyncResetRocketConfig', 'hello', True, ['ArcAsyncRegBank', 'SimDRAM', 'SimTSI']),
    'boom': ('SmallBoomV3Config', 'hello', True, ['SimDRAM', 'SimTSI']),
    'swbpd': ('boom.v3.common.WithSWBPD_SmallBoomV3Config', 'hello', True,
              ['BranchPredictorHarness', 'BTBHarness', 'SimDRAM', 'SimTSI']),
    'spike': ('SpikeConfig', 'isa64', True, ['SpikeBlackBox', 'SimDRAM', 'SimTSI']),
    'shuttle': ('ShuttleCosimConfig', 'isa64', True,
                ['SpikeCosim', 'DebugROBPushTrace', 'DebugROBPushWb', 'DebugROBPopTrace', 'SimDRAM']),
    'shuttle_plain': ('ShuttleConfig', 'isa64', True, ['SimDRAM', 'SimTSI']),
    'shuttle_three_wide': ('Shuttle3WideConfig', 'isa64', True, ['SimDRAM', 'SimTSI']),
    'tracegen': ('TraceGenConfig', 'tracegen', False, ['SimDRAM']),
    'dmi': ('dmiRocketConfig', 'isa64', False, ['TestchipSimDTM', 'SimDRAM']),
    'multiclock': ('MulticlockRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'default_clock': ('DefaultClockingRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'single_clock': ('SingleClockBroadcastRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'pll_clock': ('ChipLikeClockingRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'chip_like': ('ChipLikeRocketConfig', 'hello', False, ['SimTSI']),
    'flat_chip_like': ('FlatChipTopChipLikeRocketConfig', 'hello', False, ['SimTSI']),
    'no_axi_fast': ('NoAXI4MemPortChipLikeRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'no_axi_serial': ('NoAXI4MemPortChipLikeRocketConfig', 'hello', False, ['SimDRAM', 'SimTSI']),
    'gcd': ('GCDAXI4BlackBoxRocketConfig', 'gcd', True, ['GCDMMIOBlackBox', 'SimDRAM']),
    'gcd_tl': ('GCDTLRocketConfig', 'gcd', True, ['SimDRAM', 'SimTSI']),
    'gcd_external_clock': ('GCDExternallyClockedRocketConfig', 'gcd', True, ['SimDRAM', 'SimTSI']),
    'init_zero': ('InitZeroRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    # Pure Chisel AXI memory receives the ELF through normal serial TSI loading.
    'sim_axi': ('SimAXIRocketConfig', 'hello', False, ['SimTSI']),
    'ring_noc': ('SbusRingNoCConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'mesh_noc': ('SbusMeshNoCConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'block': ('SimBlockDeviceRocketConfig', 'blkdev', True, ['SimBlockDevice', 'SimDRAM']),
    'block_model': ('BlockDeviceModelRocketConfig', 'blkdev', True, ['SimDRAM', 'SimTSI']),
    'gpio': ('GPIORocketConfig', 'gpio', True, ['AnalogConst', 'SimDRAM']),
    'i2c': ('I2CRocketConfig', 'hello', True, ['SimDRAM', 'SimTSI']),
    'spiflash': ('SmallSPIFlashRocketConfig', 'spiflashwrite', True, ['SimSPIFlashModel', 'SimDRAM']),
    'spiflashrom': ('LargeSPIFlashROMRocketConfig', 'spiflashread', True, ['SimSPIFlashModel', 'SimDRAM']),
    'uart': ('RocketConfig', 'uart', True, ['SimUART', 'SimDRAM']),
    'jtag': ('RocketConfig', 'jtag_idle', True, ['SimJTAG', 'SimDRAM', 'SimTSI']),
    'network': ('chipyard.harness.WithSimNetwork_LoopbackNICRocketConfig', 'network', True, ['SimNetwork', 'SimDRAM']),
    'cospike': ('dmiCospikeCheckpointingRocketConfig', 'isa64', True,
                ['TestchipSimDTM', 'SpikeCosim', 'DebugROBPushTrace', 'DebugROBPushWb', 'DebugROBPopTrace']),
    'icenet': ('IceNetUnitTestConfig', 'none', False, []),
    'noceval': ('EvalTestConfig00', 'none', False, ['TrafficEvalIngress', 'TrafficEvalEgress']),
    'tethered': ('VerilatorCITetheredChipLikeRocketConfig', 'hello', True,
                 ['SimDRAM', 'SimTSI', 'SimUART', 'SimJTAG', 'ClockSourceAtFreqMHz', 'ArcLatch']),
}
MAKE_OVERRIDES = {
    'icenet': {'SUB_PROJECT': 'icenet', 'MODEL_PACKAGE': 'chipyard.unittest'},
    'noceval': {'SUB_PROJECT': 'constellation', 'MODEL_PACKAGE': 'constellation.test', 'MODEL': 'EvalHarness'},
}
for case in ('ara_rocket_4096', 'ara_rocket_8192', 'ara_shuttle_4096', 'ara_shuttle_8192'):
    MAKE_OVERRIDES[case] = {
        'USE_ARA': '1',
        'ARC_RTL_FLAGS': '--Xslang=--relax-enum-conversions --Xslang=-Wno-multiple-always-assigns',
    }


def make_settings(values):
    """Keep build tuning separate from the case's hardware and runtime identity."""
    tools = {'CC', 'CXX', 'PYTHON', 'CIRCT_OPT', 'CIRCT_VERILOG', 'ARCILATOR',
             'MLIR_TRANSLATE', 'LLVM_OPT', 'LLC', 'LLVM_CONFIG'}
    settings = {}
    for value in values:
        name, separator, setting = value.partition('=')
        if not separator or not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
            raise ValueError('expected NAME=VALUE for --make-setting: ' + value)
        if name == 'ARC_RUNTIME' or not (name.startswith('ARC_') or name in tools):
            raise ValueError('--make-setting accepts ARC_* build options and compiler tools; '
                             'use the case and --runtime for hardware/runtime selection: ' + name)
        if name in settings:
            raise ValueError('duplicate --make-setting: ' + name)
        settings[name] = setting
    return settings


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def execute(command, log, timeout, env, input_bytes=None, output_file=None, interaction=None):
    """Run a command and reap its process group on failure or interruption."""
    with log.open('w') as stream, (output_file.open('w') if output_file else nullcontext(stream)) as stdout:
        stream.write('COMMAND: ' + json.dumps([str(a) for a in command]) + '\n')
        stream.flush()
        proc = subprocess.Popen([str(a) for a in command], cwd=SIM, env=env,
                                stdout=stdout, stderr=stream,
                                stdin=subprocess.PIPE if input_bytes is not None else subprocess.DEVNULL,
                                start_new_session=True)
        try:
            interaction_result = interaction(proc, log, timeout) if interaction else None
            proc.communicate(input=input_bytes, timeout=timeout)
        except BaseException:
            # Several existing DPI drivers intercept signals. SIGKILL also
            # handles a simulator that does not respond to SIGTERM on timeout.
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass  # The child exited between poll and killpg.
            proc.wait()
            raise
    if proc.returncode:
        raise RuntimeError(f'exit {proc.returncode}; see {log}')
    return interaction_result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    parser.add_argument('--circt-bin', default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--riscv', default=os.environ.get('RISCV'))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--timeout', type=float, default=1800,
                        help='wall seconds per command; 0 disables the wall timeout')
    parser.add_argument('--max-cycles', type=int,
                        help='override the simulation cycle limit (positive integer)')
    parser.add_argument('--make-setting', action='append', default=[], metavar='NAME=VALUE',
                        help='repeatable ARC_* build option or compiler tool override, recorded with each case')
    parser.add_argument('--runtime', choices=('native', 'hybrid'), default='native')
    args = parser.parse_args()
    if not args.circt_bin or not args.riscv:
        parser.error('set CIRCT_BIN and RISCV, or supply --circt-bin and --riscv')
    if not math.isfinite(args.timeout) or args.timeout < 0:
        parser.error('--timeout must be finite and nonnegative')
    if args.max_cycles is not None and args.max_cycles <= 0:
        parser.error('--max-cycles must be positive')
    try:
        extra_settings = make_settings(args.make_setting)
    except ValueError as error:
        parser.error(str(error))
    timeout = args.timeout or None
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    riscv, circt = Path(args.riscv).resolve(), Path(args.circt_bin).resolve()
    if args.output:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=True)
    else:
        parent = SIM / 'output' / 'integration'
        parent.mkdir(parents=True, exist_ok=True)
        # Parallel launches can observe the same clock value. Reserve each
        # report directory atomically instead of sharing a timestamp path.
        output = Path(tempfile.mkdtemp(
            prefix=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ-'), dir=parent))
    env = dict(os.environ, RISCV=str(riscv), PATH=str(riscv / 'bin') + os.pathsep + os.environ['PATH'])
    for inherited_make_setting in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES'):
        env.pop(inherited_make_setting, None)
    sources = [ROOT / 'common.mk', ROOT / 'generators/rocket-chip/src/main/resources/csrc/debug_rob.cc']
    sources += [ROOT / 'generators/rocket-chip/scripts/toaxe.py']
    sources += [ROOT / 'tests' / name for name in ('hello.c', 'gcd.c', 'blkdev.c', 'atexit-workaround.c',
                                                'spiflashwrite.c', 'spiflashread.c', 'spiflash.h', 'mmio.h')]
    sources += [p for p in SIM.iterdir() if p.suffix in ('.py', '.h') or p.name == 'Makefile']
    sources += [p for p in (SIM / 'tests').iterdir() if p.is_file()]
    if args.runtime == 'native':
        sources += [p for p in (SIM / 'native').rglob('*') if p.is_file()]
    report = dict(started_at=timestamp(), runtime=args.runtime, circt_bin=str(circt), riscv=str(riscv),
                  wall_timeout_seconds=timeout, requested_max_cycles=args.max_cycles,
                  make_settings=extra_settings,
                  source_sha256={str(p.relative_to(ROOT)): digest(p) for p in sources}, cases=[])
    report['arcilator_version'] = subprocess.check_output([circt / 'arcilator', '--version'], text=True, env=env)
    report_path = output / 'report.json'
    def save():
        temporary = report_path.with_suffix('.tmp')
        temporary.write_text(json.dumps(report, indent=2) + '\n')
        temporary.replace(report_path)
    save()
    print(f'Report: {report_path}', flush=True)
    for name in args.cases:
        config, program, loadmem, required = CASES[name]
        overrides = dict(MAKE_OVERRIDES.get(name, {}), **extra_settings)
        model_package = overrides.get('MODEL_PACKAGE', 'chipyard.harness')
        model = overrides.get('MODEL', 'TestHarness')
        directory = output / name
        directory.mkdir(exist_ok=True)
        before = implementation_hashes(args.runtime)
        result = dict(case=name, config=config, make_overrides=overrides, status='running',
                      started_at=timestamp(), implementation_sha256=before)
        report['cases'].append(result)
        save()
        started = time.monotonic()
        print(f'Building/running {name}: {config}', flush=True)
        try:
            execute(['make', f'CONFIG={config}', f'CIRCT_BIN={circt}', f'ARC_RUNTIME={args.runtime}',
                     *(['native', 'VERILATOR=/not-installed/verilator'] if args.runtime == 'native' else []),
                     *(f'{key}={value}' for key, value in overrides.items())],
                    directory / 'build.log', timeout, env)
            build = SIM / 'generated-src' / f'{model_package}.{model}.{config}' / ('arcilator-native' if args.runtime == 'native' else 'arcilator')
            partition = json.loads((build / 'partition.json').read_text())
            models = {inst['module'] for inst in partition['instances']}
            if args.runtime == 'native':
                imported = json.loads((build / 'rtl.json').read_text())
                models.update(module['module'] for module in imported)
                result.update(imported_rtl=imported, imported_rtl_sha256=digest(build / 'rtl.json'))
            if missing := set(required) - models:
                raise RuntimeError(f'the selected hardware does not contain required models: {sorted(missing)}')
            if program == 'ara' and args.runtime == 'native':
                vlen = 8192 if '8192' in config else 4096
                geometry = dict(VLEN=vlen, NLANES=4 if vlen == 8192 else 2,
                                AXI_DATA_WIDTH=128 if vlen == 8192 else 64)
                ara_modules = [module for module in imported if module['module'] == 'AraBlackbox']
                for module in ara_modules:
                    parameters = dict(pair for value in module['parameters']
                                      for pair in re.findall(r'^(\w+):\s*ui\d+\s*=\s*(\d+)$', value))
                    if any(parameters.get(key) != str(value) for key, value in geometry.items()):
                        raise RuntimeError(f'Ara parameters do not match {config}: {parameters}')
                if len(ara_modules) != 1:
                    raise RuntimeError('the Ara config must contain its one complete vector unit')
                result['ara_parameters'] = geometry
            if name == 'gpio' and len(partition['analog_nets']) != 4:
                raise RuntimeError('the GPIO config did not preserve its four physical nets')
            if name == 'quadchannel' and sum(inst['module'] == 'SimDRAM' for inst in partition['instances']) != 4:
                raise RuntimeError('the selected config did not preserve its four DRAM channels')
            prefix = 'simulator-native' if args.runtime == 'native' else 'simulator'
            simulator = SIM / f'{prefix}-{model_package}-{config}'
            if args.runtime == 'native':
                symbols = subprocess.check_output(['nm', '-C', simulator], text=True, env=env)
                if re.search(r'\bVerilated\w*::', symbols):
                    raise RuntimeError('the native simulator contains Verilated runtime symbols')
                result['verilated_symbols_absent'] = True
            if program in ('tracegen', 'none'):
                binary = None  # Hardware generates its own memory traffic.
            elif program == 'ibex_isa32':
                # Keep the upstream ADD checks and CSR probes, aligning their
                # existing trap/recovery targets for Ibex's vectored mtvec.
                software = directory / 'software'
                execute([sys.executable, SIM / 'tests/ibex_isa.py',
                         '--riscv', riscv, '--work', software],
                        directory / 'software-build.log', timeout, env)
                binary = software / 'rv32ui-p-add.riscv'
                result['software_profile'] = json.loads((software / 'report.json').read_text())
            elif program == 'caliptra_aes':
                software = ROOT / 'generators/caliptra-aes-acc/software'
                binary = directory / 'caliptra_aes.riscv'
                sources = [software / 'test.c', software / 'accellib.c']
                execute([riscv / 'bin/riscv64-unknown-elf-gcc', '-O2', '-std=gnu11',
                         '-I' + str(software), '-I' + str(ROOT / 'tests'),
                         '-march=rv64gc', '-mabi=lp64d', '-mcmodel=medany',
                         '-static', '-specs=htif_nano.specs', '-T', 'htif.ld', '-Wl,--wrap=atexit',
                         *sources, ROOT / 'tests/atexit-workaround.c', '-o', binary],
                        directory / 'software-build.log', timeout, env)
                result['software_profile'] = dict(
                    source_sha256={str(p.relative_to(ROOT)): digest(p) for p in sources},
                    expected_rounds=10, workload='original Caliptra RoCC encrypt/decrypt and DMA checks')
            elif program.startswith('saturn:'):
                profile = build_saturn_software(riscv, directory / 'software',
                                                program.split(':', 1)[1])
                binary = Path(profile['binary'])
                result['software_profile'] = profile
            elif program.startswith('isa'):
                xlen = program[3:]
                binary = riscv / f'riscv64-unknown-elf/share/riscv-tests/isa/rv{xlen}ui-p-add'
            else:
                source = SIM / 'tests' / f'{program}.c'
                if not source.exists():
                    source = ROOT / 'tests' / f'{program}.c'
                binary = directory / f'{program}.riscv'
                linker = cva6_linker(riscv, directory) if name == 'cva6' else 'htif.ld'
                execute([riscv / 'bin/riscv64-unknown-elf-gcc', '-O2', '-std=gnu11',
                         '-I' + str(ROOT / 'tests'), '-march=' + ('rv64gcv' if program == 'ara' else 'rv64gc'), '-mabi=lp64d', '-mcmodel=medany',
                         *(['-fno-tree-vectorize'] if program == 'ara' else []),
                         '-static', '-specs=htif_nano.specs', '-T', linker, '-Wl,--wrap=atexit',
                         source, ROOT / 'tests/atexit-workaround.c', '-o', binary],
                        directory / 'software-build.log', timeout, env)
                if program == 'ara':
                    result['software_profile'] = dict(
                        source_sha256=digest(source), integer_elements=257, floating_point_elements=257,
                        expected_values='scalar integer arithmetic and exact binary32 quarter increments',
                        scalar_checks_vectorization_disabled=True)
                if name == 'cva6':
                    result['software_profile'] = dict(
                        linker_script=str(linker), linker_sha256=digest(linker),
                        mailboxes=check_mailboxes(riscv, binary))
            # The real fuzzer stops after 500,000 ticks of its 2 ns clock.
            # Include root-reset and response-drain cycles beyond 1,000,000 ns.
            long_boot = ('tethered', 'aes', 'sim_axi', 'default_clock', 'single_clock',
                         'pll_clock', 'chip_like', 'flat_chip_like', 'no_axi_fast', 'no_axi_serial',
                         'saturn', 'saturn_masked', 'saturn_transpose', 'saturn_sgemm')
            cycle_limit = 10000000 if name in long_boot else 2000000 if name == 'memfuzz' else 1000000
            if args.max_cycles is not None:
                cycle_limit = args.max_cycles
            result.update(max_cycles=cycle_limit, wall_timeout_seconds=timeout)
            command = [simulator, '+permissive', f'+max-cycles={cycle_limit}',
                       '+dramsim_ini_dir=' + str(ROOT / 'generators/testchipip/src/main/resources/dramsim2_ini')]
            # The stock TraceGen config also trips a TileLink assertion in
            # Verilator with DRAMSim2. Use the existing simple-memory driver.
            if 'SimDRAM' in models and name != 'tracegen':
                command.append('+dramsim')
            if program != 'uart' and name != 'tethered':
                command.append('+verbose')
            if name == 'tethered':
                # Match the repository's tethered CI boot: the host needs the
                # target cache-flush register, and serial loading takes longer.
                command.append('+cflush_addr=0x2010200')
            if loadmem:
                command.append('+loadmem=' + str(binary))
            if name == 'block':
                disk = directory / 'disk.img'
                with disk.open('wb') as stream:
                    stream.truncate(1024 * 1024)
                command.append('+blkdev=' + str(disk))
            if name in ('spiflash', 'spiflashrom'):
                # The stock workload checks this pattern, then programs and
                # reads back single/quad transfers through the physical pins.
                flash = directory / 'flash.img'
                words = 1024 * 1024 // 4
                flash.write_bytes(struct.pack(f'<{words}I', *(0xdeadbeef - 4 * i for i in range(words))))
                result['flash_initial_sha256'] = digest(flash)
                command.append('+spiflash0=' + str(flash))
            if program == 'uart':
                # The existing UART driver appends to its output file.
                (directory / 'uart0').unlink(missing_ok=True)
                command.append('+uartlog=' + str(directory / 'uart'))
            if name in ('jtag', 'custom_io_jtag'):
                command.append('+jtag_rbb_enable=1')
            command += ['+permissive-off', binary if binary else 'none']
            interaction_result = execute(command, directory / 'run.log', timeout, env,
                    input_bytes=b'?Z' if program == 'uart' else None,
                    interaction=exercise_jtag if name in ('jtag', 'custom_io_jtag') else None)
            if name in ('jtag', 'custom_io_jtag'):
                result['jtag_registers'] = interaction_result
            transcript = (directory / 'run.log').read_text(errors='replace')
            if program == 'uart':
                if (directory / 'uart0').read_bytes() != b'A' or 'PASS: SimUART' not in transcript:
                    raise RuntimeError('UART transmit/receive did not match A/Z')
                if any(line.startswith('C0:') for line in transcript.splitlines()):
                    raise RuntimeError('PRINTF_COND did not suppress native CPU diagnostics')
            if program == 'blkdev' and 'All correct' not in transcript:
                raise RuntimeError('the original block-device DMA data comparison did not finish')
            if name == 'network' and 'PASS: SimNetwork DMA' not in transcript:
                raise RuntimeError('SimNetwork did not complete the DMA payload comparison')
            if program == 'ara' and 'PASS: Ara 257 integer and 257 floating-point vector elements' not in transcript:
                raise RuntimeError('Ara did not complete the integer and floating-point scalar comparisons')
            if name == 'aes' and (transcript.count('TEST PASSED!') != 10 or 'TEST FAILED!' in transcript):
                raise RuntimeError('Caliptra RoCC workload did not complete all ten encrypt/decrypt rounds')
            if name == 'noceval' and 'Min throughput:' not in transcript:
                raise RuntimeError('the TrafficEval DPI driver did not report its measurements')
            if name in ('spiflash', 'spiflashrom'):
                opcodes = (('13', '02', '32', '38', '12', '34', '3E') if name == 'spiflash' else
                           ('13', '03', '0B', '0C', '6C', '6B', 'EB', 'EC'))
                for opcode in opcodes:
                    if f'Testing SPI flash command 0x{opcode}...' not in transcript:
                        raise RuntimeError(f'SPI flash command 0x{opcode} was not exercised')
                contents = flash.read_bytes()
                if name == 'spiflash':
                    expected = bytes.fromhex('133700ffaa55face0ff00123456789ab')
                    if any(contents[offset:offset + len(expected)] != expected
                           for offset in range(0x200, 0x800, 0x100)):
                        raise RuntimeError('SPI writes did not persist at all six image offsets')
                elif digest(flash) != result['flash_initial_sha256']:
                    raise RuntimeError('the read-only SPI workload changed its image')
                result['flash_final_sha256'] = digest(flash)
            if name == 'tracegen':
                axe = ROOT / 'tools/axe/src/axe'
                execute(['make', f'CONFIG={config}', f'CIRCT_BIN={circt}', axe],
                        directory / 'checker-build.log', timeout, env)
                trace = directory / 'memory.trace'
                trace.write_text(''.join(line + '\n' for line in transcript.splitlines()
                                         if re.match(r'^\d+: ', line)))
                if not trace.stat().st_size:
                    raise RuntimeError('TraceGen did not emit memory operations')
                converted = directory / 'memory.axe'
                execute([sys.executable, ROOT / 'generators/rocket-chip/scripts/toaxe.py', trace],
                        directory / 'trace-conversion.log', timeout, env, output_file=converted)
                execute([axe, 'check', 'wmo', converted], directory / 'memory-check.log', timeout, env)
                if (directory / 'memory-check.log').read_text().splitlines()[-1] != 'OK':
                    raise RuntimeError('Axe rejected the memory-coherence trace')
                result.update(checker_sha256=digest(axe), memory_trace_sha256=digest(trace))
            cycles = re.search(r'PASSED.*?after\s+(\d+) simulation cycles', transcript)
            if name == 'tethered' and ('Hello world from core 0' not in transcript or
                                       (not cycles if args.runtime == 'native' else
                                        not re.search(r'TestDriver\.v:\d+: Verilog \$finish', transcript))):
                raise RuntimeError('tethered software or harness did not complete')
            if program != 'uart' and name != 'tethered' and not cycles:
                raise RuntimeError('simulation exited without the TestDriver success marker')
            if implementation_hashes(args.runtime) != before:
                raise RuntimeError('backend sources changed during this case; its result cannot establish correctness')
            result.update(status='passed', simulator_sha256=digest(simulator), binary_sha256=digest(binary) if binary else None,
                          partition_sha256=digest(build / 'partition.json'), required_models=required,
                          cycles=int(cycles[1]) if cycles else None)
        except KeyboardInterrupt:
            result.update(status='interrupted', error='interrupted before validation completed')
            raise
        except Exception as error:
            result.update(status='failed', error=str(error))
        finally:
            result.update(completed_at=timestamp(), elapsed_seconds=round(time.monotonic() - started, 3))
            save()
        print(f'{name}: {result["status"]}' + (': ' + result['error'] if 'error' in result else ''), flush=True)
    report['completed_at'] = timestamp()
    save()
    return int(any(case['status'] != 'passed' for case in report['cases']))


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
