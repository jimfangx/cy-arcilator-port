#!/usr/bin/env python3
"""Check automatic reference components before running their native fixture."""
from pathlib import Path
import subprocess


def prepare(source, output, circt_opt, library):
    for kind in ('delay', 'delta', 'escape', 'global'):
        time = '1ns, 0d, 0e' if kind == 'delay' else (
            '0ns, 1d, 0e' if kind == 'delta' else '0ns, 0d, 1e')
        action = ('func.call @opaque(%selected) : (!llhd.ref<i13>) -> ()'
                  if kind == 'escape' else
                  'llhd.drv %selected, %data after %time : i13')
        outer = '%global = llhd.sig %zero : <i13>' if kind == 'global' else ''
        right = '%global' if kind == 'global' else '%b'
        text = f'''module {{
  func.func private @opaque(!llhd.ref<i13>)
  hw.module @Unsupported(in %select: i1, in %data: i13) {{
    %zero = hw.constant 0 : i13
    %time = llhd.constant_time <{time}>
    {outer}
    llhd.combinational {{
      %a = llhd.sig %zero : <i13>
      %b = llhd.sig %zero : <i13>
      cf.cond_br %select, ^join(%a : !llhd.ref<i13>), ^join({right} : !llhd.ref<i13>)
    ^join(%selected: !llhd.ref<i13>):
      {action}
      llhd.yield
    }}
    hw.output
  }}
}}
'''
        raw = output.with_name('unsupported-local-' + kind + '.mlir')
        lowered = raw.with_name(raw.stem + '-lowered.mlir')
        raw.write_text(text)
        subprocess.run([
            circt_opt, raw, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-local-memory)',
            '-o', lowered
        ],
                       check=True)
        result = lowered.read_text()
        # In the mixed global/local case, b is independent and may lower.
        if kind == 'global':
            assert result.count('llhd.sig ') == 2, kind
            assert result.count('llvm.alloca ') == 1, kind
        else:
            assert result.count('llhd.sig ') == 2, kind
            assert 'llvm.alloca ' not in result, kind

    raw = output.with_name('unsupported-local-suspension.mlir')
    raw.write_text('''module {
 hw.module @Timed(in %clock: i1) {
  llhd.process {
   %local = llhd.sig : <i13>
   cf.br ^wait
  ^wait:
   llhd.wait (%clock : i1), ^wait
  }
  hw.output
 }
}
''')
    lowered = raw.with_name(raw.stem + '-lowered.mlir')
    subprocess.run([
        circt_opt, raw, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-native-local-memory)', '-o',
        lowered
    ],
                   check=True)
    assert 'llhd.sig ' in lowered.read_text()
    assert 'llvm.alloca ' not in lowered.read_text()

    subprocess.run([
        circt_opt, source, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-native-local-memory,canonicalize)',
        '-o', output
    ],
                   check=True)
    subprocess.run([circt_opt, output, '-o', '/dev/null'], check=True)
    assert '!llhd.ref' not in Path(output).read_text()
    print(
        'PASS: local reference components lower completely; delayed, escaping '
        'and global references remain intact; suspending scopes retain their '
        'original storage',
        flush=True)
