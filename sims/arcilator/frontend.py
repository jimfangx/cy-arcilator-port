#!/usr/bin/env python3
"""Lower generated FIRRTL to native HW, preserving foreign analog connections."""
import argparse
import json
import re
import subprocess
from pathlib import Path


def normalize_chirrtl(text):
    # Chisel's text emitter can omit the comma before the read-under-write
    # mode. Keep the mode and source information; repair only its separator.
    pattern = (r'^([ \t]*smem[ \t]+[^:\n]+:[^@;\n]*\])[ \t]+'
               r'(old|new|undefined)(?=[ \t]*(?:@\[[^\n]*\])?[ \t]*(?:;[^\n]*)?$)')
    return re.subn(pattern, r'\1, \2', text, flags=re.M)


def strip_enum_metadata(text):
    # These Chisel annotations describe enum names, not hardware behavior.
    # firtool's fallback annotation resolver cannot address their memory-port
    # fields. Native waveforms display packed values; retain all other classes,
    # including memory initialization, blackbox, and preservation annotations.
    match = re.search(r'^circuit[ \t]+[^\n:]+:[ \t]*%\[', text, re.M)
    if not match:
        return text, 0
    start = match.end()
    while start < len(text) and text[start].isspace():
        start += 1
    annotations, end = json.JSONDecoder().raw_decode(text, start)
    if not isinstance(annotations, list) or not all(isinstance(a, dict) for a in annotations):
        raise ValueError('inline circuit annotations must be an array of objects')
    ignored = {'chisel3.experimental.EnumAnnotations$' + name for name in
               ('EnumComponentAnnotation', 'EnumDefAnnotation', 'EnumVecAnnotation')}
    retained = [entry for entry in annotations if entry.get('class') not in ignored]
    removed = len(annotations) - len(retained)
    if not removed:
        return text, 0
    return text[:start] + json.dumps(retained, indent=2) + text[end:], removed


def lower(args):
    source, output = Path(args.input), Path(args.output)
    common = ['--disable-all-randomization', '--disable-annotation-unknown', '--disable-annotation-classless']
    text, repaired = normalize_chirrtl(source.read_text())
    text, enums = strip_enum_metadata(text)
    if repaired or enums:
        source = output.parent / (output.name + '.input.fir')
        source.write_text(text)
        print(f'Normalized {repaired} synchronous memory mode separators; '
              f'removed {enums} enum metadata annotations', flush=True)
    if re.search(r'^\s*(?:input|output|wire) [^\n]*\bAnalog(?:<|\s|$)', text, re.M):
        # Flatten before naming physical nets: attach edges through hierarchy
        # become one net joining the actual foreign-module pins.
        annotation = output.parent / 'flatten-analog.anno.json'
        annotation.write_text(json.dumps([{'class': 'firrtl.transforms.FlattenAnnotation',
                                          'target': f'~{args.top}|{args.top}'}]))
        flat = output.parent / 'analog-flat.fir.mlir'
        subprocess.run([args.firtool, str(source), '--ir-fir', '--annotation-file=' + str(annotation),
                        *common, *args.flags, '-o', str(flat)], check=True)
        ir = flat.read_text()
        wires = []
        def hoist(match):
            wires.append((match[1], match[2]))
            return ''
        ir = re.sub(r'^      (%[\w.$-]+) = firrtl.wire[^\n]* : (!firrtl.analog<\d+>)\n', hoist, ir, flags=re.M)
        root = re.search(r'^    firrtl.module @' + re.escape(args.top) + r'\(([^\n]*)\)', ir, re.M)
        if not root:
            raise ValueError(f'cannot find flattened FIRRTL top {args.top}')
        # An attach directly between foreign pins may have no local wire.
        # Add one temporary port anchor for those nets as well.
        anchored = {name for name, _ in wires}
        anchored.update(re.findall(r'(%[\w.$-]+): !firrtl.analog<\d+>', root[1]))
        counter = 0
        def anchor_attach(match):
            nonlocal counter
            values, types = match[1], match[2]
            operands = [v.strip() for v in values.split(',')]
            if any(value in anchored for value in operands):
                return match[0]
            widths = re.findall(r'!firrtl.analog<(\d+)>', types)
            if len(widths) != len(operands) or len(set(widths)) != 1:
                raise ValueError('analog attach has incompatible or unrecognized types')
            name = f'%__chipyard_arcilator_analog_{counter}'
            counter += 1
            if name in ir:
                raise ValueError('generated analog anchor name conflicts with the design')
            ty = f'!firrtl.analog<{widths[0]}>'
            wires.append((name, ty))
            anchored.add(name)
            return f'      firrtl.attach {values}, {name} : {types}, {ty}'
        ir = re.sub(r'^      firrtl.attach ([^\n]+?) : ([^\n]+)$', anchor_attach, ir, flags=re.M)
        # A wire used outside the attach network needs an explicit lowering;
        # never discard such a use while moving the net into the foreign shell.
        body_without_attaches = re.sub(r'^      firrtl.attach [^\n]+$', '', ir[root.end():], flags=re.M)
        for name, _ in wires:
            if re.search(re.escape(name) + r'(?![\w.$-])', body_without_attaches):
                raise ValueError(f'analog wire {name} is used outside an attach network')
        if wires:
            ir = ir[:root.end(1)] + (', ' if root[1] else '') + ', '.join('out ' + name + ': ' + ty for name, ty in wires) + ir[root.end(1):]
        # The ports are temporary anchors for physical nets. The HW partitioner
        # moves them into the foreign shell, where the original tri-state HDL
        # resolves their drivers. No analog value crosses the native ABI.
        source = output.parent / 'analog-ports.fir.mlir'
        source.write_text(ir)
        common.append('--format=mlir')
    subprocess.run([args.firtool, str(source), '--ir-hw', '--lower-to-core', *common,
                    *args.flags, '-o', str(output)], check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--top', required=True)
    parser.add_argument('--firtool', required=True)
    parser.add_argument('flags', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.flags[:1] == ['--']:
        args.flags = args.flags[1:]
    try:
        lower(args)
    except (ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'frontend: {error}\n')
