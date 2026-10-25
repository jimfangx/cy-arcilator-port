#!/usr/bin/env python3
"""Compile Arc with late array normalization and bounded model procedures."""
import argparse
import json
import re
from pathlib import Path
import subprocess

from partition import plugin


def boolean(value):
    if value in (True, 'true', '1'):
        return True
    if value in (False, 'false', '0'):
        return False
    raise argparse.ArgumentTypeError('expected true or false')


def compile_model(args):
    late_wide = getattr(args, 'late_wide_arithmetic', True)
    flags = args.flags[1:] if args.flags[:1] == ['--'] else args.flags
    # Arcilator validates the complete option list. Mirror options whose
    # effects belong to the stages resumed below, including explicit false.
    options = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    for name in ('no-runtime', 'no-generate-driver', 'trace-taps'):
        options.add_argument('--' + name,
                             type=boolean,
                             nargs='?',
                             const=True,
                             default=False)
    for name, default in (('dedup', True), ('detect-enables', True),
                          ('detect-resets', False), ('lookup-tables',
                                                     True), ('inline', True)):
        options.add_argument('--' + name,
                             type=boolean,
                             nargs='?',
                             const=True,
                             default=default)
    options.add_argument('--split-funcs-threshold', type=int)
    options.add_argument('--extra-runtime-args', default='')
    options.add_argument('--jit-vcd-file', default='')
    options.add_argument('--jit-fst-file', default='')
    lower, _ = options.parse_known_args(flags)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    work = args.output.with_name(args.output.name + '.stages')
    work.mkdir(exist_ok=True)
    library = plugin(args.circt_opt, args.cxx, args.cache,
                     Path(__file__).with_name('Model.cpp'))
    load = '--load-pass-plugin=' + str(library)
    startup = plugin(args.circt_opt, args.cxx, args.cache,
                     Path(__file__).with_name('StartupState.cpp'))
    load_startup = '--load-pass-plugin=' + str(startup)
    convergence = plugin(args.circt_opt, args.cxx, args.cache,
                         Path(__file__).with_name('Convergence.cpp'))
    load_convergence = '--load-pass-plugin=' + str(convergence)
    sampling = plugin(args.circt_opt, args.cxx, args.cache,
                      Path(__file__).with_name('SamplingFreeze.cpp'))
    load_sampling = '--load-pass-plugin=' + str(sampling)
    idle = plugin(args.circt_opt, args.cxx, args.cache,
                  Path(__file__).with_name('IdleClock.cpp'))
    load_idle = '--load-pass-plugin=' + str(idle)
    arrays = plugin(args.circt_opt, args.cxx, args.cache,
                    Path(__file__).with_name('LowerArrays.cpp'))
    load_arrays = '--load-pass-plugin=' + str(arrays)
    properties = plugin(args.circt_opt, args.cxx, args.cache,
                        Path(__file__).with_name('StateProperties.cpp'))
    load_properties = '--load-pass-plugin=' + str(properties)
    i0 = plugin(args.circt_opt, args.cxx, args.cache,
                Path(__file__).with_name('RemoveI0Types.cpp'))
    load_i0 = '--load-pass-plugin=' + str(i0)
    compact = plugin(args.circt_opt, args.cxx, args.cache,
                     Path(__file__).with_name('CompactStructs.cpp'))
    load_compact = '--load-pass-plugin=' + str(compact)
    plusargs = plugin(args.circt_opt, args.cxx, args.cache,
                      Path(__file__).with_name('NativePlusArgs.cpp'))
    load_plusargs = '--load-pass-plugin=' + str(plusargs)
    formats = plugin(args.circt_opt, args.cxx, args.cache,
                     Path(__file__).with_name('FormatStrings.cpp'))
    load_formats = '--load-pass-plugin=' + str(formats)
    arc_llvm = plugin(args.circt_opt, args.cxx, args.cache,
                      Path(__file__).with_name('ArcToLLVM.cpp'))
    load_arc_llvm = '--load-pass-plugin=' + str(arc_llvm)
    rounding = plugin(args.circt_opt, args.cxx, args.cache,
                      Path(__file__).with_name('FloatMath.cpp'))
    load_rounding = '--load-pass-plugin=' + str(rounding)
    wide_load = []
    if late_wide:
        wide = plugin(args.circt_opt, args.cxx, args.cache,
                      Path(__file__).with_name('WideArithmetic.cpp'))
        wide_load = ['--load-pass-plugin=' + str(wide)]

    def run(*command):
        subprocess.run([str(value) for value in command], check=True)

    def transform(source, target, pipeline):
        run(args.circt_opt, work / source, load, load_convergence,
            load_sampling, load_idle, load_arrays, load_properties, load_i0,
            load_compact, load_plusargs, load_formats, load_arc_llvm, load_startup,
            *wide_load, '--pass-pipeline=builtin.module(' + pipeline + ')',
            '--emit-bytecode', '-o', work / target)

    # The stock state-allocation pipeline removes singleton arrays without
    # adapting every array_concat. Earlier canonicalization can recreate those
    # concats, so normalize immediately before removal, after arc optimization.
    # Resume the matching SDK optimization pipeline after arc conversion. Its
    # enable inference creates an integer constant with an aggregate type for
    # enabled array/structure state. The adapted pass creates a packed zero
    # and bitcasts it to the original type; all other optimizations are retained.
    # Imported specializations use distinct local DPI symbols but may refer to
    # one C function. The SDK must see a single compatible declaration before
    # it creates the C-ABI wrappers. Call sites and their ordering stay intact.
    # Seed only original output formals with the native two-state zero. The
    # SDK then initializes their pointer buffers using its inout copy-in path;
    # real inouts and refs retain their original inputs and the C ABI is unchanged.
    run(
        args.circt_opt, args.input, load, load_convergence, load_rounding,
        *wide_load, '--pass-pipeline=builtin.module(' +
        ('chipyard-restore-wide-arithmetic,' if late_wide else '') +
        'chipyard-lower-real-rounding,chipyard-sensitize-dpi{freeze-sampling='
        + str(args.freeze_sampling).lower() +
        '},chipyard-record-output-effects,chipyard-native-dpi-abi,chipyard-coalesce-dpi,chipyard-seed-dpi-outputs)',
        '--emit-bytecode', '-o', work / 'imports.mlirbc')
    run(args.arcilator, work / 'imports.mlirbc', *flags,
        '--until-after=arc-conv', '--emit-mlir', '-o',
        work / 'converted.mlirbc')
    startup_inputs = []
    if args.partition is not None:
        scope_part = json.loads(args.partition.read_text())
        for instance in scope_part['instances']:
            if instance['module'] not in ('ArcDpiScope', 'ArcDpiScopeRoot', 'ArcSignalMemory'):
                continue
            if instance['module'] == 'ArcSignalMemory':
                port = next(p for p in instance['ports'] if p['name'] == 'handle')
            else:
                port, = instance['ports']
            if (port['name'] != ('handle' if instance['module'] == 'ArcSignalMemory' else 'scope') or port['direction'] != 'out' or
                    port['bits'] != 64 or
                    not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', port['wire']) or
                    port['wire'] in startup_inputs):
                raise ValueError('invalid native initialized input contract')
            startup_inputs.append(port['wire'])
    startup_state = 'chipyard-lower-native-startup-state{initialized-inputs=' + ':'.join(startup_inputs) + '}'
    optimize = ['chipyard-inline-immutable-arcs',
                'chipyard-lower-procedural-dpi', 'arc-split-loops']
    if lower.dedup:
        optimize.append('arc-dedup')
    optimize += [
        'chipyard-infer-state-properties{enables=' +
        str(lower.detect_enables).lower() + ' resets=' +
        str(lower.detect_resets).lower() + '}', 'cse', 'arc-canonicalizer',
        'hw.module(arc-merge-taps)'
    ]
    if lower.lookup_tables:
        optimize.append('arc-make-tables')
    optimize += [
        'cse', 'arc-canonicalizer', 'chipyard-record-time-dependencies',
        startup_state, 'chipyard-freeze-combinational-dpi'
    ]
    if lower.inline:
        optimize.append('arc-inline')
    optimize += [
        'arc-merge-ifs', 'cse', 'arc-canonicalizer', 'arc-lower-coroutines'
    ]
    transform('converted.mlirbc', 'state.mlir', ','.join(optimize))
    trace = lower.jit_vcd_file or lower.jit_fst_file
    allocate = 'arc-allocate-state' + ('{trace-taps=true}'
                                       if lower.trace_taps or trace else '')
    pipeline = (
        'arc-lower-arcs-to-funcs,chipyard-normalize-arrays,chipyard-remove-i0-types,'
        'arc.model(' + allocate +
        ',arc-allocate-state),arc-lower-clocks-to-funcs')
    if lower.split_funcs_threshold is not None:
        pipeline += ',arc-split-funcs{split-bound=' + str(
            lower.split_funcs_threshold) + '}'
    observe = trace or lower.trace_taps or lower.extra_runtime_args or any(
        'observe' in flag for flag in flags)
    pipeline += ',chipyard-certify-write-only-outputs{enabled=' + str(
        args.output_convergence and not observe).lower() + '}'
    held = []
    if args.partition is not None:
        part = json.loads(args.partition.read_text())
        phases = part.get('phase_clocks') if isinstance(part, dict) else None
        if (not isinstance(phases, list) or any(
                not isinstance(clock, dict)
                or not isinstance(clock.get('held'), str)
                or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', clock['held'])
                for clock in phases)):
            raise ValueError('invalid native phase-clock input contract')
        held = [clock['held'] for clock in phases]
        if len(set(held)) != len(held):
            raise ValueError('duplicate native phase-clock input')
    if held:
        pipeline += ',chipyard-certify-idle-clock{enabled=' + str(
            args.idle_clocks
            and not observe).lower() + ' stable-evaluator=' + str(
                getattr(args, 'stable_evaluator',
                        True)).lower() + ' held-inputs=' + ':'.join(held) + '}'
    pipeline += (
        ',cse,arc-canonicalizer,chipyard-export-model-info{output-file=' +
        json.dumps(str(args.state_file.resolve())) + '}')
    transform('state.mlir', 'allocated.mlirbc', pipeline)

    pipeline = [] if lower.no_generate_driver else ['arc-generate-driver']
    pipeline.append('hw-convert-bitcasts{allow-partial-conversion=false}')
    if not lower.no_runtime:
        extra = lower.extra_runtime_args
        if trace:
            extra = ('vcd' if lower.jit_vcd_file else 'fst') + (';' + extra if
                                                                extra else '')
        pipeline.append('arc-insert-runtime{extra-args=' + json.dumps(extra) +
                        ' trace-file=' + json.dumps(trace) + '}')
    pipeline += [
        'chipyard-lower-arrays', 'chipyard-lower-format-strings',
        'chipyard-lower-real-formats', 'chipyard-lower-native-files',
        'chipyard-native-plusargs', 'canonicalize'
    ]
    if late_wide:
        pipeline.append('chipyard-lower-wide-arithmetic')
    transform('allocated.mlirbc', 'arrays.mlirbc', ','.join(pipeline))
    source = 'arrays.mlirbc'
    if args.max_ops:
        transform(
            source, 'outlined.mlirbc', 'chipyard-outline-model{max-ops=' +
            str(args.max_ops) + ' max-ports=' + str(args.max_ports) + '}')
        source = 'outlined.mlirbc'
    transform(
        source, 'llvm.mlirbc',
        'arc-infer-context,chipyard-lower-arc-to-llvm,reconcile-unrealized-casts,chipyard-finalize-dpi-abi,cse,arc-canonicalizer,chipyard-compact-llvm-structs'
    )
    run(args.mlir_translate, '--mlir-to-llvmir', work / 'llvm.mlirbc', '-o',
        args.output)
    # Keep failed stages for diagnosis. Successful builds retain their LLVM
    # output and SDK JSON; discard only intermediates owned by this invocation.
    for name in ('imports.mlirbc', 'converted.mlirbc', 'state.mlir',
                 'allocated.mlirbc', 'arrays.mlirbc', 'outlined.mlirbc',
                 'llvm.mlirbc'):
        (work / name).unlink(missing_ok=True)
    if not any(work.iterdir()):
        work.rmdir()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'output', 'state-file', 'arcilator', 'circt-opt',
                 'mlir-translate', 'cache'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--max-ops', type=int, default=16384)
    parser.add_argument('--max-ports', type=int, default=16384)
    parser.add_argument('--freeze-sampling',
                        type=boolean,
                        default=True,
                        help='skip phase-only combinational DPI cache updates')
    parser.add_argument('--output-convergence', type=boolean, default=True)
    parser.add_argument('--idle-clocks', type=boolean, default=True)
    parser.add_argument('--stable-evaluator', type=boolean, default=True)
    parser.add_argument(
        '--late-wide-arithmetic',
        type=boolean,
        default=True,
        help='retain proven-pure wide arithmetic through Arc optimization')
    parser.add_argument('--partition',
                        type=Path,
                        help='native phase-clock input contract')
    parser.add_argument('flags', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.max_ops < 0 or not 0 < args.max_ports < 32768:
        parser.error(
            'use a nonnegative operation budget and a port budget below 32768')
    try:
        compile_model(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'CIRCT model compilation: {error}\n')


if __name__ == '__main__':
    main()
