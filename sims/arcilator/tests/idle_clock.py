from pathlib import Path
import ctypes, json, random, subprocess, sys

root = Path(__file__).resolve().parents[1] / 'generated-src/idle-clock-test'
root.mkdir(parents=True, exist_ok=True)
main = Path(__file__).resolve().parents[1]
import argparse, os

parser = argparse.ArgumentParser()
parser.add_argument('--circt-bin', type=Path, required=True)
args = parser.parse_args()
sdk = args.circt_bin.resolve().parent
sys.path.insert(0, str(main))
from compiler.partition import plugin

cxx = os.environ.get('CXX', 'c++')
lib = plugin(sdk / 'bin/circt-opt', cxx, root / 'plugin-cache',
             main / 'compiler/IdleClock.cpp')
conv = plugin(sdk / 'bin/circt-opt', cxx, main / 'generated-src/compiler',
              main / 'compiler/Convergence.cpp')
lower = plugin(sdk / 'bin/circt-opt', cxx, main / 'generated-src/compiler',
               main / 'compiler/ArcToLLVM.cpp')
out = root / 'unit'
out.mkdir(exist_ok=True)
base = '''module attributes {chipyard.output_readback_safe=true,chipyard.source_time_dependence=false} {
 arc.model @Test io !hw.modty<input held:i1,input data:i8,output y:i8> storageBytes 64 {
 ^bb0(%storage:!arc.storage):
  %h=arc.root_input "held", %storage {offset=24:i32}:(!arc.storage)->!arc.state<i1>
  %d=arc.root_input "data", %storage {offset=25:i32}:(!arc.storage)->!arc.state<i8>
  %y=arc.root_output "y", %storage {offset=26:i32}:(!arc.storage)->!arc.state<i8>
  %reg=arc.alloc_state %storage {offset=27:i32}:(!arc.storage)->!arc.state<i8>
  %prev=arc.alloc_state %storage {offset=28:i32}:(!arc.storage)->!arc.state<i1>
  %clk=arc.state_read %h:<i1>
  %old=arc.state_read %prev:<i1>
  arc.state_write %prev = %clk:<i1>
  %change=comb.xor %clk, %old:i1
  %edge=comb.and %change, %clk:i1
  scf.if %edge {
   %v=arc.state_read %d:<i8>
   arc.state_write %reg = %v:<i8>
  }
  %value=arc.state_read %reg:<i8>
  arc.state_write %y = %value:<i8>
 }
}
'''
cases = {
    'positive': (base, True),
    'negative': (base.replace(
        '%edge=comb.and %change, %clk:i1',
        '%one=hw.constant 1:i1\n  %low=comb.xor %clk, %one:i1\n  %edge=comb.and %change, %low:i1'
    ), False),
    'both-edges': (base.replace('scf.if %edge', 'scf.if %change'), False),
    'else-effect': (base.replace('  %value=', '  %value=').replace(
        '  }\n  %value=',
        '  } else {\n   %v=arc.state_read %d:<i8>\n   arc.state_write %reg = %v:<i8>\n  }\n  %value='
    ), False),
    'unguarded-register': (base.replace(
        '  %value=',
        '  %v=arc.state_read %d:<i8>\n  arc.state_write %reg = %v:<i8>\n  %value='
    ), False),
    'clock-output':
    (base.replace('  %value=',
                  '  %wide=arith.extui %clk:i1 to i8\n  %value=').replace(
                      'arc.state_write %y = %value',
                      'arc.state_write %y = %wide'), False),
    'history-output':
    (base.replace('  %value=',
                  '  %wide=arith.extui %old:i1 to i8\n  %value=').replace(
                      'arc.state_write %y = %value',
                      'arc.state_write %y = %wide'), False),
    'clock-dependent-branch': (base.replace('scf.if %edge',
                                            'scf.if %old'), False),
    'partial-alias': (base.replace(
        '  %clk=',
        '  %alias=arc.storage.get %storage[27]: !arc.storage -> !arc.state<i16>\n  %clk='
    ), False),
    'private-alias-descriptor': (base.replace(
        '  %clk=',
        '  %alias=arc.alloc_state %storage {offset=27:i32}:(!arc.storage)->!arc.state<i8>\n  %clk='
    ), False),
    'exact-storage-alias': (base.replace(
        '  %value=arc.state_read %reg',
        '  %alias=arc.storage.get %storage[27]: !arc.storage -> !arc.state<i8>\n  %value=arc.state_read %alias'
    ), True),
    'out-of-bounds': (base.replace('offset=28:i32', 'offset=64:i32'), False),
    'reserved-prefix': (base.replace('offset=28:i32', 'offset=16:i32'), False),
    'time-dependent': (base.replace('source_time_dependence=false',
                                    'source_time_dependence=true'), False),
    'missing-time-proof':
    (base.replace(',chipyard.source_time_dependence=false', ''), False),
    'effectful-source': (base.replace('output_readback_safe=true',
                                      'output_readback_safe=false'), False),
    'sampling-input': (base.replace(
        'source_time_dependence=false',
        'source_time_dependence=false,chipyard.sampling_inputs={Test="data"}'),
                       False),
    'unknown-call':
    (base.replace(' arc.model',
                  ' func.func private @unknown()\n arc.model').replace(
                      '  %value=',
                      '  func.call @unknown():()->()\n  %value='), False),
    'guarded-unknown-call':
    (base.replace(' arc.model',
                  ' func.func private @unknown()\n arc.model').replace(
                      '   %v=',
                      '   func.call @unknown():()->()\n   %v='), True),
    'storage-escape': (base.replace(
        ' arc.model',
        ' func.func private @escape(!arc.storage)\n arc.model').replace(
            '  %clk=',
            '  func.call @escape(%storage):(!arc.storage)->()\n  %clk='),
                       False),
    'wakeup-never': (base.replace(
        '  %clk=',
        '  %ctx=arc.as_context %storage:!arc.storage\n  %never=hw.constant -1:i64\n  arc.set_next_wakeup %ctx, %never\n  %clk='
    ), True),
    'wakeup-pending': (base.replace(
        '  %clk=',
        '  %ctx=arc.as_context %storage:!arc.storage\n  %when=hw.constant 7:i64\n  arc.set_next_wakeup %ctx, %when\n  %clk='
    ), False),
    'wakeup-guarded': (base.replace(
        '  %clk=',
        '  %ctx=arc.as_context %storage:!arc.storage\n  %never=hw.constant -1:i64\n  %clk='
    ).replace('   %v=', '   arc.set_next_wakeup %ctx, %never\n   %v='), False),
    'history-extra-write': (base.replace(
        '  %value=',
        '  %zero=hw.constant 0:i1\n  arc.state_write %prev = %zero:<i1>\n  %value='
    ), False),
}
report = {'status': 'running', 'structural': [], 'compiled': []}
two = base.replace(
    'input held:i1,input data', 'input held:i1,input other:i1,input data'
).replace(
    '  %clk=',
    '''  %h2=arc.root_input "other", %storage {offset=29:i32}:(!arc.storage)->!arc.state<i1>
  %prev2=arc.alloc_state %storage {offset=30:i32}:(!arc.storage)->!arc.state<i1>
  %clk2=arc.state_read %h2:<i1>
  %old2=arc.state_read %prev2:<i1>
  arc.state_write %prev2 = %clk2:<i1>
  %change2=comb.xor %clk2, %old2:i1
  %edge2=comb.and %clk2, %change2:i1
  %clk=''').replace(
        '  %value=', '''  scf.if %edge2 {
   %v2=arc.state_read %d:<i8>
   arc.state_write %reg = %v2:<i8>
  }
  %value=''')
