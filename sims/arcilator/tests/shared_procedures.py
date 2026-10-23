"""Verify private procedure forwarding, aliases, side effects and ABI guards."""
from pathlib import Path
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
out = SIM / 'generated-src/shared-procedure-test'
out.mkdir(parents=True, exist_ok=True)
sdk = Path(os.environ['CIRCT_BIN']).resolve().parent
cxx = os.environ.get('CXX', 'c++')
sys.path.insert(0, str(SIM))
from compiler.partition import plugin

lib = plugin(sdk / 'bin/opt', cxx, SIM / 'generated-src/compiler',
             SIM / 'compiler/SharedProcedures.cpp')
layout = next((x for x in subprocess.check_output(
    [sdk / 'bin/opt', '-S', '-o', '-', '-'],
    input='target triple="x86_64-unknown-linux-gnu"\n',
    text=True).splitlines() if x.startswith('target datalayout')))
lines = [
    layout, 'target triple="x86_64-unknown-linux-gnu"',
    '%Sub=type <{i8,[2 x i16]}>', '%Packet=type {[64 x i64],ptr,i65,%Sub}'
]
params = ', '.join(['ptr %state', 'i65 %wide'] +
                   [f'i64 %p{i}' for i in range(64)])
for f, tag in [('first', 17), ('second', 31), ('third', 47)]:
    lines += [
        f'define internal fastcc %Packet @{f}({params}) noinline {{', 'entry:',
        ' %before=load i64,ptr %state,align 8', ' %sum=add i64 %before,%p0',
        f' %step=add i64 %sum,{tag}', ' %odd=and i64 %p1,1',
        ' %cond=icmp ne i64 %odd,0', ' br i1 %cond,label %left,label %right'
    ]
    for label, extra in [('left', tag * 19), ('right', tag * 23)]:
        pref = label + '_'
        lines += [
            label + ':', f' %{pref}s=xor i64 %step,{extra}',
            f' store i64 %{pref}s,ptr %state,align 8'
        ]
        aggregate = 'poison'
        for i in range(64):
            lines += [
                f' %{pref}weight{i}=add i64 %{pref}s,{i * 13 + tag}',
                f' %{pref}v{i}=xor i64 %p{i},%{pref}weight{i}',
                f' %{pref}a{i}=insertvalue [64 x i64] {aggregate},i64 %{pref}v{i},{i}'
            ]
            aggregate = '%' + pref + 'a' + str(i)
        lines += [
            f' %{pref}lo=zext i64 %{pref}s to i65',
            f' %{pref}parity=and i64 %{pref}s,1',
            f' %{pref}ph=zext i64 %{pref}parity to i65',
            f' %{pref}high=shl i65 %{pref}ph,64',
            f' %{pref}bits=xor i65 %{pref}lo,%{pref}high',
            f' %{pref}w=xor i65 %wide,%{pref}bits',
            f' %{pref}byte=trunc i64 %p2 to i8',
            f' %{pref}a=trunc i64 %p3 to i16',
            f' %{pref}b=trunc i64 %{pref}s to i16',
            f' %{pref}r0=insertvalue %Packet poison,[64 x i64] {aggregate},0',
            f' %{pref}r1=insertvalue %Packet %{pref}r0,ptr %state,1',
            f' %{pref}r2=insertvalue %Packet %{pref}r1,i65 %{pref}w,2',
            f' %{pref}r3=insertvalue %Packet %{pref}r2,i8 %{pref}byte,3,0',
            f' %{pref}r4=insertvalue %Packet %{pref}r3,i16 %{pref}a,3,1,0',
            f' %{pref}r5=insertvalue %Packet %{pref}r4,i16 %{pref}b,3,1,1',
            f' ret %Packet %{pref}r5'
        ]
    lines.append('}')
for fn, tag in [('writeA', 59), ('writeB', 61)]:
    lines += [
        f'define internal fastcc void @{fn}(ptr %state,ptr %out,' + ', '.join(
            (f'i64 %p{i}' for i in range(64))) + ') noinline {',
        ' %before=load i64,ptr %state,align 8', ' %sum=add i64 %before,%p0',
        f' %tagged=add i64 %sum,{tag}', ' %s=xor i64 %tagged,%p1',
        ' store i64 %s,ptr %state,align 8'
    ]
    for i in range(64):
        lines += [
            f' %q{i}=xor i64 %p{i},%s',
            f' %out{i}=getelementptr i64,ptr %out,i64 {i}',
            f' store i64 %q{i},ptr %out{i},align 8'
        ]
    lines += [' ret void', '}']
