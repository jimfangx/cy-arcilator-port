"""Check unsupported real reference scheduling and unsupported floating widths."""
import subprocess


def check_guards(circt_opt, library, work):
    for name, source, diagnostic in [
        ('nba', """module {
  func.func @bad(%formal: !llhd.ref<f64>) attributes {
    circt.dpi.export="bad", chipyard.dpi_export={formal_count=1:i64}
  } {
    %value=arith.constant 1.25:f64
    %time=llhd.constant_time <0ns, 1d, 0e>
    llhd.drv %formal, %value after %time:f64
    return
  }
}
""", 'temporary DPI formals require blocking assignments'),
        ('f16', """module {
  func.func @bad(%value:f16)->f16 attributes {
    circt.dpi.export="bad", chipyard.dpi_export={formal_count=1:i64}
  } {
    return %value:f16
  }
}
""", 'unsupported exported value type'),
    ]:
        path = work / ('real-export-' + name + '.mlir')
        path.write_text(source)
        result = subprocess.run([
            circt_opt, path, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-signal-memory{exports=true captures-only=true})',
            '-o', path.with_suffix('.lowered.mlir')
        ], capture_output=True, text=True)
        if not result.returncode or diagnostic not in result.stderr:
            raise RuntimeError('real export guard failed: ' + name + ': ' + result.stderr)
    print('PASS: real output call-frame lifetime and unsupported floating width guards', flush=True)