cases.update({
    'two-positive': (two, True),
    'two-mixed-polarity': (two.replace(
        '%edge2=comb.and %clk2, %change2:i1',
        '%one2=hw.constant 1:i1\n  %low2=comb.xor %clk2, %one2:i1\n  %edge2=comb.and %low2, %change2:i1'
    ), False),
    'async-reset-guard': (base.replace(
        '  scf.if %edge {',
        '  %data=arc.state_read %d:<i8>\n  %reset=comb.extract %data from 0:(i8)->i1\n  %active=comb.or %edge, %reset:i1\n  scf.if %active {'
    ), False),
    'edge-and-enable-left': (base.replace(
        '  scf.if %edge {',
        '  %data=arc.state_read %d:<i8>\n  %enable=comb.extract %data from 0:(i8)->i1\n  %active=arith.andi %edge, %enable:i1\n  scf.if %active {'
    ), True),
    'edge-and-enable-right': (base.replace(
        '  scf.if %edge {',
        '  %data=arc.state_read %d:<i8>\n  %enable=comb.extract %data from 0:(i8)->i1\n  %active=arith.andi %enable, %edge:i1\n  scf.if %active {'
    ), True),
})
for name, (source, expected) in cases.items():
    src = out / (name + '.mlir')
    dst = out / (name + '-after.mlir')
    src.write_text(source)
    clocks = 'held:other' if name.startswith('two-') else 'held'
    command = [
        str(sdk / 'bin/circt-opt'),
        str(src), '--load-pass-plugin=' + str(conv),
        '--load-pass-plugin=' + str(lib),
        '--pass-pipeline=builtin.module(chipyard-certify-write-only-outputs,chipyard-certify-idle-clock{held-inputs='
        + clocks + ' stable-evaluator=true})', '-o',
        str(dst)
    ]
    p = subprocess.run(command, capture_output=True, text=True)
    assert p.returncode == 0, (name, p.stderr)
    actual = 'chipyard.idle_clock_fn' in dst.read_text()
    assert actual == expected, (name, actual, expected, p.stderr)
    report['structural'].append({
        'name': name,
        'certified': actual,
        'diagnostic': p.stderr
    })
