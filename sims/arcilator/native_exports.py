"""Read compiler DPI export contracts and emit scoped native C ABI bridges."""
import json
import re
from pathlib import PurePosixPath
from partition import IDENT, split, unquote


def attribute(text):
    text = text.strip()
    if text.startswith('{') and text.endswith('}'):
        result = {}
        for entry in split(text[1:-1]):
            key, sep, value = entry.partition('=')
            if not sep or key.strip() in result:
                raise ValueError('invalid DPI export attribute: ' + entry)
            result[key.strip()] = attribute(value)
        return result
    if text.startswith('[') and text.endswith(']'):
        return [attribute(value) for value in split(text[1:-1])]
    if re.fullmatch(r'"(?:[^"\\]|\\.)*"', text):
        return unquote(text)
    if text in ('true', 'false'):
        return text == 'true'
    number = re.fullmatch(r'(-?\d+)\s*:\s*i\d+', text)
    if number:
        return int(number[1])
    raise ValueError('unsupported DPI export attribute: ' + text)


def contracts(text):
    result = []
    for line in text.splitlines():
        if 'chipyard.export_kernel = ' not in line:
            continue
        header = re.match(
            r'\s*func.func @(' + IDENT + r')\(.*?\) attributes (.*) \{', line)
        if not header:
            raise ValueError('invalid native DPI export packet declaration')
        info = attribute(header[2])
        kernel = info['chipyard.export_kernel']
        abi = info['chipyard.dpi_export']
        result.append(
            dict(entry=unquote(header[1]),
                 kernel=kernel['id'],
                 references=kernel['arguments'],
                 **abi))
    return result


def representation(arg):
    kind = arg['kind']
    if kind == 'unpacked_array':
        _, leaf = array_layout(arg)
        ty, element_kind = representation(leaf)
        return ty, 'array:' + element_kind
    if kind == 'string' and arg.get('native_string_handle'):
        return 'const char *', 'string'
    if kind == 'chandle' and arg.get('native_chandle_handle'):
        return 'void *', 'chandle'
    if kind in ('real', 'shortreal', 'realtime'):
        return ('float' if kind == 'shortreal' else 'double'), 'scalar'
    width = arg.get('width', 0)
    atoms = dict(byte=(8, 'char'), shortint=(16, 'short'),
                 int=(32, 'int'), longint=(64, 'long long'))
    if kind in atoms and width == atoms[kind][0]:
        return ('' if arg['signed'] else 'unsigned ') + atoms[kind][1], 'scalar'
    if kind in ('bit', 'logic') and width == 1:
        return ('svBit' if kind == 'bit' else 'svLogic'), kind
    if kind in ('packed_array', 'packed_struct', 'packed_union', 'integer',
                'time') and width > 0:
        return ('svLogicVecVal',
                'logics') if arg['four_state'] else ('svBitVecVal', 'bits')
    raise ValueError('unsupported native DPI export C representation: ' + kind)


def storage_width(arg):
    if arg['kind'] == 'unpacked_array':
        count, leaf = array_layout(arg)
        return count * storage_width(leaf)
    if arg['kind'] in ('real', 'shortreal', 'realtime'):
        return 32 if arg['kind'] == 'shortreal' else 64
    if arg['kind'] == 'string' and arg.get('native_string_handle'):
        return 64
    if arg['kind'] == 'chandle' and arg.get('native_chandle_handle'):
        return 64
    return arg['width']


def array_layout(arg):
    count = 1
    while arg['kind'] == 'unpacked_array':
        left, right = arg.get('left'), arg.get('right')
        if (type(left) is not int or type(right) is not int or
                not -(1 << 31) <= left < (1 << 31) or
                not -(1 << 31) <= right < (1 << 31)):
            raise ValueError('invalid native DPI export array bounds')
        count *= abs(left - right) + 1
        arg = arg['element']
    representation(arg)
    width = storage_width(arg)
    if width <= 0 or count * width > (1 << 24):
        raise ValueError('invalid native DPI export array storage width')
    return count, arg


