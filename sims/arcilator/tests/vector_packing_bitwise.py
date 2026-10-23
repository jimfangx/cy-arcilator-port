"""Independent bitwise value, boundary and effect fixtures."""
from pathlib import Path
import ctypes, datetime, hashlib, json, mmap, os, random, re, resource, signal, subprocess
import shlex
import sys


def check(sdk, cxx, r):
    sim = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(sim / 'compiler'))
    from partition import plugin
    from codegen import compile_model
    w = r / 'work' / 'unit'
    w.mkdir(parents=True, exist_ok=True)
    lib = library = plugin(sdk / 'opt', cxx, sim / 'generated-src/compiler',
                           sim / 'compiler/VectorPacking.cpp')
    report = {
        'status': 'running',
        'scope':
        'Private same-BB target-legal unflagged pure bitwise DAG vector representation. Independent arbitrary-byte/canary/alias/callback oracle, input loads and padded-byte stores unchanged; odd-width values explicitly truncated from byte loads; arbitrary dirty raw source bytes defined. NativeO0/O3 and guarded pages, existing freezes not cloned. Definedness checks are restricted targeted controls, not a full LLVM refinement proof.',
        'steps': [],
        'checks': 0,
        'guarded_page_checks': 0,
        'definedness_native_checks': 0
    }

    def save():
        (r / 'unit.json').write_text(json.dumps(report, indent=2) + '\n')

    def command(name, args):
        args = list(map(str, args))
        if args and args[0] == cxx: args = [*shlex.split(cxx), *args[1:]]
        row = {'name': name, 'command': args}
        report['steps'].append(row)
        save()
        p = subprocess.Popen(
            ['/usr/bin/prlimit', '--as=4294967296', '--core=0', '--', *args],
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
        (w / (name + '.stderr')).write_bytes(err)
        assert p.returncode == 0, (name, p.returncode, err.decode()[-1200:])
        return out.decode(), err.decode()

    def lower(name, src, dst, flags=None):
        _, err = command(name, [
            sdk / 'opt', '-load-pass-plugin=' + str(lib),
            '-passes=chipyard-vector-packing,verify',
            *(flags or ['-mcpu=native', '-mtriple=x86_64-unknown-linux-gnu']),
            '-S', src, '-o', dst
        ])
        rows = [
            json.loads(x[len('Vector packing: '):]) for x in err.splitlines()
            if x.startswith('Vector packing: ')
        ]
        assert len(rows) == 1
        return rows[0]

    widths = [
        65, 73, 95, 118, 128, 130, 136, 168, 176, 240, 245, 256, 260, 328, 511,
        512
    ]

    def fixture(bits, name, callback=True, optnone=False):
        bytebits = (bits + 7) // 8 * 8
        mask = (1 << bits) - 1
        lines = [
            f'define void @{name}(ptr %a, ptr %b, ptr %out)' +
            (' noinline optnone' if optnone else '') + ' {', 'entry:',
            f'  %x = load i{bits}, ptr %a, align 1',
            f'  %y = load i{bits}, ptr %b, align 1',
            f'  %f = freeze i{bits} %x', f'  %v0 = and i{bits} %f, %y'
        ]
        ops = []
        for i in range(1, 19):
            opcode = ['xor', 'or', 'and'][i % 3]
            constant = ((mask // 7) ^ (mask >> (i % 5 + 1))) & mask
            operand = '%y' if i % 4 == 0 else str(constant)
            lines.append(f'  %v{i} = {opcode} i{bits} %v{i-1}, {operand}')
            ops.append((opcode, constant if i % 4 else None))
            if i == 9 and callback:
                lines.extend([
                    '  call void @mutate(ptr %a)',
                    f'  %new = load i{bits}, ptr %a, align 1'
                ])
        lines.extend([
            f'  %joined = xor i{bits} %v6, %v18',
            f'  %result = and i{bits} %joined, ' +
            ('%new' if callback else '%f')
        ])
        for var, suffix in [('result', '0'), ('v6', '1')]:
            if bytebits != bits:
                lines.append(
                    f'  %store{suffix} = zext i{bits} %{var} to i{bytebits}')
                value = '%store' + suffix
            else:
                value = '%' + var
            if suffix == '1':
                lines.append(
                    f'  %other = getelementptr i8, ptr %out, i64 {(bits+7)//8}'
                )
            lines.append(f'  store i{bytebits} {value}, ptr ' +
                         ('%out' if suffix == '0' else '%other') + ', align 1')
        lines.extend(['  ret void', '}'])
        return '\n'.join(lines) + '\n', ops

    try:
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (4294967296, 4294967296))
        source = [
            'target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"',
            'target triple = "x86_64-unknown-linux-gnu"',
            'declare void @mutate(ptr)'
        ]
        configs = []
        for bits in widths:
            for callback in [False, True]:
                name = 'f' + str(len(configs))
                body, ops = fixture(bits, name, callback)
                source.append(body)
                configs.append((name, bits, callback, ops))
        body, _ = fixture(256, 'unchanged_optnone', False, True)
        source.append(body)
        # A frozen scalar input stays one choice, including across vector lanes.
        for kind in ['poison', 'undef']:
            source.append(f'''define i8 @frozen_{kind}() {{
      %f = freeze i256 {kind}
      %a = and i256 %f, 1234567
      %b = or i256 %a, 7654321
      %c = xor i256 %b, %f
      %d = xor i256 %c, %b
      %e = xor i256 %d, %f
      %v = trunc i256 %e to i8
      ret i8 %v
    }}''')
            source.append(f'''define i8 @masked_{kind}(i256 %x) {{
      %a = and i256 %x, {kind}
      %b = or i256 %a, 11223344
      %c = xor i256 %b, 55667788
      %d = select i1 false, i256 %c, i256 37
      %v = trunc i256 %d to i8
      ret i8 %v
    }}''')
        source.extend(
            f'define i8 @masked_call_{kind}() {{ %r = call i8 @masked_{kind}(i256 0) ret i8 %r }}'
            for kind in ['poison', 'undef'])
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
        candidate = w / 'candidate.ll'
        stats = lower('transform', src, candidate)
        report['stats'] = stats
        assert stats['vector_dags'] > 0 and stats['vector_operations'] > 0
        canonical = w / 'original.ll'
        command('canonical',
                [sdk / 'opt', '-passes=verify', '-S', src, '-o', canonical])
        old = canonical.read_text()
        new = candidate.read_text()
        memory = lambda text: [
            re.sub(r'(store i[0-9]+) %[^,]+,', r'\1 VALUE,', x.strip())
            for x in text.splitlines()
            if re.search(r'\b(load|store|call|freeze)\b', x) and not x.lstrip(
            ).startswith(';')
        ]
        assert memory(old) == memory(
            new), 'Original memory/effect/freeze sequence changed'

        def body(text, name):
            return re.search(r'^define[^\n]*@' + name + r'\([^\n]*\n(.*?)^}',
                             text, re.M | re.S)[1]

        assert body(old, 'unchanged_optnone') == body(new, 'unchanged_optnone')
        fallback = w / 'fallback.ll'
        fallback_stats = lower('unsupported-vector-target', src, fallback, [
            '-mcpu=x86-64', '-mattr=-sse,-sse2,-avx,-avx2,-avx512f',
            '-mtriple=x86_64-unknown-linux-gnu'
        ])
        assert fallback_stats['vector_operations'] == 0
        # Poison-producing disjoint flags remain scalar; no flags dropped.
        flagged = w / 'flagged.ll'
        flagged.write_text(
            fixture(256, 'flagged', False)[0].replace(' = or i256',
                                                      ' = or disjoint i256'))
        flaggedout = w / 'flagged-candidate.ll'
        fstats = lower('flagged', flagged, flaggedout)
        assert flaggedout.read_text().count(
            'or disjoint') == flagged.read_text().count('or disjoint')
        (w / 'callbacks.cpp').write_text('''#include <cstdint>
    static unsigned calls;
    extern "C" void mutate(unsigned char *p) { ++calls; p[0] ^= 0x5a; }
    extern "C" void reset_count() { calls=0; }
    extern "C" unsigned get_count() { return calls; }
    ''')
        command('callback-compile', [
            cxx, '-O3', '-fPIC', '-c', w / 'callbacks.cpp', '-o',
            w / 'callbacks.o'
        ])
        rng = random.Random(280)
        libc = ctypes.CDLL(None)
        libc.mprotect.argtypes = [
            ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int
        ]
        page = mmap.PAGESIZE
        for level in ['0', '3']:
            for phase, path in [('original', canonical),
                                ('candidate', candidate)]:
                prefix = phase + '-O' + level
                command(prefix + '-llc', [
                    sdk / 'llc', '-O' + level, '-relocation-model=pic',
                    '-filetype=obj', '-mcpu=native', path, '-o', w /
                    (prefix + '.o')
                ])
                command(prefix + '-link', [
                    cxx, '-shared', w / (prefix + '.o'), w / 'callbacks.o',
                    '-o', w / (prefix + '.so')
                ])
                dll = ctypes.CDLL(str(w / (prefix + '.so')))
                for name, bits, callback, ops in configs:
                    fn = getattr(dll, name)
                    fn.argtypes = [ctypes.c_void_p] * 3
                    fn.restype = None
                    n = (bits + 7) // 8
                    mask = (1 << bits) - 1
                    for repeat in range(48):
                        data = bytearray(rng.randbytes(512))
                        a, b, o = [(3, 80, 160), (3, 3, 160), (3, 80, 3),
                                   (3, 80, 80), (3, 4, 4),
                                   (3, 80, 81)][repeat % 6]
                        expected = data[:]
                        x = int.from_bytes(expected[a:a + n], 'little') & mask
                        y = int.from_bytes(expected[b:b + n], 'little') & mask
                        v = x & y
                        six = None
                        for i, (op, k) in enumerate(ops, 1):
                            rhs = y if k is None else k
                            v = (v ^ rhs) if op == 'xor' else (
                                v | rhs) if op == 'or' else (v & rhs)
                            if i == 6: six = v
                            if i == 9 and callback: expected[a] ^= 0x5a
                        after = int.from_bytes(
                            expected[a:a +
                                     n], 'little') & mask if callback else x
                        value = (six ^ v) & after
                        expected[o:o + n] = value.to_bytes(n, 'little')
                        expected[o + n:o + 2 * n] = six.to_bytes(n, 'little')
                        buf = (ctypes.c_ubyte * len(data)).from_buffer(data)
                        dll.reset_count()
                        fn(
                            ctypes.addressof(buf) + a,
                            ctypes.addressof(buf) + b,
                            ctypes.addressof(buf) + o)
                        assert data == expected, (prefix, name, repeat,
                                                  'byte/alias/callback')
                        assert dll.get_count() == int(callback)
                        report['checks'] += 1
                    # Put each input and output range immediately against inaccessible pages.
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
                for kind in ['poison', 'undef']:
                    fn = getattr(dll, 'frozen_' + kind)
                    fn.argtypes = []
                    fn.restype = ctypes.c_uint8
                    for _ in range(64):
                        assert fn() == 0
                        report['definedness_native_checks'] += 1
                    masked = getattr(dll, 'masked_call_' + kind)
                    masked.argtypes = []
                    masked.restype = ctypes.c_uint8
                    for _ in range(64):
                        assert masked() == 37
                        report['definedness_native_checks'] += 1
                save()
        report.update(status='passed',
                      memory_effect_freeze_sequence_unchanged=True,
                      optnone_fallback_unchanged=True,
                      unsupported_target_no_vectorization=True,
                      poison_flags_retained=True,
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
