#!/usr/bin/env python3
"""Check reference-component validation before executing CFG memory fixtures."""
import subprocess
from pathlib import Path


def prepare(source, output, circt_opt, library):
    # Rejected components must retain every root. Allocating the first root
    # before discovering an unsupported incoming edge would corrupt the IR.
    for kind in ('delay', 'escape', 'initializer'):
        value = '%data' if kind == 'initializer' else '%zero'
        action = ('func.call @opaque(%selected) : (!llhd.ref<i13>) -> ()'
                  if kind == 'escape' else
                  'llhd.drv %selected, %data after %time : i13')
        time = '1ns' if kind == 'delay' else '0ns'
        text = f'''module {{
  func.func private @opaque(!llhd.ref<i13>)
  hw.module @Unsupported(in %select: i1, in %data: i13) {{
    %zero = hw.constant 0 : i13
    %time = llhd.constant_time <{time}, 0d, 0e>
    %a = llhd.sig {value} : <i13>
    %b = llhd.sig %zero : <i13>
    llhd.combinational {{
      cf.cond_br %select, ^join(%a : !llhd.ref<i13>), ^join(%b : !llhd.ref<i13>)
    ^join(%selected: !llhd.ref<i13>):
      {action}
      llhd.yield
    }}
    hw.output
  }}
}}
'''
        raw = output.with_name('unsupported-' + kind + '.mlir')
        lowered = raw.with_name(raw.stem + '-lowered.mlir')
        raw.write_text(text)
        subprocess.run([
            circt_opt, raw, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-signal-memory)',
            '-o', lowered
        ], check=True)
        result = lowered.read_text()
        assert result.count('llhd.sig ') == 2, kind
        assert 'ArcSignalMemory' not in result, kind

    subprocess.run([
        circt_opt, source, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(hw-flatten-modules,chipyard-native-signal-memory,canonicalize,hw.module(llhd-lower-processes),canonicalize)',
        '-o', output
    ], check=True)
    assert '!llhd.ref' not in Path(output).read_text()
    print('PASS: unsupported delay, escape and initializer retain entire '
          'reference components; supported CFG references lower completely',
          flush=True)
