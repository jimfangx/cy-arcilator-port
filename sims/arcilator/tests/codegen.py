#!/usr/bin/env python3
"""Check packed-cache codegen, original flags, and failed-build preservation."""
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim / 'compiler'))
from codegen import compile_model
from partition import plugin


def cache_ir():
    # Reproduce the packed 256 x 128-bit cache update shape that made SLP
    # create a 134-million-bit vector in the original Vexii model. The C++
    # reference updates bytes directly and compares every byte of every image.
    lines = [
        'define internal i32768 @cache(i32768 %old, ptr %base, i8 %index,',
        '                              i128 %data, i1 %enabled) noinline {',
        '  %base.value = load i32768, ptr %base, align 1'
    ]
    accumulator = None
    for index in range(256):
        lines += [
            f'  %old.shift.{index} = lshr i32768 %old, {index * 128}',
            f'  %old.{index} = trunc i32768 %old.shift.{index} to i128',
            f'  %base.shift.{index} = lshr i32768 %base.value, {index * 128}',
            f'  %base.{index} = trunc i32768 %base.shift.{index} to i128',
            f'  %hit.{index} = icmp eq i8 %index, {index}',
            f'  %write.{index} = select i1 %hit.{index}, i128 %data, i128 %base.{index}',
            f'  %lane.{index} = select i1 %enabled, i128 %write.{index}, i128 %old.{index}',
            f'  %extended.{index} = zext i128 %lane.{index} to i32768',
            f'  %positioned.{index} = shl i32768 %extended.{index}, {index * 128}'
        ]
        value = f'%positioned.{index}'
        if accumulator:
            lines.append(
                f'  %joined.{index} = or i32768 {accumulator}, {value}')
            value = f'%joined.{index}'
        accumulator = value
    lines += [
        f'  ret i32768 {accumulator}', '}', '''
define void @cache_update(ptr %out, ptr %old, ptr %base, i8 %index,
                          i64 %low, i64 %high, i1 %enabled) {
  %old.value = load i32768, ptr %old, align 1
  %lo = zext i64 %low to i128
  %hi = zext i64 %high to i128
  %hi.shifted = shl i128 %hi, 64
  %data = or i128 %lo, %hi.shifted
  %value = call i32768 @cache(i32768 %old.value, ptr %base, i8 %index,
                             i128 %data, i1 %enabled)
  store i32768 %value, ptr %out, align 1
  ret void
}
define i64 @preserved(i64 %x) noimplicitfloat noinline {
  %r = add i64 %x, 2
  ret i64 %r
}
define double @floatprobe(double %x) noinline {
  %product = fmul double %x, 1.25
  %result = fadd double %product, 2.0
  ret double %result
}
'''
    ]
    return '\n'.join(lines)