print('structural', len(cases), flush=True)
source = out / 'positive-after.mlir'
lowered = out / 'positive-llvm.mlir'
llvm = out / 'positive.ll'
subprocess.run([
    str(sdk / 'bin/circt-opt'),
    str(source), '--load-pass-plugin=' + str(lower),
    '--pass-pipeline=builtin.module(arc-infer-context,chipyard-lower-arc-to-llvm,reconcile-unrealized-casts)',
    '-o',
    str(lowered)
],
               check=True)
subprocess.run([
    str(sdk / 'bin/mlir-translate'), '--mlir-to-llvmir',
    str(lowered), '-o',
    str(llvm)
],
               check=True)
for opt in ['0', '3']:
    bc = out / ('test-O' + opt + '.bc')
    obj = bc.with_suffix('.o')
    so = bc.with_suffix('.so')
    subprocess.run(
        [str(sdk / 'bin/opt'), '-O' + opt,
         str(llvm), '-o',
         str(bc)],
        check=True)
    subprocess.run([
        str(sdk / 'bin/llc'), '-O' + opt, '-relocation-model=pic',
        '-filetype=obj',
        str(bc), '-o',
        str(obj)
    ],
                   check=True)
    subprocess.run([cxx, '-shared', str(obj), '-o', str(so)], check=True)
    dll = ctypes.CDLL(str(so))
    evaluate = dll.Test_eval
    evaluate.argtypes = [ctypes.c_void_p]
    idle = dll.Test_idle_clock
    idle.argtypes = [ctypes.c_void_p]
    idle.restype = ctypes.c_uint32
    rng = random.Random(195)
    count = 0
    for old in [0, 1]:
        for data in range(256):
            for register in range(256):
                raw = bytearray(rng.randbytes(64))
                raw[24:29] = bytes([old, data, 0, register, old])
                state = (ctypes.c_ubyte * 64).from_buffer(raw)
                evaluate(state)
                evaluate(state)
                raw[24] = 0
                expected = bytearray(raw)
                trial = bytearray(raw)
                evaluate((ctypes.c_ubyte * 64).from_buffer(expected))
                assert idle((ctypes.c_ubyte * 64).from_buffer(trial))
                assert trial == expected, (opt, old, data, register)
                # Repeated settling after the helper is an exact identity.
                before = bytes(trial)
                evaluate((ctypes.c_ubyte * 64).from_buffer(trial))
                assert bytes(trial) == before
                # A clock that stays high is also an identity.
                trial[24] = 1
                trial[28] = 1
                before = bytes(trial)
                assert idle((ctypes.c_ubyte * 64).from_buffer(trial))
                assert bytes(trial) == before
                # Ineligible rising clocks return false without any writes.
                trial[24] = 1
                trial[28] = 0
                before = bytes(trial)
                assert not idle((ctypes.c_ubyte * 64).from_buffer(trial))
                assert bytes(trial) == before
                count += 1
    report['compiled'].append({
        'optimization': opt,
        'exact_state_comparisons': count,
        'post_helper_identity_checks': count,
        'fallback_no_write_checks': count
    })
    print('compiled', opt, count, flush=True)
source = out / 'two-positive-after.mlir'
lowered = out / 'two-llvm.mlir'
llvm = out / 'two.ll'
subprocess.run([
    str(sdk / 'bin/circt-opt'),
    str(source), '--load-pass-plugin=' + str(lower),
    '--pass-pipeline=builtin.module(arc-infer-context,chipyard-lower-arc-to-llvm,reconcile-unrealized-casts)',
    '-o',
    str(lowered)
],
               check=True)
