"""Lift large canonical seq.firmem arrays into native sparse memory bindings."""
import re


def lower_memories(body, externs, minimum_bits):
    from partition import SSA, opcode
    lines = list(body.splitlines())
    memories, ports = {}, {}
    declaration = re.compile(
        r'\s*(' + SSA +
        r') = seq.firmem(?: sym .*?)?(?: name "(?:[^"\\]|\\.)*")? '
        r'(\d+), (\d+), (undefined|old|new), (undefined|port_order)'
        r'(.*?) : <(\d+) x (\d+)(?:, mask (\d+))?>')
    access = re.compile(
        r'\s*(?:(' + SSA +
        r') = )?seq.firmem\.(read_port|write_port|read_write_port) (' + SSA +
        r')\[(' + SSA + r')\](?: = (' + SSA + r'))?(?: if (' + SSA +
        r'))?, clock (' + SSA + r')(?: enable (' + SSA + r'))?(?: mask (' +
        SSA + r'))? : <[^>]+>(?:, i\d+)?')
    for i, line in enumerate(lines):
        if opcode(line) == 'seq.firmem':
            match = declaration.fullmatch(line)
            if not match:
                continue
            name, read, write, ruw, wuw, attrs, depth, bits, masks = match.groups(
            )
            depth, bits, masks = int(depth), int(bits), int(masks or 1)
            if depth * bits < minimum_bits or int(read) not in (
                    0, 1) or int(write) != 1:
                continue
            # Initialization and externally referenced memory symbols retain
            # their original compiler representation until explicitly supported.
            if attrs.strip(
            ) or ' sym ' in line or not depth or not bits or not masks or bits % masks:
                continue
            memories[name] = dict(line=i,
                                  read=int(read),
                                  ruw=ruw,
                                  depth=depth,
                                  bits=bits,
                                  masks=masks)
        elif opcode(line).startswith('seq.firmem.'):
            match = access.fullmatch(line)
            if match:
                ports[i] = match.groups()
    selected = {}
    for name, memory in memories.items():
        uses = []
        eligible = True
        for i, line in enumerate(lines):
            if name not in line:
                continue
            code = line
            # String literals are opaque; only SSA operands refer to memory.
            code = re.sub(r'"(?:[^"\\]|\\.)*"', '', code)
            if name not in re.findall(SSA, code) or i == memory['line']:
                continue
            if i not in ports or ports[i][2] != name:
                eligible = False
                break
            uses.append(i)
        if eligible and uses:
            selected[name] = (memory, uses)
    omitted, additions = set(), []
    used_names = set(re.findall(SSA, "\n".join(lines)))
    for index, (name, (memory, uses)) in enumerate(selected.items()):
        prefix, module = f'%arc_memory_{index}', f'ArcSparseMemory_{index}'
        while module in externs or any(
                value.startswith(prefix + '_') for value in used_names):
            index += 1
            prefix, module = f'%arc_memory_{index}', f'ArcSparseMemory_{index}'
        externs[module] = 'ArcSparseMemory'
        omitted.add(memory['line'])
        inputs, outputs, results = [], [], []
        reads = writes = 0
        bits, masks = memory['bits'], memory['masks']
        address_bits = max(1, (memory['depth'] - 1).bit_length())
        additions += [
            f'    {prefix}_true = hw.constant true',
            f'    {prefix}_mask = hw.constant -1 : i{masks}'
        ]
        for i in uses:
            omitted.add(i)
            result, kind, _, address, data, mode, clock, enable, mask = ports[
                i]
            enable = enable or prefix + '_true'
            read_enable, write_enable = enable, enable
            if kind == 'read_write_port':
                additions += [
                    f'    {prefix}_not_{i} = comb.xor {mode}, {prefix}_true : i1',
                    f'    {prefix}_read_{i} = comb.and {enable}, {prefix}_not_{i} : i1',
                    f'    {prefix}_write_{i} = comb.and {enable}, {mode} : i1'
                ]
                read_enable, write_enable = f'{prefix}_read_{i}', f'{prefix}_write_{i}'
            if kind != 'write_port':
                port = f'r{reads}'
                inputs += [
                    f'{port}_clock: {clock}: !seq.clock',
                    f'{port}_address: {address}: i{address_bits}',
                    f'{port}_enable: {read_enable}: i1'
                ]
                outputs.append(f'{port}_data: i{bits}')
                results.append(result)
                reads += 1
            if kind != 'read_port':
                port = f'w{writes}'
                inputs += [
                    f'{port}_clock: {clock}: !seq.clock',
                    f'{port}_address: {address}: i{address_bits}',
                    f'{port}_enable: {write_enable}: i1',
                    f'{port}_data: {data}: i{bits}',
                    f'{port}_mask: {mask or prefix + "_mask"}: i{masks}'
                ]
                writes += 1
        parameters = f'DEPTH: ui64 = {memory["depth"]}, BITS: ui32 = {bits}, MASKS: ui32 = {masks}, READS: ui32 = {reads}, WRITES: ui32 = {writes}, READ_LATENCY: ui32 = {memory["read"]}, READ_NEW: ui32 = {int(memory["ruw"] == "new")}'
        result_text = ', '.join(results) + ' = ' if results else ''
        additions.append(
            f'    {result_text}hw.instance "arc_memory_{index}" @{module}<{parameters}>('
            + ', '.join(inputs) + ') -> (' + ', '.join(outputs) + ')')
    if selected:
        print(
            f'Bound {len(selected)} large FIRRTL memories to native sparse storage',
            flush=True)
    return '\n'.join(line for i, line in enumerate(lines)
                     if i not in omitted) + '\n' + '\n'.join(additions)
