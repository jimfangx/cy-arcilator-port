#!/usr/bin/env python3
"""Partition FIRRTL-generated HW at its declared foreign-module boundaries.

The HW assembly is produced and normalized by the matching CIRCT tools.  No
Verilog source rewriting or peripheral-specific port descriptions are used.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
from contextlib import ExitStack
from pathlib import Path
from compiler.partition import plugin

IDENT = r'(?:"(?:[^"\\]|\\.)*"|[\w.$-]+)'
SSA = r'%[\w.$-]+(?:#\d+)?'
QUOTED = r'("(?:[^"\\]|\\.)*")'


def opcode(line):
    """Read the operation token, without inspecting string-valued attributes."""
    result = SSA + r'(?::\d+)?'
    match = re.match(
        r'\s*(?:' + result + r'(?:,\s*' + result + r')*\s*=\s*)?([\w.]+)',
        line)
    return match[1] if match else ''


def map_code(text, transform):
    """Transform assembly outside string literals, which are opaque payloads."""
    return ''.join(part if i % 2 else transform(part)
                   for i, part in enumerate(re.split(QUOTED, text)))


def split(text):
    """Split an assembly list, respecting strings and nested types/attributes."""
    parts, start, stack, quoted, escaped = [], 0, [], False, False
    for i, c in enumerate(text):
        if quoted:
            if escaped:
                escaped = False
            elif c == '\\':
                escaped = True
            elif c == '"':
                quoted = False
        elif c == '"':
            quoted = True
        elif c in '(<[{':
            stack.append(c)
        elif c in ')>]}':
            # The arrow in function types is not a closing angle bracket.
            if c == '>' and i and text[i - 1] == '-':
                continue
            if stack:
                stack.pop()
        elif c == ',' and not stack:
            parts.append(text[start:i].strip())
            start = i + 1
    if text[start:].strip():
        parts.append(text[start:].strip())
    return parts


def unquote(value):
    if value.startswith('"'):
        # MLIR permits hexadecimal byte escapes, escaped quotes, and escaped
        # backslashes. Decode bytes together so UTF-8 is preserved as well.
        encoded = value[1:-1].encode('utf-8')
        return re.sub(
            rb'\\([\\"]|[0-9A-Fa-f]{2})', lambda m: m[1]
            if len(m[1]) == 1 else bytes([int(m[1], 16)]),
            encoded).decode('utf-8')
    return value


def svstring(value):
    # SV does not recognize JSON's Unicode escapes. Octal byte escapes retain
    # exact strings, including control characters and non-ASCII text.
    return '"' + ''.join('\\' + chr(c) if c in (
        34, 92) else chr(c) if 32 <= c < 127 else f'\\{c:03o}'
                         for c in value.encode('utf-8')) + '"'


def width(ty):
    if ty == '!seq.clock':
        return 1
    if re.fullmatch(r'i[1-9]\d*', ty):
        return int(ty[1:])
    raise ValueError(
        f'foreign boundary type {ty} is not a flattened integer or clock')


def packed_width(ty):
    """Bit width of an integer or nested packed HW array/structure."""
    spelling = ty.removeprefix('!hw.')
    if re.fullmatch(r'i[1-9]\d*', spelling):
        return int(spelling[1:])
    array = re.fullmatch(r'array<(\d+)x(.*)>', spelling)
    if array:
        return int(array[1]) * packed_width(array[2].strip())
    if spelling.startswith('struct<') and spelling.endswith('>'):
        total = 0
        for field in split(spelling[len('struct<'):-1]):
            entry = re.fullmatch('(' + IDENT + r'):\s*(.*)', field)
            if not entry:
                raise ValueError('unsupported packed structure field: ' +
                                 field)
            total += packed_width(entry[2])
        return total
    raise ValueError('unsupported asynchronous register type: ' + ty)


def svname(name):
    name = unquote(name)
    # Escaping is also necessary for ordinary-looking SV keywords such as
    # "signed". Escaped and simple spellings denote the same legal identifier.
    return '\\' + name + ' '


def svparam(param):
    name, rest = param.split(':', 1)
    ty, value = rest.split('=', 1)
    value = value.strip()
    if value.startswith('"'):
        value = svstring(unquote(value))
    elif re.fullmatch(r'[us]?i\d+', ty.strip()):
        bits = int(re.search(r'\d+', ty)[0])
        number = {'true': 1, 'false': 0}.get(value)
        if number is None:
            number = int(value, 16 if value.lower().startswith('0x') else 10)
        value = f"{bits}'h{number % (1 << bits):x}"
    elif ty.strip() in ('f32', 'f64'):
        float(
            value
        )  # Reject non-numeric attributes instead of changing their meaning.
    else:
        raise ValueError(f'unsupported foreign-module parameter: {param}')
    return f'.{svname(name.strip())}({value})'


def procedural_guard(condition, statements, tag):
    # SCF's condition propagation scans all users of each if-condition. A
    # shared enable with hundreds of thousands of print sites makes that
    # quadratic. This external identity gives each guard a separate condition;
    # it runs only when the enclosing procedural event has actually triggered.
    # Use i32 for an unambiguous C ABI in both native and hybrid runtimes.
    return (
        f'      %arc_guard_input_{tag} = arith.extui {condition} : i1 to i32\n'
        f'      %arc_guard_value_{tag} = func.call @chipyard_arc_guard(%arc_guard_input_{tag}) : (i32) -> i32\n'
        f'      %arc_guard_condition_{tag} = arith.trunci %arc_guard_value_{tag} : i32 to i1\n'
        f'      scf.if %arc_guard_condition_{tag} {{\n' + statements +
        '\n      }')


class FileLines:
    """Repeatable line iteration over a byte range of canonical MLIR assembly."""

    def __init__(self, path, start=0, end=None):
        self.path, self.start, self.end = Path(path), start, end

    def splitlines(self):
        with self.path.open('rb') as stream:
            stream.seek(self.start)
            position = self.start
            while self.end is None or position < self.end:
                line = stream.readline()
                if not line:
                    break
                position += len(line)
                yield line.decode('utf-8').rstrip('\r\n')


class LineSink:
    """The append/count interface used by rewrites, backed by an output file."""

    def __init__(self, path):
        self.path, self.count = Path(path), 0
        self.stream = self.path.open('w')

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.stream.close()

    def append(self, line):
        self.stream.write(line + '\n')
        self.count += 1

    def extend(self, lines):
        for line in lines:
            self.append(line)

    def __iadd__(self, lines):
        self.extend(lines)
        return self

    def __len__(self):
        return self.count

    def close(self):
        self.stream.close()
        return FileLines(self.path)


def root_range(path, top):
    pattern = re.compile(r'^  hw.module @' + re.escape(top) +
                         r'\((.*?)\)(?: attributes .*?)? \{\n$')
    start = 0
    with Path(path).open('rb') as stream:
        for raw in stream:
            if raw.startswith(b'  hw.module @'):
                match = pattern.match(raw.decode('utf-8'))
                if match:
                    signature = match[1]
                    body_start = start + len(raw)
                    position = body_start
                    for line in stream:
                        if line.startswith(b'  }'):
                            return signature, start, body_start, position, position + 3
                        position += len(line)
                    break
            start += len(raw)
    raise ValueError('cannot find flattened top module ' + top)


def copy_prefix(source, output, end, declarations):
    """Copy module-level IR, inserting declarations at the outer module."""
    position, injected = 0, False
    with Path(source).open('rb') as stream:
        while position < end:
            raw = stream.readline()
            if not raw or position + len(raw) > end:
                raise ValueError('native module prefix ends inside a line')
            position += len(raw)
            line = raw.decode('utf-8')
            if (not injected and re.match(r'^(?:builtin\.)?module\b', line)
                    and line.rstrip().endswith('{')):
                line = line.rstrip('\r\n') + '\n' + declarations + '\n'
                injected = True
            output.write(line.encode('utf-8'))
    if not injected:
        raise ValueError('flattened hardware is missing its outer module')


def lower_special_ops(body,
                      externs,
                      wide_helpers,
                      output=None,
                      native_combin=False):
    """Keep asynchronous state exact and lower assertions to native callbacks."""
    if output is not None:
        with LineSink(output) as lines:
            _lower_special_ops(body, externs, wide_helpers, lines,
                               native_combin)
        return FileLines(output)
    lines = []
    _lower_special_ops(body, externs, wide_helpers, lines, native_combin)
    return '\n'.join(lines)


def _lower_special_ops(body,
                       externs,
                       wide_helpers,
                       lines,
                       native_combin=False):
    groups, aggregate_results = {}, []
    formats = {}
    reserved = set()
    for line in body.splitlines():
        if native_combin:
            reserved.update(re.findall(SSA, line))
        m = re.match(r'\s*(' + SSA + r') = (sim\.fmt\..*)', line)
        if m:
            formats[m[1]] = m[2]
    source_lines = body.splitlines()
    if native_combin:
        from compiler.native_combin import lower_native_combin
        source_lines = lower_native_combin(source_lines, externs, reserved)
    for line in source_lines:
        if line.strip().startswith('sim.proc.print'):
            # Formatting operations must stay in the procedural region. If
            # outlined into arcs returning !sim.fstring, LLVM cannot fold them.
            cloned = {}

            def clone(match):
                value = match[0]
                if value not in formats:
                    return value
                if value not in cloned:
                    expr = map_code(formats[value],
                                    lambda code: re.sub(SSA, clone, code))
                    name = f'%arc_fmt_{len(lines)}'
                    lines.append(f'      {name} = {expr}')
                    cloned[value] = name
                return cloned[value]

            line = map_code(line, lambda code: re.sub(SSA, clone, code))
            if 'chipyard.file_output' not in line:
                line = procedural_guard('%arc_printf_enable', line, len(lines))
        elif opcode(line) in ('func.call',
                              'call') and 'chipyard.native_print' in line:
            if 'chipyard.file_output' not in line:
                line = procedural_guard('%arc_printf_enable', line, len(lines))
        if opcode(line) in ('comb.shl', 'comb.shru', 'comb.shrs'):
            shift = re.fullmatch(
                r'\s*(' + SSA + r') = comb\.(shl|shru|shrs)(?: bin)? (' + SSA +
                r'), (' + SSA + r')(?: \{.*\})? : i([1-9]\d*)', line)
            if not shift:
                raise ValueError(f'unrecognized shift: {line}')
            result, operation, data, amount, bits = shift.groups()
            bits = int(bits)
            if bits > 1024:
                # Keep the operation in the native dataflow. Exporting these
                # operands as foreign ports adds settling iterations for each
                # dependent shift, while LLVM's wide variable-shift expansion
                # consumes excessive compiler memory. The LLVM bridge calls a
                # linear byte implementation directly from this evaluation.
                mode = {'shl': 0, 'shru': 1, 'shrs': 2}[operation]
                name = f'chipyard_arc_wide_shift_{mode}_{bits}'
                wide_helpers.add((name, bits))
                lines.append(
                    f'    {result} = sim.func.dpi.call @{name}({data}, {amount}) : (i{bits}, i{bits}) -> i{bits}'
                )
                continue
        if opcode(line) == 'seq.firreg' and re.search(
                r' clock ' + SSA + r' reset async ', line):
            m = re.fullmatch(
                r'\s*(' + SSA + r') = seq.firreg (' + SSA + r') clock (' +
                SSA + r') reset async (' + SSA + r'), (' + SSA +
                r')(?: \{.*\})? : (.*)', line)
            if not m:
                raise ValueError(f'unrecognized asynchronous register: {line}')
            result, data, clock, reset, reset_data, ty = m.groups()
            bits = packed_width(ty)
            if bits <= 0:
                raise ValueError('empty asynchronous register type: ' + ty)
            if not re.fullmatch(r'i[1-9]\d*', ty):
                # Keep CIRCT's packed layout in bitcasts. Only the bank's ABI
                # uses integers; the surrounding hardware retains its types.
                prefix = f'%arc_async_cast_{len(lines)}'
                lines.append(
                    f'    {prefix}_d = hw.bitcast {data} : ({ty}) -> i{bits}')
                lines.append(
                    f'    {prefix}_reset = hw.bitcast {reset_data} : ({ty}) -> i{bits}'
                )
                aggregate_results.append(
                    f'    {result} = hw.bitcast {prefix}_q : (i{bits}) -> {ty}'
                )
                result, data, reset_data = prefix + '_q', prefix + '_d', prefix + '_reset'
            groups.setdefault((clock, reset), []).append(
                (result, data, reset_data, bits))
        elif opcode(line) in ('verif.clocked_assert', 'verif.clocked_assume'):
            m = re.fullmatch(
                r'\s*verif.clocked_(?:assert|assume) (' + SSA + r')(?: if (' +
                SSA + r'))?, (posedge|negedge|edge) (' + SSA +
                r')(?: label \".*\")? : i1', line)
            if not m:
                raise ValueError(f'unrecognized clocked assertion: {line}')
            predicate, enable, edge, clock = m.groups()
            idx = len(lines)
            prefix = f'%arc_assert_{idx}'
            lines += [
                f'    {prefix}_true = hw.constant true',
                f'    {prefix}_bad = comb.xor {predicate}, {prefix}_true : i1'
            ]
            bad = f'{prefix}_bad'
            if enable:
                lines.append(
                    f'    {prefix}_enabled = comb.and {bad}, {enable} : i1')
                bad = f'{prefix}_enabled'
            lines.append(
                f'    {prefix}_stoppable = comb.and {bad}, %arc_stop_enable : i1'
            )
            bad = f'{prefix}_stoppable'
            for active_edge in ('posedge',
                                'negedge') if edge == 'edge' else (edge, ):
                active_clock = clock
                if active_edge == 'negedge':
                    lines.append(
                        f'    {prefix}_inv = comb.xor {clock}, {prefix}_true : i1'
                    )
                    active_clock = f'{prefix}_inv'
                lines += [
                    f'    {prefix}_{active_edge} = seq.to_clock {active_clock}',
                    f'    sim.triggered {prefix}_{active_edge} if {bad} {{',
                    '      func.call @chipyard_arc_assert_fail() : () -> ()',
                    '    }'
                ]
        elif line.strip().startswith('sim.terminate '):
            m = re.fullmatch(
                r'\s*sim.terminate (success|failure), (quiet|verbose)', line)
            if not m:
                raise ValueError(f'unrecognized native termination: {line}')
            callback = 'chipyard_arc_finish' if m[
                1] == 'success' else 'chipyard_arc_stop_fail'
            lines.append(
                procedural_guard(
                    '%arc_stop_enable',
                    f'        func.call @{callback}() : () -> ()', len(lines)))
        else:
            lines.append(line)
    # Giant concatenated banks turn into very wide LLVM integers with costly
    # legalization. Bound each boundary vector, including individual registers
    # wider than one bank; all pieces retain the original clock and reset.
    bank_limit, banks, reassemble = 1024, [], []
    for (clock, reset), regs in groups.items():
        bank, bank_bits = [], 0
        for result, data, reset_data, bits in regs:
            pieces = [(result, data, reset_data, bits)]
            if bits > bank_limit:
                pieces = []
                for offset in range(0, bits, bank_limit):
                    piece_bits = min(bank_limit, bits - offset)
                    name = f'%arc_async_slice_{len(lines)}'
                    for suffix, value in [('d', data), ('reset', reset_data)]:
                        lines.append(
                            f'    {name}_{suffix} = comb.extract {value} from {offset} : (i{bits}) -> i{piece_bits}'
                        )
                    pieces.append(
                        (name, name + '_d', name + '_reset', piece_bits))
                values = ', '.join(piece[0] for piece in reversed(pieces))
                tys = ', '.join(f'i{piece[3]}' for piece in reversed(pieces))
                reassemble.append(
                    f'    {result} = comb.concat {values} : {tys}')
            for piece in pieces:
                if bank and bank_bits + piece[3] > bank_limit:
                    banks.append(((clock, reset), bank))
                    bank, bank_bits = [], 0
                bank.append(piece)
                bank_bits += piece[3]
        if bank:
            banks.append(((clock, reset), bank))
    for idx, ((clock, reset), regs) in enumerate(banks):
        bits = sum(r[3] for r in regs)
        prefix = f'%arc_async_{idx}'
        tys = ', '.join(f'i{r[3]}' for r in reversed(regs))
        for name, index in [('d', 1), ('reset_data', 2)]:
            values = ', '.join(r[index] for r in reversed(regs))
            lines.append(f'    {prefix}_{name} = comb.concat {values} : {tys}')
        module = f'ArcAsyncRegBank_{idx}'
        externs[module] = 'ArcAsyncRegBank'
        lines.append(
            f'    {prefix}_q = hw.instance "arc_async_{idx}" @{module}<W: ui32 = {bits}>(clock: {clock}: !seq.clock, reset: {reset}: i1, d: {prefix}_d: i{bits}, reset_data: {prefix}_reset_data: i{bits}) -> (q: i{bits})'
        )
        offset = 0
        for result, _, _, regbits in regs:
            lines.append(
                f'    {result} = comb.extract {prefix}_q from {offset} : (i{bits}) -> i{regbits}'
            )
            offset += regbits
    lines.extend(reassemble)
    lines.extend(aggregate_results)


def normalize_input(source, out):
    lines = FileLines(source).splitlines()
    with tempfile.NamedTemporaryFile(dir=out,
                                     prefix='normalized-',
                                     suffix='.mlir',
                                     delete=False) as temporary:
        normalized_path = Path(temporary.name)
    normal = out / 'normalized.mlir'
    externs = {}
    try:
        with LineSink(normalized_path) as cleaned:
            skip_emit = False
            for line in lines:
                if line.startswith('  emit.file '):
                    skip_emit = True
                    continue
                if skip_emit:
                    if line.startswith('  }'):
                        skip_emit = False
                    continue
                if line.startswith('  sv.verbatim.source '):
                    continue
                if line.startswith('  sv.verbatim.module ') or line.startswith(
                        '  hw.module.extern '):
                    match = re.match(
                        r'  (?:sv.verbatim.module|hw.module.extern) (?:private )?@('
                        + IDENT + r')(.*?)(?: attributes (.*))?$', line)
                    if not match:
                        raise ValueError(
                            f'unrecognized foreign module declaration: {line}')
                    name, signature, attrs = match.groups()
                    attrs = attrs or ''
                    vm = re.search(r'verilogName = ("(?:[^"\\]|\\.)*")', attrs)
                    externs[unquote(name)] = unquote(
                        vm[1]) if vm else unquote(name)
                    cleaned.append(
                        f'  hw.module.extern private @{name}{signature}')
                else:
                    cleaned.append(line)
        normalized_path.replace(normal)
    finally:
        normalized_path.unlink(missing_ok=True)
    return normal, externs


def prepare(args):
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    normal, externs = normalize_input(args.input, out)
    flat = out / 'flat.mlir'
    # Resolve live probes before inlining changes their instance paths. Dead
    # hierarchy metadata can then be removed without pinning replicated modules.
    # The formatter dependency map spans procedural regions. Their printed
    # SSA names must be unique, including values in sibling LLHD processes.
    sim = Path(__file__).resolve().parent
    tool = Path(shutil.which(str(args.circt_opt)) or args.circt_opt)
    properties = plugin(tool, getattr(args, 'cxx',
                                      os.environ.get('CXX', 'c++')),
                        sim / 'generated-src/compiler',
                        sim / 'compiler/Properties.cpp')
    temporal = plugin(tool, getattr(args, 'cxx', os.environ.get('CXX', 'c++')),
                      sim / 'generated-src/compiler',
                      sim / 'compiler/TemporalProperties.cpp')
    hierarchy = plugin(tool, getattr(args, 'cxx', os.environ.get('CXX',
                                                                 'c++')),
                       sim / 'generated-src/compiler',
                       sim / 'compiler/HierarchyFormats.cpp')
    subprocess.run([
        args.circt_opt,
        str(normal), '--load-pass-plugin=' + str(properties),
        '--load-pass-plugin=' + str(temporal),
        '--load-pass-plugin=' + str(hierarchy),
        '--pass-pipeline=builtin.module(arc-resolve-xmr,symbol-dce,chipyard-capture-hierarchy-formats{root='
        + args.top +
        '},hw-flatten-modules,chipyard-resolve-hierarchy-formats{root=' +
        args.top +
        '},chipyard-lower-pointwise-properties,chipyard-lower-temporal-properties,canonicalize)',
        '--mlir-print-unique-ssa-ids', '-o',
        str(flat)
    ],
                   check=True)
    partition_flat(flat,
                   out,
                   args.top,
                   externs,
                   getattr(args, 'sparse_memory_min_bits', 1 << 20) if getattr(
                       args, 'native_sparse_memories', False) else None,
                   native_combin=getattr(args, 'native_combinational_models',
                                         False))


def partition_flat(flat,
                   out,
                   top,
                   externs,
                   memory_min_bits=None,
                   native_combin=False):
    with tempfile.TemporaryDirectory(dir=out,
                                     prefix='partition-text-') as temporary:
        with ExitStack() as files:
            _partition_flat(flat, out, top, externs, Path(temporary), files,
                            memory_min_bits, native_combin)


def _partition_flat(flat,
                    out,
                    top,
                    externs,
                    work,
                    files,
                    memory_min_bits=None,
                    native_combin=False):
    signature, root_start, body_start, body_end, tail_start = root_range(
        flat, top)
    source_body = FileLines(flat, body_start, body_end)
    root_clock_used = False
    for line in source_body.splitlines():

        def observe(code):
            nonlocal root_clock_used
            root_clock_used |= any(m[0] == '%clock'
                                   for m in re.finditer(SSA, code))
            return code

        map_code(line, observe)
        if root_clock_used:
            break
    ports, analog_nets, native_signature = [], {}, []
    for port in split(signature):
        m = re.fullmatch(
            r'(in|out|inout) %?(' + IDENT + r')\s*:\s*(.*?)(?: \{.*\})?', port)
        if not m:
            raise ValueError(f'unrecognized top port: {port}')
        direction, name, ty = m.groups()
        if ty.startswith('!hw.inout<'):
            analog = re.fullmatch(r'!hw.inout<(i[1-9]\d*)>', ty)
            if not analog:
                raise ValueError(f'unsupported analog net type: {ty}')
            analog_nets['%' + name] = dict(wire=unquote(name),
                                           bits=width(analog[1]))
            continue
        native_signature.append(port)
        ports.append(
            dict(name=unquote(name), direction=direction, bits=width(ty)))
    if any(p['name'] in ('arc_printf_enable', 'arc_stop_enable')
           for p in ports):
        raise ValueError(
            'top port conflicts with an Arcilator runtime control')
    body, outputs, instances = files.enter_context(
        LineSink(work / 'native-body.mlir')), [], []
    inputs = [('arc_printf_enable', 'i1'), ('arc_stop_enable', 'i1')]
    wide_helpers = set()
    root_body = lower_special_ops(source_body, externs, wide_helpers,
                                  work / 'special-body.mlir', native_combin)
    if memory_min_bits is not None:
        from compiler.memories import lower_memories
        memory_body = files.enter_context(LineSink(work / 'memory-body.mlir'))
        memory_body.extend(
            lower_memories(root_body, externs, memory_min_bits).splitlines())
        root_body.path.unlink()
        root_body = memory_body.close()
    # Native register clocks advance after the foreign models sample this
    # edge. Combinational evaluation uses their previous committed levels.
    from_clocks, to_clocks = {}, {}
    for line in root_body.splitlines():
        if opcode(line) in ('seq.from_clock', 'seq.to_clock'):
            match = re.match(
                r'\s*(' + SSA + r') = seq\.(from_clock|to_clock) (' + SSA +
                r')', line)
            if not match:
                raise ValueError(f'unrecognized clock conversion: {line}')
            (from_clocks
             if match[2] == 'from_clock' else to_clocks)[match[1]] = match[3]
    clock_aliases = {}

    def clock_key(value):
        seen = set()
        while value not in seen and value in to_clocks:
            seen.add(value)
            source = to_clocks[value]
            if source not in from_clocks:
                # All casts of the same bit have exactly the same edges.
                # Assertions introduce many such casts after HW optimization.
                return clock_aliases.setdefault(source, value)
            value = from_clocks[source]
        return value

    phase_clocks = {}
    adjusted = files.enter_context(LineSink(work / 'adjusted-body.mlir'))
    for line in root_body.splitlines():
        pattern = None
        if opcode(line) == 'seq.firreg' or opcode(line).startswith(
                'seq.firmem.'):
            pattern = r'(?<= clock )' + SSA
        elif line.strip().startswith('sim.triggered '):
            pattern = r'(?<=sim.triggered )' + SSA
        if pattern:

            def phase_clock(match):
                key = clock_key(match[0])
                idx = phase_clocks.setdefault(key, len(phase_clocks))
                return f'%arc_phase_clock_{idx}'

            line = map_code(line,
                            lambda code: re.sub(pattern, phase_clock, code))
        adjusted.append(line)
    adjusted = adjusted.close()
    root_body.path.unlink()
    phase_info = []
    for value, idx in phase_clocks.items():
        held, observed = f'arc_held_clock_{idx}', f'arc_observed_clock_{idx}'
        inputs.append((held, 'i1'))
        outputs.append((observed, f'%{observed}_value', 'i1'))
        body += [
            f'    %arc_phase_clock_{idx} = seq.to_clock %{held}',
            f'    %{observed}_value = seq.from_clock {value}'
        ]
        phase_info.append(dict(held=held, observed=observed))
    original_output = None
    for line in adjusted.splitlines():
        if opcode(line) != 'hw.instance':
            if line.strip().startswith('hw.output'):
                original_output = line.strip()[len('hw.output'):].strip()
                continue
            body.append(line)
            continue
        m = re.fullmatch(
            r'\s*(?:(.*?) = )?hw.instance ("(?:[^"\\]|\\.)*")(?: sym @' +
            IDENT + r')? @(' + IDENT +
            r')(?:<(.*?)>)?\((.*?)\) -> \((.*?)\)(?: \{.*\})?', line)
        if not m:
            raise ValueError(f'unrecognized flattened instance: {line}')
        results, instname, module, params, ins, outs = m.groups()
        module = unquote(module)
        if module not in externs:
            raise ValueError(
                f'non-foreign instance survived flattening: {module}')
        idx = len(instances)
        instance = dict(name=unquote(instname),
                        module=externs[module],
                        parameters=split(params or ''),
                        ports=[])
        for item in split(ins):
            p = re.fullmatch(r'(' + IDENT + r'):\s*(' + SSA + r'):\s*(.*)',
                             item)
            if not p:
                raise ValueError(f'unrecognized instance input: {item}')
            name, value, ty = p.groups()
            if ty.startswith('!hw.inout<'):
                if value not in analog_nets:
                    raise ValueError(
                        f'analog instance port does not reference a lifted physical net: {item}'
                    )
                instance['ports'].append(
                    dict(name=unquote(name),
                         direction='inout',
                         **analog_nets[value]))
                continue
            wire = f'arc_b{idx}_p{len(instance["ports"])}'
            instance['ports'].append(
                dict(name=unquote(name),
                     wire=wire,
                     direction='in',
                     bits=width(ty)))
            if ty == '!seq.clock':
                body.append(f'    %{wire}_value = seq.from_clock {value}')
                value = f'%{wire}_value'
            outputs.append((wire, value, f'i{width(ty)}'))
        result_list = split(results or '')
        if len(result_list) == 1:
            grouped = re.fullmatch('(' + SSA + r'):(\d+)', result_list[0])
            if grouped:
                result_list = [
                    f'{grouped[1]}#{i}' for i in range(int(grouped[2]))
                ]
        out_list = split(outs)
        if len(result_list) != len(out_list):
            raise ValueError(f'instance {instname} result count mismatch')
        for result, item in zip(result_list, out_list):
            name, ty = item.split(':', 1)
            ty = ty.strip()
            wire = f'arc_b{idx}_p{len(instance["ports"])}'
            instance['ports'].append(
                dict(name=unquote(name.strip()),
                     wire=wire,
                     direction='out',
                     bits=width(ty)))
            inputs.append((wire, f'i{width(ty)}'))
            if ty == '!seq.clock':
                body.append(f'    {result} = seq.to_clock %{wire}')
            else:
                body.append(
                    f'    {result} = hw.bitcast %{wire} : (i{width(ty)}) -> {ty}'
                )
        instances.append(instance)
    adjusted.path.unlink()
    if original_output is None:
        raise ValueError('flattened top module has no hw.output')
    sig = ', '.join(native_signature)
    additions = [f'in %{name} : {ty}' for name, ty in inputs
                 ] + [f'out {name} : {ty}' for name, _, ty in outputs]
    if additions:
        sig += (', ' if sig.strip() else '') + ', '.join(additions)
    vals, types = [], []
    if original_output:
        vals_text, types_text = original_output.split(' : ', 1)
        vals, types = split(vals_text), split(types_text)
    vals.extend(value for _, value, _ in outputs)
    types.extend(ty for _, _, ty in outputs)
    body.append('    hw.output' + (' ' + ', '.join(vals) + ' : ' +
                                   ', '.join(types) if vals else ''))
    body.close()
    declarations = '\n'.join('  func.func private @' + name +
                             '() attributes {chipyard.no_output_readback}'
                             for name in ('chipyard_arc_assert_fail',
                                          'chipyard_arc_finish',
                                          'chipyard_arc_stop_fail'))
    declarations += '\n  func.func private @chipyard_arc_guard(i32) -> i32 attributes {chipyard.no_output_readback}'
    declarations += ''.join(
        f'\n  func.func private @{name}(i{bits}, i{bits}) -> i{bits} attributes {{chipyard.no_output_readback, chipyard.native_pure_wide_shift}}'
        for name, bits in sorted(wide_helpers))
    target = out / 'partitioned.mlir'
    with tempfile.NamedTemporaryFile(dir=out,
                                     prefix='partitioned-',
                                     suffix='.mlir.tmp',
                                     delete=False) as output:
        temporary = Path(output.name)
    try:
        with temporary.open('wb') as output:
            copy_prefix(flat, output, root_start, declarations)
            output.write(f'  hw.module @ArcModel({sig}) {{\n'.encode('utf-8'))
            with body.path.open('rb') as stream:
                shutil.copyfileobj(stream, output, 1 << 20)
            output.write(b'  }')
            with Path(flat).open('rb') as stream:
                stream.seek(tail_start)
                while chunk := stream.read(1 << 20):
                    output.write(chunk)
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    body.path.unlink()
    (out / 'partition.json').write_text(
        json.dumps(dict(top=top,
                        ports=ports,
                        instances=instances,
                        phase_clocks=phase_info,
                        root_clock_used=root_clock_used,
                        analog_nets=list(analog_nets.values())),
                   indent=2) + '\n')
    generate_shell(out, top, ports, instances, phase_info)


def generate_shell(out, top, ports, instances, phase_info):
    foreign = [p for inst in instances for p in inst['ports']]
    # Fixed packed bit vectors give every DPI argument the standard svBitVecVal
    # ABI, including one-bit and wider-than-64-bit signals.
    native_inputs = [
        dict(p, wire=p['name']) for p in ports if p['direction'] == 'in'
    ] + [p for p in foreign if p['direction'] == 'out']
    native_outputs = [
        dict(p, wire=p['name']) for p in ports if p['direction'] == 'out'
    ] + [p for p in foreign if p['direction'] == 'in']
    phase_ports = [dict(wire=c['observed'], bits=1) for c in phase_info]
    native_outputs += phase_ports
    all_ports = native_inputs + native_outputs
    dpi = [
        'input chandle state', 'input int unsigned epoch',
        'input bit printf_enable', 'input bit stop_enable'
    ] + [
        f'{direction} bit [{p["bits"]-1}:0] arg{i}'
        for direction, ps in [('input',
                               native_inputs), ('output', native_outputs)]
        for i, p in enumerate(
            ps, 0 if direction == 'input' else len(native_inputs))
    ]
    lines = [
        f'module {svname(top)}(\n  ' + ',\n  '.join(
            f'{"input" if p["direction"] == "in" else "output"} wire [{p["bits"]-1}:0] {svname(p["name"])}'
            for p in ports) + '\n);',
        '  import "DPI-C" function chandle chipyard_arc_create();',
        '  import "DPI-C" function void chipyard_arc_destroy(input chandle state);',
        '  import "DPI-C" function void chipyard_arc_eval(\n    ' +
        ',\n    '.join(dpi) + '\n  );', '  chandle arc_state;',
        '  int unsigned arc_epoch = 0;',
        '  final chipyard_arc_destroy(arc_state);'
    ]
    lines += [
        '`ifdef PRINTF_COND', '  wire arc_printf_enable = `PRINTF_COND;',
        '`else', "  wire arc_printf_enable = 1'b1;", '`endif',
        '`ifdef STOP_COND', '  wire arc_stop_enable = `STOP_COND;', '`else',
        "  wire arc_stop_enable = 1'b1;", '`endif'
    ]
    unique_wires = {p['wire']: p for p in foreign + phase_ports}
    for p in unique_wires.values():
        lines.append(f'  wire [{p["bits"]-1}:0] {p["wire"]};')
    for idx, inst in enumerate(instances):
        params = ' #(\n    ' + ',\n    '.join(
            svparam(p) for p in
            inst['parameters']) + '\n  )' if inst['parameters'] else ''
        lines.append(f'  {svname(inst["module"])}{params} bb_{idx} (\n    ' +
                     ',\n    '.join(f'.{svname(p["name"])}({p["wire"]})'
                                    for p in inst['ports']) + '\n  );')
    if phase_info:
        events = ' or\n    '.join(edge + ' ' + p['wire'] for p in phase_ports
                                  for edge in ('posedge', 'negedge'))
        lines.append('  always @(' + events + ') arc_epoch <= arc_epoch + 1;')
    arguments = [
        'arc_state', 'arc_epoch', 'arc_printf_enable', 'arc_stop_enable'
    ] + [svname(p['wire']) for p in all_ports]
    # This is an opaque stateful evaluator, not a combinational SV function.
    # An always_comb call makes every output depend on every input. With
    # foreign clock/feedback paths, that can put unrelated delayed assignments
    # in one scheduling region and repeatedly launch unchanged delays. A
    # timing process preserves the exact input-change sensitivity instead.
    sensitivity = ['arc_epoch', 'arc_printf_enable', 'arc_stop_enable'
                   ] + [svname(p['wire']) for p in native_inputs]
    lines += [
        '  initial begin', '    arc_state = chipyard_arc_create();',
        '    forever begin', '      chipyard_arc_eval(\n        ' +
        ',\n        '.join(arguments) + '\n      );',
        '      @(' + ' or\n        '.join(sensitivity) + ');', '    end',
        '  end', 'endmodule'
    ]
    lines += [
        'module ArcAsyncRegBank #(parameter W = 1) (',
        '  input wire clock, reset, input wire [W-1:0] d, reset_data,',
        '  output reg [W-1:0] q);',
        '  always @(posedge clock or posedge reset)',
        '    if (reset) q <= reset_data; else q <= d;', 'endmodule',
        'module ArcWideShift #(parameter W=1, OP=0) (',
        '  input wire [W-1:0] data, amount, output wire [W-1:0] out);',
        '  generate if (OP == 0) assign out = data << amount;',
        '    else if (OP == 1) assign out = data >> amount;',
        '    else assign out = $signed(data) >>> amount;', '  endgenerate',
        'endmodule'
    ]
    (out / 'ArcHarness.sv').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--top', required=True)
    parser.add_argument('--circt-opt', default='circt-opt')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--native-sparse-memories', action='store_true')
    parser.add_argument(
        '--native-combinational-models',
        action='store_true',
        help='compile scalar IO/clock logic; retain clock-enable latches')
    parser.add_argument('--sparse-memory-min-bits', type=int, default=1 << 20)
    try:
        prepare(parser.parse_args())
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'partition: {error}\n')
