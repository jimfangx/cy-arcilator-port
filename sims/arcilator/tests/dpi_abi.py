#!/usr/bin/env python3
"""Check original SV DPI type provenance and native C ABI admission guards."""
import argparse
import os
from pathlib import Path
import re
from types import SimpleNamespace
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin
from compiler.verilog.build import build as build_frontend
from partition import split, unquote


def attribute(text):
    text = text.strip()
    if text.startswith('{'):
        return {
            unquote(k.strip()): attribute(v)
            for k, v in (field.split('=', 1) for field in split(text[1:-1]))
        }
    if text.startswith('['):
        return [attribute(field) for field in split(text[1:-1])]
    if text.startswith('"'):
        return unquote(text)
    if text in ('true', 'false'):
        return text == 'true'
    assert re.fullmatch(r'-?\d+ : i\d+', text), text
    return int(text.partition(':')[0])


def imports(text):
    tail = text.partition('chipyard.dpi_imports = ')[2]
    assert tail.startswith('{')
    depth = 0
    for match in re.finditer(r'"(?:\\.|[^"\\])*"|[{}]', tail):
        if match[0] == '{':
            depth += 1
        elif match[0] == '}':
            depth -= 1
            if depth == 0:
                return attribute(tail[:match.end()])
    raise AssertionError('unterminated DPI metadata')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/dpi-abi-guards')
    args = parser.parse_args()
    sdk, work = args.circt_bin.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    frontend = build_frontend(
        SimpleNamespace(circt_verilog=sdk / 'circt-verilog',
                        cache=SIM / 'generated-src/compiler/verilog',
                        circt_source=None,
                        slang_source=None,
                        slang_build=None,
                        jobs=2))
    raw = work / 'metadata.moore.mlir'
    subprocess.run([
        frontend, '--ir-moore', '--top=DPIABIMetadata',
        HERE / 'dpi_abi_metadata.sv', '-o', raw
    ],
                   check=True)
    metadata = imports(raw.read_text())
    assert set(metadata) == {
        'types_alias', 'pure_alias', 'context_alias', 'task_alias',
        'types_alias_open_actual'
    }
    # A call with a fixed actual retains both its original open formal and
    # its actual-shape specialization. Every other source field is identical.
    specialized = metadata.pop('types_alias_open_actual')
    actual = specialized['abi'][19].pop('native_open_actual')
    assert actual == {
        'kind': 'unpacked_array', 'left': 0, 'right': 1,
        'element': {'kind': 'int', 'width': 32, 'signed': True,
                    'four_state': False}
    }
    assert specialized == metadata['types_alias']
    for name, entry in metadata.items():
        assert entry['version'] == 1
        decl = entry['declaration']
        assert decl['name'] == name and decl['scope'] == 'DPIABIMetadata'
        assert decl['c_name'] == 'actual_' + name.removesuffix('_alias')
        assert decl['timeunit_fs'] == 10000000 and decl[
            'timeprecision_fs'] == 100000
        assert len(decl['source_types']) == len(entry['abi'])
        assert decl['pure'] == (name == 'pure_alias')
        assert decl['context'] == (name == 'context_alias')
        assert decl['task'] == (name == 'task_alias')
    abi = metadata['types_alias']['abi']
    assert [a['kind'] for a in abi] == [
        'bit', 'logic', 'byte', 'byte', 'shortint', 'int', 'longint',
        'packed_array', 'packed_array', 'int', 'packed_struct', 'packed_union',
        'integer', 'time', 'real', 'shortreal', 'realtime', 'string',
        'chandle', 'open_array', 'int', 'longint'
    ]
    assert [a['direction'] for a in abi] == ['in'] * 20 + ['out', 'inout']
    assert metadata['pure_alias']['abi'][-1]['direction'] == 'return'
    assert abi[2]['signed'] and not abi[3]['signed']
    assert (abi[7]['left'], abi[7]['right'], abi[7]['width']) == (0, 31, 32)
    assert not abi[7]['four_state'] and abi[8]['four_state']
    assert abi[9]['width'] == 32 and abi[9]['signed']  # enum underlying int
    assert abi[10]['width'] == abi[11]['width'] == 9
    assert not abi[10]['four_state'] and abi[11]['four_state']
    assert abi[19]['element']['kind'] == 'int' and not abi[19]['packed']
    assert metadata['types_alias']['declaration']['source_types'][7].endswith(
        '.bits_t')

    cxx = os.environ.get('CXX', 'c++')
    library = plugin(sdk / 'circt-opt', cxx, SIM / 'generated-src/compiler',
                     SIM / 'compiler/Model.cpp')
    rtl = plugin(sdk / 'circt-opt', cxx, SIM / 'generated-src/compiler',
                 SIM / 'compiler/RTL.cpp')

    def lower(name,
              text,
              pipeline='chipyard-native-dpi-abi{validate-only=true}',
              error=None):
        source, target = work / (name + '.mlir'), work / (name +
                                                          '-lowered.mlir')
        source.write_text(text)
        result = subprocess.run([
            sdk / 'circt-opt', source, '--load-pass-plugin=' + str(library),
            '--load-pass-plugin=' + str(rtl),
            '--pass-pipeline=builtin.module(' + pipeline + ')', '-o', target
        ],
                                capture_output=True,
                                text=True)
        if error:
            assert result.returncode and error in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        return target.read_text()

    def declaration(kind='int',
                    width=32,
                    logical='i32',
                    flags='',
                    direction='in',
                    extra=''):
        info = f'{{kind = "{kind}", width = {width} : i64, signed = true, four_state = false, direction = "{direction}"}}'
        formal = f'{direction} %x: {logical}' if direction in (
            'in', 'inout', 'ref') else f'{direction} x: {logical}'
        return f'''module {{
          sim.func.dpi private @sv({formal}) attributes {{verilogName = "actual_c",
            chipyard.dpi_abi = [{info}], chipyard.dpi_decl = {{context = false, task = false {flags}}}}}
          {extra}
        }}'''

    accepted = 0
    for kind, width in [('bit', 1), ('logic', 1), ('byte', 8),
                        ('shortint', 16), ('int', 32), ('longint', 64),
                        ('packed_array', 129), ('packed_struct', 9),
                        ('packed_union', 33), ('integer', 32)]:
        for direction in ('in', 'out', 'inout'):
            text = declaration(kind,
                               width,
                               'i' + str(width),
                               direction=direction)
            lower(f'{kind}-{direction}', text)
            accepted += 1
    for kind, width in [('real', 64), ('shortreal', 32), ('realtime', 64)]:
        for direction in ('in', 'out', 'inout'):
            lower(kind + '-' + direction, declaration(kind, width, 'f' + str(width), direction=direction))
            accepted += 1
    for kind in ('time', 'string', 'chandle', 'open_array',
                 'unpacked_array', 'unsupported'):
        lower('reject-' + kind,
              declaration(kind),
              error=('invalid native unpacked DPI array shape or direction' if kind == 'unpacked_array' else 'unsupported native DPI formal representation'))
    for flag, message in [
        ('context', 'context DPI requires native declaration-scope binding'),
        ('task', 'DPI task disable/suspension ABI requires a native binding')
    ]:
        text = declaration().replace(flag + ' = false', flag + ' = true')
        lower('reject-' + flag, text, error=message)
    lower('missing',
          'module {sim.func.dpi private @sv(in %x: i32)}',
          error='native DPI requires original SystemVerilog ABI metadata')
    lower('mismatch',
          declaration('int', 64, 'i64'),
          error='does not match logical formal type')
    lower('ref',
          declaration(logical='!llvm.ptr', direction='ref'),
          error='unsupported native DPI formal representation')
    lower('packed-return',
          declaration('packed_array', 32, direction='return'),
          error='packed DPI returns require a native binding')
    lowered = lower('bridge', declaration(), 'chipyard-native-dpi-abi')
    assert 'chipyard.native_dpi_bridge = @chipyard_dpi_bridge' in lowered
    assert lower('bridge-again', lowered, 'chipyard-native-dpi-abi') == lowered
    retained = lower('bridge-dce', lowered.replace('private @sv', '@sv'),
                     'symbol-dce')
    assert 'func.func private @chipyard_dpi_bridge' in retained
    for kind, width in [('byte', 8), ('shortint', 16)]:
        for signed, extension in [(True, 'llvm.signext'),
                                  (False, 'llvm.zeroext')]:
            text = declaration(kind,
                               width,
                               'i' + str(width),
                               direction='return')
            if not signed:
                text = text.replace('signed = true', 'signed = false')
            result = lower(f'{kind}-return-{signed}', text,
                           'chipyard-native-dpi-abi')
            assert extension in result, result
    second = declaration('packed_array').partition('module {')[2].rsplit(
        '}', 1)[0]
    second = second.replace('@sv(', '@other(')
    conflict = declaration().rsplit('}', 1)[0] + second + '}'
    lower('c-prototype-conflict',
          conflict,
          'chipyard-native-dpi-abi',
          error='conflicting native C ABI for DPI symbol')
    second = declaration('byte', 8,
                         'i8').partition('module {')[2].rsplit('}', 1)[0]
    second = second.replace('@sv(', '@other(').replace('signed = true',
                                                       'signed = false')
    conflict = declaration('byte', 8, 'i8').rsplit('}', 1)[0] + second + '}'
    lower('c-extension-conflict',
          conflict,
          'chipyard-native-dpi-abi',
          error='conflicting narrow-integer DPI C ABI extension')
    def string_declaration(direction='in', logical='i64'):
        return declaration('string', 0, logical, direction=direction).replace(
            'kind = "string",',
            'kind = "string", native_string_handle = true,')

    for direction in ('in', 'out', 'inout', 'return'):
        text = string_declaration(direction)
        lower('string-' + direction, text)
        accepted += 1
        converted = lower('string-bridge-' + direction, text,
                          'chipyard-native-dpi-abi')
        assert 'func.func private @actual_c(' in converted
        assert '!llvm.ptr' in converted
        assert lower('string-bridge-again-' + direction, converted,
                     'chipyard-native-dpi-abi') == converted
    lower('string-width', string_declaration(logical='i32'),
          error='does not match logical formal type')
    lower('string-unconverted',
          string_declaration().replace('native_string_handle = true',
                                       'native_string_handle = false'),
          error='unsupported native DPI formal representation')
    for name, signature in [
        ('chipyard_arc_string_c_str', '(i32) -> !llvm.ptr'),
        ('chipyard_arc_string_from_c', '(!llvm.ptr) -> i32'),
    ]:
        text = string_declaration().rsplit('}', 1)[0]
        text += f'func.func private @{name}{signature}\n}}'
        lower('string-helper-' + name, text, 'chipyard-native-dpi-abi',
              error='conflicting native DPI string helper')
    second = declaration().partition('module {')[2].rsplit('}', 1)[0]
    second = second.replace('@sv(', '@other(')
    lower('string-c-prototype-conflict',
          string_declaration().rsplit('}', 1)[0] + second + '}',
          'chipyard-native-dpi-abi',
          error='conflicting native C ABI for DPI symbol')
    def handle_declaration(direction='in', logical='i64'):
        return declaration('chandle', 0, logical, direction=direction).replace(
            'kind = "chandle",',
            'kind = "chandle", native_chandle_handle = true,')

    for direction in ('in', 'out', 'inout', 'return'):
        text = handle_declaration(direction)
        lower('chandle-' + direction, text)
        accepted += 1
        converted = lower('chandle-bridge-' + direction, text,
                          'chipyard-native-dpi-abi')
        assert 'func.func private @actual_c(' in converted and '!llvm.ptr' in converted
        assert 'llvm.inttoptr' in converted or 'llvm.ptrtoint' in converted
        assert lower('chandle-bridge-again-' + direction, converted,
                     'chipyard-native-dpi-abi') == converted
    lower('chandle-width', handle_declaration(logical='i32'),
          error='does not match logical formal type')
    lower('chandle-unconverted',
          handle_declaration().replace('native_chandle_handle = true',
                                       'native_chandle_handle = false'),
          error='unsupported native DPI formal representation')
    lower('chandle-c-prototype-conflict',
          handle_declaration().rsplit('}', 1)[0] + second + '}',
          'chipyard-native-dpi-abi',
          error='conflicting native C ABI for DPI symbol')
    floats = '''module {
      hw.module @FloatingProcess(in %clock: i1, in %data: i64, out result: i64) {
        %zero = arith.constant 0.0 : f64
        %delay = llhd.constant_time <1ns, 0d, 0e>
        %result = llhd.process -> f64 {
          cf.br ^sleep(%zero : f64)
        ^sleep(%last: f64):
          llhd.wait yield (%last : f64), delay %delay, (%clock : i1), ^wake(%last : f64)
        ^wake(%prior: f64):
          %value = arith.bitcast %data : i64 to f64
          %sum = arith.addf %prior, %value : f64
          llhd.halt %sum : f64
        }
        %bits = arith.bitcast %result : f64 to i64
        hw.output %bits : i64
      }
    }'''
    converted = lower(
        'float-process', floats,
        'hw.module(chipyard-bitcast-float-processes),canonicalize')
    assert 'llhd.process -> i64' in converted
    assert not re.search(r'llhd.(?:wait|halt).*f64', converted), converted
    assert lower('float-process-again', converted,
                 'hw.module(chipyard-bitcast-float-processes),canonicalize'
                 ) == converted
    schema = '''module attributes {chipyard.dpi_imports = {sv = {version = 1 : i32,
      abi = [{kind = "int", direction = "in", width = 32 : i64, signed = true, four_state = false}],
      declaration = {context = false, task = false, scope = "original"}}}} {
      sim.func.dpi private @sv(in %x: i32)
      func.func @caller(%x: i32) {
        sim.func.dpi.call @sv(%x) : (i32) -> ()
        return
      }
    }'''
    restored = lower('restore', schema, 'chipyard-restore-dpi-abi')
    assert 'chipyard.dpi_imports' not in restored and restored.count(
        'scope = "original"') == 2
    assert lower('restore-again', restored,
                 'chipyard-restore-dpi-abi') == restored
    lower('bad-version',
          schema.replace('version = 1', 'version = 2'),
          'chipyard-restore-dpi-abi',
          error='invalid original SystemVerilog DPI signature metadata')
    lower('bad-arity',
          re.sub(r'abi = \[[^\]]*\]', 'abi = []', schema),
          'chipyard-restore-dpi-abi',
          error='invalid original SystemVerilog DPI signature metadata')
    print(
        f'PASS: original SV ABI provenance, aliases, typedefs/enums, domains, ranges, qualifiers, scopes and timescales; {accepted} admitted signatures, unsupported ABI guards, metadata restoration, bridge retention and idempotence',
        flush=True)


if __name__ == '__main__':
    main()