def array_parameter(arg, index):
    _, leaf = array_layout(arg)
    ty, kind = representation(leaf)
    if arg['direction'] == 'in':
        if kind != 'string':
            ty = 'const ' + ty
    return ty + ' *arg' + str(index)


def array_transfer(arg, index, output=False):
    """Map declaration-order C elements to compact, low-bit-first SV storage."""
    count, leaf = array_layout(arg)
    width = storage_width(leaf)
    ty, kind = representation(leaf)
    values, pointer = f'values[{index}]', f'arg{index}'
    lines = [f'  for (unsigned element = 0; element < {count}; ++element) {{',
             f'    unsigned base = ({count} - 1 - element) * {width};']
    if not output:
        if kind == 'string':
            lines += [f'    if (!{pointer}[element]) throw std::invalid_argument("null native DPI export string element");',
                      f'    uint64_t value = runtime.intern_string({pointer}[element]);']
        elif kind in ('bit', 'logic'):
            value = (f'{pointer}[element] == 1' if kind == 'logic' else
                     f'{pointer}[element] & 1')
            lines.append(f'    uint64_t value = {value};')
        elif kind not in ('bits', 'logics'):
            lines += ['    uint64_t value = 0;',
                      f'    std::memcpy(&value, &{pointer}[element], sizeof({ty}));']
        if kind in ('bits', 'logics'):
            words = (width + 31) // 32
            value = f'{pointer}[element * {words} + bit / 32]'
            if kind == 'logics':
                value = f'({value}.aval & ~{value}.bval)'
            value = f'({value} >> (bit % 32))'
        else:
            value = '(value >> bit)'
        lines += [f'    for (unsigned bit = 0; bit < {width}; ++bit)',
                  f'      {values}[(base + bit) / 8] |= uint8_t(({value} & 1u) << ((base + bit) % 8));']
    elif kind in ('bits', 'logics'):
        words = (width + 31) // 32
        lines += [f'    for (unsigned word = 0; word < {words}; ++word) {{',
                  '      uint32_t value = 0;',
                  f'      for (unsigned bit = 0; bit < 32 && word * 32 + bit < {width}; ++bit) {{',
                  '        unsigned position = base + word * 32 + bit;',
                  f'        value |= uint32_t(({values}[position / 8] >> (position % 8)) & 1u) << bit;',
                  '      }',
                  f'      {pointer}[element * {words} + word] = ' +
                  ('{value, 0};' if kind == 'logics' else 'value;'),
                  '    }']
    else:
        lines += ['    uint64_t value = 0;',
                  f'    for (unsigned bit = 0; bit < {width}; ++bit)',
                  f'      value |= uint64_t(({values}[(base + bit) / 8] >> ((base + bit) % 8)) & 1u) << bit;']
        if kind == 'string':
            lines.append(f'    {pointer}[element] = runtime.string_value(value).c_str();')
        else:
            lines.append(f'    std::memcpy(&{pointer}[element], &value, sizeof({ty}));')
    return lines + ['  }']


def prototype(contract):
    if contract['declaration'].get('task') and contract.get(
            'task_mode') != 'synchronous':
        raise ValueError(
            'native DPI exported tasks require a suspension binding')
    params, result, signature = [], 'void', []
    for i, arg in enumerate(contract['abi']):
        ty, kind = representation(arg)
        direction = arg['direction']
        if result != 'void':
            raise ValueError('native DPI export return must follow formals')
        if direction not in ('in', 'out', 'inout', 'return'):
            raise ValueError('unsupported native DPI export direction: ' +
                             direction)
        if direction == 'return':
            if kind in ('bits', 'logics') or kind.startswith('array:') or result != 'void':
                raise ValueError(
                    'unsupported packed or repeated native DPI export return')
            result = ty
            signature.append((direction, ty, kind))
            continue
        if kind.startswith('array:'):
            params.append(array_parameter(arg, i))
            signature.append((direction, ty, kind))
            continue
        if kind == 'string':
            params.append('const char ' +
                          ('*' if direction == 'in' else '**') + 'arg' +
                          str(i))
            signature.append((direction, ty, kind))
            continue
        pointer = direction != 'in' or kind in ('bits', 'logics')
        decl = ('const ' if direction == 'in' and pointer else
                '') + ty + (' *' if pointer else ' ')
        params.append(decl + 'arg' + str(i))
        signature.append((direction, ty, kind))
    if len(params) != contract['formal_count'] or len(
            contract['references']) != len(params) + len(contract['captures']):
        raise ValueError('native DPI export signature/capture mismatch')
    for i, arg in enumerate(contract['abi'][:len(params)]):
        if contract['references'][i] != (arg['direction'] != 'in'):
            raise ValueError('native DPI export argument direction mismatch')
    return result, params, signature


