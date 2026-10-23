#!/usr/bin/env python3
"""Check formatting intrinsics among large numbers of ordinary calls."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def source(count, markers=True, error=None):
    lines = ['module {']
    lines += [f'func.func private @padding_{index}(i64) -> i64'
              for index in range(count)]
    lines.append('func.func private @ordinary(i64) -> i64')
    if markers:
        lines += [
            'func.func private @field(i64,i32,i32,i32) -> !sim.fstring attributes {chipyard.native_format_field}',
            'func.func private @render(!sim.fstring) -> i64 attributes {chipyard.native_format_render}']
    if error == 'callback':
        lines.append('func.func private @chipyard_arc_format_buffer_begin(i32) -> i32')
    result = '!sim.fstring' if error == 'consumer' else 'i64'
    lines += [f'func.func @test(%a: i64) -> {result} {{',
              '%unused = sim.fmt.literal "unused format literal"']
    previous = '%a'
    for index in range(count):
        lines.append(f'%v{index} = func.call @ordinary({previous}) '
                     f'{{chipyard.test_order = {index} : i32}} : (i64) -> i64')
        previous = f'%v{index}'
    if markers:
        lines += ['%none = hw.constant -1 : i32',
                  '%zero = hw.constant 0 : i32',
                  '%space = hw.constant 32 : i32',
                  f'%field = func.call @field({previous}, %none, %zero, %space) : (i64,i32,i32,i32) -> !sim.fstring']
        if error == 'consumer':
            lines += ['return %field : !sim.fstring', '}', '}']
            return '\n'.join(lines) + '\n'
        lines.append('%render = func.call @render(%field) : (!sim.fstring) -> i64')
        previous = '%render'
    lines += [f'return {previous} : i64', '}', '}']
    return '\n'.join(lines) + '\n'


def check(circt, cxx, build):
    build.mkdir(parents=True, exist_ok=True)
    library = plugin(circt / 'circt-opt', cxx, SIM / 'generated-src/compiler',
                     SIM / 'compiler/FormatStrings.cpp')
    reports = []
    for name, marker in [('ordinary', False), ('mixed', True)]:
        before, after = build / (name + '.mlir'), build / (name + '-after.mlir')
        before.write_text(source(16384, marker))
        start = time.monotonic()
        subprocess.run([str(circt / 'circt-opt'), str(before),
                        '--load-pass-plugin=' + str(library),
                        '--pass-pipeline=builtin.module(chipyard-lower-format-strings)',
                        '-o', str(after)], check=True)
        text = after.read_text()
        assert [int(x) for x in re.findall(r'chipyard.test_order = (\d+) : i32', text)] == list(range(16384))
        assert text.count('call @ordinary(') == 16384
        assert 'unused format literal' not in text
        assert 'call @field(' not in text and 'call @render(' not in text
        assert 'func.func private @field(' not in text and 'func.func private @render(' not in text
        for callback in ['buffer_begin', 'buffer_end', 'native_string']:
            assert ('call @chipyard_arc_format_' + callback + '(' in text) is marker
        reports.append(dict(case=name, ordinary_calls=16384, markers=marker,
                            elapsed_seconds=time.monotonic()-start))
    for error, expected in [('callback', 'conflicting native formatting callback'),
                            ('consumer', 'native string field has an unsupported consumer')]:
        before = build / (error + '.mlir')
        before.write_text(source(4, True, error))
        result = subprocess.run([str(circt / 'circt-opt'), str(before),
                                 '--load-pass-plugin=' + str(library),
                                 '--pass-pipeline=builtin.module(chipyard-lower-format-strings)',
                                 '-o', str(build / (error + '-after.mlir'))],
                                text=True, capture_output=True)
        (build / (error + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode != 0 and expected in result.stderr
    (build / 'report.json').write_text(json.dumps(dict(status='passed', cases=reports,
        rejected=['conflicting_callback','unsupported_consumer']), indent=2) + '\n')
    print('PASS: 32768 ordered ordinary calls, exact native format callbacks, '
          'marker-free cleanup and preserved formatting errors')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--work', type=Path, default=SIM / 'generated-src/format-index')
    args = parser.parse_args()
    if args.circt_bin is None:
        parser.error('set CIRCT_BIN or supply --circt-bin')
    check(args.circt_bin, os.environ.get('CXX','c++'), args.work)
