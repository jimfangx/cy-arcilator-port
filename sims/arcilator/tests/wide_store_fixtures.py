"""Independent full-byte arithmetic and callback oracle for wide stores."""
import ctypes
import random


def fixture(name,
            bits=1088,
            raw=None,
            variant='constant',
            polarity=True,
            barrier='none',
            condition='%enabled',
            masked_poison=False):
    raw = raw or bits + 256
    N = bits // 8
    K = int.from_bytes(bytes((i * 29 + 17 & 255 for i in range(N))), 'little')
    constant = K if variant == 'pattern' else 0
    other = K ^ (1 << bits) - 1
    lines = [
        f'define void @{name}(ptr %a, ptr %b, ptr %out, ptr %mask, i8 %branch8, i8 %enable8) {{',
        'entry:', '  call void @ordinary(i64 1, ptr null, i64 0)',
        '  %branch = trunc i8 %branch8 to i1',
        '  %enabled = trunc i8 %enable8 to i1',
        '  br i1 %branch, label %yes, label %no', 'yes:',
        f'  call void @ordinary(i64 3, ptr %a, i64 {raw // 8})',
        f'  %raw = load i{raw}, ptr %a, align 1'
    ]
    if raw != bits:
        lines += [f'  %small = trunc i{raw} %raw to i{bits}']
    value = '%small' if raw != bits else '%raw'
    if barrier == 'pred-call':
        lines += [f'  call void @ordinary(i64 5, ptr %a, i64 {raw // 8})']
    if barrier == 'pred-store':
        lines += ['  store i8 83, ptr %a, align 1']
    lines += ['  br label %join', 'no:']
    if variant == 'loads':
        lines += [
            f'  call void @ordinary(i64 4, ptr %b, i64 {raw // 8})',
            f'  %rawb = load i{raw}, ptr %b, align 1'
        ]
        if raw != bits:
            lines += [f'  %smallb = trunc i{raw} %rawb to i{bits}']
        false = '%smallb' if raw != bits else '%rawb'
    else:
        false = str(constant)
    lines += [
        '  br label %join', 'join:',
        f'  %phi = phi i{bits} [ {value}, %yes ], [ {false}, %no ]',
        f'  %old = load i{bits}, ptr %out, align 1',
        f'  %maskvalue = load i{bits}, ptr %mask, align 1'
    ]
    if barrier == 'join-call':
        lines += [f'  call void @ordinary(i64 7, ptr %a, i64 {raw // 8})']
    if barrier == 'join-store':
        lines += ['  store i8 167, ptr %a, align 1']
    if barrier == 'join-atomic':
        lines += ['  %atomic = load atomic i8, ptr %a monotonic, align 1']
    if variant == 'no-select':
        chosen = '%phi'
    else:
        left, right = ('%phi', str(other)) if polarity else (str(other),
                                                             '%phi')
        lines += [
            f'  %choice = select i1 {condition}, i{bits} {left}, i{bits} {right}'
        ]
        chosen = '%choice'
    if masked_poison:
        lines += [f'  %safe = select i1 %enabled, i{bits} {chosen}, i{bits} 0']
        chosen = '%safe'
    lines += [
        f'  %diff = xor i{bits} {chosen}, %old',
        f'  %masked = and i{bits} %diff, %maskvalue',
        f'  %result = xor i{bits} %masked, %old',
        f'  store i{bits} %result, ptr %out, align 1',
        '  call void @ordinary(i64 9, ptr null, i64 0)', '  ret void', '}'
    ]
    return ('\n'.join(lines) + '\n',
            dict(name=name,
                 bits=bits,
                 raw=raw,
                 variant=variant,
                 polarity=polarity,
                 barrier=barrier,
                 constant=constant,
                 other=other,
                 masked_poison=masked_poison))


def note(mem, tag, offset, length, trace):
    trace[0] = trace[0] * 17 + tag & (1 << 64) - 1
    trace[1] += 1
    if length:
        mem[offset] ^= tag
        mem[offset + length - 1] ^= tag * 11 & 255


def exercise(lib, configs, subset=False):
    lib.bind_trace.argtypes = [ctypes.c_void_p]
    checks = 0
    rng = random.Random(261)
    for c in configs:
        fn = getattr(lib, c['name'])
        fn.argtypes = [ctypes.c_void_p] * 4 + [ctypes.c_uint8] * 2
        fn.restype = None
        N = c['bits'] // 8
        raw = c['raw'] // 8
        for trial in range(4 if subset else 24):
            A = 256
            B = 4096
            M = 12288
            O = [8192, A, A + 1, A - 7, B + 3, M + 5][trial % 6]
            aliasmask = trial % 5 == 0
            M = A + 9 if aliasmask else M
            mem = bytearray((rng.getrandbits(8) for _ in range(16384)))
            expected = mem[:]
            branch = trial // 2 & 1
            enabled = trial & 1
            if c['masked_poison']:
                enabled = 0
            trace = [rng.getrandbits(64), 0]
            T = (ctypes.c_uint64 * 2)(*trace)
            buf = (ctypes.c_uint8 * len(mem)).from_buffer(mem)
            lib.bind_trace(T)
            note(expected, 1, 0, 0, trace)
            old = int.from_bytes(expected[O:O + N], 'little')
            mask = int.from_bytes(expected[M:M + N], 'little')
            if branch:
                note(expected, 3, A, raw, trace)
                picked = int.from_bytes(expected[A:A + N], 'little')
                if c['barrier'] == 'pred-call':
                    note(expected, 5, A, raw, trace)
                if c['barrier'] == 'pred-store':
                    expected[A] = 83
            elif c['variant'] == 'loads':
                note(expected, 4, B, raw, trace)
                picked = int.from_bytes(expected[B:B + N], 'little')
            else:
                picked = c['constant']
            if c['barrier'] == 'join-call':
                note(expected, 7, A, raw, trace)
            if c['barrier'] == 'join-store':
                expected[A] = 167
            if c['masked_poison']:
                picked = 0
            elif c['variant'] != 'no-select' and bool(
                    enabled) != c['polarity']:
                picked = c['other']
            expected[O:O + N] = (old ^ (picked ^ old) & mask).to_bytes(
                N, 'little')
            note(expected, 9, 0, 0, trace)
            base = ctypes.addressof(buf)
            fn(base + A, base + B, base + O, base + M, branch, enabled)
            assert mem == expected and list(T) == trace, (
                c['name'], trial, 'full bytes or callbacks mismatch')
            checks += 1
    return checks
