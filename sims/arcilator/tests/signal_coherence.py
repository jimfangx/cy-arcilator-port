#!/usr/bin/env python3
"""Check native signal coalescing guards and undriven constant preservation."""
import argparse
from pathlib import Path
import re
import subprocess
import sys

here = Path(__file__).resolve().parent
sim = here.parent
sys.path.insert(0, str(sim))
from compiler.partition import plugin
from native_hw import prepare_native_signals

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-opt', type=Path, required=True)
parser.add_argument('--work',
                    type=Path,
                    default=sim / 'generated-src/signal-coherence-guards')
args = parser.parse_args()
args.work.mkdir(parents=True, exist_ok=True)
library = plugin(args.circt_opt, 'c++', sim / 'generated-src/compiler',
                 sim / 'compiler/SignalState.cpp')


def module(name, body):
    return f'''  hw.module @{name}(in %clock: !seq.clock, in %other_clock: !seq.clock,
        in %next: i13, in %other_next: i13, in %reset: i1, in %other_reset: i1,
        in %enable: i1, out q: i13) {{
    %zero = hw.constant 0 : i13
    %one = hw.constant 1 : i13
    %immediate = llhd.constant_time <0ns, 0d, 1e>
    %delayed = llhd.constant_time <1ns, 0d, 0e>
    %nba = llhd.constant_time <0ns, 1d, 0e>
    {body}
  }}'''


cases = {
    'equal': ({}, 1),
    'equal_preset': ({
        'preset_a': 173,
        'preset_b': 173
    }, 1),
    'different_next': ({
        'next_b': '%other_next'
    }, 2),
    'different_clock': ({
        'clock_b': '%other_clock'
    }, 2),
    'different_reset': ({
        'reset_b': '%other_reset'
    }, 2),
    'different_reset_value': ({
        'reset_value_b': '%one'
    }, 2),
    'different_reset_mode': ({
        'mode_b': 'async'
    }, 2),
    'different_preset': ({
        'preset_a': 173,
        'preset_b': 174
    }, 2),
    'missing_preset': ({
        'preset_a': 173
    }, 2),
    'enabled_drive': ({
        'enable_b': ' if %enable'
    }, 2),
    'physical_delay': ({
        'delay_b': '%delayed'
    }, 2),
    'nba_drive': ({
        'delay_b': '%nba'
    }, 2),
    'different_name': ({
        'name_b': 'different'
    }, 2),
}
modules = []
for name, (options, count) in cases.items():
    regs = []
    for suffix in ('a', 'b'):
        preset = options.get('preset_' + suffix)
        attrs = 'name = "' + options.get('name_' + suffix, 'same') + '"'
        if preset is not None:
            attrs += f', preset = {preset} : i13'
        regs.append(
            f'''%{suffix} = seq.firreg {options.get('next_' + suffix, '%next')}
          clock {options.get('clock_' + suffix, '%clock')}
          reset {options.get('mode_' + suffix, 'sync')} {options.get('reset_' + suffix, '%reset')},
          {options.get('reset_value_' + suffix, '%zero')} {{{attrs}}} : i13''')
    body = '\n'.join(regs) + f'''
    %signal = llhd.sig : <i13>
    llhd.drv %signal, %a after %immediate : i13
    llhd.drv %signal, %b after {options.get('delay_b', '%immediate')}{options.get('enable_b', '')} : i13
    %q = llhd.prb %signal : i13
    hw.output %q : i13'''
    modules.append(module(name, body))
modules.append(
    module(
        'undriven_zero', '''%signal = llhd.sig : <i13>
    %q = llhd.prb %signal : i13
    hw.output %q : i13'''))
modules.append(
    module(
        'undriven_constant', '''%initial = hw.constant 4093 : i13
    %signal = llhd.sig %initial : <i13>
    %q = llhd.prb %signal : i13
    hw.output %q : i13'''))
modules.append(
    '''hw.module @undriven_aggregate(out q: !hw.struct<a: i5, b: !hw.array<2xi17>>) {
    %initial = hw.aggregate_constant [17 : i5, [31 : i17, 65537 : i17]] : !hw.struct<a: i5, b: !hw.array<2xi17>>
    %signal = llhd.sig %initial : <!hw.struct<a: i5, b: !hw.array<2xi17>>>
    %q = llhd.prb %signal : !hw.struct<a: i5, b: !hw.array<2xi17>>
    hw.output %q : !hw.struct<a: i5, b: !hw.array<2xi17>>
  }''')
modules.append(
    module(
        'dynamic_initializer', '''%signal = llhd.sig %next : <i13>
    %q = llhd.prb %signal : i13
    hw.output %q : i13'''))
# An escaped reference could have a producer through its user. It is not an
# undriven net, even if no direct llhd.drv appears at this level.
modules.append(
    module(
        'escaped', '''%signal = llhd.sig : <i13>
    func.call @opaque(%signal) : (!llhd.ref<i13>) -> ()
    %q = llhd.prb %signal : i13
    hw.output %q : i13'''))
