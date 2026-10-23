"""Validate global reference admission before executing shared-state models."""
import subprocess


def check_guards(circt_opt, library, work):
    for kind in ('export', 'initializer', 'initializer-effect', 'delay',
                 'escape'):
        initial = (
            '%value = func.call @seed() : () -> i13'
            if kind == 'initializer' else
            '%unused = func.call @seed() : () -> i13\n    %value = hw.constant 17 : i13'
            if kind == 'initializer-effect' else
            '%value = hw.constant 17 : i13')
        body = '''func.func @exported() -> i13 {
    %ref = llhd.get_global_signal @shared : <i13>
    %value = llhd.prb %ref : i13
    return %value : i13
  }''' if kind == 'export' else '''hw.module @Guard(out q: i13) {
    %time = llhd.constant_time <1ns, 0d, 0e>
    %ref = llhd.get_global_signal @shared : <i13>
    %value = llhd.prb %ref : i13
    ACTION
    hw.output %value : i13
  }'''.replace(
            'ACTION', 'llhd.drv %ref, %value after %time : i13' if kind
            == 'delay' else 'func.call @opaque(%ref) : (!llhd.ref<i13>) -> ()'
            if kind == 'escape' else '')
        source = work / ('global-guard-' + kind + '.mlir')
        normal = source.with_name(source.stem + '-normal.mlir')
        lowered = source.with_name(source.stem + '-lowered.mlir')
        source.write_text('''module {
  func.func private @seed() -> i13
  func.func private @opaque(!llhd.ref<i13>)
  llhd.global_signal @shared : i13 init {
    ''' + initial + '''
    llhd.yield %value : i13
  }
  ''' + body + '\n}\n')
        subprocess.run([circt_opt, source, '-o', normal], check=True)
        lowered.unlink(missing_ok=True)
        result = subprocess.run([
            circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-signal-memory)',
            '-o', lowered
        ],
                                capture_output=True,
                                text=True)
        if kind in ('export', 'initializer', 'initializer-effect'):
            result.check_returncode()
            assert lowered.read_bytes() == normal.read_bytes(), kind
        else:
            assert result.returncode, kind
            assert 'cannot bind native global reference component' in result.stderr, result.stderr
            assert not lowered.exists(), kind
    print(
        'PASS: exported and nonconstant global components remain intact; '
        'delayed and escaping globals reject independent local storage',
        flush=True)