subprocess.run([
    str(sdk / 'bin/mlir-translate'), '--mlir-to-llvmir',
    str(lowered), '-o',
    str(llvm)
],
               check=True)
for opt in ['0', '3']:
    bc = out / ('two-O' + opt + '.bc')
    obj = bc.with_suffix('.o')
    so = bc.with_suffix('.so')
    subprocess.run(
        [str(sdk / 'bin/opt'), '-O' + opt,
         str(llvm), '-o',
         str(bc)],
        check=True)
    subprocess.run([
        str(sdk / 'bin/llc'), '-O' + opt, '-relocation-model=pic',
        '-filetype=obj',
        str(bc), '-o',
        str(obj)
    ],
                   check=True)
    subprocess.run([cxx, '-shared', str(obj), '-o', str(so)], check=True)
    dll = ctypes.CDLL(str(so))
    evaluate = dll.Test_eval
    evaluate.argtypes = [ctypes.c_void_p]
    idle = dll.Test_idle_clock
    idle.argtypes = [ctypes.c_void_p]
    idle.restype = ctypes.c_uint32
    rng = random.Random(195)
    eligible = fallbacks = 0
    for clocks in range(16):
        old1, old2, new1, new2 = [(clocks >> i) & 1 for i in range(4)]
        for trial_index in range(2048):
            raw = bytearray(rng.randbytes(64))
            raw[24] = old1
            raw[28] = old1
            raw[29] = old2
            raw[30] = old2
            state = (ctypes.c_ubyte * 64).from_buffer(raw)
            evaluate(state)
            evaluate(state)
            raw[24] = new1
            raw[29] = new2
            expected = bytearray(raw)
            trial = bytearray(raw)
            actual = idle((ctypes.c_ubyte * 64).from_buffer(trial))
            want = not ((new1 and not old1) or (new2 and not old2))
            assert actual == want, (opt, clocks)
            if actual:
                eligible += 1
                evaluate((ctypes.c_ubyte * 64).from_buffer(expected))
                assert trial == expected
                before = bytes(trial)
                evaluate((ctypes.c_ubyte * 64).from_buffer(trial))
                assert bytes(trial) == before
            else:
                fallbacks += 1
                assert trial == raw
    report['compiled'].append({
        'optimization': opt,
        'two_clock_combinations': 16,
        'exact_state_and_identity_checks': eligible,
        'fallback_no_write_checks': fallbacks
    })
    print('two clocks', opt, eligible, fallbacks, flush=True)
import hashlib

report['helper_source_sha256'] = hashlib.sha256(
    (main / 'compiler/IdleClock.cpp').read_bytes()).hexdigest()

# Execute the real generated model and helper through the native runtime. The
# reference retains full evaluations; callback and waveform records must match.
runtime_source = base.replace(
    'input data:i8,output y:i8',
    'input data:i8,input physical:i1,input reset:i1,output y:i8,output observed:i1,output echo:i8'
)
runtime_source = runtime_source.replace(
    '  %clk=',
    '''  %physical=arc.root_input "physical", %storage {offset=31:i32}:(!arc.storage)->!arc.state<i1>
  %reset=arc.root_input "reset", %storage {offset=33:i32}:(!arc.storage)->!arc.state<i1>
  %observed=arc.root_output "observed", %storage {offset=32:i32}:(!arc.storage)->!arc.state<i1>
  %echo=arc.root_output "echo", %storage {offset=34:i32}:(!arc.storage)->!arc.state<i8>
  %incoming=arc.state_read %d:<i8>
  arc.state_write %echo = %incoming:<i8>
  %p=arc.state_read %physical:<i1>
  arc.state_write %observed = %p:<i1>
  %clk=''')
runtime_source = runtime_source.replace(
    '   arc.state_write %reg = %v:<i8>', '''   %r=arc.state_read %reset:<i1>
   %preset=hw.constant 90:i8
   %next=comb.mux %r, %preset, %v:i8
   arc.state_write %reg = %next:<i8>''')
src = out / 'runtime.mlir'
dst = out / 'runtime-after.mlir'
src.write_text(runtime_source)
subprocess.run([
    str(sdk / 'bin/circt-opt'),
    str(src), '--load-pass-plugin=' + str(conv),
    '--load-pass-plugin=' + str(lib),
    '--pass-pipeline=builtin.module(chipyard-certify-write-only-outputs,chipyard-certify-idle-clock{held-inputs=held stable-evaluator=true})',
    '-o',
    str(dst)
],
               check=True)