def main():
    circt = Path(os.environ['CIRCT_BIN']).resolve()
    cxx = os.environ.get('CXX', 'c++')
    build = sim / 'generated-src/codegen-test'
    build.mkdir(parents=True, exist_ok=True)
    from large_value_abi import check as check_large_values
    check_large_values(circt, cxx, build / 'large-value-abi')
    from dpi_symbol_scaling import check as check_dpi_symbols
    check_dpi_symbols(circt, cxx, build / 'dpi-symbol-scaling')
    from format_index import check as check_format_index
    check_format_index(circt, cxx, build / 'format-index')
    from dpi_finalize_scaling import check as check_dpi_finalize
    check_dpi_finalize(circt, cxx, build / 'dpi-finalize-scaling')
    from arc_llvm_scaling import check as check_arc_llvm
    check_arc_llvm(circt, cxx, build / 'arc-llvm-scaling')
    from aggregate_stores import check as check_aggregate_stores
    store_checks = check_aggregate_stores(circt, cxx,
                                          build / 'aggregate-stores')
    from aggregate_codegen import check as check_aggregate_codegen
    aggregate_checks = check_aggregate_codegen(circt, cxx,
                                               build / 'aggregate-codegen')
    from wide_shift_abi import check as check_wide_shift_abi
    wide_shift_checks = check_wide_shift_abi(circt, cxx,
                                             build / 'wide-shift-abi')
    from scalar_shift import check as check_scalar_shift
    scalar_shift_checks = check_scalar_shift(circt, cxx,
                                             build / 'scalar-shift')
    from early_inline import check as check_early_inline
    early_inline_checks = check_early_inline(circt, cxx,
                                             build / 'early-inline')
    from wide_stores import check as check_wide_stores
    wide_store_checks = check_wide_stores(circt, cxx, build / 'wide-stores')
    from vector_packing import check as check_vector_packing
    vector_packing_checks = check_vector_packing(circt, cxx,
                                                 build / 'vector-packing')
    cache = sim / 'generated-src/compiler'
    library = plugin(circt / 'opt', cxx, cache, sim / 'compiler/Codegen.cpp')
    source = build / 'cache.ll'
    source.write_text(cache_ir())
    checks = []
    for level in ('0', '2', '3'):
        obj = build / ('cache-O' + level + '.o')
        compile_model(source, obj, circt / 'opt', circt / 'llc', cache, cxx,
                      level, level)
        ir = subprocess.check_output(
            [circt / 'llvm-dis',
             obj.with_suffix('.bc'), '-o', '-'], text=True)
        assert 'chipyard.temporary.vector.guard' not in ir
        assert 'chipyard.original.noimplicitfloat' not in ir
        attributes = dict(
            re.findall(r'^attributes #(\d+) = \{([^\n]+)\}', ir, re.M))
        preserved = []
        for line in ir.splitlines():
            if line.startswith(('define ', 'declare ')):
                name = re.search(r'@([^ (]+)', line)
                group = re.search(r' #(\d+)', line)
                if group and 'noimplicitfloat' in attributes[group[1]]:
                    preserved.append(name[1])
        assert preserved == ['preserved'], preserved
        assert ' cache' in subprocess.check_output(['nm', obj], text=True)
        executable = build / ('test-O' + level)
        subprocess.run([
            *shlex.split(cxx), '-O2', sim / 'tests/codegen_cache.cpp', obj,
            '-o', executable
        ],
                       check=True)
        subprocess.run([executable], check=True)
        checks.append(
            dict(level=level,
                 cache_checks=1536,
                 sha256=hashlib.sha256(obj.read_bytes()).hexdigest()))

    # Entry and tail-call constraints must survive even when a function's
    # weighted cost would otherwise create several basic blocks.
    tail = ['define i32 @tail(i32 %x) {']
    value = '%x'
    for index in range(3000):
        tail.append(f'  %v{index} = add i32 {value}, 1')
        value = f'%v{index}'
    tail += [
        f'  %r = musttail call i32 @target(i32 {value})', '  ret i32 %r', '}',
        'declare i32 @target(i32)', '''
declare void @llvm.localescape(...)
define void @entry_constraint() {
  %x = alloca i32
  call void (...) @llvm.localescape(ptr %x)
  ret void
}
'''
    ]
    constrained = build / 'constrained.ll'
    constrained.write_text('\n'.join(tail))
    transformed = subprocess.check_output([
        circt / 'opt', '-load-pass-plugin=' + str(library),
        '-passes=chipyard-bound-codegen-blocks,verify', constrained, '-S',
        '-o', '-'
    ],
                                          text=True)
    assert 'chipyard.codegen.chunk' not in transformed

    # Inlining can propagate noimplicitfloat into a caller that had no wide
    # values or marker at the initial scan. Cleanup must include that caller.
    inline = build / 'inline.ll'
    inline.write_text(
        '''define internal void @wide(ptr %in, ptr %out) alwaysinline {
  %x = load i4096, ptr %in, align 1
  %y = add i4096 %x, 1
  store i4096 %y, ptr %out, align 1
  ret void
}
define void @narrow(ptr %in, ptr %out) {
  call void @wide(ptr %in, ptr %out)
  ret void
}
''')
    optimized = subprocess.check_output([
        circt / 'opt', '-load-pass-plugin=' + str(library),
        '-passes=chipyard-guard-wide-vectors,default<O3>,chipyard-restore-vector-flags,verify',
        inline, '-S', '-o', '-'
    ],
                                        text=True)
    assert 'define void @narrow' in optimized and 'define internal void @wide' not in optimized
    assert 'noimplicitfloat' not in optimized
    assert 'chipyard.temporary.vector.guard' not in optimized

    for name, pipeline, text in [
        ('double-begin',
         'chipyard-guard-wide-vectors,chipyard-guard-wide-vectors',
         'define void @f() { ret void }'),
        ('missing-begin', 'chipyard-restore-vector-flags',
         'define void @f() { ret void }'),
        ('reserved-marker', 'chipyard-guard-wide-vectors',
         'define void @f() "chipyard.temporary.vector.guard" { ret void }')
    ]:
        path = build / (name + '.ll')
        path.write_text(text)
        with (build / (name + '.log')).open('w') as log:
            result = subprocess.run([
                circt / 'opt', '-load-pass-plugin=' + str(library),
                '-passes=' + pipeline, path, '-disable-output'
            ],
                                    stdout=log,
                                    stderr=subprocess.STDOUT)
        assert result.returncode != 0, name + ' should reject invalid guard state'

    tiny = build / 'tiny.ll'
    tiny.write_text('define i64 @tiny(i64 %x) { ret i64 %x }\n')
    obj, bitcode = build / 'preserved.o', build / 'preserved.bc'
    obj.write_bytes(b'original object')
    bitcode.write_bytes(b'original bitcode')
    try:
        compile_model(tiny,
                      obj,
                      circt / 'opt',
                      Path('/bin/false'),
                      cache,
                      cxx,
                      bitcode=bitcode)
    except subprocess.CalledProcessError:
        pass
    else:
        raise AssertionError('failed code generator accepted')
    assert obj.read_bytes() == b'original object'
    assert bitcode.read_bytes() == b'original bitcode'
    (build / 'report.json').write_text(
        json.dumps(dict(status='passed',
                        levels=checks,
                        total_cache_checks=4608,
                        aggregate_codegen=aggregate_checks,
                        aggregate_stores=store_checks,
                        wide_shift_abi=wide_shift_checks,
                        scalar_shift=scalar_shift_checks,
                        early_inline=early_inline_checks,
                        wide_stores=wide_store_checks,
                        vector_packing=vector_packing_checks,
                        original_attributes_and_explicit_fp=True,
                        constrained_functions_preserved=True,
                        invalid_guard_states_rejected=True,
                        failed_compile_preserves_complete_outputs=True),
                   indent=2) + '\n')
    print(
        'PASS: bounded native LLVM codegen at O0/O2/O3, 4608 full cache images, '
        'original flags/FP, constrained functions, and failed-build preservation'
    )


if __name__ == '__main__':
    main()
