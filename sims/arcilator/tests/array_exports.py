"""Check rejected array ABI shapes and exported temporary-reference lifetimes."""
import copy
import subprocess
from native_exports import array_layout, prototype, generate_exports


def check_guards(circt_opt, library, work):
    leaf = dict(kind='int', width=32, signed=True, four_state=False)
    base = dict(kind='unpacked_array', left=3, right=1, element=leaf, direction='in')
    cases = []
    for field, value in [('left', True), ('right', 1.5), ('left', -(1 << 31) - 1)]:
        arg = copy.deepcopy(base)
        arg[field] = value
        cases.append((arg, 'array bounds'))
    cases += [
        (dict(base, left=-(1 << 31), right=(1 << 31) - 1), 'array storage width'),
        (dict(base, element=dict(kind='open_array')), 'C representation'),
        (dict(base, element=dict(kind='unsupported')), 'C representation'),
    ]
    for arg, diagnostic in cases:
        try:
            array_layout(arg)
        except ValueError as error:
            if diagnostic not in str(error):
                raise RuntimeError('wrong array ABI diagnostic: ' + str(error))
        else:
            raise RuntimeError('invalid native export array admitted')
    contract = dict(declaration=dict(c_name='array_guard'), formal_count=1,
                    captures=[], references=[False], abi=[base],
                    kernel='guard', entry='guard')
    returned = dict(contract, formal_count=0, references=[],
                    abi=[dict(base, direction='return')])
    try:
        prototype(returned)
    except ValueError as error:
        if 'unsupported packed' not in str(error):
            raise
    else:
        raise RuntimeError('unsupported array return admitted')
    other = copy.deepcopy(contract)
    other.update(kernel='other', entry='other')
    other['abi'][0]['element'] = dict(kind='shortint', width=16, signed=True, four_state=False)
    try:
        generate_exports([dict(exports=[contract, other])], dict(instances=[]))
    except ValueError as error:
        if 'conflicting C ABI' not in str(error):
            raise
    else:
        raise RuntimeError('incompatible array pointer element types admitted')
    source = work / 'array-export-nba.mlir'
    source.write_text('''module {
  func.func @bad(%formal:!llhd.ref<!hw.array<3xi13>>) attributes {
    circt.dpi.export="bad", chipyard.dpi_export={formal_count=1:i64}
  } {
    %zero=hw.constant 0:i39
    %value=hw.bitcast %zero:(i39)->!hw.array<3xi13>
    %time=llhd.constant_time <0ns, 1d, 0e>
    llhd.drv %formal, %value after %time:!hw.array<3xi13>
    return
  }
}
''')
    result = subprocess.run([
        circt_opt, source, '--load-pass-plugin=' + str(library),
        '--pass-pipeline=builtin.module(chipyard-native-signal-memory{exports=true captures-only=true})',
        '-o', source.with_suffix('.lowered.mlir')
    ], capture_output=True, text=True)
    if not result.returncode or 'temporary DPI formals require blocking assignments' not in result.stderr:
        raise RuntimeError('array temporary-reference lifetime guard failed: ' + result.stderr)
    print('PASS: native array bounds/size/leaf/prototype/return/lifetime guards', flush=True)
