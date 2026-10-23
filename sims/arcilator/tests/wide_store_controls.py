"""Boundary and restricted lifetime/poison controls for wide store motion."""
import ctypes
import datetime
import json
import random
import re
import resource
import shlex
import subprocess
from wide_store_fixtures import fixture


def check(sdk, cxx, library, r):
    w = r / 'work/controls'
    w.mkdir(parents=True)
    u = json.loads((r / 'unit.json').read_text())
    assert u[
        'status'] == 'passed_values_requires_definedness_and_boundary_controls'
    report = dict(
        status='running',
        scope=
        'Protected MMU boundaries with full mapped-page canaries, independent bytes/callback trace; additional SSA/lifetime CFG guards; restricted path interpreter for actual before/after fixture IR. This is not a complete LLVM semantic proof. A wrong critical-edge early store must fail native alias/callback oracle; a store before lifetime.start must fail independent definedness oracle.',
        boundary_runs=[],
        definedness=[],
        guards=[])

    def save():
        p = r / 'controls.json'
        q = p.with_suffix('.tmp')
        q.write_text(json.dumps(report, indent=2) + '\n')
        q.replace(p)

    def command(a):
        a = list(map(str, a))
        if a and a[0] == cxx: a = [*shlex.split(cxx), *a[1:]]
        return subprocess.run(a,
                              check=True,
                              capture_output=True,
                              text=True,
                              timeout=180)

    def lower(a, b):
        p = command([
            sdk / 'opt', '-load-pass-plugin=' + library,
            '-passes=chipyard-distribute-wide-stores,verify', '-S', a, '-o', b
        ])
        return json.loads(
            next(x[len('Wide store distribution: '):]
                 for x in p.stderr.splitlines()
                 if x.startswith('Wide store distribution: ')))

    configs = []
    for bits in [1032, 1088, 2048, 4160, 8192]:
        for kind in ['constant', 'pattern', 'loads', 'no-select']:
            for polarity in [False, True]:
                _, c = fixture('entry_' + str(len(configs)),
                               bits=bits,
                               variant=kind,
                               polarity=polarity)
                configs.append(c)
    for barrier in [
            'pred-call', 'pred-store', 'join-call', 'join-store', 'join-atomic'
    ]:
        _, c = fixture('entry_' + str(len(configs)),
                       bits=2048,
                       barrier=barrier)
        configs.append(c)
    for _ in range(4):
        _, c = fixture('entry_' + str(len(configs)))
        configs.append(c)

    libc = ctypes.CDLL(None)
    libc.mmap.argtypes = [
        ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_long
    ]
    libc.mmap.restype = ctypes.c_void_p
    libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    class Guarded:

        def __init__(self, n, tail, rng):
            self.n = n
            self.middle = ((n + 4095) // 4096) * 4096
            self.total = self.middle + 8192
            self.base = libc.mmap(None, self.total, 0, 0x22, -1, 0)
            assert self.base not in [None, ctypes.c_void_p(-1).value]
            assert libc.mprotect(self.base + 4096, self.middle, 3) == 0
            self.offset = self.middle - n if tail else 0
            self.ptr = self.base + 4096 + self.offset
            self.bytes = bytearray([0x91] * self.middle)
            self.bytes[self.offset:self.offset + n] = rng.randbytes(n)
            ctypes.memmove(self.base + 4096, bytes(self.bytes), self.middle)

        def data(self):
            return self.bytes[self.offset:self.offset + self.n]

        def write(self, data):
            self.bytes[self.offset:self.offset + self.n] = data

        def mutate(self, tag):
            self.bytes[self.offset] ^= tag
            self.bytes[self.offset + self.n - 1] ^= tag * 11 & 255

        def check(self):
            assert ctypes.string_at(
                self.base + 4096,
                self.middle) == self.bytes, 'mapped bytes/canaries changed'

        def close(self):
            assert libc.munmap(self.base, self.total) == 0

    def boundaries(so):
        lib = ctypes.CDLL(str(so))
        lib.bind_trace.argtypes = [ctypes.c_void_p]
        rng = random.Random(272)
        checks = 0
        for i in [0, 1, 3, 4, 6, 16, 24, 32, 45, 46, 47, 48]:
            c = configs[i]
            fn = getattr(lib, c['name'])
            fn.argtypes = [ctypes.c_void_p] * 4 + [ctypes.c_uint8] * 2
            fn.restype = None
            N = c['bits'] // 8
            for tail in [False, True]:
                for branch in [0, 1]:
                    for enabled in [0, 1]:
                        regions = [
                            Guarded(c['raw'] // 8, tail, rng),
                            Guarded(c['raw'] // 8, tail, rng),
                            Guarded(N, tail, rng),
                            Guarded(N, tail, rng)
                        ]
                        try:
                            a, b, out, mask = regions
                            old = int.from_bytes(out.data(), 'little')
                            m = int.from_bytes(mask.data(), 'little')
                            trace = [17, 0]

                            def note(tag, obj=None):
                                trace[0] = (trace[0] * 17 + tag) & (
                                    (1 << 64) - 1)
                                trace[1] += 1
                                if obj: obj.mutate(tag)

                            note(1)
                            if branch:
                                note(3, a)
                                picked = int.from_bytes(
                                    a.data()[:N], 'little')
                            elif c['variant'] == 'loads':
                                note(4, b)
                                picked = int.from_bytes(
                                    b.data()[:N], 'little')
                            else:
                                picked = c['constant']
                            if c['variant'] != 'no-select' and bool(
                                    enabled) != c['polarity']:
                                picked = c['other']
                            out.write((old ^ ((picked ^ old) & m)).to_bytes(
                                N, 'little'))
                            note(9)
                            T = (ctypes.c_uint64 * 2)(17, 0)
                            lib.bind_trace(T)
                            fn(a.ptr, b.ptr, out.ptr, mask.ptr, branch,
                               enabled)
                            assert list(T) == trace
                            for obj in regions:
                                obj.check()
                            checks += 1
                        finally:
                            for obj in regions:
                                obj.close()
        return checks

    DL = 'target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"\n'
    decl = 'declare void @llvm.lifetime.start.p0(ptr)\ndeclare void @llvm.lifetime.end.p0(ptr)\ndeclare void @ordinary(i64,ptr,i64)\n'
    life = '''define void @life_guard(ptr %out, i8 %choice) {
    entry:
     %space = alloca i1040, align 1
     %branch = trunc i8 %choice to i1
     br i1 %branch, label %left, label %right
    left: br label %join
    right: br label %join
    join:
     %v = phi i1040 [1, %left], [2, %right]
     call void @llvm.lifetime.start.p0(ptr %space)
     store i1040 %v, ptr %space, align 1
     %result = load i1040, ptr %space, align 1
     store i1040 %result, ptr %out, align 1
     call void @llvm.lifetime.end.p0(ptr %space)
     ret void
    }
    '''
    end = '''define void @source_end(ptr %out, i8 %choice) {
    entry:
     %space = alloca i1040, align 1
     call void @llvm.lifetime.start.p0(ptr %space)
     store i1040 37, ptr %space, align 1
     %branch = trunc i8 %choice to i1
     br i1 %branch, label %left, label %right
    left:
     %loaded = load i1040, ptr %space, align 1
     br label %join
    right: br label %join
    join:
     %v = phi i1040 [%loaded, %left], [0, %right]
     call void @llvm.lifetime.end.p0(ptr %space)
     store i1040 %v, ptr %out, align 1
     ret void
    }
    '''
    edge = '''define void @edge_guard(ptr %out, i8 %choice) {
    entry:
     %branch = trunc i8 %choice to i1
     br i1 %branch, label %left, label %join
    left:
     call void @ordinary(i64 19, ptr %out, i64 130)
     %loaded = load i1040, ptr %out, align 1
     br label %join
    join:
     %v = phi i1040 [1, %entry], [%loaded, %left]
     store i1040 %v, ptr %out, align 1
     ret void
    }
    '''
    masked = '''define void @masked_poison(ptr %out, i8 %choice) {
    entry:
     %branch = trunc i8 %choice to i1
     br i1 %branch, label %left, label %right
    left: br label %join
    right: br label %join
    join:
     %v = phi i1040 [poison, %left], [3, %right]
     %bad = select i1 poison, i1040 %v, i1040 97
     %safe = select i1 false, i1040 %bad, i1040 0
     store i1040 %safe, ptr %out, align 1
     ret void
    }
    '''
    POISON = object()

    def definedness(text, name, choice):
        # This interpreter is intentionally restricted to this fixture language.
        # Dead stores are errors; dead/uninitialized loads produce poison; select
        # evaluates only its chosen arm. No LLVM-wide proof is implied.
        m = re.search(
            r'^[ \t]*define void @' + name + r'\([^\n]*\) \{\n(.*?)\n[ \t]*\}',
            text, re.M | re.S)
        assert m, name
        body = m[1]
        blocks = {}
        label = 'entry'
        for line in body.splitlines():
            line = line.split(';')[0].strip()
            if not line: continue
            lab = re.match(r'^([\w.]+):\s*(.*)', line)
            if lab:
                label = lab[1]
                line = lab[2].strip()
            blocks.setdefault(label, [])
            if line: blocks[label].append(line)
        starts = set(
            re.findall(r'lifetime.start.p0\(ptr(?: nonnull)? (%[\w.]+)\)',
                       body))
        objects = {
            'out': dict(live=True, value=0, bits=1040)
        }
        values = {
            '%out': 'out',
            '%choice': choice
        }
        block = 'entry'
        previous = None

        def val(x):
            x = x.strip()
            if x in ['poison', 'undef']: return POISON
            if x in ['true', 'false']: return int(x == 'true')
            return values[x] if x.startswith('%') else int(x)

        for step in range(100):
            lines = blocks[block]
            updates = {}
            for line in lines:
                p = re.match(r'(%[\w.]+) = phi i(\d+) (.*)', line)
                if p:
                    arms = re.findall(r'\[\s*([^,]+),\s*%([\w.]+)\s*\]', p[3])
                    updates[p[1]] = val(
                        next(v for v, b in arms if b == previous))
            values.update(updates)
            jump = None
            for line in lines:
                if ' = phi ' in line: continue
                p = re.match(r'(%[\w.]+) = alloca i(\d+)', line)
                if p:
                    objects[p[1]] = dict(live=p[1] not in starts,
                                         value=POISON,
                                         bits=int(p[2]))
                    values[p[1]] = p[1]
                    continue
                p = re.match(r'(%[\w.]+) = trunc i8 (%[\w.]+) to i1', line)
                if p:
                    values[p[1]] = val(p[2]) & 1
                    continue
                p = re.match(
                    r'call void @llvm.lifetime.(start|end).p0\(ptr(?: nonnull)? (%[\w.]+)\)',
                    line)
                if p:
                    obj = objects[val(p[2])]
                    obj['live'] = p[1] == 'start'
                    obj['value'] = POISON
                    continue
                p = re.match(r'store i(\d+) ([^,]+), ptr (%[\w.]+)', line)
                if p:
                    obj = objects[val(p[3])]
                    if not obj['live']:
                        raise RuntimeError('store to dead allocation')
                    assert int(p[1]) <= obj['bits']
                    obj['value'] = val(p[2])
                    continue
                p = re.match(r'(%[\w.]+) = load i(\d+), ptr (%[\w.]+)', line)
                if p:
                    obj = objects[val(p[3])]
                    values[p[1]] = obj['value'] if obj['live'] else POISON
                    continue
                p = re.match(
                    r'(%[\w.]+) = select i1 ([^,]+), i\d+ ([^,]+), i\d+ (.*)',
                    line)
                if p:
                    c = val(p[2])
                    values[p[1]] = POISON if c is POISON else val(
                        p[3] if c else p[4])
                    continue
                p = re.match(
                    r'br i1 ([^,]+), label %([\w.]+), label %([\w.]+)', line)
                if p:
                    c = val(p[1])
                    assert c is not POISON, 'poison branch'
                    jump = p[2] if c else p[3]
                    break
                p = re.match(r'br label %([\w.]+)', line)
                if p:
                    jump = p[1]
                    break
                if line == 'ret void': return objects['out']['value']
                raise AssertionError(
                    ('unsupported interpreter instruction', line))
            assert jump
            previous, block = block, jump
        raise AssertionError('interpreter path budget')

    try:
        assert not (r / 'controls.json').exists()
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (4294967296, 4294967296))
        save()
        for phase in ['original', 'candidate']:
            for level in [0, 3]:
                so = r / 'work/unit' / f'{phase}-O{level}.so'
                n = boundaries(so)
                report['boundary_runs'].append(
                    dict(phase=phase,
                         level=level,
                         full_page_canary_and_callback_checks=n))
                save()
                print(report['boundary_runs'][-1], flush=True)
        src = w / 'original.ll'
        src.write_text(DL + decl + life + end + edge + masked)
        dest = w / 'candidate.ll'
        stats = lower(src, dest)
        assert stats['distributed_stores'] == 3 and stats[
            'split_edges'] == 1, stats
        report['additional_stats'] = stats
        for name, expected in [('life_guard', lambda c: 1 if c & 1 else 2),
                               ('source_end', lambda c: 37 if c & 1 else 0),
                               ('masked_poison', lambda c: 0)]:
            for phase, p in [('original', src), ('candidate', dest)]:
                for choice in range(8):
                    got = definedness(p.read_text(), name, choice)
                    assert got is not POISON and got == expected(choice), (
                        name, phase, choice)
                    report['definedness'].append(
                        dict(name=name, phase=phase, choice=choice, value=got))
        # A destination lifetime beginning in the join must block distribution.
        wrong_life = life.replace(
            'left: br label %join',
            'left:\n store i1040 1, ptr %space, align 1\n br label %join'
        ).replace(
            'right: br label %join',
            'right:\n store i1040 2, ptr %space, align 1\n br label %join'
        ).replace(' %v = phi i1040 [1, %left], [2, %right]\n',
                  '').replace(' store i1040 %v, ptr %space, align 1\n', '')
        bad = w / 'wrong-before-lifetime.ll'
        bad.write_text(DL + decl + wrong_life)
        command([sdk / 'opt', '-passes=verify', '-disable-output', bad])
        detected = 0
        for c in [0, 1]:
            try:
                definedness(bad.read_text(), 'life_guard', c)
            except RuntimeError:
                detected += 1
        assert detected == 2
        report[
            'wrong_lifetime_store_negative_definedness_paths_detected'] = detected
        # An extra predecessor store before its conditional branch is a valid-IR
        # semantic corruption: the true path's callback and load observe it.
        wrong_edge = edge.replace(
            ' %branch = trunc i8 %choice to i1',
            ' store i1040 1, ptr %out, align 1\n %branch = trunc i8 %choice to i1'
        )
        badedge = w / 'wrong-critical-edge.ll'
        badedge.write_text(DL + decl + wrong_edge)
        spy = r / 'work/unit/spy.o'
        for label, p in [('original', src), ('candidate', dest),
                         ('wrong-edge', badedge)]:
            for level in [0, 3]:
                obj = w / f'{label}-O{level}.o'
                command([
                    sdk / 'llc', '-O' + str(level), '-relocation-model=pic',
                    '-filetype=obj', p, '-o', obj
                ])
                so = obj.with_suffix('.so')
                command([cxx, '-shared', obj, spy, '-o', so])
                lib = ctypes.CDLL(str(so))
                lib.bind_trace.argtypes = [ctypes.c_void_p]
                fn = lib.edge_guard
                fn.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
                fn.restype = None
                failures = 0
                for choice in range(8):
                    mem = bytearray([0x91] * 256)
                    expected = mem[:]
                    trace = [17, 0]
                    if choice & 1:
                        expected[17] ^= 19
                        expected[17 + 129] ^= (19 * 11) & 255
                        trace = [17 * 17 + 19, 1]
                    else:
                        expected[17:17 + 130] = (1).to_bytes(130, 'little')
                    buf = (ctypes.c_uint8 * len(mem)).from_buffer(mem)
                    t = (ctypes.c_uint64 * 2)(17, 0)
                    lib.bind_trace(t)
                    fn(ctypes.addressof(buf) + 17, choice)
                    if mem != expected or list(t) != trace: failures += 1
                assert failures == (4 if label == 'wrong-edge' else
                                    0), (label, level, failures)
                report['guards'].append(
                    dict(name=label,
                         level=level,
                         native_critical_edge_negative_failures=failures))
                save()
        report.update(status='passed',
                      boundary_checks=sum(
                          x['full_page_canary_and_callback_checks']
                          for x in report['boundary_runs']),
                      definedness_positive_paths=len(report['definedness']),
                      finished_at=datetime.datetime.now(
                          datetime.timezone.utc).isoformat())
        save()
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        save()
        raise

    return report