source = args.work / 'input.mlir'
output = args.work / 'output.mlir'
source.write_text('module {\n func.func private @opaque(!llhd.ref<i13>)\n' +
                  '\n'.join(modules) + '\n}\n')
subprocess.run([
    args.circt_opt, source, '--load-pass-plugin=' + str(library),
    '--pass-pipeline=builtin.module(chipyard-native-signal-state)', '-o',
    output
],
               check=True)
text = output.read_text()
sections = {
    m[1]: m[2]
    for m in re.finditer(r'^  hw.module @(\w+)\(.*?\{\n(.*?)^  }', text, re.M
                         | re.S)
}
for name, (_, count) in cases.items():
    body = sections[name]
    assert len(re.findall(r'\bllhd.drv\b', body)) == count, name
for name, value in (('undriven_zero', '0'), ('undriven_constant', '4093')):
    body = sections[name]
    assert 'llhd.sig' not in body and 'llhd.prb' not in body, name
    assert re.search(r'hw.constant ' + value + r' : i13', body), name
for name in ('dynamic_initializer', 'escaped'):
    assert 'llhd.sig' in sections[name] and 'llhd.prb' in sections[name], name
aggregate = sections['undriven_aggregate']
bits = (17 << 34) | (31 << 17) | 65537
assert 'llhd.sig' not in aggregate and 'llhd.prb' not in aggregate
assert f'hw.constant {bits - (1 << 39)} : i39' in aggregate
print(
    'PASS: equivalent-register controls, presets, names, drive enables/delays; undriven constants; escaped and dynamic-initialized signals retained'
)

struct = '!hw.struct<a: i17, b: i17, c: i17>'
aggregate_modules = []
for name in ('complete', 'partial', 'shared', 'effect'):
    right_b = ('%b1 = hw.struct_inject %a1["b"], %data : ' +
               struct if name != 'partial' else '')
    right_value = '%b1' if name != 'partial' else '%a1'
    new_value = '%new'
    computation = '%new = comb.add %data, %one : i17'
    if name == 'shared':
        computation = '%old = hw.struct_extract %merged["c"] : ' + struct
        computation += '\n %new = comb.xor %old, %data : i17'
    elif name == 'effect':
        computation = '%new = func.call @effect(%data) : (i17) -> i17'
    aggregate_modules.append(
        f'''hw.module @struct_{name}(in %condition: i1, in %data: i17, out q: {struct}) {{
    %one = hw.constant 1 : i17
    %immediate = llhd.constant_time <0ns, 0d, 1e>
    %result = llhd.combinational -> {struct} {{
      %signal = llhd.sig : <{struct}>
      %prior = llhd.prb %signal : {struct}
      cf.cond_br %condition, ^left, ^right
    ^left:
      %a0 = hw.struct_inject %prior["a"], %data : {struct}
      %b0 = hw.struct_inject %a0["b"], %one : {struct}
      cf.br ^join(%b0 : {struct})
    ^right:
      %a1 = hw.struct_inject %prior["a"], %one : {struct}
      {right_b}
      cf.br ^join({right_value} : {struct})
    ^join(%merged: {struct}):
      {computation}
      %final = hw.struct_inject %merged["c"], {new_value} : {struct}
      llhd.drv %signal, %final after %immediate : {struct}
      llhd.yield %final : {struct}
    }}
    hw.output %result : {struct}
  }}''')
aggregate_input = args.work / 'struct-input.mlir'
aggregate_output = args.work / 'struct-output.mlir'
aggregate_input.write_text(
    'module {\nfunc.func private @effect(i17) -> i17\n' +
    '\n'.join(aggregate_modules) + '\n}\n')
prepare_native_signals(aggregate_input, aggregate_output, args.circt_opt,
                       library)
text = aggregate_output.read_text()
sections = {
    m[1]: m[2]
    for m in re.finditer(r'^  hw.module @(\w+)\(.*?\{\n(.*?)^  }', text, re.M
                         | re.S)
}
assert 'llhd.sig' not in sections[
    'struct_complete'] and 'llhd.drv' not in sections['struct_complete']
for name in ('partial', 'shared', 'effect'):
    assert 'llhd.sig' in sections['struct_' + name], name
    assert 'llhd.drv' in sections['struct_' + name], name
negative = args.work / 'struct-without-motion.mlir'
subprocess.run([
    args.circt_opt, aggregate_input, '--load-pass-plugin=' + str(library),
    '--pass-pipeline=builtin.module(canonicalize,hw.module(llhd-mem2reg),canonicalize,chipyard-native-signal-state,hw.module(llhd-sig2reg,llhd-remove-control-flow),canonicalize)',
    '-o', negative
],
               check=True)
text = negative.read_text()
complete = re.search(r'^  hw.module @struct_complete\(.*?\{\n(.*?)^  }', text,
                     re.M | re.S)[1]
assert 'llhd.sig' in complete and 'llhd.drv' in complete
print(
    'PASS: complete branch-merged struct updates eliminate old-value dependencies; partial writes, shared reads and side effects remain; removing motion reproduces unresolved signal'
)
