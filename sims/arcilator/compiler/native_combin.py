"""Keep native scalar glue in the compiled model; retain real latch state."""
import re
from partition import IDENT, SSA, opcode, split, unquote

INSTANCE = re.compile(
    r'\s*(?:(.*?) = )?hw.instance ("(?:[^"\\]|\\.)*")(?: sym @' +
    IDENT + r')? @(' + IDENT +
    r')(?:<(.*?)>)?\((.*?)\) -> \((.*?)\)(?: \{.*\})?')
INTERFACES = {
    'GenericDigitalInIOCell': ({'pad', 'ie'}, 'i'),
    'GenericDigitalOutIOCell': ({'o', 'oe'}, 'pad'),
    'ClockInverter': ({'clockIn'}, 'clockOut'),
    'ClockOr2': ({'clocksIn_0', 'clocksIn_1'}, 'clockOut'),
    'ClockSignalNor2': ({'clockIn', 'signalIn'}, 'clockOut'),
    'EICG_wrapper': ({'in', 'en', 'test_en'}, 'out')
}


def lower_native_combin(lines, externs, reserved):
    namespace = '%arc_native_glue'
    while any(value.startswith(namespace) for value in reserved):
        namespace += '_'
    latch = '__ArcNativeClockGateLatch'
    while latch in externs:
        latch += '_'
    serial = 0
    for line in lines:
        if opcode(line) != 'hw.instance':
            yield line
            continue
        match = INSTANCE.fullmatch(line)
        if not match:
            yield line
            continue
        results, instance, declaration, parameters, ins, outs = match.groups()
        module = externs.get(unquote(declaration))
        expected = INTERFACES.get(module)
        if expected is None or parameters or len(split(results or '')) != 1:
            yield line
            continue
        inputs = {}
        valid = True
        for item in split(ins):
            value = re.fullmatch(
                r'(' + IDENT + r'):\s*(' + SSA + r'):\s*(.*)', item)
            if not value or unquote(value[1]) in inputs:
                valid = False
                break
            inputs[unquote(value[1])] = (value[2], value[3].strip())
        outputs = split(outs)
        output = re.fullmatch(r'(' + IDENT + r'):\s*(.*)',
                              outputs[0]) if len(outputs) == 1 else None
        result = (results or '').strip()
        # Directed, scalar stock interfaces only. Unusual declarations retain
        # the native binding, including physical analog/inout connections.
        if (not valid or inputs.keys() != expected[0] or not output or
                unquote(output[1]) != expected[1] or
                not re.fullmatch(SSA, result) or
                any(ty not in ('i1', '!seq.clock') for _, ty in inputs.values()) or
                output[2].strip() not in ('i1', '!seq.clock') or
                any(inputs[name][1] != 'i1'
                    for name in ('en', 'test_en', 'ie', 'oe', 'signalIn')
                    if name in inputs)):
            yield line
            continue
        prefix = namespace + '_' + str(serial)
        serial += 1
        generated = []

        def bit(name):
            value, ty = inputs[name]
            if ty == '!seq.clock':
                cast = prefix + '_' + name + '_bit'
                generated.append(f'    {cast} = seq.from_clock {value}')
                return cast
            return value

        def binary(name, operation, first, second):
            target = prefix + '_' + name
            generated.append(
                f'    {target} = comb.{operation} {first}, {second} : i1')
            return target

        def inverse(name, value):
            one = prefix + '_' + name + '_one'
            generated.append(f'    {one} = hw.constant true')
            return binary(name, 'xor', value, one)

        if module == 'GenericDigitalInIOCell':
            value = binary('out', 'and', bit('pad'), bit('ie'))
        elif module == 'GenericDigitalOutIOCell':
            value = binary('out', 'and', bit('o'), bit('oe'))
        elif module == 'ClockInverter':
            value = inverse('out', bit('clockIn'))
        elif module == 'ClockOr2':
            value = binary('out', 'or', bit('clocksIn_0'), bit('clocksIn_1'))
        elif module == 'ClockSignalNor2':
            joined = binary('joined', 'or', bit('clockIn'), bit('signalIn'))
            value = inverse('out', joined)
        else:
            clock = bit('in')
            enabled = binary('enable', 'or', bit('en'), bit('test_en'))
            transparent = inverse('transparent', clock)
            held = prefix + '_latched'
            # The gate's enable remains a transparent low-level latch. Only
            # its AND gate moves into the native combinational dataflow.
            externs[latch] = 'ArcLatch'
            generated.append(f'    {held} = hw.instance {instance} @{latch}(value: {enabled}: i1, write: {transparent}: i1) -> (q: i1)')
            value = binary('out', 'and', clock, held)
        if output[2].strip() == '!seq.clock':
            generated.append(f'    {result} = seq.to_clock {value}')
        else:
            generated.append(f'    {result} = hw.bitcast {value} : (i1) -> i1')
        yield from generated
