#!/usr/bin/env python3
"""Compile and execute a native mixed-state regression without Verilator."""
import os
import argparse
from pathlib import Path
import shlex
import subprocess
import random
import json
import sys
import tempfile

here = Path(__file__).resolve().parent
sim = here.parent
sys.path.insert(0, str(sim))
from native_hw import prepare_rtl
from compiler.partition import plugin
from prefix_feedback import native_design as prefix_design, rr_design
from run_loop import check as check_run_loop_trace
circt = Path(os.environ['CIRCT_BIN']).resolve()
parser = argparse.ArgumentParser(description=__doc__)
variants = ['compiled-combin', 'delay-transport', 'delay-precision', 'delayed-signals', 'hierarchical-captures', 'realtime-exports', 'array-exports', 'real-exports', 'format-strings', 'task-finish', 'task-callbacks', 'task-effects', 'function-effects', 'lexical-format', 'hierarchy-format', 'test-plusargs', 'string-struct', 'static-formals', 'pointer-arrays', 'open-arrays', 'unpacked-imports', 'context-scopes', 'handles', 'string-imports', 'sparse-memory', 'write-only', 'dynamic-writes', 'readmem-multi', 'case-effects', 'bit-builtins', 'bare-feedback', 'comb-print', 'procedural-feedback', 'aggregate-feedback', 'readmem', 'unit-static', 'exports', 'async', 'tracerom', 'rtl', 'rtl-debug', 'port-rules', 'parameters', 'cfg-loops', 'cfg-memory', 'packed-cfg', 'packed-struct', 'packed-union', 'local-memory', 'static-storage', 'global-storage', 'global-startup', 'memory', 'files', 'procedural', 'tasks', 'real-format', 'sensitized-dpi', 'seeded-dpi', 'dpi-chain', 'dpi-abi', 'latches', 'feedback-state', 'proven-feedback', 'crossings', 'slices', 'initial', 'comb-drives', 'signal-state', 'signal-coherence', 'scopes', 'properties', 'temporal', 'array-feedback', 'prefix-feedback', 'dtm', 'optional', 'unused-clock', 'plusarg', 'control', 'many-ports', 'arrays', 'shift', 'time', 'scheduled-dpi', 'time-output', 'time-wakeup', 'dram-parameter-model']
parser.add_argument('--variants', nargs='+', choices=variants + ['scan', 'radiance-cache', 'rr-arbiter'], default=variants)
sharing = parser.add_mutually_exclusive_group()
sharing.add_argument('--share-procedures', action='store_true', default=True,
                     help='enable private procedure workspace forwarding (default)')
sharing.add_argument('--no-share-procedures', dest='share_procedures',
                     action='store_false', help='disable private procedure workspace forwarding')