assert 'chipyard.idle_clock_fn' in dst.read_text()
subprocess.run([
    str(sdk / 'bin/circt-opt'),
    str(dst), '--load-pass-plugin=' + str(lower),
    '--pass-pipeline=builtin.module(arc-infer-context,chipyard-lower-arc-to-llvm,reconcile-unrealized-casts)',
    '-o',
    str(out / 'runtime-llvm.mlir')
],
               check=True)
subprocess.run([
    str(sdk / 'bin/mlir-translate'), '--mlir-to-llvmir',
    str(out / 'runtime-llvm.mlir'), '-o',
    str(out / 'runtime.ll')
],
               check=True)
subprocess.run([
    str(sdk / 'bin/opt'), '-O3',
    str(out / 'runtime.ll'), '-o',
    str(out / 'runtime.bc')
],
               check=True)
subprocess.run([
    str(sdk / 'bin/llc'), '-O3', '-relocation-model=pic', '-filetype=obj',
    str(out / 'runtime.bc'), '-o',
    str(out / 'runtime-model.o')
],
               check=True)
import shlex

objects = []
with (out / 'runtime-build.log').open('w') as log:
    for source in sorted((main / 'native').glob('*.cpp')):
        obj = out / ('runtime-' + source.stem + '.o')
        subprocess.run([
            *shlex.split(cxx), '-O2', '-std=c++17', '-DARC_NATIVE_NO_MAIN',
            *(['-fno-rtti'] if source.name == 'files.cpp' else []),
            '-I' + str(sdk / 'include'), '-I' + str(main),
            '-I' + str(main / 'native/include'), '-c',
            str(source), '-o',
            str(obj)
        ],
                       stdout=log,
                       stderr=subprocess.STDOUT,
                       check=True)
        objects.append(obj)
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(main),
        '-I' + str(sdk / 'include'),
        str(main / 'tests/native_idle_clock.cpp'),
        str(out / 'runtime-model.o'), *map(str, objects),
        '-Wl,--wrap=arcRuntimeOnEval', '-L' + str(sdk / 'lib'),
        '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle', '-lz',
        '-lpthread', '-ldl', '-o',
        str(out / 'runtime-test')
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
subprocess.run([str(out / 'runtime-test')], cwd=out, check=True)
report['runtime_quartets'] = 80
report['runtime_contract_rejections'] = 11

# Reuse the same coherent native objects for callback-schedule conformance.
# The fixture compares every observable callback and state byte against opt-out.
with (out / 'callback-schedule-build.log').open('w') as log:
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(main),
        '-I' + str(sdk / 'include'),
        str(main / 'tests/callback_schedule.cpp'), *map(str, objects),
        '-Wl,--wrap=arcRuntimeOnEval', '-L' + str(sdk / 'lib'),
        '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle', '-lz',
        '-lpthread', '-ldl', '-o',
        str(out / 'callback-schedule-test')
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
subprocess.run([str(out / 'callback-schedule-test')], cwd=out, check=True)
report['callback_schedules'] = {
    'recreations': 3,
    'entrypoints': ['run', 'advance_fs'],
    'whole_state_and_callback_sequence_identical': True,
    'unmarked_hidden_and_extended_overrides_retained': True,
}

# An unreported commit write must defeat the identity step. Verify that the
# callback/waveform oracle detects intentionally removing that snapshot guard.
fault_source = (main / 'native/runtime.cpp').read_text()
guard = '!std::memcmp(current->modelState, staged->modelState, size)'
assert fault_source.count(guard) == 1
fault = out / 'runtime-fault.cpp'
fault.write_text(fault_source.replace(guard, 'true'))
with (out / 'fault-build.log').open('w') as log:
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(sdk / 'include'),
        '-I' + str(main), '-I' + str(main / 'native'),
        '-I' + str(main / 'native/include'), '-c',
        str(fault), '-o',
        str(out / 'runtime-fault.o')
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
    fault_objects = [
        out / 'runtime-fault.o' if p.name == 'runtime-runtime.o' else p
        for p in objects
    ]
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(main),
        '-I' + str(sdk / 'include'),
        str(main / 'tests/native_idle_clock.cpp'),
        str(out / 'runtime-model.o'), *map(str, fault_objects),
        '-Wl,--wrap=arcRuntimeOnEval', '-L' + str(sdk / 'lib'),
        '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle', '-lz',
        '-lpthread', '-ldl', '-o',
        str(out / 'runtime-fault')
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
import resource