def thunk(contract, helper=None):
    name = contract['declaration']['c_name']
    if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name):
        raise ValueError('invalid native DPI export C symbol: ' + name)
    result, params, signature = prototype(contract)
    count = len(contract['abi'])
    lines = [
        (f'static {result} {helper}(' if helper else f'extern "C" {result} {name}(') + ', '.join(params) + ') {',
        '  auto *owner = chipyard_arc::active_runtime();',
        '  if (!owner) throw std::runtime_error("no active native model for DPI export");',
        '  auto &runtime = *owner;',
        f'  std::vector<chipyard_arc::ExportBytes> values({count});'
    ]
    if any(kind in ('chandle', 'array:chandle') for _, _, kind in signature):
        lines.append('  static_assert(sizeof(void *) == sizeof(uint64_t), "native chandle storage requires a 64-bit host");')
    for i, arg in enumerate(contract['abi']):
        direction, ty, kind = signature[i]
        lines += [
            f'  unsigned width{i} = runtime.export_width({json.dumps(name)}, {i});',
            f'  values[{i}].resize((width{i} + 7) / 8, 0);'
        ]
        if direction == 'return':
            continue
        if kind.startswith('array:'):
            lines.append(f'  if (!arg{i}) throw std::invalid_argument("null native DPI export array argument");')
            if direction != 'out':
                lines += array_transfer(arg, i)
            continue
        if kind == 'string':
            lines.append(
                f'  if (!arg{i}' +
                (f' || !*arg{i}' if direction == 'inout' else '') +
                ') throw std::invalid_argument("null native DPI string argument");'
            )
            if direction != 'out':
                value = 'arg' + str(i) if direction == 'in' else '*arg' + str(
                    i)
                lines += [
                    f'  uint64_t input{i} = runtime.intern_string({value});',
                    f'  std::memcpy(values[{i}].data(), &input{i}, sizeof(input{i}));'
                ]
            continue
        pointer = direction != 'in' or kind in ('bits', 'logics')
        if pointer:
            lines.append(
                f'  if (!arg{i}) throw std::invalid_argument("null native DPI export argument");'
            )
        if direction == 'out':
            continue
        if kind in ('bits', 'logics'):
            word = f'arg{i}[byte / 4]' + ('.aval & ~arg' + str(i) +
                                          '[byte / 4].bval'
                                          if kind == 'logics' else '')
            lines += [
                f'  for (unsigned byte = 0; byte < values[{i}].size(); ++byte)',
                f'    values[{i}][byte] = ({word}) >> ((byte % 4) * 8);'
            ]
        else:
            value = ('*' if pointer else '') + 'arg' + str(i)
            if kind == 'logic':
                value = '(' + value + ' == 1)'
            lines.append(f'  std::memcpy(values[{i}].data(), ' +
                         ('&arg' +
                          str(i) if not pointer and kind != 'logic' else '&' +
                          f'input{i}') + f', values[{i}].size());')
            if pointer or kind == 'logic':
                lines.insert(len(lines) - 1, f'  {ty} input{i} = {value};')
        lines.append(
            f'  if (width{i} % 8) values[{i}].back() &= (1u << (width{i} % 8)) - 1;'
        )
    lines.append(f'  runtime.invoke_export({json.dumps(name)}, values);')
    for i, arg in enumerate(contract['abi']):
        direction, ty, kind = signature[i]
        if direction == 'in':
            continue
        if kind.startswith('array:'):
            lines += array_transfer(arg, i, output=True)
            continue
        if kind == 'string':
            lines += [
                f'  uint64_t result{i} = 0;',
                f'  std::memcpy(&result{i}, values[{i}].data(), sizeof(result{i}));'
            ]
            value = f'runtime.string_value(result{i}).c_str()'
            lines.append('  ' + (
                'return ' if direction == 'return' else f'*arg{i} = ') +
                         value + ';')
            continue
        if direction == 'return':
            lines += [
                f'  {ty} result{{}};',
                f'  std::memcpy(&result, values[{i}].data(), values[{i}].size());',
                '  return result;'
            ]
        else:
            if kind in ('bits', 'logics'):
                lines += [
                    f'  for (unsigned word = 0; word < (width{i} + 31) / 32; ++word) {{',
                    '    uint32_t value = 0;',
                    f'    for (unsigned byte = 0; byte < 4 && word * 4 + byte < values[{i}].size(); ++byte)',
                    f'      value |= uint32_t(values[{i}][word * 4 + byte]) << (byte * 8);',
                    f'    arg{i}[word]' +
                    (' = {value, 0};' if kind == 'logics' else ' = value;'),
                    '  }'
                ]
            else:
                lines += [
                    f'  *arg{i} = 0;',
                    f'  std::memcpy(arg{i}, values[{i}].data(), values[{i}].size());'
                ]
    lines += ['}']
    return lines, signature


