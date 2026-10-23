"""Reject delayed writes that would outlive a native DPI output call frame."""
import subprocess


def check_guards(circt_opt, library, work):
    for merged in (False, True):
        body = '''
    cf.br ^write(%formal : !llhd.ref<i13>)
  ^write(%target: !llhd.ref<i13>):
''' if merged else ''
        target = '%target' if merged else '%formal'
        source = work / ('export-output-nba-' + str(int(merged)) + '.mlir')
        output = source.with_suffix('.lowered.mlir')
        source.write_text('''module {
  func.func @bad(%formal: !llhd.ref<i13>) attributes {
    circt.dpi.export = "bad", chipyard.dpi_export = {formal_count = 1 : i64}
  } {''' + body + '''
    %value = hw.constant 7 : i13
    %time = llhd.constant_time <0ns, 1d, 0e>
    llhd.drv ''' + target + ''', %value after %time : i13
    return
  }
}
''')
        result = subprocess.run([
            circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-signal-memory{exports=true captures-only=true})',
            '-o', output
        ],
                                capture_output=True,
                                text=True)
        assert result.returncode, 'delayed output reference escaped its call frame'
        assert 'temporary DPI formals require blocking assignments' in result.stderr, result.stderr
    for kind in ('suspends', 'missing-abi'):
        source = work / ('export-task-' + kind + '.mlir')
        output = source.with_suffix('.lowered.mlir')
        body = 'llhd.wait (%event : i1), ^done\n  ^done:\n    llhd.return' if kind == 'suspends' else 'llhd.return'
        source.write_text(
            'module {\n  llhd.coroutine @unsupported(%event:i1) attributes {circt.dpi.export="unsupported"} {\n    '
            + body + '\n  }\n}\n')
        result = subprocess.run([
            circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-signal-memory{exports=true captures-only=true})',
            '-o', output
        ],
                                capture_output=True,
                                text=True)
        diagnostic = 'suspending DPI exports require a scheduler call frame' if kind == 'suspends' else 'native DPI task export requires original ABI metadata'
        assert result.returncode and diagnostic in result.stderr, result.stderr
    print(
        'PASS: direct and CFG-merged NBA output formals reject dangling DPI call frames',
        flush=True)


def check_context_guards(circt_opt, library, work):
    declaration = '''  sim.func.dpi private @ctx(in %index : i32, return result : i32) attributes {chipyard.dpi_abi = [{direction = "in", four_state = false, kind = "int", signed = true, width = 32 : i64}, {direction = "return", four_state = false, kind = "int", signed = true, width = 32 : i64}], chipyard.dpi_decl = {c_name = "ctx", context = true, pure = false, task = false}, verilogName = "ctx"}
    '''
    export = '''  func.func @bound(%x:i32)->i32 attributes {chipyard.lowered_dpi_export="bound"} {
        %r=sim.func.dpi.call @ctx(%x):(i32)->i32
        return %r:i32
      }
    '''
    unbound = '''  hw.module @Guard(in %x:i32,out q:i32) {
        %r=sim.func.dpi.call @ctx(%x):(i32)->i32
        hw.output %r:i32
      }
    '''
    results = []
    for kind, body in [('bound', export), ('mixed', export + unbound),
                       ('unbound', unbound), ('unused', '')]:
        source = work / ('context-' + kind + '.mlir')
        output = source.with_suffix('.checked.mlir')
        source.write_text('module {\n' + declaration + body + '}\n')
        result = subprocess.run([
            circt_opt, source, '--load-pass-plugin=' + str(library),
            '--pass-pipeline=builtin.module(chipyard-native-dpi-abi{validate-only=true})',
            '-o', output
        ],
                                capture_output=True,
                                text=True)
        if kind == 'bound': assert result.returncode == 0, result.stderr
        else:
            assert result.returncode and 'context DPI requires native declaration-scope binding' in result.stderr, result.stderr
        results.append(dict(case=kind, returncode=result.returncode))
        print(kind, 'passed', flush=True)


def check_qualifier_guards(circt_opt, library, work):
    abi = '[{direction="in",four_state=false,kind="int",signed=true,width=32:i64},{direction="return",four_state=false,kind="int",signed=true,width=32:i64}]'
    for qualifier in ('context', 'pure', 'task'):
        source = work / ('dpi-qualifier-' + qualifier + '.mlir')
        output = source.with_suffix('.checked.mlir')
        declarations = []
        for name, active in [('plain', False), ('different', True)]:
            values = {key: False for key in ('context', 'pure', 'task')}
            values[qualifier] = active
            flags = ','.join(key + '=' + str(value).lower()
                             for key, value in values.items())
            declarations.append(
                '  sim.func.dpi private @' + name +
                '(in %x:i32, return result:i32) attributes {chipyard.dpi_abi='
                + abi + ',chipyard.dpi_decl={c_name="same_c",' + flags +
                '},verilogName="same_c"}')
        source.write_text(
            'module {\n' + '\n'.join(declarations) +
            '\n  hw.module @Guard(in %x:i32,out q:i32) {\n    %a=sim.func.dpi.call @plain(%x):(i32)->i32\n    %b=sim.func.dpi.call @different(%a):(i32)->i32\n    hw.output %b:i32\n  }\n}\n'
        )
        command = [circt_opt, source, '--load-pass-plugin=' + str(library)]
        result = subprocess.run([
            *command, '--pass-pipeline=builtin.module(chipyard-coalesce-dpi)',
            '-o', output
        ],
                                capture_output=True,
                                text=True)
        result.check_returncode()
        assert output.read_text().count(
            'sim.func.dpi private @') == 2, qualifier
        if qualifier == 'context':
            result = subprocess.run([
                *command,
                '--pass-pipeline=builtin.module(chipyard-coalesce-dpi,chipyard-native-dpi-abi{validate-only=true})',
                '-o', output
            ],
                                    capture_output=True,
                                    text=True)
            assert result.returncode and 'context DPI requires native declaration-scope binding' in result.stderr, result.stderr
            # Separate frontend invocations can produce imports with one C
            # name and different qualifiers. SV rejects these in one source
            # compilation, so exercise the merged module at its IR boundary.
            source.write_text(
                'module {\n' + '\n'.join(declarations) +
                '\n  func.func @bound(%x:i32)->i32 attributes {chipyard.lowered_dpi_export="bound"} {\n'
                '    %a=sim.func.dpi.call @plain(%x):(i32)->i32\n'
                '    %b=sim.func.dpi.call @different(%a):(i32)->i32\n'
                '    return %b:i32\n  }\n}\n')
            result = subprocess.run([
                *command,
                '--pass-pipeline=builtin.module(chipyard-coalesce-dpi,chipyard-native-dpi-abi)',
                '-o', output
            ],
                                    capture_output=True,
                                    text=True)
            result.check_returncode()
            lowered = output.read_text()
            assert lowered.count('chipyard.native_dpi_bridge = ') == 2, lowered
            assert lowered.count('func.func private @same_c(') == 1, lowered
            assert lowered.count('call @same_c(') == 2, lowered
            assert lowered.count('call @svGetScope(') == 1, lowered
            assert lowered.count('call @svSetScope(') == 1, lowered
    print(
        'PASS: shared C names preserve context, pure and task qualifiers; coalescing cannot bypass scope admission',
        flush=True)
