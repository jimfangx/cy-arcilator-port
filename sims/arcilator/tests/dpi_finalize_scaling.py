#!/usr/bin/env python3
"""Check final LLVM DPI ABI attributes among large scoped symbol tables."""
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


def source(count=16384, external_calls=True):
    lines = ['module {']
    lines += [f'llvm.func @padding_{index}(i64) -> i64' for index in range(count)]
    lines += [
        'llvm.func @external(i1 {llvm.zeroext}, i8 {llvm.signext}, i13 {llvm.zeroext}, i32 {llvm.signext}, i65 {llvm.zeroext}) -> (i1 {llvm.zeroext})',
        'llvm.func @defined(%a: i1) -> i1 { llvm.return %a : i1 }',
        'llvm.func @driver(%a:i1, %b:i8, %c:i13, %d:i32, %e:i65) -> i1 {']
    previous = '%a'
    for index in range(count):
        if external_calls:
            lines.append(f'%e{index} = llvm.call @external({previous}, %b, %c, %d, %e) '
                         f'{{chipyard.test_order = {index} : i32, arg_attrs = [{{chipyard.keep}}, {{}}, {{}}, {{}}, {{}}], res_attrs = [{{chipyard.result_keep}}]}} '
                         ': (i1, i8, i13, i32, i65) -> i1')
            previous = f'%e{index}'
        lines.append(f'%d{index} = llvm.call @defined({previous}) '
                     f'{{chipyard.ordinary_order = {index} : i32}} : (i1) -> i1')
        previous = f'%d{index}'
    lines += [f'llvm.return {previous} : i1', '}',
              'llvm.func @indirect(%fn: !llvm.ptr, %a:i1) -> i1 {',
              '%x = llvm.call %fn(%a) : !llvm.ptr, (i1) -> i1',
              'llvm.return %x : i1', '}',
              'module @nested {',
              'llvm.func @external(i8 {llvm.zeroext}) -> (i8 {llvm.signext})',
              'llvm.func @nested_driver(%a:i8) -> i8 {',
              '%x = llvm.call @external(%a) : (i8) -> i8',
              'llvm.return %x : i8', '}', '}', '}']
    return '\n'.join(lines) + '\n'


def check(circt, cxx, build):
    build.mkdir(parents=True, exist_ok=True)
    library = plugin(circt/'circt-opt', cxx, SIM/'generated-src/compiler', SIM/'compiler/Model.cpp')
    reports = []
    for name, external_calls in [('mixed', True), ('defined', False)]:
        before, after = build/(name+'.mlir'), build/(name+'-after.mlir')
        before.write_text(source(16384, external_calls))
        start = time.monotonic()
        subprocess.run([str(circt/'circt-opt'),str(before),'--load-pass-plugin='+str(library),
                        '--pass-pipeline=builtin.module(chipyard-finalize-dpi-abi)',
                        '-o',str(after)],check=True)
        text = after.read_text()
        assert [int(x) for x in re.findall(r'chipyard.ordinary_order = (\d+) : i32',text)] == list(range(16384))
        assert [int(x) for x in re.findall(r'chipyard.test_order = (\d+) : i32',text)] == (list(range(16384)) if external_calls else [])
        calls = [line for line in text.splitlines() if 'llvm.call @external(' in line]
        root = [line for line in calls if 'chipyard.test_order' in line]
        assert len(root) == (16384 if external_calls else 0)
        for line in root:
            assert ': (i1 {chipyard.keep, llvm.zeroext}, i8 {llvm.signext}, i13 {llvm.zeroext}, i32, i65) -> (i1 {chipyard.result_keep, llvm.zeroext})' in line
        nested = [line for line in calls if 'chipyard.test_order' not in line]
        assert len(nested)==1 and ': (i8 {llvm.zeroext}) -> (i8 {llvm.signext})' in nested[0]
        assert all(': (i1) -> i1' in line and 'llvm.zeroext' not in line and 'llvm.signext' not in line for line in text.splitlines() if 'llvm.call @defined(' in line)
        assert re.search(r'llvm.call %\w+\(%\w+\) : !llvm.ptr, \(i1\) -> i1',text)
        reports.append(dict(case=name,symbols=16384,ordered_external_calls=len(root),ordered_internal_calls=16384,elapsed_seconds=time.monotonic()-start))
    (build/'report.json').write_text(json.dumps(dict(status='passed',cases=reports),indent=2)+'\n')
    print('PASS: final LLVM DPI scalar ABI attributes, 49152 ordered direct calls, signed/unsigned narrow formals, untouched wide/internal/indirect calls and shadowed declarations')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin',type=Path,default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--work',type=Path,default=SIM/'generated-src/dpi-finalize-scaling')
    args=parser.parse_args()
    if args.circt_bin is None: parser.error('set CIRCT_BIN or supply --circt-bin')
    check(args.circt_bin,os.environ.get('CXX','c++'),args.work)