def scope_suffix(relative):
    parts = []
    while relative:
        if not relative.startswith('.'):
            raise ValueError('invalid native DPI export relative scope: ' +
                             relative)
        relative = relative[1:]
        if relative.startswith('\\'):
            match = re.match(r'\\(\S+)\s', relative)
        else:
            match = re.match(r'([A-Za-z_$][A-Za-z_0-9$]*)', relative)
        if not match:
            raise ValueError('invalid native DPI export scope component: ' +
                             relative)
        part = match[1]
        relative = relative[match.end():]
        while relative.startswith('['):
            index = re.match(r'\[-?\d+\]', relative)
            if not index:
                raise ValueError('invalid native DPI export scope index')
            part += index[0]
            relative = relative[index.end():]
        parts.append(part)
    return parts


def generate_exports(metadata, part):
    kernels = {}
    declarations, registrations, thunks = [], [], []
    signatures = {}
    array_helpers = {}
    for variant in metadata:
        for contract in variant.get('exports', []):
            key = contract['kernel']
            if key in kernels:
                raise ValueError('duplicate native DPI export kernel: ' + key)
            alias = 'arc_export_kernel_' + str(len(kernels))
            contract = dict(contract, alias=alias)
            kernels[key] = contract
            declarations.append(
                f'extern "C" void {alias}(uint8_t **, uint64_t *) asm({json.dumps(contract["entry"])});'
            )
            name = contract['declaration']['c_name']
            arrays = any(arg['kind'] == 'unpacked_array' for arg in contract['abi'])
            helper = 'arc_export_array_' + str(len(kernels) - 1) if arrays else None
            code, signature = thunk(contract, helper)
            if name in signatures and signatures[name] != signature:
                raise ValueError('conflicting C ABI for native DPI export: ' +
                                 name)
            if arrays:
                thunks.extend(code)
                array_helpers.setdefault(name, []).append((alias, helper, contract))
            elif name not in signatures:
                thunks.extend(code)
            signatures[name] = signature
    for name, helpers in array_helpers.items():
        result, params, _ = prototype(helpers[0][2])
        thunks += [f'extern "C" {result} {name}(' + ', '.join(params) + ') {',
                   '  auto *owner = chipyard_arc::active_runtime();',
                   '  if (!owner) throw std::runtime_error("no active native model for DPI export");',
                   f'  const auto &layout = owner->export_layout({json.dumps(name)});']
        arguments = ', '.join('arg' + str(i) for i in range(helpers[0][2]['formal_count']))
        for _, helper, contract in helpers:
            thunks.append(f'  if (layout == {json.dumps(contract["kernel"])}) return {helper}({arguments});')
        thunks += ['  throw std::runtime_error("unknown native DPI export array layout");', '}']
    targets = {}
    for instance in part['instances']:
        if instance['module'] != 'ArcSignalMemory':
            continue
        params = {}
        for text in instance['parameters']:
            match = re.fullmatch(r'([^:]+):\s*([^=]+)=\s*(.*)', text)
            if not match:
                raise ValueError('invalid native memory binding parameter')
            params[match[1].strip()] = unquote(match[3].strip())
        captures = json.loads(params.get('CAPTURES', '[]'))
        for capture in captures:
            kernel = capture['kernel']
            if kernel not in kernels:
                raise ValueError('missing compiled DPI export kernel: ' +
                                 kernel)
            path = params['PATH']
            storage = capture['storage']
            if not path.endswith(storage) or (len(path) > len(storage) and
                                              path[-len(storage) - 1] != '/'):
                raise ValueError('invalid DPI export storage path: ' + path)
            relative = capture['scope'].removeprefix(capture['instance'])
            if relative and not relative.startswith('.'):
                raise ValueError(
                    'DPI export scope is outside its declaring instance')
            # hw-flatten preserves physical instance components with slash paths.
            base = PurePosixPath(instance['name']).parent
            scope = str(
                PurePosixPath(part['top']) / base /
                path[:-len(storage)].rstrip('/'))
            if relative:
                scope += '/' + '/'.join(scope_suffix(relative))
            target = targets.setdefault((scope, kernel), {})
            index = capture['capture']
            if index < 0:
                continue
            if index in target:
                raise ValueError(
                    'duplicate storage for native DPI export capture')
            handle = next(p['wire'] for p in instance['ports']
                          if p['name'] == 'handle')
            target[index] = handle, int(params['BITS'])
    seen_names = set()
    for (scope, key), captures in sorted(targets.items()):
        contract = kernels[key]
        formals = contract['formal_count']
        widths = [
            storage_width(a) for a in contract['abi']
            if a['direction'] != 'return'
        ]
        expected = contract['captures']
        if set(captures) != set(range(len(expected))):
            raise ValueError('incomplete native DPI export storage binding: ' +
                             contract['entry'])
        for i, capture in enumerate(expected):
            if capture['argument'] != formals + i or capture[
                    'width'] != captures[i][1]:
                raise ValueError(
                    'native DPI export storage width/order mismatch')
            widths.append(captures[i][1])
        resultbits = next(
            (storage_width(a)
             for a in contract['abi'] if a['direction'] == 'return'), 0)
        name = contract['declaration']['c_name']
        if (scope, name) in seen_names:
            raise ValueError('duplicate native DPI export in scope: ' + scope +
                             '/' + name)
        seen_names.add((scope, name))
        registrations.append('  runtime.add_export(' + ', '.join([
            json.dumps(scope),
            json.dumps(name), contract['alias'],
            str(formals),
            str(resultbits), '{' + ', '.join(map(str, widths)) + '}', '{' +
            ', '.join('true' if r else 'false'
                      for r in contract['references']) + '}', '{' +
            ', '.join('runtime.signal(' + json.dumps(captures[i][0]) + ')'
                      for i in range(len(expected))) + '}',
            json.dumps(contract['kernel'])
        ]) + ');')
    unbound = set(kernels) - {key for _, key in targets}
    if unbound:
        raise ValueError('native DPI export has no bound instance scope: ' +
                         ', '.join(sorted(unbound)))
    return declarations + thunks, registrations