fault_run = subprocess.run(
    [str(out / 'runtime-fault')],
    cwd=out,
    capture_output=True,
    text=True,
    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
assert fault_run.returncode != 0 and 'callback sequence' in fault_run.stderr, fault_run
report['unreported_commit_fault_detected'] = True

# A false query must retain the full snapshot. The manual held-clock case
# changes private history during settling and makes a forced-true query visible
# through the independent callback-sequence oracle.
fault_query_source = (main / 'native/runtime.cpp').read_text()
query_call = 'stable_combinational_model(current->modelState)'
assert fault_query_source.count(query_call) == 1
fault_query = out / 'runtime-query-fault.cpp'
fault_query.write_text(
    fault_query_source.replace(query_call, '(' + query_call + ', true)'))
with (out / 'query-fault-build.log').open('w') as log:
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(sdk / 'include'),
        '-I' + str(main), '-I' + str(main / 'native'),
        '-I' + str(main / 'native/include'), '-c',
        str(fault_query), '-o',
        str(out / 'runtime-query-fault.o')
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
    fault_objects = [
        out / 'runtime-query-fault.o' if p.name == 'runtime-runtime.o' else p
        for p in objects
    ]
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(main),
        '-I' + str(sdk / 'include'),
        str(main / 'tests/native_idle_clock.cpp'),
        str(out / 'runtime-model.o'), *map(str, fault_objects),
        '-Wl,--wrap=arcRuntimeOnEval', '-L' + str(sdk / 'lib'),
        '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle', '-lz',
        '-lpthread', '-ldl', '-o',
        str(out / 'runtime-query-fault')
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
fault_run = subprocess.run(
    [str(out / 'runtime-query-fault')],
    cwd=out,
    capture_output=True,
    text=True,
    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
assert fault_run.returncode != 0 and 'callback sequence' in fault_run.stderr, fault_run
report['forced_stable_query_fault_detected'] = True

# A model-state write from the SDK hook must invalidate an earlier query.
# The runtime fixture marks this call as requiring full evaluation.
fault_post_source = (main / 'native/runtime.cpp').read_text()
post_query_call = 'stable_combinational_model(state->modelState)'
assert fault_post_source.count(post_query_call) == 1
fault_post = out / 'runtime-post-sdk-fault.cpp'
fault_post.write_text(
    fault_post_source.replace(post_query_call,
                              '(' + post_query_call + ', true)'))
with (out / 'post-sdk-fault-build.log').open('w') as log:
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(sdk / 'include'),
        '-I' + str(main), '-I' + str(main / 'native'),
        '-I' + str(main / 'native/include'), '-c',
        str(fault_post), '-o',
        str(out / 'runtime-post-sdk-fault.o')
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
    fault_objects = [
        out /
        'runtime-post-sdk-fault.o' if p.name == 'runtime-runtime.o' else p
        for p in objects
    ]
    subprocess.run([
        *shlex.split(cxx), '-O2', '-std=c++17', '-I' + str(main),
        '-I' + str(sdk / 'include'),
        str(main / 'tests/native_idle_clock.cpp'),
        str(out / 'runtime-model.o'), *map(str, fault_objects),
        '-Wl,--wrap=arcRuntimeOnEval', '-L' + str(sdk / 'lib'),
        '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle', '-lz',
        '-lpthread', '-ldl', '-o',
        str(out / 'runtime-post-sdk-fault')
    ],
                   stdout=log,
                   stderr=subprocess.STDOUT,
                   check=True)
fault_run = subprocess.run(
    [str(out / 'runtime-post-sdk-fault')],
    cwd=out,
    capture_output=True,
    text=True,
    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
assert fault_run.returncode != 0 and 'stale pre-SDK certificate' in fault_run.stderr, fault_run
report['stale_post_sdk_query_fault_detected'] = True

from idle_clock_bindings import check as check_bindings

report['bindings'] = check_bindings(sdk, root / 'bindings', cxx)
from stable_snapshots import check as check_stable_snapshots

report['stable_snapshots'] = check_stable_snapshots(sdk,
                                                    root / 'stable-snapshots',
                                                    cxx)
report['status'] = 'passed'
(root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
