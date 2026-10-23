"""Independent packing value, boundary and effect fixtures."""
from pathlib import Path
import ctypes, datetime, hashlib, json, mmap, os, random, re, resource, signal, subprocess
import shlex
import sys


def check(sdk, cxx, r):
    sim = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(sim / 'compiler'))
    from partition import plugin
    from codegen import compile_model
    w = r / 'work' / 'packing-unit'
    w.mkdir(parents=True, exist_ok=True)
    lib = library = plugin(sdk / 'opt', cxx, sim / 'generated-src/compiler',
                           sim / 'compiler/VectorPacking.cpp')
    report = {
        'status': 'running',
        'scope':
        'Generic same-BB target-legal bitwise, zero-extension, scalar-conditioned select, constant shift packing. Independent byte/mutation/canary/alias arithmetic oracle with odd-width truncation after every left shift. Full padded-byte stores preserve destination canaries. Dirty raw source bytes use byte-sized loads plus unflagged truncation; original odd-width load poison semantics are not assumed away. NativeO0/O3, no original memory/effect/freeze changes. Targeted definedness and mutant controls are restricted and not a complete LLVM proof.',
        'steps': [],
        'byte_alias_callback_checks': 0,
        'guarded_page_checks': 0,
        'targeted_definedness_checks': 0
    }

    def save():
        (r /
         'packing-unit.json').write_text(json.dumps(report, indent=2) + '\n')

    def run(name, cmd):
        cmd = list(map(str, cmd))
        if cmd and cmd[0] == cxx: cmd = [*shlex.split(cxx), *cmd[1:]]
        report['steps'].append({'name': name, 'command': cmd})
        save()
        p = subprocess.Popen(
            ['/usr/bin/prlimit', '--as=4294967296', '--core=0', '--', *cmd],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            start_new_session=True)
        try:
            out, err = p.communicate(timeout=180)
        except BaseException:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
            raise
        (w / (name + '.log')).write_bytes(out + err)
        assert p.returncode == 0, (name, p.returncode, err.decode()[-1500:])
        return out.decode(), err.decode()

    def transform(name, src, out, flags=None):
        _, err = run(name, [
            sdk / 'opt', '-load-pass-plugin=' + str(library),
            '-passes=chipyard-vector-packing,verify',
            *(flags or ['-mcpu=native', '-mtriple=x86_64-unknown-linux-gnu']),
            '-S', src, '-o', out
        ])
        stats, = [
            json.loads(x[len('Vector packing: '):]) for x in err.splitlines()
            if x.startswith('Vector packing: ')
        ]
        return stats

    widths = [
        65, 73, 95, 118, 128, 130, 136, 168, 176, 240, 245, 256, 260, 328, 511,
        512
    ]
    configs = []
    source = [
        'target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"',
        'target triple = "x86_64-unknown-linux-gnu"',
        'declare void @mutate(ptr)'
    ]
    for bits in widths:
        patterns = [(0, 0, 0, 0), (1, 1, 1, 1), (63, 1, 1, 63), (64, 1, 1, 64),
                    (bits - 1, bits - 1, 1, 1),
                    (bits // 2, 1, bits - 1, bits - 1)]
        if bits > 65: patterns.append((65, 64, 64, 65))
        for shifts in patterns:
            for callback in [False, True]:
                name = 'g' + str(len(configs))
                n = (bits + 7) // 8
                bytebits = n * 8
                mask = (1 << bits) - 1
                a, b, c, d = shifts
                lines = [
                    f'define void @{name}(ptr %a, ptr %b, ptr %out) {{',
                    'entry:', f'  %x = load i{bits}, ptr %a, align 1',
                    f'  %y = load i{bits}, ptr %b, align 1',
                    '  %small = load i16, ptr %b, align 1',
                    f'  %z = zext i16 %small to i{bits}',
                    f'  %f = freeze i{bits} %x',
                    f'  %pred = trunc i{bits} %x to i1',
                    f'  %base = or i{bits} %f, %z',
                    f'  %left = shl i{bits} %base, {a}',
                    f'  %right = lshr i{bits} %left, {b}',
                    f'  %choice = select i1 %pred, i{bits} %right, i{bits} %y'
                ]
                previous = '%choice'
                ops = []
                for i in range(24):
                    op = ['and', 'xor', 'or'][i % 3]
                    k = ((mask // 11) ^ (mask >> (i % 7 + 1))) & mask
                    lines.append(f'  %v{i} = {op} i{bits} {previous}, {k}')
                    previous = '%v' + str(i)
                    ops.append((op, k))
                if callback:
                    lines.extend([
                        '  call void @mutate(ptr %a)',
                        f'  %new = load i{bits}, ptr %a, align 1'
                    ])
                    rhs = '%new'
                else:
                    rhs = '%f'
                lines.extend([
                    f'  %mix = xor i{bits} {previous}, {rhs}',
                    f'  %left2 = shl i{bits} %mix, {c}',
                    f'  %right2 = lshr i{bits} %left2, {d}',
                    f'  %result = or i{bits} %right2, %z'
                ])
                for value, suffix in [('result', '0'), ('left', '1')]:
                    stored = '%' + value
                    if bits != bytebits:
                        lines.append(
                            f'  %stored{suffix} = zext i{bits} %{value} to i{bytebits}'
                        )
                        stored = '%stored' + suffix
                    if suffix == '1':
                        lines.append(
                            f'  %other = getelementptr i8, ptr %out, i64 {n}')
                    lines.append(f'  store i{bytebits} {stored}, ptr ' +
                                 ('%out' if suffix == '0' else '%other') +
                                 ', align 1')
                lines.extend(['  ret void', '}'])
                source.append('\n'.join(lines))
                configs.append((name, bits, shifts, callback, ops))
    # These are normal scalar functions with a single choice across all vector lanes.
    for kind in ['poison', 'undef']:
        source.append(f'''define i8 @freeze_{kind}() {{
      %f = freeze i245 {kind}
      %a = shl i245 %f, 193
      %b = lshr i245 %a, 193
      %c = xor i245 %b, %b
      %d = or i245 %c, 37
      %e = xor i245 %d, 0
      %v = trunc i245 %e to i8
      ret i8 %v
    }}''')
        source.append(f'''define i8 @masked_{kind}() {{
      %a = shl i245 {kind}, 129
      %b = lshr i245 %a, 129
      %c = or i245 %b, 113
      %d = select i1 false, i245 %c, i245 37
      %v = trunc i245 %d to i8
      ret i8 %v
    }}''')
    # Explicit effect/flag/fallback probes; their special instructions must remain.
    source.append('''define i128 @guards(i128 %x, i128 %n) {
     %a = shl nuw i128 %x, 64
     %b = lshr exact i128 %a, 3
     %c = shl i128 %b, %n
     %d = shl i128 %c, 128
     ret i128 %d
    }''')
    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (4294967296, 4294967296))
        src = w / 'source.ll'
        unit_text = '\n'.join(source) + '\n'

        def byte_load(match):
            name, bits, address = match.groups()
            bits = int(bits)
            if bits % 8 == 0: return match.group(0)
            rounded = (bits + 7) // 8 * 8
            return f'  {name}.raw = load i{rounded}, {address}\n  {name} = trunc i{rounded} {name}.raw to i{bits}'

        unit_text = re.sub(
            r'^\s*(%[A-Za-z0-9$._-]+) = load i([0-9]+), ([^\n]+)',
            byte_load,
            unit_text,
            flags=re.M)
        src.write_text(unit_text)
        out = w / 'candidate.ll'
        stats = transform('transform', src, out)
        report['stats'] = stats
        assert stats['vector_operations'] > 0
        assert any(
            c['accepted'] and c['shifts'] and c['selects'] and c['extensions']
            for c in
            stats['costs']), 'Packing path not covered by admitted unit DAG'
        original = w / 'original.ll'
        run('canonical',
            [sdk / 'opt', '-passes=verify', '-S', src, '-o', original])
        old = original.read_text()
        new = out.read_text()
        effects = lambda t: [
            re.sub(r'(store i[0-9]+) %[^,]+,', r'\1 VALUE,', x.strip())
            for x in t.splitlines()
            if re.search(r'\b(load|store|call|freeze)\b', x) and not x.lstrip(
            ).startswith(';')
        ]
        assert effects(old) == effects(new)
        body = lambda t: re.search(r'^define[^\n]*@guards\([^\n]*\n(.*?)^}', t,
                                   re.M | re.S)[1]
        assert body(old) == body(new)
        big = w / 'big-endian.ll'
        big.write_text(src.read_text().replace('"e-m:e-', '"E-m:e-'))
        bigstats = transform('big-endian-fallback', big,
                             w / 'big-endian-candidate.ll')
        assert bigstats['vector_operations'] == 0
        (w / 'callbacks.cpp').write_text(
            'extern \"C\" { static unsigned calls; void mutate(unsigned char *p) { ++calls; p[0] ^= 0x5a; } void reset_count() { calls=0; } unsigned get_count() { return calls; } }\n'
        )
        run('callbacks-compile', [
            cxx, '-O3', '-fPIC', '-c', w / 'callbacks.cpp', '-o',
            w / 'callbacks.o'
        ])
        rng = random.Random(281)
        libc = ctypes.CDLL(None)
        libc.mprotect.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int
        ]
        page = mmap.PAGESIZE

        def exercise(dll, limit=None):
            for index, (name, bits, shifts, callback,
                        ops) in enumerate(configs):
                if limit is not None and index >= limit: break
                fn = getattr(dll, name)
                fn.argtypes = [ctypes.c_void_p] * 3
                fn.restype = None
                n = (bits + 7) // 8
                mask = (1 << bits) - 1
                sa, sb, sc, sd = shifts
                for repeat in range(24):
                    data = bytearray(rng.randbytes(512))
                    a, b, o = [(3, 80, 160), (3, 3, 160), (3, 80, 3),
                               (3, 80, 80), (3, 4, 4), (3, 80, 81)][repeat % 6]
                    expected = data[:]
                    x = int.from_bytes(expected[a:a + n], 'little') & mask
                    y = int.from_bytes(expected[b:b + n], 'little') & mask
                    z = int.from_bytes(expected[b:b + 2], 'little')
                    left = ((x | z) << sa) & mask
                    right = left >> sb
                    value = right if x & 1 else y
                    for opcode, k in ops:
                        value = (value & k) if opcode == 'and' else (
                            value ^ k) if opcode == 'xor' else (value | k)
                    if callback: expected[a] ^= 0x5a
                    after = int.from_bytes(expected[a:a + n],
                                           'little') & mask if callback else x
                    value = ((((value ^ after) << sc) & mask) >> sd) | z
                    expected[o:o + n] = value.to_bytes(n, 'little')
                    expected[o + n:o + 2 * n] = left.to_bytes(n, 'little')
                    buf = (ctypes.c_ubyte * len(data)).from_buffer(data)
                    dll.reset_count()
                    fn(
                        ctypes.addressof(buf) + a,
                        ctypes.addressof(buf) + b,
                        ctypes.addressof(buf) + o)
                    assert data == expected, ('packing-byte', name, bits,
                                              shifts, repeat)
                    assert dll.get_count() == int(callback)
                    report['byte_alias_callback_checks'] += 1
                for placement in [0, 1]:
                    maps = []
                    views = []
                    addresses = []
                    try:
                        for span in [n, n, 2 * n]:
                            mm = mmap.mmap(-1,
                                           3 * page,
                                           prot=mmap.PROT_READ
                                           | mmap.PROT_WRITE)
                            view = (ctypes.c_ubyte *
                                    (3 * page)).from_buffer(mm)
                            addr = ctypes.addressof(view)
                            assert libc.mprotect(
                                addr, page, 0) == 0 and libc.mprotect(
                                    addr + 2 * page, page, 0) == 0
                            position = addr + page if placement == 0 else addr + 2 * page - span
                            ctypes.memset(position, 0xa5, span)
                            maps.append(mm)
                            views.append(view)
                            addresses.append(position)
                        dll.reset_count()
                        fn(*addresses)
                        assert dll.get_count() == int(callback)
                        report['guarded_page_checks'] += 1
                    finally:
                        for mm, view in zip(maps, views):
                            del view
                        views.clear()
                        for mm in maps:
                            mm.close()

        for level in ['0', '3']:
            for phase, path in [('original', original), ('candidate', out)]:
                prefix = phase + '-O' + level
                run(prefix + '-llc', [
                    sdk / 'llc', '-O' + level, '-relocation-model=pic',
                    '-filetype=obj', '-mcpu=native', path, '-o', w /
                    (prefix + '.o')
                ])
                run(prefix + '-link', [
                    cxx, '-shared', w / (prefix + '.o'), w / 'callbacks.o',
                    '-o', w / (prefix + '.so')
                ])
                dll = ctypes.CDLL(str(w / (prefix + '.so')))
                exercise(dll)
                for kind in ['poison', 'undef']:
                    for label in ['freeze', 'masked']:
                        fn = getattr(dll, label + '_' + kind)
                        fn.argtypes = []
                        fn.restype = ctypes.c_uint8
                        for _ in range(64):
                            assert fn() == 37
                            report['targeted_definedness_checks'] += 1
                save()
        report['positive_byte_alias_callback_checks'] = report[
            'byte_alias_callback_checks']
        report['positive_guarded_page_checks'] = report['guarded_page_checks']
        # A deliberately missing odd-width mask must fail the independent byte oracle.
        mutant = w / 'missing-mask.ll'
        bad = re.sub(
            r'^(\s*%vp\.width[0-9]* = )and (<[0-9]+ x i64>) (%[^,]+), .*',
            r'\1or \2 \3, zeroinitializer',
            new,
            flags=re.M)
        assert bad != new
        mutant.write_text(bad)
        run('missing-mask-llc', [
            sdk / 'llc', '-O3', '-relocation-model=pic', '-filetype=obj',
            '-mcpu=native', mutant, '-o', w / 'missing-mask.o'
        ])
        run('missing-mask-link', [
            cxx, '-shared', w / 'missing-mask.o', w / 'callbacks.o', '-o',
            w / 'missing-mask.so'
        ])
        detected = False
        try:
            exercise(ctypes.CDLL(str(w / 'missing-mask.so')))
        except AssertionError as error:
            detected = 'packing-byte' in str(error)
        assert detected, 'Missing-width-mask fault escaped byte oracle'
        # Exercise the real compile_model API in both pipelines, with the option off/on.
        api = []
        for pipeline in ['stock', 'bounded']:
            for enabled in [False, True]:
                label = 'api-' + pipeline + ('-on' if enabled else '-off')
                obj = w / (label + '.o')
                timing = w / (label + '.json')
                builder_cxx = '/not-installed/c++' if pipeline == 'stock' and not enabled else cxx
                compile_model(
                    original,
                    obj,
                    sdk / 'opt',
                    sdk / 'llc',
                    sim / 'generated-src/compiler',
                    builder_cxx,
                    abi_buffers=False,
                    pipeline=pipeline,
                    report=timing,
                    opt_flags='-mcpu=native -mtriple=x86_64-unknown-linux-gnu',
                    vector_packing=enabled)
                data = json.loads(timing.read_text())
                assert data['vector_packing'] is enabled
                run(label + '-link', [
                    cxx, '-shared', obj, w / 'callbacks.o', '-o', w /
                    (label + '.so')
                ])
                dll = ctypes.CDLL(str(w / (label + '.so')))
                before = report['byte_alias_callback_checks']
                exercise(dll, limit=8)
                api.append({
                    'pipeline':
                    pipeline,
                    'enabled':
                    enabled,
                    'checks':
                    report['byte_alias_callback_checks'] - before
                })
        report['api'] = api
        tiny = w / 'failed-input.ll'
        tiny.write_text('define i64 @f(i64 %x) { ret i64 %x }\n')
        preserved = w / 'preserved.o'
        preserved_bc = w / 'preserved.bc'
        preserved.write_bytes(b'original object')
        preserved_bc.write_bytes(b'original bitcode')
        try:
            compile_model(tiny,
                          preserved,
                          sdk / 'opt',
                          Path('/bin/false'),
                          sim / 'generated-src/compiler',
                          cxx,
                          bitcode=preserved_bc,
                          vector_packing=True)
        except subprocess.CalledProcessError:
            pass
        else:
            raise AssertionError('Failed vector-enabled build accepted')
        assert preserved.read_bytes(
        ) == b'original object' and preserved_bc.read_bytes(
        ) == b'original bitcode'
        report['failed_vector_build_preserves_outputs'] = True
        report.update(status='passed',
                      masked_shift_fault_detected=True,
                      original_memory_effect_freeze_sequence_preserved=True,
                      big_endian_fallback=True,
                      flag_dynamic_and_out_of_range_shifts_unchanged=True,
                      completed_at=datetime.datetime.now(
                          datetime.timezone.utc).isoformat())
        save()
        print({
            k: v
            for k, v in report.items() if k not in ['steps', 'stats']
        },
              flush=True)
    except BaseException as e:
        report.update(status='failed', error=repr(e))
        save()
        raise
    return report