lines += [
    'define void @evaluate(ptr %state,i64 %x,ptr %out) {', 'entry:',
    ' %wide=zext i64 %x to i65'
]
for i in range(64):
    lines += [f' %p{i}=add i64 %x,{i * 17}']
for f, prefix, source in [('first', 'a', 'p'), ('second', 'b', 'a'),
                          ('third', 'c', 'mix')]:
    if f == 'third':
        for i in range(64):
            lines += [f' %mix{i}=xor i64 %a{i},%b{i}']
    wide = '%wide' if f == 'first' else '%aw' if f == 'second' else '%bw'
    state = '%state' if f == 'first' else '%ap' if f == 'second' else '%bp'
    args = ', '.join([f'ptr {state}', f'i65 {wide}'] +
                     [f'i64 %{source}{i}' for i in range(64)])
    lines += [
        f' %{prefix}=call fastcc %Packet @{f}({args})',
        f' %{prefix}array=extractvalue %Packet %{prefix},0'
    ]
    for i in range(64):
        lines += [f' %{prefix}{i}=extractvalue [64 x i64] %{prefix}array,{i}']
    lines += [
        f' %{prefix}p=extractvalue %Packet %{prefix},1',
        f' %{prefix}w=extractvalue %Packet %{prefix},2'
    ]
for fn, offset in [('writeA', 230), ('writeB', 294)]:
    lines += [
        f' %{fn}_out=getelementptr i64,ptr %out,i64 {offset}',
        f' call fastcc void @{fn}(ptr %cp,ptr %{fn}_out,' + ', '.join(
            (f'i64 %c{i}' for i in range(64))) + ')'
    ]
index = 0
for prefix in ['a', 'b', 'c']:
    for i in range(64):
        lines += [
            f' %out{index}=getelementptr i64,ptr %out,i64 {index}',
            f' store i64 %{prefix}{i},ptr %out{index},align 8'
        ]
        index += 1
    for field, ty in [('1', 'ptr'), ('2', 'i65'), ('3,0', 'i8'),
                      ('3,1,0', 'i16'), ('3,1,1', 'i16')]:
        lines += [f' %f{index}=extractvalue %Packet %{prefix},{field}']
        if ty == 'ptr':
            lines += [f' %n{index}=ptrtoint ptr %f{index} to i64']
            values = ['%n' + str(index)]
        elif ty == 'i65':
            lines += [
                f' %n{index}=trunc i65 %f{index} to i64',
                f' %h{index}=lshr i65 %f{index},64',
                f' %high{index}=trunc i65 %h{index} to i64'
            ]
            values = ['%n' + str(index), '%high' + str(index)]
        else:
            lines += [f' %n{index}=zext {ty} %f{index} to i64']
            values = ['%n' + str(index)]
        for value in values:
            lines += [
                f' %out{index}=getelementptr i64,ptr %out,i64 {index}',
                f' store i64 {value},ptr %out{index},align 8'
            ]
            index += 1
lines += [' ret void', '}']
assert index == 210
(out / 'input.ll').write_text('\n'.join(lines) + '\n')
check = SIM / 'tests/shared_procedures.cpp'
report = {
    'status': 'running',
    'runs': [],
    'source_sha256': hashlib.sha256(
        (out / 'input.ll').read_bytes()).hexdigest(),
    'checker_sha256': hashlib.sha256(check.read_bytes()).hexdigest(),
    'plugin': str(lib)
}


def run(label, cmd):
    with (out / (label + '.log')).open('w') as log:
        p = subprocess.run([str(v) for v in cmd],
                           stdout=log,
                           stderr=subprocess.STDOUT)
    report['runs'].append(dict(label=label, returncode=p.returncode))
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    assert p.returncode == 0, (label,
                               (out / (label + '.log')).read_text()[-4000:])


run('transform', [
    sdk / 'bin/opt', '-load-pass-plugin=' + str(lib),
    '-passes=chipyard-share-procedure-values,verify', '-S', out / 'input.ll',
    '-o', out / 'shared.ll'
])
assert 'Shared workspace for 5 private procedures' in (
    out / 'transform.log').read_text()
run('twice', [
    sdk / 'bin/opt', '-load-pass-plugin=' + str(lib),
    '-passes=chipyard-share-procedure-values,verify', '-S', out / 'shared.ll',
    '-o', out / 'twice.ll'
])
norm = lambda s: re.sub('^; ModuleID.*\\n', '', s)
assert norm((out / 'shared.ll').read_text()) == norm(
    (out / 'twice.ll').read_text())
