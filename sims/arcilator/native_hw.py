#!/usr/bin/env python3
"""Compile selected foreign RTL into CIRCT HW before native partitioning."""
import argparse
import json
import os
import re
import shlex
import subprocess
from pathlib import Path
from types import SimpleNamespace

from partition import IDENT, opcode, split, unquote, svparam, packed_width, svname, width
from native_exports import contracts
from rtl_libraries import discover as discover_libraries
from compiler.partition import plugin
from compiler.verilog.build import build as build_frontend


def bound_models():
    manifest = Path(__file__).parent / 'native/models.def'
    return set(
        re.findall(r'ARC_\w+_MODEL\(\w+,\s*"([^"]+)"\)', manifest.read_text()))


DECL = re.compile(
    r'^  (?:sv.verbatim.module|hw.module.extern) (?:private )?@(' + IDENT +
    r')(?:<.*?>)?(\(.*\))(?: attributes (.*))?$', re.M)
INSTANCE = re.compile(r'(\bhw.instance "(?:[^"\\]|\\.)*"(?: sym @' + IDENT +
                      r')? @)(' + IDENT + r')(?:<(.*?)>)?(?=\()')
NATIVE = re.compile(
    r'^  hw.module (?:private )?@(' + IDENT +
    r')(\(.*\))(?: attributes .*?)? \{$', re.M)


def ports(signature):
    result = {}
    for entry in split(signature[1:-1]):
        match = re.fullmatch(
            r'(in|out|inout) %?(' + IDENT + r')\s*:\s*(.*?)(?: \{.*\})?',
            entry)
        if not match:
            raise ValueError('unrecognized RTL port: ' + entry)
        direction, name, ty = match.groups()
        result[unquote(name)] = (direction, name, ty)
    return result


def write_if_changed(path, text):
    # Binding registry changes often leave the hardware identical. Preserve
    # timestamps so adding a device binding does not recompile every CPU model.
    if path.is_file() and path.read_text() == text:
        return
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(text)
    temporary.replace(path)


def sv_boundary(name, module, signature, parameters):
    """Elaborate source port connections before adapting HW clocks/aggregates.

    The actual SV declaration supplies omitted-input defaults and the signedness
    of output extension. Its elaborator also applies ordinary width truncation;
    these rules cannot be recovered from signless HW port types afterward.
    """
    declarations, connections = [], []
    for port, (direction, _, ty) in ports(signature).items():
        if direction not in ('in', 'out'):
            raise ValueError(
                f'native RTL boundary requires directed ports: {module}.{port}'
            )
        bits = width(ty) if ty == '!seq.clock' else packed_width(ty)
        if bits <= 0:
            raise ValueError(
                f'native RTL boundary requires a positive packed width: {module}.{port}'
            )
        vector = f'[{bits - 1}:0] ' if bits > 1 else ''
        identifier = svname(port)
        declarations.append(('input' if direction == 'in' else 'output') +
                            ' wire ' + vector + identifier)
        connections.append('.' + identifier + '(' + identifier + ')')
    parameter_values = ', '.join(svparam(p) for p in split(parameters))
    specialization = ' #(' + parameter_values + ')' if parameter_values else ''
    return ('module ' + svname(name) + '(\n  ' + ',\n  '.join(declarations) +
            '\n);\n  ' + svname(module) + specialization + ' rtl(\n    ' +
            ',\n    '.join(connections) + '\n  );\nendmodule\n')