args = parser.parse_args()


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def check_format_string_trace(executable):
    result = subprocess.run([str(executable.resolve())], cwd=executable.parent,
                            capture_output=True, text=True, check=True)
    actual = [line for line in result.stdout.splitlines() if line.startswith('NATIVE_FORMAT')]
    expected = [line.removeprefix('EXPECTED_NATIVE_FORMAT ') for line in result.stderr.splitlines()
                if line.startswith('EXPECTED_NATIVE_FORMAT ')]
    if not actual or sorted(actual) != sorted(expected):
        raise RuntimeError('formatted string trace/control mismatch: ' + result.stdout + result.stderr)
    import hashlib
    reference = here / 'format_strings'
    profile = json.loads((reference / 'references.json').read_text())
    for name, digest in {**profile['sources_sha256'],
                         **profile['raw_outputs_sha256'],
                         **profile['outputs_sha256']}.items():
        if hashlib.sha256((reference / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('stock format oracle input/output fingerprint changed: ' + name)
    # Stock tools disagree on formatting the packed integer's embedded NUL.
    # Retain the raw Icarus corpus and verify its sole corrected field against
    # an independently linked, unmodified Slang constant evaluator. Every byte
    # of the resulting reference still participates in the native comparison.
    packed = profile['packed_constant']
    oracle = json.loads((reference / packed['oracle']).read_text())
    scalar = oracle['cases'][packed['case']]
    value = bytes.fromhex(scalar['hex'])
    if (oracle['status'] != 'passed' or scalar['length'] != len(value) or
            scalar['expression'] != '$sformatf("%s",32\'h41004243)' or
            packed['expected_field'].encode() != b' p=' + value + b']'):
        raise RuntimeError('independent packed-string scalar oracle changed')
    for tag in range(2):
        raw = (reference / f'reference-{tag}.txt').read_bytes()
        expected = (reference / f'expected-{tag}.txt').read_bytes()
        if (raw.count(packed['raw_field'].encode()) != packed['records_per_file'] or
                raw.replace(packed['raw_field'].encode(),
                            packed['expected_field'].encode()) != expected):
            raise RuntimeError('format corpus changed beyond the proven packed-NUL field')
        if (executable.parent / f'format-native-{tag}.log').read_bytes() != expected:
            raise RuntimeError('formatted file bytes differ from exact combined oracle')
    print(next(line for line in result.stdout.splitlines() if line.startswith('PASS:')), flush=True)
    print(f'PASS: {len(actual)} exact dynamic format print records and file/control/effect checks', flush=True)


def check_task_callback_trace(executable):
    result = subprocess.run([executable], capture_output=True, text=True, check=True)
    actual = [line for line in result.stdout.splitlines() if line.startswith('NATIVE_TASK_CALLBACK')]
    expected = [line.removeprefix('EXPECTED_TASK_CALLBACK ') for line in result.stderr.splitlines() if line.startswith('EXPECTED_TASK_CALLBACK ')]
    if len(actual) != 28 or sorted(actual) != sorted(expected):
        raise RuntimeError('exported task print/control values changed: ' + result.stdout + result.stderr)
    for marker in range(4):
        message = ('Fatal: ' + ('fixed' if marker // 2 else 'relative') +
                   ' child callback scope/value changed') if marker % 2 else ''
        expected_fatal = f'FATAL_BEGIN_{marker}\n{message}\nFATAL_END_{marker}\n'
        if result.stdout.count(expected_fatal) != 1:
            raise RuntimeError('original exported fatal bytes/control changed: ' + result.stdout + result.stderr)
    print(next(line for line in result.stdout.splitlines() if line.startswith('PASS:')), flush=True)
    print('PASS: 28 exact task callback print records, original controls and independent process order', flush=True)


def check_hierarchy_trace(executable):
    result = subprocess.run([executable],capture_output=True,text=True,check=True)
    actual=[line for line in result.stdout.splitlines() if line.startswith('NATIVE_HIER')]
    expected=[line.removeprefix('EXPECTED_HIER ') for line in result.stderr.splitlines() if line.startswith('EXPECTED_HIER ')]
    if not actual or sorted(actual)!=sorted(expected):
        raise RuntimeError('source hierarchy print values/counts changed: '+result.stdout+result.stderr)
    print(next(line for line in result.stderr.splitlines() if line.startswith('PASS:')),flush=True)
    print('PASS: '+str(len(actual))+' exact hierarchy print records; independent parallel process order is unrestricted',flush=True)


def check_combinational_trace(executable, kind):
    result = subprocess.run([executable], capture_output=True, text=True, check=True)
    actual = [line for line in result.stdout.splitlines()
              if line.startswith(('NATIVE_COMB_', 'NATIVE_LOOP_', 'NATIVE_AGG_'))]
    expected = [line.removeprefix('EXPECTED ') for line in result.stderr.splitlines()
                if line.startswith('EXPECTED ')]
    if not actual or actual != expected:
        raise RuntimeError('combinational event trace differs: ' + result.stdout + result.stderr)
    print(next(line for line in result.stderr.splitlines() if line.startswith('PASS:')), flush=True)
    if kind == 'procedural_feedback':
        result = subprocess.run([executable, '+test-oscillation'], capture_output=True,
                                text=True, check=True)
        if 'nonconvergent procedural feedback is diagnosed' not in result.stdout:
            raise RuntimeError('oscillating procedural feedback did not fail convergence')
        print(result.stdout, end='', flush=True)


# Each invocation owns its objects, so simultaneous test runs cannot reuse
# stale flags or overwrite an object another linker is reading.
(sim / 'generated-src').mkdir(exist_ok=True)
common_work = tempfile.TemporaryDirectory(prefix='native-runtime-test-',
                                         dir=sim / 'generated-src')
common_objects = {}


def runtime_objects(level):
    if level not in common_objects:
        output = Path(common_work.name) / ('O' + level)
        output.mkdir()
        objects = []
        for source in sorted((sim / 'native').glob('*.cpp')):
            obj = output / (source.stem + '.o')
            # LLVM's runtime has no RTTI. Only the stream subclass needs this
            # flag; selected original device drivers retain their usual flags.
            flags = ['-fno-rtti'] if source.name == 'files.cpp' else []
            run(*shlex.split(os.environ.get('CXX', 'c++')), '-O' + level,
                '-std=c++17', '-DARC_NATIVE_NO_MAIN', *flags,
                '-I' + str(sim), '-I' + str(sim / 'native/include'),
                '-I' + str(circt.parent / 'include'), '-c', source, '-o', obj)
            objects.append(obj)
        common_objects[level] = objects
    return common_objects[level]


run_loop_work = Path(tempfile.mkdtemp(prefix='native-run-loop-test-', dir=sim / 'generated-src'))
run(*shlex.split(os.environ.get('CXX', 'c++')), '-O2', '-std=c++17',
    '-I' + str(sim), '-I' + str(sim / 'native/include'), '-I' + str(circt.parent / 'include'),
    here / 'native_run_loop.cpp', *runtime_objects('2'),
    '-L' + str(circt.parent / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle',
    '-lz', '-lpthread', '-ldl', '-o', run_loop_work / 'test')
run(run_loop_work / 'test', run_loop_work)
check_run_loop_trace(run_loop_work)

# Check the original SimDRAM callback contract independently of a hardware
# model: explicit WORD_SIZE may differ from the DATA_BITS-sized DPI arrays.
dram_work = Path(tempfile.mkdtemp(prefix='native-dram-parameters-test-', dir=sim / 'generated-src'))
run(*shlex.split(os.environ.get('CXX', 'c++')), '-O2', '-std=c++17',
    '-I' + str(sim), '-I' + str(sim / 'native/include'), '-I' + str(circt.parent / 'include'),
    here / 'native_dram_parameters.cpp', *runtime_objects('2'),
    '-L' + str(circt.parent / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle',
    '-lz', '-lpthread', '-ldl', '-o', dram_work / 'test')
run(dram_work / 'test')

run('python3', here / 'sparse_memories.py')
run('python3', here / 'frontend.py', '--firtool', circt / 'firtool')
run('python3', here / 'rtl_source_scope.py', '--circt-bin', circt)
run('python3', here / 'union_bits.py', '--circt-bin', circt)
run('python3', here / 'mem2reg.py', '--circt-bin', circt)
run('python3', here / 'compiler_arrays.py', '--circt-bin', circt)
run('python3', here / 'execute_arrays.py', '--circt-bin', circt)
run('python3', here / 'boolean_arrays.py', '--circt-bin', circt)
run('python3', here / 'aggregate_arrays.py', '--circt-bin', circt)
run('python3', here / 'zero_width.py', '--circt-bin', circt)
run('python3', here / 'singleton_cfg.py', '--circt-bin', circt)
run('python3', here / 'state_properties.py', '--circt-bin', circt)
run('python3', here / 'compiler_dpi.py', '--circt-bin', circt)
run('python3', here / 'sensitized_dpi.py', '--circt-bin', circt)
run('python3', here / 'sampling_phases.py', '--circt-bin', circt)
run('python3', here / 'pure_wide_shift.py', '--circt-bin', circt)
run('python3', here / 'convergence.py', '--circt-bin', circt)
run('python3', here / 'idle_clock.py', '--circt-bin', circt)
run('python3', here / 'seeded_dpi.py', '--circt-bin', circt)
run('python3', here / 'compiler_tasks.py', '--circt-bin', circt)
run('python3', here / 'compiler_formats.py', '--circt-bin', circt)
run('python3', here / 'time_dependencies.py', '--circt-bin', circt)
run('python3', here / 'dpi_abi.py', '--circt-bin', circt)
run('python3', here / 'dpi_declarations.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'dpi_instance_metadata.py', '--circt-bin', circt)
run('python3', here / 'array_feedback.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'prefix_feedback.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'proven_feedback.py', '--circt-bin', circt)
run('python3', here / 'procedural_feedback.py', '--circt-bin', circt)
run('python3', here / 'partition_literals.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'partition_boundary.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'partition_scopes.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'partition_stream.py', circt / 'circt-opt')
run('python3', here / 'native_combin.py')
run('python3', here / 'shared_procedures.py')
run('python3', here / 'native_strings.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'native_plusargs.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'native_handles.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'native_contexts.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'plugin_headers.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'hierarchy_formats.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'scope_frames.py', '--circt-bin', circt)
run('python3', here / 'native_unpacked.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'native_open.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'native_pointer_arrays.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'latches.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'signal_initializers.py', '--circt-opt', circt / 'circt-opt', '--work', sim / 'generated-src/signal-initializer-guards')
run('python3', here / 'signal_coherence.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'constant_strings.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'properties.py', '--circt-opt', circt / 'circt-opt')
run('python3', here / 'temporal.py', '--circt-opt', circt / 'circt-opt', '--work', sim / 'generated-src/temporal-guards')
if any(kind in args.variants for kind in ('scan', 'radiance-cache', 'rr-arbiter')):
    run('python3', here / 'package_resources.py', '--circt-bin', circt)


for variant in args.variants:
    if variant == 'shift':
        run('python3', here / 'wide_helper.py')
    build = sim / ('generated-src/native-' + variant + '-test')
    build.mkdir(parents=True, exist_ok=True)
    kind = variant.removesuffix('-debug').replace('-', '_')
    test_kind = {'packed_union': 'packed_struct', 'packed_cfg': 'cfg_memory', 'global_startup': 'global_storage'}.get(kind, kind)
    source, top = {'compiled_combin': (here / 'native_compiled_combin.mlir', 'TestHarness'),
                   'dram_parameter_model': (here / 'native_dram_parameter_model.mlir', 'DRAMParametersHarness'),
                   'delay_transport': (here / 'native_delay_transport.mlir', 'DelayTransportHarness'),
                   'delay_precision': (here / 'native_delay_precision.mlir', 'DelayPrecisionHarness'),
                   'delayed_signals': (here / 'native_delayed_signals.mlir', 'DelayedSignalsHarness'),
                   'hierarchical_captures': (here / 'native_hierarchical_captures.mlir', 'CapturedHarness'),
                   'array_exports': (here / 'native_array_exports.mlir', 'ArrayExportHarness'),
                   'real_exports': (here / 'native_real_exports.mlir', 'RealExportHarness'),
                   'realtime_exports': (here / 'native_realtime_exports.mlir', 'RealtimeHarness'),
                   'format_strings': (here / 'native_format_strings.mlir', 'FormatStringsHarness'),
                   'task_finish': (here / 'native_task_finish.mlir', 'TaskFinishHarness'),
                   'task_callbacks': (here / 'native_task_callbacks.mlir', 'TaskCallbacksHarness'),
                   'task_effects': (here / 'native_task_effects.mlir', 'TestHarness'),
                   'function_effects': (here / 'native_function_effects.mlir', 'TestHarness'),
                   'lexical_format': (here / 'native_lexical_format.mlir', 'TestHarness'),
                   'hierarchy_format': (here / 'native_hierarchy_format.mlir', 'TestHarness'),
                   'test_plusargs': (here / 'native_test_plusargs.mlir', 'TestPlusargsHarness'),
                   'string_struct': (here / 'native_string_struct.mlir', 'StringStructImportsHarness'),
                   'static_formals': (here / 'native_static_formals.mlir', 'StaticFormalsHarness'),
                   'pointer_arrays': (here / 'native_pointer_arrays.mlir', 'PointerHarness'),
                   'open_arrays': (here / 'native_open_arrays.mlir', 'OpenHarness'),
                   'unpacked_imports': (here / 'native_unpacked_imports.mlir', 'UnpackedHarness'),
                   'context_scopes': (here / 'native_context_scopes.mlir', 'ContextHarness'),
                   'handles': (here / 'native_handles.mlir', 'HandlesHarness'),
                   'string_imports': (here / 'native_string_imports.mlir', 'StringImportsHarness'),
                   'write_only': (here / 'native_write_only.mlir', 'WriteOnlyHarness'),
                   'sparse_memory': (here / 'native_sparse_memory.mlir', 'SparseMemoryHarness'),
                   'dynamic_writes': (here / 'native_dynamic_writes.mlir', 'DynamicWritesHarness'),
                   'bare_feedback': (here / 'native_bare_feedback.mlir', 'NativeBareHarness'),
                   'comb_print': (here / 'native_comb_print.mlir', 'NativePrintHarness'),
                   'procedural_feedback': (here / 'native_procedural_feedback.mlir', 'NativeLoopHarness'),
                   'aggregate_feedback': (here / 'native_aggregate_feedback.mlir', 'NativeAggregateHarness'),'readmem': (here / 'native_readmem.mlir', 'ReadMemHarness'),
                   'readmem_multi': (here / 'native_readmem_multi.mlir', 'ReadMemMultiHarness'),
                   'unit_static': (here / 'native_unit_static.mlir', 'UnitHarness'),
                   'exports': (here / 'native_exports.mlir', 'ExportHarness'),
                   'async': (here / 'native_async.mlir', 'NativeAsyncHarness'),
                   'tracerom': (here / 'tracerom.mlir', 'TraceHarness'),
                   'rtl': (here / 'native_rtl.mlir', 'NativeRTLHarness'),
                   'port_rules': (here / 'native_port_rules.mlir', 'NativePortRulesHarness'),
                   'parameters': (here / 'parameters.mlir', 'Parameters'),
                   'cfg_loops': (here / 'native_cfg_loops.mlir', 'NativeCFGLoopsHarness'),
                   'memory': (here / 'native_memory.mlir', 'NativeMemoryHarness'),
                   'cfg_memory': (here / 'native_cfg_memory.mlir', 'CFGMemoryHarness'),
                   'packed_cfg': (here / 'native_packed_cfg.mlir', 'CFGMemoryHarness'),
                   'local_memory': (here / 'native_local_memory.mlir', 'LocalHarness'),
                   'static_storage': (here / 'native_static_storage.mlir', 'StaticScopesHarness'),
                   'global_storage': (here / 'native_global_storage.mlir', 'GlobalHarness'),
                   'global_startup': (here / 'native_global_storage.mlir', 'GlobalHarness'),
                   'packed_struct': (here / 'native_packed_struct.mlir', 'PackedSelectHarness'),
                   'packed_union': (here / 'native_packed_struct.mlir', 'PackedSelectHarness'),
                   'files': (here / 'native_files.mlir', 'NativeFilesHarness'),
                   'procedural': (here / 'native_procedural.mlir', 'NativeProceduralHarness'),
                   'tasks': (here / 'native_tasks.mlir', 'NativeTasksHarness'),
                   'real_format': (here / 'native_real_format.mlir', 'NativeRealFormatHarness'),
                   'sensitized_dpi': (here / 'sensitized_dpi.mlir', 'SensitizedHarness'),
                   'seeded_dpi': (here / 'seeded_dpi.mlir', 'SeededDPIHarness'),
                   'dpi_chain': (here / 'native_dpi_chain.mlir', 'DPIChainHarness'),
                   'dpi_abi': (here / 'native_dpi_abi.mlir', 'NativeDPIABIHarness'),
                   'bit_builtins': (here / 'native_bit_builtins.mlir', 'BitBuiltinsHarness'),
                   'case_effects': (here / 'native_case_effects.mlir', 'CaseEffectsHarness'),
                   'latches': (here / 'native_latches.mlir', 'NativeLatchesHarness'),
                   'feedback_state': (here / 'native_feedback_state.mlir', 'NativeFeedbackStateHarness'),
                   'proven_feedback': (here / 'native_proven_feedback.mlir', 'NativeFeedbackHarness'),
                   'crossings': (here / 'native_crossings.mlir', 'NativeCrossingsHarness'),
                   'slices': (here / 'native_slices.mlir', 'NativeSlicesHarness'),
                   'initial': (here / 'native_initial.mlir', 'NativeInitialHarness'),
                   'scan': (here / 'native_scan.mlir', 'NativeScanHarness'),
                   'comb_drives': (here / 'native_comb_drives.mlir', 'NativeCombDrivesHarness'),
                   'signal_state': (here / 'native_signal_state.mlir', 'NativeSignalStateHarness'),
                   'signal_coherence': (here / 'native_signal_coherence.mlir', 'NativeSignalCoherenceHarness'),
                   'scopes': (here / 'native_scopes.mlir', 'NativeScopesHarness'),
                   'properties': (here / 'native_properties.mlir', 'NativePropertiesHarness'),
                   'temporal': (here / 'native_temporal.mlir', 'NativeTemporalHarness'),
                   'radiance_cache': (here / 'native_radiance_cache.mlir', 'NativeRadianceCacheHarness'),
                   'dtm': (here / 'native_dtm.mlir', 'NativeDTMHarness'),
                   'optional': (here / 'native_optional.mlir', 'NativeOptionalHarness'),
                   'unused_clock': (here / 'native_unused_clock.mlir', 'UnusedClockHarness'),
                   'scheduled_dpi': (here / 'native_scheduled_dpi.mlir', 'ScheduledDPIHarness'),
                   'time_output': (here / 'native_time_output.mlir', 'TimeOutputHarness'),
                   'time_wakeup': (here / 'native_time_wakeup.mlir', 'TimeWakeupHarness'),
                   'plusarg': (here / 'native_plusarg.mlir', 'WidePlusargHarness'),
                   'control': (here / 'native_control.mlir', 'NativeControlHarness'),
                   'many_ports': (build / 'design.mlir', 'ManyPortsHarness'),
                   'arrays': (here / 'native_arrays.mlir', 'NativeArrayHarness'),
                   'array_feedback': (here / 'native_array_feedback.mlir', 'NativeArrayFeedbackHarness'),
                   'prefix_feedback': (build / 'design.mlir', 'NativePrefixFeedbackHarness'),
                   'rr_arbiter': (build / 'input.mlir', 'NativeRRArbiterHarness'),
                   'shift': (here / 'native_shift.mlir', 'ShiftHarness'),
                   'time': (here / 'native_time.mlir', 'NativeTimeHarness')}[kind]
    if kind == 'prefix_feedback':
        source.write_text(prefix_design())
    if kind == 'rr_arbiter':
        source.write_text(rr_design())
        run(circt / 'circt-opt', source, '-o', build / 'normalized-input.mlir')
        source = build / 'normalized-input.mlir'
    if kind == 'many_ports':
        count = 16500
        ports = ['in %clock:i1', 'in %reset:i1']
        ports += [port for i in range(count) for port in (f'in %a{i}:i8', f'in %b{i}:i8')]
        ports += ['out io_success:i1'] + [f'out o{i}:i8' for i in range(count)]
        lines = ['module {', ' hw.module @ManyPortsHarness(' + ', '.join(ports) + ') {',
                 '  %false = hw.constant false', '  %clk = seq.to_clock %clock',
                 '  %message = sim.fmt.literal "NATIVE_PARTITION_LITERAL\\0A"',
                 '  %error = sim.fmt.literal "NATIVE_PARTITION_STDERR\\0A"',
                 '  %stderr = sim.stderr_stream']
        for i in range(count):
            lines.append(f'  %o{i} = comb.xor %a{i}, %b{i}:i8')
            if i % 4096 == 0:
                lines += ['  sim.triggered %clk {', '    sim.proc.print %message',
                          '    sim.proc.print %error to %stderr', '  }']
        lines += ['  hw.output %false, ' + ', '.join(f'%o{i}' for i in range(count)) +
                  ' : i1, ' + ', '.join('i8' for _ in range(count)), ' }', '}']
        source.write_text('\n'.join(lines) + '\n')
    filelist = None
    if kind == 'compiled_combin':
        import hashlib
        reference = here / 'compiled_combin'
        profile = json.loads((reference / 'references.json').read_text())
        for name, digest in profile['sources_sha256'].items():
            assert hashlib.sha256((reference / name).read_bytes()).hexdigest() == digest, name
        assert hashlib.sha256(source.read_bytes()).hexdigest() == profile['native_source_sha256']
        assert hashlib.sha256((reference / 'reference.bin').read_bytes()).hexdigest() == profile['reference_sha256']
    if kind == 'array_exports':
        from array_exports import check_guards as check_array_export_guards
        library = plugin(circt / 'circt-opt', os.environ.get('CXX', 'c++'),
                         sim / 'generated-src/compiler', sim / 'compiler/SignalMemory.cpp')
        check_array_export_guards(circt / 'circt-opt', library, build)
    if kind == 'delay_transport':
        import hashlib
        profile = json.loads((here / 'delay_transport/references.json').read_text())
        for path, key in [(here / 'native_delay_transport.mlir', 'source_sha256'),
                          (here / 'delay_transport/reference-design.sv', 'reference_design_sha256'),
                          (here / 'delay_transport/stimulus.txt', 'stimulus_sha256'),
                          (here / 'delay_transport/raw-reference.log', 'raw_reference_sha256'),
                          (here / 'delay_transport/reference.txt', 'reference_sha256')]:
            if hashlib.sha256(path.read_bytes()).hexdigest() != profile[key]:
                raise RuntimeError('independent stockSV transport reference changed: ' + str(path))
    if kind in ('delayed_signals', 'delay_precision'):
        import hashlib
        profile = json.loads((here / kind / 'references.json').read_text())
        for path, key in [(here / f'native_{kind}.sv', 'source_sha256'),
                          (here / kind / 'stimulus.txt', 'stimulus_sha256'),
                          (here / kind / 'raw-reference.log', 'raw_reference_sha256'),
                          (here / kind / 'reference.txt', 'reference_sha256')]:
            if hashlib.sha256(path.read_bytes()).hexdigest() != profile[key]:
                raise RuntimeError('independent stockSV timing source/reference changed: ' + str(path))
    if kind == 'real_exports':
        from real_exports import check_guards as check_real_export_guards
        library = plugin(circt / 'circt-opt', os.environ.get('CXX', 'c++'),
                         sim / 'generated-src/compiler', sim / 'compiler/SignalMemory.cpp')
        check_real_export_guards(circt / 'circt-opt', library, build)
    if kind == 'exports':
        from exports import check_guards as check_export_guards
        library = plugin(circt / 'circt-opt', os.environ.get('CXX', 'c++'),
                         sim / 'generated-src/compiler', sim / 'compiler/SignalMemory.cpp')
        check_export_guards(circt / 'circt-opt', library, build)
        from exports import check_context_guards
        model_library = plugin(circt / 'circt-opt', os.environ.get('CXX', 'c++'),
                               sim / 'generated-src/compiler', sim / 'compiler/Model.cpp')
        check_context_guards(circt / 'circt-opt', model_library, build)
        from exports import check_qualifier_guards
        check_qualifier_guards(circt / 'circt-opt', model_library, build)
    if kind in ('global_storage', 'global_startup'):
        from global_memory import check_guards
        library = plugin(circt / 'circt-opt', os.environ.get('CXX', 'c++'),
                         sim / 'generated-src/compiler', sim / 'compiler/SignalMemory.cpp')
        check_guards(circt / 'circt-opt', library, build)
    if kind in ('array_exports', 'real_exports', 'realtime_exports', 'hierarchical_captures', 'delay_precision', 'delayed_signals', 'format_strings', 'task_finish', 'task_callbacks', 'task_effects', 'function_effects', 'lexical_format', 'hierarchy_format', 'test_plusargs', 'string_struct', 'static_formals', 'pointer_arrays', 'open_arrays', 'unpacked_imports', 'context_scopes', 'handles', 'string_imports', 'write_only', 'dynamic_writes', 'case_effects', 'bit_builtins', 'comb_print', 'procedural_feedback', 'temporal', 'readmem', 'readmem_multi', 'unit_static', 'exports', 'global_storage', 'global_startup', 'static_storage', 'rtl', 'port_rules', 'parameters', 'cfg_loops', 'packed_struct', 'packed_union', 'memory', 'files', 'procedural', 'tasks', 'real_format', 'dpi_abi', 'latches', 'feedback_state', 'crossings', 'slices', 'initial', 'scan', 'comb_drives', 'signal_state', 'signal_coherence', 'radiance_cache', 'rr_arbiter'):
        filelist = build / 'blackboxes.f'
        rtl_flags = '+define+ARC_NATIVE_IMPORT_TEST=1'
        if kind == 'dynamic_writes':
            rtl_flags += ' --Xslang=-Wno-multiple-always-assigns'
        if kind == 'global_startup':
            filelist.write_text(str(here / 'native_global_storage.sv') + '\n')
            rtl_flags += ' +define+ARC_GLOBAL_STARTUP=1'
        elif kind == 'parameters':
            filelist.write_text(str(here / 'parameters.sv') + '\n')
        elif kind == 'latches':
            vendor = sim.parent.parent / 'generators/ibex/src/main/resources/vsrc/ibex/vendor/lowrisc_ip/ip/prim_generic/rtl/prim_generic_clock_gating.sv'
            filelist.write_text(str(here / 'native_latches.sv') + '\n' + str(vendor) + '\n')
        elif kind in ('scan', 'rr_arbiter'):
            vortex = sim.parent.parent / 'generators/radiance/src/main/resources/vsrc/vortex'
            names = ['VX_scan.sv'] if kind == 'scan' else ['VX_rr_arbiter.sv', 'VX_onehot_encoder.sv']
            filelist.write_text(''.join(str(vortex / 'hw/rtl/libs' / name) + '\n' for name in names))
            rtl_flags = '+define+SIMULATION -I' + str(vortex / 'hw/rtl')
            rtl_flags += ' -I' + str(vortex / 'hw/dpi')
        elif kind == 'radiance_cache':
            vortex = sim.parent.parent / 'generators/radiance/src/main/resources/vsrc/vortex'
            files = sorted((vortex / 'hw/rtl/cache').glob('*.sv'))
            files += sorted((vortex / 'hw/rtl/libs').glob('*.sv'))
            files += [vortex / relative for relative in (
                'hw/rtl/VX_gpu_pkg.sv', 'hw/rtl/fpu/VX_fpu_pkg.sv',
                'hw/rtl/mem/VX_mem_bus_if.sv', 'hw/rtl/mem/VX_mem_perf_if.sv',
                'third_party/fpnew/src/fpnew_pkg.sv')]
            filelist.write_text(''.join(str(p) + '\n' for p in files))
            rtl_flags = '+define+SIMULATION+PERF_ENABLE+FPU_FPNEW'
            rtl_flags += ' -I' + str(vortex / 'hw/dpi')
            rtl_flags += ' -I' + str(vortex / 'third_party/fpnew/src/common_cells/include')
        else:
            filelist.write_text(str(here / ('native_' + kind + '.sv')) + '\n')
        imported = build / 'design.mlir'
        run('python3', sim / 'native_hw.py', '--input', source, '--output', imported,
            '--filelist', filelist, '--work', build / 'rtl',
            '--circt-verilog', circt / 'circt-verilog', '--circt-opt', circt / 'circt-opt',
            '--cxx', os.environ.get('CXX', 'c++'),
            '--flags=' + rtl_flags)
        source = imported
        if kind in ('exports', 'readmem', 'readmem_multi'):
            from native_strings import check_contracts
            check_contracts(build / 'rtl.json')
    if kind in ('time', 'time_wakeup', 'delay_transport'):
        library = plugin(circt / 'circt-opt', os.environ.get('CXX', 'c++'),
                         sim / 'generated-src/compiler', sim / 'compiler/RTL.cpp')
        prepared = build / 'design.mlir'
        prepare_rtl(source, prepared, circt / 'circt-opt', library)
        source = prepared
    if kind in ('cfg_memory', 'packed_cfg'):
        from cfg_signal_memory import prepare as prepare_cfg_memory
        library = plugin(circt / 'circt-opt', os.environ.get('CXX', 'c++'),
                         sim / 'generated-src/compiler', sim / 'compiler/SignalMemory.cpp')
        prepared = build / 'design.mlir'
        prepare_cfg_memory(source, prepared, circt / 'circt-opt', library)
        source = prepared
    if kind == 'local_memory':
        from local_memory import prepare as prepare_local_memory
        library = plugin(circt / 'circt-opt', os.environ.get('CXX', 'c++'),
                         sim / 'generated-src/compiler', sim / 'compiler/LocalMemory.cpp')
        prepared = build / 'design.mlir'
        prepare_local_memory(source, prepared, circt / 'circt-opt', library)
        source = prepared
    memory_flags = ['--native-sparse-memories', '--native-combinational-models']
    if kind == 'sparse_memory':
        memory_flags += ['--sparse-memory-min-bits', '1']
    run('python3', sim / 'partition.py', '--input', source, '--output', build,
        '--top', top, '--circt-opt', circt / 'circt-opt', *memory_flags)
    if kind == 'compiled_combin':
        from collections import Counter
        instances = json.loads((build / 'partition.json').read_text())['instances']
        assert Counter(i['module'] for i in instances) == {
            'ArcLatch': 2, 'ArcAsyncRegBank': 4}
    run('python3', sim / 'compiler/partition.py', '--input', build / 'partitioned.mlir',
        '--output', build / 'outlined.mlirbc', '--circt-opt', circt / 'circt-opt',
        '--cache', sim / 'generated-src/compiler', '--max-ops',
        '4096' if kind == 'many_ports' else '8', '--emit-bytecode', '--native-feedback')
    run(circt / 'circt-opt', build / 'outlined.mlirbc', '--symbol-dce', '--canonicalize', '-o', build / 'model.mlir')
    observe = ['--observe-registers', '--observe-wires', '--observe-ports'] if variant.endswith('-debug') else []
    run('python3', sim / 'compiler/model.py', '--input', build / 'model.mlir',
        '--output', build / 'model.ll', '--state-file', build / 'model.json',
        '--arcilator', circt / 'arcilator', '--circt-opt', circt / 'circt-opt',
        '--mlir-translate', circt / 'mlir-translate', '--cache', sim / 'generated-src/compiler',
        '--max-ops', '1' if kind == 'arrays' else '16',
        '--partition', build / 'partition.json',
        '--', *observe, '--split-funcs-threshold=8192')
    if kind in ('scheduled_dpi', 'time_output', 'time_wakeup'):
        info, = json.loads((build / 'model.json').read_text())
        assert info['hasTimeReaders'] is (kind != 'scheduled_dpi')
    run('python3', sim / 'compiler/llvm.py', build / 'model.ll',
        '--llvm-opt', circt / 'opt', '--cache', sim / 'generated-src/compiler',
        '--cxx', os.environ.get('CXX', 'c++'),
        '--share-procedures' if args.share_procedures else '--no-share-procedures')
    run('python3', sim / 'compiler/codegen.py', '--input', build / 'model.ll',
        '--output', build / 'model.o', '--llvm-opt', circt / 'opt',
        '--llc', circt / 'llc', '--cache', sim / 'generated-src/compiler',
        '--cxx', os.environ.get('CXX', 'c++'), '--opt-level=0', '--llc-opt-level=2')
    if kind == 'dtm':
        filelist = build / 'blackboxes.f'
        filelist.write_text(str(sim.parent.parent / 'generators/riscv-sodor/src/main/resources/sodor/vsrc/SimDTM.v') + '\n')
    run('python3', sim / 'native.py', build / 'model.json', build / 'partition.json', build / 'model.cpp',
        *(['--filelist=' + str(filelist)] if filelist else []))
    link = shlex.split(subprocess.check_output([circt / 'llvm-config', '--libs', 'support'], text=True))
    sources, flags = [], ['+global-startup'] if kind == 'global_startup' else []
    if kind == 'compiled_combin':
        flags = [str(here / 'compiled_combin/reference.bin')]
    if kind == 'task_finish':
        flags = ['+max-cycles=2000']
    if kind in ('time', 'time_output', 'time_wakeup'):
        flags = [str(build / 'time.vcd')]
    if kind == 'files':
        link += ['-Wl,--wrap=chipyard_arc_sv_fopen', '-Wl,--wrap=chipyard_arc_sv_stream']
    if kind in ('task_finish', 'task_callbacks', 'task_effects', 'function_effects'):
        link += ['-Wl,--wrap=chipyard_arc_stop_fail']
    if kind == 'shift':
        # Python's independent arbitrary-precision integers provide expected
        # results; no production shift implementation participates here.
        bits, rng = 2051, random.Random(1979)
        mask = (1 << bits) - 1
        vectors = build / 'expected.txt'
        with vectors.open('w') as stream:
            for data in (0, 1, mask, 1 << (bits - 1), *[rng.getrandbits(bits) for _ in range(8)]):
                signed = data - (1 << bits) if data >> (bits - 1) else data
                for amount in (0, 1, 7, 8, 63, 64, 2048, 2050, 2051, 100000, 1 << 1000):
                    values = (data, amount, (data << amount) & mask if amount < bits else 0,
                              data >> amount if amount < bits else 0,
                              (signed >> amount) & mask if amount < bits else mask if signed < 0 else 0)
                    stream.write(' '.join(format(value, 'x') for value in values) + '\n')
        flags = [str(vectors)]
    if variant == 'tracerom':
        sources = [sim.parent.parent / 'generators/icenet/src/main/resources/csrc/TraceROM.cc']
        packets = build / 'packets.txt'
        packets.write_text('fedcba9876543210 ff 0\n0123456789abcdef ff 0\n8000000000001357 03 1\n')
        flags = ['+trace=' + str(packets), '+macaddr=102030405060']
    if kind == 'rtl':
        flags = ['+vcdfile=' + str(build / 'native.vcd'), '+dump-start=1']
    if kind == 'optional':
        drivers = sim.parent.parent / 'generators/radiance/src/main/resources/csrc'
        sources = [drivers / name for name in ('SimMemTrace.cc', 'SimMemTraceLogger.cc', 'SimEmulator.cc', 'SimMemFuzzer.cc')]
        packets = build / 'trace.txt'
        packets.write_text('0 STORE 0 0 33 0x8000000000001111 0xfedcba9876543210 8\n'
                           '0 LOAD 0 2 35 0x123456789abcdef 0x8877665544332211 4\n'
                           '1 STORE 0 1 34 0xfffffff001020304 0x123456789abcdef 2\n')
        flags = ['+memtracefile=' + str(packets), '+drj_rom=boot.rom', '+drj_dtb=tree.dtb', '+drj_bin=program.elf']
    if kind in ('unused_clock', 'scheduled_dpi'):
        flags = [str(build)]
        link += ['-Wl,--wrap=ArcModel_eval']
    if kind in ('properties', 'temporal'):
        link += ['-Wl,--wrap=chipyard_arc_assert_fail']
    if kind in ('sensitized_dpi', 'seeded_dpi'):
        model, = json.loads((build / 'model.json').read_text())
        ports = [p for p in model['states'] if p['type'] in ('input', 'output')]
        layout = ''.join(f'constexpr unsigned {p["name"]}Offset={p["offset"]};\n' for p in ports)
        layout += 'struct DpiPortLayout {const char *name; unsigned offset, bytes;};\n'
        layout += 'constexpr DpiPortLayout dpiPorts[] = {\n'
        layout += ''.join(f'{{"{p["name"]}", {p["offset"]}, {(p["numBits"] + 7) // 8}}},\n' for p in ports)
        layout += '};\n'
        (build / (kind + '_layout.h')).write_text(layout)
    run(*shlex.split(os.environ.get('CXX', 'c++')), '-O2', '-std=c++17', '-DARC_NATIVE_NO_MAIN',
        *(['-include', here / ('native_realtime_exports_api.h' if kind == 'realtime_exports' else 'native_array_exports_api.h')] if kind in ('array_exports', 'realtime_exports') else []),
        '-I' + str(build), '-I' + str(sim), '-I' + str(sim / 'native/include'), '-I' + str(circt.parent / 'include'),
        build / 'model.cpp', here / ('native_' + test_kind + '.cpp'), *runtime_objects('2'), build / 'model.o', *sources,
        '-L' + str(circt.parent / 'lib'), '-lCIRCTArcRuntime', *link, '-lz', '-lpthread', '-ldl',
        '-o', build / 'test')
    if kind in ('comb_print', 'procedural_feedback', 'aggregate_feedback'):
        check_combinational_trace(build / 'test', kind)
    elif kind == 'files':
        subprocess.run([build / 'test'], cwd=build, check=True)
    elif kind == 'procedural':
        result = subprocess.run([build / 'test'], capture_output=True, text=True, check=True)
        markers = [line for line in result.stdout.splitlines() if line.startswith('NATIVE_PROCEDURAL ')]
        if markers != ['NATIVE_PROCEDURAL 0 3', 'NATIVE_PROCEDURAL 10 6']:
            raise RuntimeError('procedural prints changed order, value, or count: ' + result.stdout)
        print(result.stdout, end='', flush=True)
    elif kind == 'tasks':
        result = subprocess.run([build / 'test'], capture_output=True, text=True, check=True)
        actual = [line.removeprefix('NATIVE_TASK ') for line in result.stdout.splitlines() if line.startswith('NATIVE_TASK ')]
        expected = [line.removeprefix('EXPECTED_TASK ') for line in result.stderr.splitlines() if line.startswith('EXPECTED_TASK ')]
        if not actual or actual != expected:
            raise RuntimeError('task print values, activation or count changed: ' + result.stdout)
        print(next(line for line in result.stderr.splitlines() if line.startswith('PASS:')), flush=True)
        print(f'PASS: {len(actual)} task prints match independent expected values and exact activation counts', flush=True)
    elif kind == 'real_format':
        result = subprocess.run([build / 'test'], capture_output=True, text=True, check=True)
        actual = [line.removeprefix('NATIVE_REAL ') for line in result.stdout.splitlines() if line.startswith('NATIVE_REAL ')]
        expected = [line.removeprefix('EXPECTED_REAL ') for line in result.stderr.splitlines() if line.startswith('EXPECTED_REAL ')]
        if not actual or actual != expected:
            raise RuntimeError('real trace values, activation or count changed: ' + result.stdout + result.stderr)
        print(next(line for line in result.stderr.splitlines() if line.startswith('PASS:')), flush=True)
        print(f'PASS: {len(actual)} mixed real/integer trace messages match independent values and exact activations', flush=True)
    elif kind == 'control':
        for verbose, failure in ((False, False), (True, False), (True, True)):
            result = subprocess.run([build / 'test', '+max-cycles=800',
                                     *(['+verbose'] if verbose else []),
                                     *(['+test-failure'] if failure else [])],
                                    capture_output=True, text=True)
            if result.returncode != int(failure):
                raise RuntimeError('native stop status mismatch: ' + result.stdout + result.stderr)
            if result.stdout.count('NATIVE_GUARD_MARKER') != int(verbose):
                raise RuntimeError('native print guard/reset mismatch: ' + result.stdout)
            expected = 'RTL stopped with failure' if failure else 'Completed after 779 simulation cycles'
            if expected not in result.stderr:
                raise RuntimeError('native stop guard/reset mismatch: ' + result.stderr)
        print('PASS: native print/stop controls, reset suppression, successful and failing termination', flush=True)
    elif kind == 'many_ports':
        result = subprocess.run([build / 'test'], capture_output=True, text=True, check=True)
        if result.stdout.count('NATIVE_PARTITION_LITERAL') != 10:
            raise RuntimeError('partitioned shared print literals or clock edges changed: ' + result.stdout)
        if result.stderr.count('NATIVE_PARTITION_STDERR') != 10:
            raise RuntimeError('partitioned shared stderr stream changed: ' + result.stderr)
        print(result.stdout, end='', flush=True)
    elif kind == 'format_strings':
        check_format_string_trace(build / 'test')
    elif kind == 'task_callbacks':
        check_task_callback_trace(build / 'test')
    elif kind in ('task_effects', 'function_effects', 'lexical_format', 'hierarchy_format'):
        check_hierarchy_trace(build / 'test')
    else:
        run(build / 'test', *flags)
    if kind in ('properties', 'temporal'):
        result = subprocess.run([build / 'test', '+test-failure', '+max-cycles=800'],
                                capture_output=True, text=True)
        if result.returncode != 1 or 'RTL assertion failed' not in result.stderr:
            raise RuntimeError('native assertion failure was lost: ' + result.stdout + result.stderr)
        print('PASS: native assertion failure propagates through the standard run loop', flush=True)
    if kind == 'scopes':
        run(build / 'test', '+scope-run')
    if kind == 'plusarg':
        run(build / 'test', '+mode=hex', '+wide=0x3_0123456789abcdef_fedcba9876543210',
            '+small=12', '+signed=18446744073709551616')
        run(build / 'test', '+mode=negative', '+wide=0', '+small=0', '+signed=-2')
        run(build / 'test', '+mode=formats',
            '+upper=1ffffffffffffffffffffffffffffffff', '+zero=37', '+lane%=777')
    if kind == 'optional':
        run(build / 'test', *flags, '+run-fuzzer', '+max-cycles=1000')
    if kind == 'rtl':
        waveform = (build / 'native.vcd').read_text()
        times = [int(line[1:]) for line in waveform.splitlines() if line.startswith('#')]
        if times != [500, 505] or '$timescale 1ps $end' not in waveform:
            raise RuntimeError('native waveform time/dump-start mismatch')
        if variant.endswith('-debug') and not all(name in waveform for name in ('first/rtl/rtl/q', 'second/rtl/rtl/q')):
            raise RuntimeError('native debug waveform did not retain imported RTL registers')
        print('PASS: native waveform timestamps, dump-start, and observed state', flush=True)

    if kind == 'array_exports':
        negative = subprocess.run([str((build / 'test').resolve()), '+array-export-fault'],
                                  capture_output=True, text=True)
        if negative.returncode != 1 or 'array atom return mismatch' not in negative.stderr:
            raise RuntimeError('native array export expected-value fault was not rejected')
        print('PASS: native array export one-bit expected-value fault rejected', flush=True)
    if kind in ('compiled_combin', 'array_exports', 'real_exports', 'realtime_exports', 'hierarchical_captures', 'delay_transport', 'delay_precision', 'delayed_signals', 'scheduled_dpi', 'time_output', 'time_wakeup', 'time', 'format_strings', 'task_finish', 'task_callbacks', 'task_effects', 'function_effects', 'lexical_format', 'hierarchy_format', 'test_plusargs', 'string_struct', 'static_formals', 'pointer_arrays', 'open_arrays', 'unpacked_imports', 'context_scopes', 'handles', 'string_imports', 'sparse_memory', 'write_only', 'dynamic_writes', 'case_effects', 'bit_builtins', 'bare_feedback', 'comb_print', 'procedural_feedback', 'aggregate_feedback', 'temporal', 'readmem', 'readmem_multi', 'unit_static', 'exports', 'dpi_abi', 'cfg_memory', 'packed_cfg', 'packed_struct', 'packed_union', 'local_memory', 'static_storage', 'global_storage', 'global_startup'):
        for level in (('0', '2', '3') if kind in ('array_exports', 'real_exports', 'realtime_exports', 'hierarchical_captures', 'delay_transport', 'delay_precision', 'delayed_signals') else ('0', '2')):
            bitcode, obj = build / ('abi-O' + level + '.bc'), build / ('abi-O' + level + '.o')
            executable = build / ('test-O' + level)
            run('python3', sim / 'compiler/codegen.py', '--input', build / 'model.ll',
                '--output', obj, '--bitcode', bitcode, '--llvm-opt', circt / 'opt',
                '--llc', circt / 'llc', '--cache', sim / 'generated-src/compiler',
                '--cxx', os.environ.get('CXX', 'c++'), '--opt-level=' + level,
                '--llc-opt-level=' + level)
            run(*shlex.split(os.environ.get('CXX', 'c++')), '-O' + level, '-std=c++17', '-DARC_NATIVE_NO_MAIN',
                *(['-include', here / ('native_realtime_exports_api.h' if kind == 'realtime_exports' else 'native_array_exports_api.h')] if kind in ('array_exports', 'realtime_exports') else []),
                '-I' + str(build), '-I' + str(sim), '-I' + str(sim / 'native/include'), '-I' + str(circt.parent / 'include'),
                build / 'model.cpp', here / ('native_' + test_kind + '.cpp'), *runtime_objects(level), obj,
                '-L' + str(circt.parent / 'lib'), '-lCIRCTArcRuntime', *link, '-lz', '-lpthread', '-ldl', '-o', executable)
            if kind in ('comb_print', 'procedural_feedback', 'aggregate_feedback'):
                check_combinational_trace(executable, kind)
            elif kind == 'format_strings':
                check_format_string_trace(build / ('test-O' + level))
            elif kind == 'task_callbacks':
                check_task_callback_trace(executable)
            elif kind in ('task_effects', 'function_effects', 'lexical_format', 'hierarchy_format'):
                check_hierarchy_trace(executable)
            else:
                run(executable, *flags)