for kind in ['input', 'shared']:
    for level in ['0', '3']:
        bc = out / (kind + '-O' + level + '.bc')
        obj = bc.with_suffix('.o')
        exe = bc.with_suffix('')
        run(kind + '-opt' + level,
            [sdk / 'bin/opt', '-O' + level, out / (kind + '.ll'), '-o', bc])
        run(kind + '-llc' + level, [
            sdk / 'bin/llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj', bc, '-o', obj
        ])
        run(kind + '-link' + level,
            [*shlex.split(cxx), '-O' + level, check, obj, '-o', exe])
        run(kind + '-run' + level, [exe])
report['status'] = 'running_additional_guards'
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(report['status'], flush=True)
run('inspect-optimized',
    [sdk / 'bin/llvm-dis', out / 'shared-O3.bc', '-o', out / 'shared-O3.ll'])
for name in ['writeA', 'writeB']:
    header = next((x for x in (out / 'shared-O3.ll').read_text().splitlines()
                   if x.startswith('define ') and '@' + name + '(' in x))
    assert header.count('ptr ') == 1, header
print(
    'PASS: void workspace ABIs retained through whole O3; no argument promotion'
)
guard_params = ', '.join(['ptr %state'] + [f'i64 %p{i}' for i in range(64)])
guard_args = ', '.join(['ptr %state'] + [f'i64 %p{i}' for i in range(64)])


def guard_module(second_attrs='noinline', result='%R', declaration=False):
    definition = f'declare fastcc {result} @b({guard_params}) {second_attrs}' if declaration else f'define internal fastcc {result} @b({guard_params}) {second_attrs} {{ret {result} zeroinitializer}}'
    return '\n'.join([
        layout, 'target triple="x86_64-unknown-linux-gnu"',
        '%R=type {[64 x i64]}',
        f'define internal fastcc {result} @a({guard_params}) noinline {{ret {result} zeroinitializer}}',
        definition, f'define fastcc {result} @entry({guard_params}) {{',
        f'%a=call fastcc {result} @a({guard_args})',
        f'%b=call fastcc {result} @b({guard_args})', f'ret {result} %b', '}'
    ]) + '\n'


base = guard_module()
guards = {
    'public':
    base.replace('define internal fastcc %R @b', 'define fastcc %R @b'),
    'address-taken':
    base + '@keep=global ptr @b\n',
    'multiple-calls':
    base.replace('ret %R %b',
                 f'%again=call fastcc %R @b({guard_args})\nret %R %b'),
    'musttail':
    base.replace('%b=call', '%b=musttail call'),
    'operand-bundle':
    base.replace(f'%b=call fastcc %R @b({guard_args})',
                 f'%b=call fastcc %R @b({guard_args}) ["deopt"()]'),
    'byval':
    base.replace('@b(ptr %state', '@b(ptr byval(i64) %state'),
    'inreg':
    base.replace('@b(ptr %state', '@b(ptr inreg %state'),
    'noipa':
    guard_module('noinline noipa'),
    'optnone':
    guard_module('noinline optnone'),
    'inline-eligible':
    guard_module(''),
    'naked':
    guard_module('noinline naked'),
    'gc':
    guard_module('noinline gc "shadow-stack"'),
    'presplit-coroutine':
    guard_module('noinline presplitcoroutine'),
    'declaration':
    guard_module(declaration=True),
    'zero-sized':
    base.replace('%R=type {[64 x i64]}', '%R=type {}'),
    'oversized':
    base.replace('%R=type {[64 x i64]}', '%R=type {[262144 x i64]}'),
    'scalable':
    guard_module(result='<vscale x 4 x i64>')
}
guards.update({
    'module-assembly':
    base + 'module asm ".byte 0"\n',
    'inline-assembly':
    base.replace('%a=call', 'call void asm sideeffect "", ""()\n%a=call'),
    'noipa-caller':
    base.replace(f'@entry({guard_params}) {{',
                 f'@entry({guard_params}) noipa {{'),
    'coroutine-caller':
    base.replace(f'@entry({guard_params}) {{',
                 f'@entry({guard_params}) presplitcoroutine {{')
})
for name, text in guards.items():
    src = out / ('guard-' + name + '.ll')
    src.write_text(text)
    canonical = out / ('guard-' + name + '-canonical.ll')
    actual = out / ('guard-' + name + '-after.ll')
    run('guard-' + name + '-verify',
        [sdk / 'bin/opt', '-passes=verify', '-S', src, '-o', canonical])
    run('guard-' + name + '-transform', [
        sdk / 'bin/opt', '-load-pass-plugin=' + str(lib),
        '-passes=chipyard-share-procedure-values,verify', '-S', src, '-o',
        actual
    ])
    assert norm(canonical.read_text()) == norm(actual.read_text()), name
