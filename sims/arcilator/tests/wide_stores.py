#!/usr/bin/env python3
"""Check opt-in wide store distribution, memory ordering and definedness."""
import ctypes
import datetime
import json
import os
from pathlib import Path
import random
import re
import resource
import shlex
import subprocess
import sys
import tempfile

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim / 'compiler'))
from codegen import compile_model
from partition import plugin
from wide_store_fixtures import fixture as base_fixture, exercise


def _check(sdk, cxx, r):
    w = r / 'work/unit'
    w.mkdir(parents=True)
    library = plugin(sdk / 'opt', cxx, sim / 'generated-src/compiler',
                     sim / 'compiler/WideStores.cpp')
    report = dict(
        status='running',
        scope=
        'Full retained-width store distribution over closed SSA expressions. Independent bytes, aliases, callback order, edge execution and conservative CFG/effect guards.',
        runs=[],
        guards=[])

    def save():
        p = r / 'unit.json'
        q = p.with_suffix('.tmp')
        q.write_text(json.dumps(report, indent=2) + '\n')
        q.replace(p)

    def command(args):
        args = list(map(str, args))
        if args and args[0] == cxx:
            args = [*shlex.split(cxx), *args[1:]]
        return subprocess.run(args,
                              check=True,
                              capture_output=True,
                              text=True,
                              timeout=180)

    def lower(src, dest):
        p = command([
            sdk / 'opt', '-load-pass-plugin=' + str(library),
            '-passes=chipyard-distribute-wide-stores,verify', '-S', src, '-o',
            dest
        ])
        rows = [
            json.loads(x[len('Wide store distribution: '):])
            for x in p.stderr.splitlines()
            if x.startswith('Wide store distribution: ')
        ]
        if not rows:
            assert not p.stderr
            return dict(distributed_stores=0, split_edges=0)
        assert len(rows) == 1
        return rows[0]

    def normalized(p):
        return '\n'.join(x for x in p.read_text().splitlines()
                         if not x.startswith('; ModuleID ='))

    DL = 'target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n8:16:32:64-S128"\n'
    configs = []

    def fixture(name,
                critical=False,
                life=False,
                chain_freeze=False,
                **kwargs):
        text, c = base_fixture(name, **kwargs)
        bits = c['bits']
        loads = f'  %old = load i{bits}, ptr %out, align 1\n  %maskvalue = load i{bits}, ptr %mask, align 1\n'
        assert text.count(loads) == 1
        text = text.replace(loads, '')
        point = '  br i1 %branch, label %yes, label %no'
        assert text.count(point) == 1
        text = text.replace(point, loads + point)
        if critical:
            assert c['variant'] in ['constant', 'pattern', 'no-select']
            text = text.replace('label %yes, label %no',
                                'label %yes, label %join')
            text = text.replace('no:\n  br label %join\n', '')
            text = text.replace(', %no ]', ', %entry ]')
        if life:
            text = text.replace(
                'entry:\n',
                'entry:\n  %scratch = alloca i64, align 8\n  call void @llvm.lifetime.start.p0(ptr %scratch)\n'
            )
            phi = next(x for x in text.splitlines() if ' = phi i' in x)
            text = text.replace(
                phi + '\n',
                phi + '\n  call void @llvm.lifetime.end.p0(ptr %scratch)\n')
        if chain_freeze:
            phi = next(x for x in text.splitlines() if ' = phi i' in x)
            text = text.replace(phi + '\n',
                                phi + f'\n  %once = freeze i{bits} %phi\n')
            text = text.replace(f'i{bits} %phi,', f'i{bits} %once,')
        return text, c

    decl = 'declare void @ordinary(i64, ptr, i64)\ndeclare void @llvm.lifetime.start.p0(ptr)\ndeclare void @llvm.lifetime.end.p0(ptr)\n'

    def guards():
        text, _ = fixture('probe')
        cases = [
            ('eligible', text, 1),
            ('critical-edge', fixture('probe', critical=True)[0], 1),
            ('unrelated-lifetime-end', fixture('probe', life=True)[0], 1),
            ('closed-freeze', fixture('probe', chain_freeze=True)[0], 1),
            ('join-call', fixture('probe', barrier='join-call')[0], 0),
            ('join-store', fixture('probe', barrier='join-store')[0], 0),
            ('join-atomic', fixture('probe', barrier='join-atomic')[0], 0),
            ('pred-call-preserved', fixture('probe',
                                            barrier='pred-call')[0], 1),
            ('pred-store-preserved', fixture('probe',
                                             barrier='pred-store')[0], 1),
            ('nonbyte', fixture('probe', bits=1033)[0], 0),
            ('volatile-target',
             text.replace('  store i1088', '  store volatile i1088'), 0),
            ('optnone',
             text.replace('i8 %enable8) {',
                          'i8 %enable8) noinline optnone {'), 0),
            ('shared-phi',
             text.replace('  %choice =',
                          '  %extra = trunc i1088 %phi to i8\n  %choice ='),
             0),
            ('join-freeze-dependency',
             text.replace(
                 '  %choice =',
                 '  %local = freeze i1 %enabled\n  %choice =').replace(
                     'select i1 %enabled,', 'select i1 %local,'), 0),
            ('join-alias-read',
             text.replace('  %choice =',
                          '  %read = load i8, ptr %out, align 1\n  %choice ='),
             0),
            ('join-lifetime-start',
             text.replace('entry:\n',
                          'entry:\n  %scratch = alloca i64, align 8\n').
             replace(
                 '  %choice =',
                 '  call void @llvm.lifetime.start.p0(ptr %scratch)\n  %choice ='
             ), 0)
        ]
        for name, body, expected in cases:
            a = w / (name + '.ll')
            b = w / (name + '-candidate.ll')
            a.write_text(DL + decl + body)
            stats = lower(a, b)
            assert stats['distributed_stores'] == expected, (name, stats)
            if not expected:
                assert normalized(a) == normalized(b) or normalized(
                    command_canonical(a)) == normalized(b), (
                        name, 'fallback changed IR')
            if name == 'critical-edge':
                assert stats['split_edges'] == 1
            assert len(re.findall(r'\bload\b', a.read_text())) == len(
                re.findall(r'\bload\b',
                           b.read_text())), (name, 'source loads changed')
            report['guards'].append(dict(name=name, stats=stats))

    def command_canonical(p):
        q = p.with_name(p.stem + '-original-canonical.ll')
        command([sdk / 'opt', '-passes=verify', '-S', p, '-o', q])
        return q

    def three_checks(lib):
        fn = lib.three
        fn.argtypes = [ctypes.c_void_p, ctypes.c_uint8]
        fn.restype = None
        for choice in range(24):
            data = bytearray([0x73] * 512)
            expected = data[:]
            expected[17:17 + 136] = (choice % 3 + 1).to_bytes(136, 'little')
            buf = (ctypes.c_uint8 * len(data)).from_buffer(data)
            fn(ctypes.addressof(buf) + 17, choice)
            assert data == expected, ('three-incoming', choice)
        return 24

    try:
        assert not (r / 'unit.json').exists()
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_AS, (4294967296, 4294967296))
        save()
        guards()
        source = [DL, decl]
        for bits in [1032, 1088, 2048, 4160, 8192]:
            for kind in ['constant', 'pattern', 'loads', 'no-select']:
                for polarity in [False, True]:
                    t, c = fixture('entry_' + str(len(configs)),
                                   bits=bits,
                                   variant=kind,
                                   polarity=polarity)
                    source.append(t)
                    configs.append(c)
        for barrier in [
                'pred-call', 'pred-store', 'join-call', 'join-store',
                'join-atomic'
        ]:
            t, c = fixture('entry_' + str(len(configs)),
                           bits=2048,
                           barrier=barrier)
            source.append(t)
            configs.append(c)
        for critical, life, freezing in [(True, False, False),
                                         (False, True, False),
                                         (True, True, False),
                                         (False, False, True)]:
            t, c = fixture('entry_' + str(len(configs)),
                           critical=critical,
                           life=life,
                           chain_freeze=freezing)
            source.append(t)
            configs.append(c)
        for condition in ['poison', 'undef']:
            t, c = fixture('entry_' + str(len(configs)),
                           condition=condition,
                           masked_poison=True)
            source.append(t)
            configs.append(c)
        source += [
            '''define void @three(ptr %out, i8 %choice) {
    entry:
     %c = urem i8 %choice, 3
     switch i8 %c, label %a [i8 1, label %b i8 2, label %cblock]
    a: br label %join
    b: br label %join
    cblock: br label %join
    join:
     %value = phi i1088 [1, %a], [2, %b], [3, %cblock]
     store i1088 %value, ptr %out, align 1
     ret void
    }
    '''
        ]
        original = w / 'original.ll'
        original.write_text(''.join(source))
        pre = w / 'pre-candidate.ll'
        report['pre_O3_stats'] = lower(original, pre)
        assert report['pre_O3_stats']['distributed_stores'] >= 40
        assert report['pre_O3_stats']['split_edges'] >= 2
        spy = w / 'spy.cpp'
        spy.write_text('''#include <cstdint>
    static uint64_t *trace;
    extern "C" void bind_trace(uint64_t *p) { trace=p; }
    extern "C" void ordinary(uint64_t tag,uint8_t *p,uint64_t n) { trace[0]=trace[0]*17+tag; ++trace[1]; if(n) { p[0]^=tag; p[n-1]^=tag*11; } }
    ''')
        command([cxx, '-O2', '-fPIC', '-c', spy, '-o', w / 'spy.o'])
        for level in [0, 3]:
            plain = w / f'original-O{level}.ll'
            command([
                sdk / 'opt', '-passes=' +
                ('verify' if level == 0 else 'default<O3>,verify'),
                '-mtriple=x86_64-unknown-linux-gnu', '-mcpu=native', '-S',
                original, '-o', plain
            ])
            candidate = w / f'candidate-O{level}.ll'
            stats = lower(plain, candidate)
            assert stats['distributed_stores'] > 0
            for phase, ll in [('original', plain), ('candidate', candidate)]:
                obj = w / f'{phase}-O{level}.o'
                command([
                    sdk / 'llc', '-O' + str(level), '-relocation-model=pic',
                    '-filetype=obj', ll, '-o', obj
                ])
                so = obj.with_suffix('.so')
                command([cxx, '-shared', obj, w / 'spy.o', '-o', so])
                lib = ctypes.CDLL(str(so))
                checks = exercise(lib, configs) + three_checks(lib)
                report['runs'].append(
                    dict(phase=phase,
                         level=level,
                         independent_byte_canary_alias_callback_checks=checks,
                         post_optimization_stats=stats
                         if phase == 'candidate' else None))
                save()
                print(report['runs'][-1], flush=True)
        # Additional robustness if a later general LLVM pipeline reprocesses IR.
        ll = w / 'candidate-pre-then-O3.ll'
        command(
            [sdk / 'opt', '-passes=default<O3>,verify', '-S', pre, '-o', ll])
        obj = ll.with_suffix('.o')
        command([
            sdk / 'llc', '-O3', '-relocation-model=pic', '-filetype=obj', ll,
            '-o', obj
        ])
        so = obj.with_suffix('.so')
        command([cxx, '-shared', obj, w / 'spy.o', '-o', so])
        lib = ctypes.CDLL(str(so))
        checks = exercise(lib, configs) + three_checks(lib)
        report['runs'].append(
            dict(phase='candidate-pre-then-O3',
                 level=3,
                 independent_byte_canary_alias_callback_checks=checks))
        save()
        # Corruption negative through the actual transformed multi-edge store.
        broken = w / 'corrupt.ll'
        text = (w / 'candidate-O0.ll').read_text()
        start = text.index('define void @three(')
        end = text.index('\n}', start)
        part = text[start:end]
        mutated = part.replace('store i1088 1,', 'store i1088 9,', 1)
        assert part != mutated
        broken.write_text(text[:start] + mutated + text[end:])
        obj = w / 'corrupt.o'
        command([
            sdk / 'llc', '-O3', '-relocation-model=pic', '-filetype=obj',
            broken, '-o', obj
        ])
        so = obj.with_suffix('.so')
        command([cxx, '-shared', obj, w / 'spy.o', '-o', so])
        try:
            three_checks(ctypes.CDLL(str(so)))
        except AssertionError:
            report['corrupt_incoming_edge_store_detected'] = True
        else:
            raise AssertionError(
                'corrupted incoming store escaped independent oracle')
        report.update(
            status='passed_values_requires_definedness_and_boundary_controls',
            fixture_count=len(configs) + 1,
            checks=sum(x['independent_byte_canary_alias_callback_checks']
                       for x in report['runs']),
            finished_at=datetime.datetime.now(
                datetime.timezone.utc).isoformat())
        save()
    except BaseException as error:
        report.update(status='failed', error=repr(error))
        save()
        raise

    from wide_store_controls import check as check_controls
    controls = check_controls(sdk, cxx, str(library), r)
    api = []
    for pipeline in ('stock', 'bounded'):
        for enabled in (False, True):
            label = pipeline + ('-enabled' if enabled else '-default')
            obj = w / (label + '.o')
            timing = w / (label + '.json')
            # Disabled stock codegen retains its existing no-C++-plugin path.
            builder_cxx = '/not-installed/c++' if pipeline == 'stock' and not enabled else cxx
            compile_model(original,
                          obj,
                          sdk / 'opt',
                          sdk / 'llc',
                          sim / 'generated-src/compiler',
                          builder_cxx,
                          abi_buffers=False,
                          pipeline=pipeline,
                          report=timing,
                          distribute_wide_stores=enabled)
            data = json.loads(timing.read_text())
            assert data['distribute_wide_stores'] is enabled
            so = obj.with_suffix('.so')
            command([cxx, '-shared', obj, w / 'spy.o', '-o', so])
            lib = ctypes.CDLL(str(so))
            checks = exercise(lib, configs) + three_checks(lib)
            api.append(dict(pipeline=pipeline, enabled=enabled, checks=checks))
    report.update(status='passed', controls=controls, api=api)
    save()


def check(circt, cxx, build):
    build.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix='run-', dir=build))
    subprocess.run([
        sys.executable,
        str(Path(__file__).resolve()), '--worker',
        str(circt), cxx,
        str(run)
    ],
                   check=True)
    result = json.loads((run / 'unit.json').read_text())
    assert result['status'] == 'passed'
    return dict(
        checks=result['checks'],
        boundary_checks=result['controls']['boundary_checks'],
        definedness_paths=result['controls']['definedness_positive_paths'],
        api_checks=sum(row['checks'] for row in result['api']))


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == '--worker':
        _check(Path(sys.argv[2]).resolve(), sys.argv[3], Path(sys.argv[4]))
    else:
        print(
            json.dumps(
                check(
                    Path(os.environ['CIRCT_BIN']).resolve(),
                    os.environ.get('CXX', 'c++'),
                    sim / 'generated-src/wide-store-test')))
