"""Check that compiler stability queries only admit output-only state changes."""
from pathlib import Path
import ast
import json
import ctypes
import random
import subprocess
import sys
import hashlib


def check(sdk, work, cxx):
    sim = Path(__file__).resolve().parents[1]
    work.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(sim))
    from compiler.partition import plugin
    cache = sim / 'generated-src/compiler'
    idle = plugin(sdk / 'bin/circt-opt', cxx, cache,
                  sim / 'compiler/IdleClock.cpp')
    conv = plugin(sdk / 'bin/circt-opt', cxx, cache,
                  sim / 'compiler/Convergence.cpp')
    lower = plugin(sdk / 'bin/circt-opt', cxx, cache,
                   sim / 'compiler/ArcToLLVM.cpp')
    tree = ast.parse((sim / 'tests/idle_clock.py').read_text())
    assign = next(n for n in tree.body if isinstance(n, ast.Assign) and any(
        isinstance(t, ast.Name) and t.id == 'base' for t in n.targets))
    base = ast.literal_eval(assign.value)
    env = {'base': base}
    second = next(n for n in tree.body if isinstance(n, ast.Assign) and any(
        isinstance(t, ast.Name) and t.id == 'two' for t in n.targets))
    exec(
        compile(ast.Module(body=[second], type_ignores=[]),
                '<existing-two-clock-fixture>', 'exec'), env)
    two = env['two']
    context = base.replace(
        '  %clk=',
        '  %ctx=arc.as_context %storage:!arc.storage\n  %never=hw.constant -1:i64\n  arc.set_next_wakeup %ctx, %never\n  %clk='
    )
    aux = ''' func.func @Test_set_wakeup(%s:!arc.storage,%w:i64) {
     %c=arc.as_context %s:!arc.storage
     arc.set_next_wakeup %c, %w
     return
     }
    '''
    context = context.replace(' arc.model', aux + ' arc.model', 1)
    report = {
        'status': 'running',
        'scope':
        'Compiler read-only stable-state query property checks. Runtime integration is tested separately. Unsettled output bytes and arbitrary ordinary inputs allowed; accepted frames may change only the certified output byte after full evaluation. Matching SDK context setter avoids guessing context offsets.',
        'cases': []
    }

    def run(cmd):
        subprocess.run(list(map(str, cmd)),
                       check=True,
                       stdout=subprocess.DEVNULL)

    try:
        for name, text, clocks in [('single', base, 'held'),
                                   ('two', two, 'held:other'),
                                   ('context', context, 'held')]:
            out = work / name
            out.mkdir(exist_ok=True)
            src = out / 'input.mlir'
            src.write_text(text)
            arc = out / 'certified.mlir'
            run([
                sdk / 'bin/circt-opt', src, '--load-pass-plugin=' + str(conv),
                '--load-pass-plugin=' + str(idle),
                '--pass-pipeline=builtin.module(chipyard-certify-write-only-outputs,chipyard-certify-idle-clock{held-inputs='
                + clocks + ' stable-evaluator=true})', '-o', arc
            ])
            assert 'chipyard.stable_combinational_fn' in arc.read_text()
            run([
                sdk / 'bin/circt-opt', arc, '--load-pass-plugin=' + str(lower),
                '--pass-pipeline=builtin.module(arc-infer-context,chipyard-lower-arc-to-llvm,reconcile-unrealized-casts)',
                '-o', out / 'llvm.mlir'
            ])
            run([
                sdk / 'bin/mlir-translate', '--mlir-to-llvmir',
                out / 'llvm.mlir', '-o', out / 'model.ll'
            ])
            for level in ['0', '3']:
                run([
                    sdk / 'bin/opt', '-O' + level, out / 'model.ll', '-o',
                    out / ('O' + level + '.bc')
                ])
                run([
                    sdk / 'bin/llc', '-O' + level, '-relocation-model=pic',
                    '-filetype=obj', out / ('O' + level + '.bc'), '-o',
                    out / ('O' + level + '.o')
                ])
                run([
                    cxx, '-shared', out / ('O' + level + '.o'), '-o',
                    out / ('O' + level + '.so')
                ])
                dll = ctypes.CDLL(str(out / ('O' + level + '.so')))
                query = dll.Test_stable_combinational
                query.argtypes = [ctypes.c_void_p]
                query.restype = ctypes.c_uint32
                evaluate = dll.Test_eval
                evaluate.argtypes = [ctypes.c_void_p]
                stable_evaluate = dll.Test_stable_eval
                stable_evaluate.argtypes = [ctypes.c_void_p]
                set_wakeup = None
                if name == 'context':
                    set_wakeup = dll.Test_set_wakeup
                    set_wakeup.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
                rng = random.Random(198)
                accepted = rejected = 0
                for trial in range(65536):
                    frame = bytearray(rng.randbytes(64))
                    held = trial & 1
                    history = (trial >> 1) & 1
                    frame[24] = held
                    frame[28] = history
                    expected = held == history
                    if name == 'two':
                        frame[29] = (trial >> 2) & 1
                        frame[30] = (trial >> 3) & 1
                        expected &= frame[29] == frame[30]
                    if name == 'context':
                        wakeup = [0, 7, (1 << 64) - 1,
                                  1 << 63][(trial >> 2) & 3]
                        set_wakeup((ctypes.c_uint8 * 64).from_buffer(frame),
                                   wakeup)
                        expected &= wakeup == (1 << 64) - 1
                    before = bytes(frame)
                    result = query((ctypes.c_uint8 * 64).from_buffer(frame))
                    assert bytes(frame) == before, 'query modified state'
                    assert result == int(expected), (name, level, trial,
                                                     result, expected)
                    if result:
                        accepted += 1
                        fast_frame = bytearray(frame)
                        stable_evaluate(
                            (ctypes.c_uint8 * 64).from_buffer(fast_frame))
                        evaluate((ctypes.c_uint8 * 64).from_buffer(frame))
                        assert fast_frame == frame, (
                            name, level, trial, 'stable evaluator differs')
                        assert frame[:26] == before[:26] and frame[
                            27:] == before[27:], (name, level, trial,
                                                  'non-output state changed')
                    else:
                        rejected += 1
                # Exhaust physical bytes too. Logical i1 equality alone misses
                # padding normalization/propagation in the original evaluator.
                idle_fn = dll.Test_idle_clock
                idle_fn.argtypes = [ctypes.c_void_p]
                idle_fn.restype = ctypes.c_uint32
                dirty_cases = 0
                dirty_slots = [(24, 28),
                               (29, 30)] if name == 'two' else [(24, 28)]
                for held_slot, history_slot in dirty_slots:
                    for held_byte in range(256):
                        for history_byte in range(256):
                            if held_byte <= 1 and history_byte <= 1:
                                continue
                            frame = bytearray(rng.randbytes(64))
                            frame[24] = frame[28] = 0
                            if name == 'two':
                                frame[29] = frame[30] = 0
                            frame[held_slot] = held_byte
                            frame[history_slot] = history_byte
                            if name == 'context':
                                set_wakeup(
                                    (ctypes.c_uint8 * 64).from_buffer(frame),
                                    (1 << 64) - 1)
                            before = bytes(frame)
                            assert not query(
                                (ctypes.c_uint8 * 64).from_buffer(frame))
                            assert bytes(
                                frame) == before, 'dirty query modified state'
                            assert not idle_fn(
                                (ctypes.c_uint8 * 64).from_buffer(frame))
                            assert bytes(
                                frame
                            ) == before, 'dirty idle fallback modified state'
                            dirty_cases += 1
                report['cases'].append({
                    'dirty_padding_fallback_checks_per_helper':
                    dirty_cases,
                    'fixture':
                    name,
                    'optimization':
                    level,
                    'read_only_queries':
                    65536,
                    'accepted_exact_nonoutput_state_comparisons':
                    accepted,
                    'rejected':
                    rejected
                })
                print('stable query',
                      name,
                      level,
                      accepted,
                      rejected,
                      flush=True)
        report.update(status='passed',
                      compiler_source_sha256=hashlib.sha256(
                          (sim /
                           'compiler/IdleClock.cpp').read_bytes()).hexdigest())
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    except BaseException as e:
        report.update(status='failed', error=repr(e))
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        raise

    # The pinned LLVM load contract requires canonical padding for non-byte
    # integer loads. Evaluate only valid payloads, retaining independent raw-byte
    # oracles. Dirty payload padding is tested through the read-only query alone.
    # https://raw.githubusercontent.com/llvm/llvm-project/a17069fea5b271c6824ea773874b007116de9644/llvm/docs/LangRef.md
    report['partial_width_registers'] = []
    for width in [1, 3, 7, 9, 65, 127, 1025, 2051]:
        out = work / ('partial-' + str(width))
        out.mkdir(exist_ok=True)
        source = f"""module attributes {{chipyard.output_readback_safe=true,chipyard.source_time_dependence=false}} {{
 arc.model @Test io !hw.modty<input held:i1,input data:i{width},output y:i{width}> storageBytes 2048 {{
 ^bb0(%s:!arc.storage):
  %held=arc.root_input \"held\", %s {{offset=24:i32}}:(!arc.storage)->!arc.state<i1>
  %data=arc.root_input \"data\", %s {{offset=128:i32}}:(!arc.storage)->!arc.state<i{width}>
  %y=arc.root_output \"y\", %s {{offset=1152:i32}}:(!arc.storage)->!arc.state<i{width}>
  %reg=arc.alloc_state %s {{offset=640:i32}}:(!arc.storage)->!arc.state<i{width}>
  %history=arc.alloc_state %s {{offset=25:i32}}:(!arc.storage)->!arc.state<i1>
  %clock=arc.state_read %held:<i1>
  %old=arc.state_read %history:<i1>
  arc.state_write %history = %clock:<i1>
  %change=comb.xor %clock,%old:i1
  %edge=comb.and %change,%clock:i1
  scf.if %edge {{
   %v=arc.state_read %data:<i{width}>
   arc.state_write %reg = %v:<i{width}>
  }}
  %value=arc.state_read %reg:<i{width}>
  arc.state_write %y = %value:<i{width}>
 }}
}}
"""
        src = out / 'input.mlir'
        src.write_text(source)
        arc = out / 'certified.mlir'
        run([
            sdk / 'bin/circt-opt', src, '--load-pass-plugin=' + str(conv),
            '--load-pass-plugin=' + str(idle),
            '--pass-pipeline=builtin.module(chipyard-certify-write-only-outputs,chipyard-certify-idle-clock{held-inputs=held stable-evaluator=true})',
            '-o', arc
        ])
        assert 'chipyard.stable_combinational_fn' in arc.read_text()
        run([
            sdk / 'bin/circt-opt', arc, '--load-pass-plugin=' + str(lower),
            '--pass-pipeline=builtin.module(arc-infer-context,chipyard-lower-arc-to-llvm,reconcile-unrealized-casts)',
            '-o', out / 'llvm.mlir'
        ])
        run([
            sdk / 'bin/mlir-translate', '--mlir-to-llvmir', out / 'llvm.mlir',
            '-o', out / 'model.ll'
        ])
        for level in ['0', '3']:
            run([
                sdk / 'bin/opt', '-O' + level, out / 'model.ll', '-o',
                out / ('O' + level + '.bc')
            ])
            run([
                sdk / 'bin/llc', '-O' + level, '-relocation-model=pic',
                '-filetype=obj', out / ('O' + level + '.bc'), '-o',
                out / ('O' + level + '.o')
            ])
            run([
                cxx, '-shared', out / ('O' + level + '.o'), '-o',
                out / ('O' + level + '.so')
            ])
            dll = ctypes.CDLL(str(out / ('O' + level + '.so')))
            query = dll.Test_stable_combinational
            query.argtypes = [ctypes.c_void_p]
            query.restype = ctypes.c_uint32
            evaluate = dll.Test_eval
            evaluate.argtypes = [ctypes.c_void_p]
            stable_evaluate = dll.Test_stable_eval
            stable_evaluate.argtypes = [ctypes.c_void_p]
            rng = random.Random(198 + width)
            size = (width + 7) // 8
            for trial in range(8192):
                frame = bytearray(rng.randbytes(2048))
                frame[24] = frame[25] = trial & 1
                if width % 8:
                    tail_mask = (1 << (width % 8)) - 1
                    frame[128 + size - 1] &= tail_mask
                    frame[640 + size - 1] &= tail_mask
                before = bytes(frame)
                expected_output = before[640:640 + size]
                assert query((ctypes.c_uint8 * 2048).from_buffer(frame)) == 1
                assert bytes(frame) == before
                fast_frame = bytearray(frame)
                stable_evaluate(
                    (ctypes.c_uint8 * 2048).from_buffer(fast_frame))
                evaluate((ctypes.c_uint8 * 2048).from_buffer(frame))
                assert fast_frame[:1152] == frame[:1152] and fast_frame[
                    1152 + size:] == frame[1152 + size:]
                assert frame[1152:1152 + size] == expected_output, (
                    width, level, trial, 'full evaluator output differs')
                assert fast_frame[1152:1152 + size] == expected_output, (
                    width, level, trial, 'stable evaluator output differs')
                assert frame[:1152] == before[:1152] and frame[
                    1152 + size:] == before[1152 + size:], (width, level,
                                                            trial)
                if width % 8:
                    dirty = bytearray(before)
                    dirty[128 + size - 1] |= 0xff ^ tail_mask
                    dirty[640 + size - 1] |= 0xff ^ tail_mask
                    dirty_before = bytes(dirty)
                    assert query(
                        (ctypes.c_uint8 * 2048).from_buffer(dirty)) == 1
                    assert bytes(dirty) == dirty_before, (
                        width, level, trial, 'dirty payload query wrote state')
            report['partial_width_registers'].append({
                'width':
                width,
                'optimization':
                level,
                'exact_nonoutput_state_checks':
                8192,
                'stable_evaluator_declared_output_and_nonoutput_checks':
                8192,
                'defined_input_and_register_payload_checks':
                8192,
                'independent_complete_output_byte_checks':
                8192,
                'query_only_dirty_payload_frames':
                8192 if width % 8 else 0
            })
            print('partial register', width, level, '8192', flush=True)
    (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    return report