# Two mutually exclusive groups still share one per-caller storage budget.
large = '%R=type {[32768 x i64]}'
functions = [
    f'define internal fastcc %R @{name}({guard_params}) noinline {{ret %R zeroinitializer}}'
    for name in ['a', 'b', 'c', 'd']
]
cap_source = '\n'.join([
    layout, 'target triple="x86_64-unknown-linux-gnu"', large, *functions,
    f'define fastcc void @entry({guard_params}) {{',
    '%choose=icmp eq i64 %p0,0', 'br i1 %choose,label %left,label %right',
    'left:', f'%a=call fastcc %R @a({guard_args})',
    f'%b=call fastcc %R @b({guard_args})', 'br label %done', 'right:',
    f'%c=call fastcc %R @c({guard_args})',
    f'%d=call fastcc %R @d({guard_args})', 'br label %done', 'done:',
    'ret void', '}'
])
cap_input = out / 'caller-cap.ll'
cap_input.write_text(cap_source + '\n')
cap_output = out / 'caller-cap-shared.ll'
run('caller-cap', [
    sdk / 'bin/opt', '-load-pass-plugin=' + str(lib),
    '-passes=chipyard-share-procedure-values,verify', '-S', cap_input, '-o',
    cap_output
])
assert 'Shared workspace for 2 private procedures' in (
    out / 'caller-cap.log').read_text()
allocations = re.findall(r'alloca \[(\d+) x i8\]', cap_output.read_text())
assert len(allocations) == 1 and sum(map(int, allocations)) <= 1048576
report['per_caller_workspace_cap_verified'] = True

for mode in ['default', 'shared', 'separate']:
    cli = out / ('cli-' + mode + '.ll')
    cli.write_bytes((out / 'input.ll').read_bytes())
    run('cli-' + mode, [
        sys.executable, SIM / 'compiler/llvm.py', cli, '--llvm-opt', sdk /
        'bin/opt', '--cache', SIM / 'generated-src/compiler', '--cxx', cxx, *({
            'shared': ['--share-procedures'],
            'separate': ['--no-share-procedures']
        }.get(mode, []))
    ])
    if mode != 'separate':
        assert 'chipyard.procedure_workspace' in cli.read_text()
    else:
        assert 'chipyard.procedure_workspace' not in cli.read_text()
    bc = cli.with_suffix('.bc')
    obj = cli.with_suffix('.o')
    exe = cli.with_suffix('')
    run('cli-' + mode + '-opt', [sdk / 'bin/opt', '-O3', cli, '-o', bc])
    run('cli-' + mode + '-llc', [
        sdk / 'bin/llc', '-O3', '-relocation-model=pic', '-filetype=obj', bc,
        '-o', obj
    ])
    run('cli-' + mode + '-link',
        [*shlex.split(cxx), '-O3', check, obj, '-o', exe])
    run('cli-' + mode + '-run', [exe])
invalid = out / 'invalid-wide.ll'
invalid.write_text((out / 'input.ll').read_text() +
                   '\ndeclare i65 @chipyard_arc_wide_shift_0_65(i65,i65)\n')
original = invalid.read_bytes()
failed = subprocess.run([
    sys.executable, SIM / 'compiler/llvm.py', invalid, '--llvm-opt',
    sdk / 'bin/opt', '--cache', SIM / 'generated-src/compiler', '--cxx', cxx,
    '--share-procedures'
],
                        capture_output=True,
                        text=True)
assert failed.returncode != 0 and 'invalid generated wide arithmetic declaration' in failed.stderr
assert invalid.read_bytes() == original and (not list(
    out.glob('invalid-wide.ll.buffered.*')))
report.update(status='passed',
              oracle_words=6291456,
              abi_exclusion_cases=list(guards),
              transaction_failure_preserves_input=True)
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(
    'PASS: 6291456 independent words, 22 conservative ABI/storage guards, CLI sharing default/overrides, and failed-transaction preservation'
)
