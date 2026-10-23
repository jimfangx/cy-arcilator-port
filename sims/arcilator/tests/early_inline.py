#!/usr/bin/env python3
"""Check early LLVM forwarding inlining and its disabled native ABI path."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM / 'compiler'))
from codegen import compile_model
from llvm import WORD_DECLARATION
from scalar_shift import exercise, toy_source


def indirect_source():
    """Hide each narrow value behind an ordinary, unnamed-by-the-lowering call."""
    lines, pairs = [], set()
    for line in toy_source().replace(WORD_DECLARATION, '').splitlines():
        match = re.fullmatch(r'  %d = zext i(\d+) (%\w+) to i(\d+)', line)
        if match:
            small, operand, width = match.groups()
            pairs.add((int(width), int(small)))
            line = (f'  %d = call i{width} @forwarded_{width}_{small}('
                    f'i{small} {operand})')
        lines.append(line)
    assert len(pairs) == 42
    for width, small in sorted(pairs):
        lines += [
            f'define internal i{width} @forwarded_{width}_{small}(i{small} %x) {{',
            f'  %r = zext i{small} %x to i{width}', f'  ret i{width} %r', '}'
        ]
    return '\n'.join(lines) + '\n'


def check(circt, cxx, work):
    import ctypes
    work.mkdir(parents=True, exist_ok=True)
    helper = work / 'wide.o'
    subprocess.run([
        *shlex.split(cxx), '-O3', '-std=c++17', '-fPIC', '-c',
        str(SIM / 'native/wide.cpp'), '-o', str(helper)
    ], check=True)
    cases = []
    for enabled in (False, True):
        directory = work / ('enabled' if enabled else 'disabled')
        directory.mkdir(exist_ok=True)
        source = directory / 'source.ll'
        source.write_text(indirect_source())
        command = [
            sys.executable, str(SIM / 'compiler/llvm.py'), str(source),
            '--llvm-opt=' + str(circt / 'opt'),
            '--cache=' + str(SIM / 'generated-src/compiler'),
            '--cxx=/not-installed/c++', '--no-share-procedures',
            '--no-abi-buffers'
        ]
        if not enabled:
            command.append('--no-early-inline')
        subprocess.run(command, check=True)
        scalar_calls = source.read_text().count(
            'call void @chipyard_arc_private_shift_scalar(')
        assert scalar_calls == (42 if enabled else 0)
        for pipeline in ('stock', 'bounded'):
            obj = directory / (pipeline + '.o')
            compile_model(source, obj, circt / 'opt', circt / 'llc',
                          SIM / 'generated-src/compiler', cxx,
                          abi_buffers=False, pipeline=pipeline)
            library = obj.with_suffix('.so')
            subprocess.run([*shlex.split(cxx), '-shared', str(obj), str(helper),
                            '-o', str(library)], check=True)
            vectors = exercise(ctypes.CDLL(str(library.resolve())))
            cases.append(dict(early_inline=enabled, pipeline=pipeline,
                              independent_integer_vectors=vectors))
    result = dict(status='passed', cases=cases,
                  no_custom_plugin_required_for_lowering=True,
                  disabled_path_retains_generic_arithmetic=True)
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS: early/default and disabled LLVM forwarding paths, both '
          'codegen pipelines, independent full/scalar-amount shift vectors',
          flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path,
                        default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--build', type=Path,
                        default=SIM / 'generated-src/early-inline-test')
    args = parser.parse_args()
    if args.circt_bin is None:
        parser.error('set CIRCT_BIN or supply --circt-bin')
    check(args.circt_bin.resolve(), args.cxx, args.build.resolve())


if __name__ == '__main__':
    main()
