"""Check scalar native lowering and conservative interface fallbacks."""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from partition import lower_special_ops


def lower(line, module, enabled=True, externs=None):
    declarations = dict(externs or {}, G=module)
    return lower_special_ops(line, declarations, set(),
                             native_combin=enabled), declarations


cases = [
    ('GenericDigitalInIOCell', 'pad: %a: i1, ie: %b: i1', 'i: i1'),
    ('GenericDigitalOutIOCell', 'o: %a: i1, oe: %b: i1', 'pad: i1'),
    ('ClockInverter', 'clockIn: %a: !seq.clock', 'clockOut: !seq.clock'),
    ('ClockOr2', 'clocksIn_0: %a: !seq.clock, clocksIn_1: %b: !seq.clock',
     'clockOut: !seq.clock'),
    ('ClockSignalNor2', 'clockIn: %a: !seq.clock, signalIn: %b: i1',
     'clockOut: !seq.clock'),
    ('EICG_wrapper', 'in: %a: !seq.clock, en: %b: i1, test_en: %c: i1',
     'out: !seq.clock'),
]
for module, inputs, output in cases:
    line = f'    %r = hw.instance "glue" @G({inputs}) -> ({output})'
    result, declarations = lower(line, module)
    assert result != line
    if '!seq.clock' in output:
        assert ' = seq.to_clock ' in result
    assert lower(line, module, False)[0] == line
    if module == 'EICG_wrapper':
        assert 'ArcLatch' in declarations.values()
        assert 'comb.or %b, %c : i1' in result
        assert 'comb.xor' in result and 'comb.and' in result
    else:
        assert 'hw.instance' not in result
    for changed in [line.replace('@G(', '@G<W: ui32 = 1>('),
                    line.replace('%r =', '%r:1 ='),
                    line.replace(output + ')', output + ', extra: i1)'),
                    line.replace('i1', 'i8'),
                    line.replace(inputs, inputs + ', extra: %e: i1')]:
        # Clock-only interfaces need a non-scalar clock input for this case.
        if changed == line:
            changed = line.replace('!seq.clock', '!hw.inout<i1>')
        assert lower(changed, module)[0] == changed, changed
    assert lower(line, 'CustomCell')[0] == line

line = ('    %arc_native_glue_0_out = hw.instance "gate" @G('
        'in: %a: !seq.clock, en: %b: i1, test_en: %c: i1) -> (out: !seq.clock)')
result, declarations = lower(line, 'EICG_wrapper', externs={
    '__ArcNativeClockGateLatch': 'UnrelatedCustomCell'})
assert declarations['__ArcNativeClockGateLatch'] == 'UnrelatedCustomCell'
assert declarations['__ArcNativeClockGateLatch_'] == 'ArcLatch'
definitions = re.findall(r'^\s*(%[\w.$-]+) =', result, re.M)
assert len(definitions) == len(set(definitions))
assert definitions.count('%arc_native_glue_0_out') == 1
quoted = line.replace('@G(', '@"quoted gate"(')
from compiler.native_combin import lower_native_combin
assert list(lower_native_combin([quoted], {'quoted gate': 'EICG_wrapper'}, set())) != [quoted]
print('PASS: six native scalar interfaces; hybrid/parameter/width/inout/extra-port fallbacks and reserved-name collisions')