def connect_wrapper(wrapper, signature, target, target_signature, module):
    expected, actual = ports(signature), ports(target_signature)
    if not expected.keys() <= actual.keys() or any(
            actual[n][0] != 'out' for n in actual.keys() - expected.keys()):
        raise ValueError(
            f'RTL port names do not match the declared boundary for {module}')
    lines = [f'  hw.module private @{wrapper}{signature} {{']
    inputs, outputs, results = [], [], []

    def convert(value, source_ty, target_ty):
        if source_ty == target_ty:
            return value
        op = {
            ('i1', '!seq.clock'): 'seq.to_clock',
            ('!seq.clock', 'i1'): 'seq.from_clock'
        }.get((source_ty, target_ty))
        name = f'%cast_{len(lines)}'
        if op:
            lines.append(f'    {name} = {op} {value}')
        elif packed_width(source_ty) == packed_width(target_ty):
            lines.append(
                f'    {name} = hw.bitcast {value} : ({source_ty}) -> {target_ty}'
            )
        else:
            raise ValueError(
                f'RTL port type mismatch for {module}: {source_ty} versus {target_ty}'
            )
        return name

    for name, (direction, quoted, ty) in actual.items():
        e_direction, e_quoted, e_ty = expected.get(name,
                                                   (direction, quoted, ty))
        if direction != e_direction:
            raise ValueError(
                f'RTL port direction mismatch for {module}.{name}')
        if direction == 'in':
            value = convert('%' + e_quoted, e_ty, ty)
            inputs.append(f'{quoted}: {value}: {ty}')
        elif direction == 'out':
            outputs.append(f'{quoted}: {ty}')
            results.append(f'%rtl_{len(results)}')
        else:
            raise ValueError(
                f'native imported RTL inout requires analog support: {module}.{name}'
            )
    result = ', '.join(results) + ' = ' if results else ''
    lines.append(f'    {result}hw.instance "rtl" @{target}(' +
                 ', '.join(inputs) + ') -> (' + ', '.join(outputs) + ')')
    values = {}
    for name, value in zip((n for n, p in actual.items() if p[0] == 'out'),
                           results):
        if name in expected:
            values[name] = convert(value, actual[name][2], expected[name][2])
    output_names = [n for n, p in expected.items() if p[0] == 'out']
    lines.append('    hw.output' +
                 (' ' + ', '.join(values[n] for n in output_names) + ' : ' +
                  ', '.join(expected[n][2]
                            for n in output_names) if output_names else ''))
    lines.append('  }')
    return '\n'.join(lines)


def prepare_native_signals(raw, core, circt_opt, library):
    memory_library = plugin(
        circt_opt, os.environ.get('CXX', 'c++'),
        Path(__file__).parent / 'generated-src/compiler',
        Path(__file__).parent / 'compiler/NativeMem2Reg.cpp')
    subprocess.run([
        str(circt_opt),
        str(raw), '--load-pass-plugin=' + str(library),
        '--load-pass-plugin=' + str(memory_library),
        '--pass-pipeline=builtin.module(chipyard-push-struct-updates,canonicalize,hw.module(chipyard-native-mem2reg),canonicalize,chipyard-native-signal-state,hw.module(llhd-sig2reg,llhd-remove-control-flow),hw-convert-bitcasts{allow-partial-conversion=true},canonicalize)',
        '-o',
        str(core)
    ],
                   check=True)


def prepare_rtl(raw, core, circt_opt, library, state_library=None):
    memory_library = plugin(
        circt_opt, os.environ.get('CXX', 'c++'),
        Path(__file__).parent / 'generated-src/compiler',
        Path(__file__).parent / 'compiler/NativeMem2Reg.cpp')
    if state_library is None:
        state_library = plugin(
            circt_opt, os.environ.get('CXX', 'c++'),
            Path(__file__).parent / 'generated-src/compiler',
            Path(__file__).parent / 'compiler/SignalState.cpp')
    # SCCP cannot safely propagate values across LLHD suspension edges and
    # opaque calls: it can replace a yielded DPI result with its initial value.
    # The matching SDK pipeline uses local canonicalization on these regions.
    # Interface references become ordinary signals after inlining. Promote
    # these again before inferring registers, then retain effect-only and
    # timed processes for Arc's coroutine lowering.
    pipeline = (
        'builtin.module(chipyard-restore-dpi-abi,chipyard-native-boundaries,chipyard-inline-instant-tasks,'
        'hw-flatten-modules,canonicalize,'
        'hw.module(sroa,mem2reg,chipyard-native-mem2reg,llhd-hoist-signals,'
        'chipyard-bitcast-float-signals,canonicalize,'
        'llhd-deseq,llhd-lower-processes,chipyard-bitcast-float-processes,canonicalize,llhd-combine-drives),'
        'chipyard-native-signal-state{feedback-only=true},hw.module(llhd-sig2reg,'
        'canonicalize,chipyard-unroll-loops,canonicalize,'
        'chipyard-infer-late-presets,canonicalize,'
        'chipyard-hoist-combinational-drives,canonicalize,'
        'chipyard-prepare-rtl-processes,canonicalize,llhd-deseq,'
        'canonicalize,llhd-lower-processes,chipyard-bitcast-float-processes,canonicalize,llhd-remove-control-flow,'
        'llhd-combine-drives,chipyard-coalesce-constant-drives,'
        'llhd-sig2reg,canonicalize),'
        # Expose aggregate read dependencies before Arc checks for loops.
        # A packed concat/bitcast can otherwise hide independent array lanes
        # whose constant-index reads feed later lanes in the same aggregate.
        'hw-convert-bitcasts{allow-partial-conversion=true},canonicalize,symbol-dce)'
    )
    subprocess.run([
        str(circt_opt),
        str(raw), '--load-pass-plugin=' + str(library), '--load-pass-plugin=' +
        str(state_library), '--load-pass-plugin=' + str(memory_library),
        '--pass-pipeline=' + pipeline, '-o',
        str(core)
    ],
                   check=True)


def compile_rtl(args):
    design = args.input.read_text()
    bound = bound_models()
    declarations = {}
    for match in DECL.finditer(design):
        symbol, signature, attrs = match.groups()
        verilog = re.search(r'verilogName = ("(?:[^"\\]|\\.)*")', attrs or '')
        module = unquote(verilog[1]) if verilog else unquote(symbol)
        if module in bound or module.startswith(
                'ClockSourceAtFreqFromPlusArg'):
            continue
        declarations[symbol] = (module, signature)
    variants = {}
    for line in design.splitlines():
        if opcode(line) != 'hw.instance':
            continue
        match = INSTANCE.search(line)
        if not match:
            raise ValueError('unrecognized HW instance: ' + line)
        _, symbol, parameters = match.groups()
        if symbol in declarations:
            variants.setdefault((symbol, parameters or ''),
                                f'ArcRTL_{len(variants)}')
    files = [
        Path(s) for s in shlex.split(args.filelist.read_text(), comments=True)
        if Path(s).suffix in ('.v', '.sv') and Path(s).stem != 'TestDriver'
    ]
    library_manifest = getattr(args, 'library_manifest', None)
    libraries = (json.loads(library_manifest.read_text()) if library_manifest
                 else discover_libraries(args.filelist))
    modules = {}
    native = {}
    # Only bridge native modules mentioned by the selected foreign sources.
    # The testbench references the generated harness itself, including analog
    # ports. It is not imported RTL and must not create a circular boundary.
    # The declarations carry ports for elaboration, never replacement logic.
    referenced = set()
    for path in files:
        code = re.sub(r'"(?:[^"\\]|\\.)*"|//[^\n]*|/\*.*?\*/',
                      '',
                      path.read_text(),
                      flags=re.S)
        referenced.update(re.findall(r'[\w$]+', code))
        for name in re.findall(r'\bmodule\s+(?:automatic\s+)?([\w$]+)\b',
                               code):
            modules.setdefault(name, path)
    imported, metadata = [], []
    runtime_controls = False
    runtime_formats = False
    args.work.mkdir(parents=True, exist_ok=True)
    native_declarations = []
    for match in NATIVE.finditer(design):
        symbol, signature = match.groups()
        name = unquote(symbol)
        if name not in referenced or name in modules:
            continue
        native[name] = (symbol, signature)
        declarations_sv = []
        for port, (direction, _, ty) in ports(signature).items():
            if direction not in ('in', 'out'):
                raise ValueError(
                    f'native RTL boundary requires directed ports: {name}.{port}'
                )
            bits = width(ty) if ty == '!seq.clock' else packed_width(ty)
            vector = f'[{bits - 1}:0] ' if bits > 1 else ''
            declarations_sv.append(
                f'{"input" if direction == "in" else "output"} wire {vector}{svname(port)}'
            )
        native_declarations.append(
            '(* chipyard_arc_native_boundary *) module ' + svname(name) + '(' +
            ', '.join(declarations_sv) + '); endmodule')
    native_file = args.work / 'native-boundaries.sv'
    write_if_changed(native_file, '\n'.join(native_declarations) + '\n')
    frontend = args.circt_verilog
    cache = Path(__file__).parent / 'generated-src/compiler'
    if variants and args.frontend_patches == 'auto' and frontend.name == 'circt-verilog':
        version = subprocess.check_output([frontend, '--version'], text=True)
        if '\nCIRCT firtool-1.162.0\n' in version:
            frontend = build_frontend(
                SimpleNamespace(circt_verilog=frontend,
                                cache=cache / 'verilog',
                                circt_source=None,
                                slang_source=None,
                                slang_build=None,
                                jobs=4))
    for (symbol, parameters), wrapper in variants.items():
        module, signature = declarations[symbol]
        if module not in modules:
            raise ValueError(
                f'no selected RTL source or native binding for {module}')
        source = modules[module]
        frontend_top = '__ChipyardArcPorts_' + wrapper
        while frontend_top in modules or frontend_top in native:
            frontend_top += '_'
        boundary = args.work / (wrapper + '-boundary.sv')
        write_if_changed(
            boundary, sv_boundary(frontend_top, module, signature, parameters))
        # Selected RTL shares package guards and simulator macros. Clock
        # inference would change a bit used by an interface to !seq.clock;
        # preserve the source types and convert only at the Chisel boundary.
        command = [
            str(frontend), '--single-unit', '--infer-clock-ports=false',
            '-DARCILATOR', '--Xslang=--allow-hierarchical-const', '--ir-llhd',
            '--timescale=1ns/1ps', '--top=' + frontend_top,
            str(source)
        ]
        for path in files:
            if path != source:
                command.append(str(path))
        if native:
            command.append(str(native_file))
        command.append(str(boundary))
        for directory in sorted({str(p.parent) for p in files}):
            command += ['-I', directory]
        command += libraries['flags']
        for flag in shlex.split(args.flags):
            if flag.startswith('+define+'):
                command += [
                    '-D' + value for value in flag[len('+define+'):].split('+')
                    if value
                ]
            elif flag.startswith('+incdir+'):
                command += [
                    '-I' + value for value in flag[len('+incdir+'):].split('+')
                    if value
                ]
            else:
                command.append(flag)
        raw, core = args.work / (wrapper + '.mlir'), args.work / (wrapper +
                                                                  '-core.mlir')
        early = args.work / (wrapper + '-llhd.mlir')
        sliced = args.work / (wrapper + '-slices.mlir')
        print('Importing native RTL: ' + module +
              ('<' + parameters + '>' if parameters else ''),
              flush=True)
        subprocess.run(command + ['-o', str(early)], check=True)
        library = plugin(args.circt_opt, args.cxx, cache,
                         Path(__file__).parent / 'compiler/RTL.cpp')
        signal_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/SignalWrites.cpp')
        union_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/UnionBits.cpp')
        contexts_library = plugin(args.circt_opt, args.cxx, cache,
                                  Path(__file__).parent / 'compiler/NativeContexts.cpp')
        hierarchy_library = plugin(args.circt_opt, args.cxx, cache,
                                   Path(__file__).parent / 'compiler/HierarchyFormats.cpp')
        # Preserve original reference ranges before the SDK drive hoister can
        # turn separate lane/bit writers into overlapping whole-value drivers.
        subprocess.run([
            str(args.circt_opt),
            str(early), '--load-pass-plugin=' + str(library),
            '--load-pass-plugin=' + str(hierarchy_library),
            '--load-pass-plugin=' + str(signal_library),
            '--load-pass-plugin=' + str(union_library),
            '--load-pass-plugin=' + str(contexts_library),
            '--pass-pipeline=builtin.module(chipyard-lower-unions,chipyard-native-boundaries,chipyard-restore-dpi-abi,chipyard-native-dpi-tasks,chipyard-inline-instant-tasks,chipyard-prune-open-dpi,chipyard-native-contexts{root=' + frontend_top + '},inline,chipyard-capture-hierarchy-formats{root=' + frontend_top + ' local=true},hw-flatten-modules,chipyard-resolve-native-contexts{root=' + frontend_top + '},chipyard-resolve-hierarchy-formats{root=' + frontend_top + ' local=true},hw.module(chipyard-split-signal-writes))',
            '-o',
            str(sliced)
        ],
                       check=True)
        exports_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/SignalMemory.cpp')
        strings_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/NativeStrings.cpp')
        formats_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/FormatStrings.cpp')
        handles_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/NativeHandles.cpp')
        strings = args.work / (wrapper + '-strings.mlir')
        subprocess.run([
            str(args.circt_opt),
            str(sliced), '--load-pass-plugin=' + str(strings_library),
            '--load-pass-plugin=' + str(formats_library),
            '--load-pass-plugin=' + str(library),
            '--load-pass-plugin=' + str(handles_library),
            '--pass-pipeline=builtin.module(chipyard-restore-dpi-abi,chipyard-native-handles,chipyard-prepare-format-strings,chipyard-native-strings)', '-o',
            str(strings)
        ],
                       check=True)
        sliced = strings
        exported = args.work / (wrapper + '-exports.mlir')
        subprocess.run([
            str(args.circt_opt),
            str(sliced), '--load-pass-plugin=' + str(exports_library),
            '--pass-pipeline=builtin.module(chipyard-native-signal-memory{exports=true captures-only=true},canonicalize)',
            '-o',
            str(exported)
        ],
                       check=True)
        sliced = exported
        state_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/SignalState.cpp')
        memory_promotion_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/NativeMem2Reg.cpp')
        # Preserve feedback initializers before the matching SDK pipeline
        # promotes unconditional signals. Registers/memories keep SDK inference.
        subprocess.run([
            str(args.circt_opt),
            str(sliced), '--load-pass-plugin=' + str(state_library),
            '--load-pass-plugin=' + str(memory_promotion_library),
            '--pass-pipeline=builtin.module(chipyard-prepare-native-llhd)',
            '-o',
            str(raw)
        ],
                       check=True)
        prepare_rtl(raw, core, args.circt_opt, library, state_library)
        state = args.work / (wrapper + '-state.mlir')
        prepare_native_signals(core, state, args.circt_opt, state_library)
        state.replace(core)
        local_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/LocalMemory.cpp')
        local = args.work / (wrapper + '-local.mlir')
        subprocess.run([
            str(args.circt_opt),
            str(core), '--load-pass-plugin=' + str(local_library),
            '--pass-pipeline=builtin.module(chipyard-native-local-memory,canonicalize)',
            '-o',
            str(local)
        ],
                       check=True)
        local.replace(core)
        memory_library = plugin(
            args.circt_opt, args.cxx, cache,
            Path(__file__).parent / 'compiler/SignalMemory.cpp')
        memory = args.work / (wrapper + '-memory.mlir')
        subprocess.run([
            str(args.circt_opt),
            str(core), '--load-pass-plugin=' + str(memory_library),
            '--pass-pipeline=builtin.module(chipyard-native-signal-memory,canonicalize)',
            '-o',
            str(memory)
        ],
                       check=True)
        memory.replace(core)
        text = core.read_text()
        top = re.search(
            r'^  hw.module @' + re.escape(frontend_top) +
            r'(\(.*?\))(?: attributes .*?)? \{', text, re.M)
        if not top:
            raise ValueError(f'CIRCT did not produce HW for {module}')
        if any(
                opcode(line).startswith('sim.func.dpi')
                for line in text.splitlines()):
            abi_library = plugin(args.circt_opt, args.cxx, cache,
                                 Path(__file__).parent / 'compiler/Model.cpp')
            checked = core.with_name(core.stem + '-dpi-checked.mlir')
            subprocess.run([
                str(args.circt_opt),
                str(core), '--load-pass-plugin=' + str(abi_library),
                '--pass-pipeline=builtin.module(chipyard-native-dpi-abi{validate-only=true})',
                '-o',
                str(checked)
            ],
                           check=True)
            checked.replace(core)
            text = core.read_text()
        runtime_controls |= any(name in text for name in
                                ('chipyard_arc_runtime_control',
                                 'chipyard_arc_export_scope_lookup',
                                 'chipyard_arc_export_finish',
                                 'chipyard_arc_export_finished'))
        # Late inlining can merge even literal format fragments through CFG
        # arguments; their native string transport also needs this component.
        runtime_formats |= ('chipyard.native_format' in text or
                            'sim.fmt.' in text)
        if any(
                opcode(line) in ('llhd.sig', 'llhd.drv')
                or opcode(line).startswith('moore.')
                for line in text.splitlines()):
            raise ValueError(
                f'{module} requires a native model binding: RTL import retained unresolved signals/drives'
            )
        expected, actual = ports(signature), ports(top[1])
        if not expected.keys() <= actual.keys() or any(
                actual[n][0] != 'out'
                for n in actual.keys() - expected.keys()):
            raise ValueError(
                f'RTL port names do not match the declared foreign boundary for {module}'
            )
        native_imports = []
        bound_declarations = {}
        for match in DECL.finditer(text):
            name = unquote(match[1])
            verilog = re.search(r'verilogName = ("(?:[^"\\]|\\.)*")', match[3]
                                or '')
            binding = unquote(verilog[1]) if verilog else name
            if name not in native and binding in bound:
                bound_declarations[match[1]] = (binding, bool(verilog))
                continue
            if name not in native:
                raise ValueError(
                    f'RTL import retained unresolved module {name}')
            target, target_signature = native[name]
            native_imports.append(
                (match[1], match[2], target, target_signature))

        def retained_declaration(match):
            binding = bound_declarations.get(match[1])
            if not binding:
                return ''
            name, explicit = binding
            if explicit:
                return match[0]
            # Keep the factory name stable while namespacing the HW symbol.
            attribute = 'verilogName = ' + json.dumps(name)
            attrs = match[3]
            if not attrs:
                return match[0] + ' attributes {' + attribute + '}'
            return match[0][:-len(attrs)] + attrs[:-1] + (
                ', ' if attrs[1:-1].strip() else '') + attribute + '}'

        text = DECL.sub(retained_declaration, text)
        # Namespace every imported symbol, including helper functions/memories.
        references = re.compile(r'@(' + IDENT + r')|"(?:[^"\\]|\\.)*"')
        symbols = {m[1] for m in references.finditer(text) if m[1]}

        def quoted_symbol(symbol):
            value = (wrapper + '_' + unquote(symbol)).encode('utf-8')
            return '"' + ''.join(
                chr(c) if 32 <= c < 127 and c not in (34, 92) else f'\\{c:02X}'
                for c in value) + '"'

        renames = {s: quoted_symbol(s) for s in symbols}
        text = references.sub(lambda m: '@' + renames[m[1]]
                              if m[1] else m[0], text)
        text = re.sub(r'(^  hw.module) @', r'\1 private @', text, flags=re.M)
        body = text[text.index('module {') + len('module {'):text.rfind('}')]
        imported.append(body)
        for name, native_signature, target, target_signature in native_imports:
            imported.append(
                connect_wrapper(renames[name], native_signature, target,
                                target_signature, unquote(name)))
        imported.append(
            connect_wrapper(wrapper, signature, renames[frontend_top], top[1],
                            module))
        metadata.append(
            dict(module=module,
                 symbol=unquote(symbol),
                 parameters=split(parameters),
                 wrapper=wrapper,
                 source=str(source),
                 boundary_source=str(boundary),
                 frontend_top=frontend_top,
                 exports=contracts(text),
                 native_modules=[
                     unquote(target) for _, _, target, _ in native_imports
                 ]))

    def replace_instance(match):
        prefix, symbol, parameters = match.groups()
        return prefix + variants[
            (symbol, parameters or '')] if symbol in declarations else match[0]

    design = '\n'.join(
        INSTANCE.sub(replace_instance, line) if opcode(line) ==
        'hw.instance' else line for line in design.splitlines()) + '\n'
    design = DECL.sub(lambda m: '' if m[1] in declarations else m[0], design)
    end = design.rfind('}')
    for needed, filename in ((runtime_controls, 'control.cpp'),
                             (runtime_formats, 'format_strings.cpp')):
        if not needed:
            continue
        support = str(Path(__file__).parent / 'native' / filename)
        entries = shlex.split(args.filelist.read_text(), comments=True)
        if support not in entries:
            # The original foreign-driver builder accepts extra C++ sources.
            # Select additive control/format support through the original
            # foreign-driver builder, preserving its existing runtime inputs.
            original_files = args.filelist.read_text()
            write_if_changed(args.filelist, original_files.rstrip('\n') +
                             '\n' + shlex.quote(support) + '\n')
    write_if_changed(args.output,
                     design[:end] + '\n'.join(imported) + '\n' + design[end:])
    write_if_changed(args.work.parent / 'rtl.json',
                     json.dumps(metadata, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'output', 'filelist', 'work', 'circt-verilog',
                 'circt-opt'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--flags', default='')
    parser.add_argument('--library-manifest', type=Path)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--frontend-patches',
                        choices=('auto', 'off'),
                        default='auto')
    try:
        compile_rtl(parser.parse_args())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'native RTL import: {error}\n')
