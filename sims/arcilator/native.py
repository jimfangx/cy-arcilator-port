#!/usr/bin/env python3
"""Generate native Arcilator model/foreign-binding connections from compiler IO."""
import argparse
import json
import re
import shlex
from pathlib import Path
from partition import unquote
from native_exports import generate_exports


def clock_parameters(module, filelist):
    """Read the contract of Chisel's inline plusarg clock, including its default."""
    if not filelist.is_file():
        raise ValueError(
            f'{module} requires its original HDL filelist: {filelist}')
    files = [
        Path(p) for p in shlex.split(filelist.read_text(), comments=True)
        if Path(p).suffix in ('.v', '.sv')
    ]
    files.sort(key=lambda p: p.stem != module)
    for path in files:
        if not path.is_file():
            continue
        body = re.search(
            r'\bmodule\s+' + re.escape(module) + r'\b(.*?)\bendmodule\b',
            path.read_text(), re.S)
        if not body:
            continue
        fmt = re.search(
            r'\$value\$plusargs\s*\(\s*"([^"]+)"\s*,\s*FREQ_MHZ\s*\)', body[1])
        default = re.search(r'\bFREQ_MHZ\s*=\s*([0-9.eE+-]+)\s*;', body[1])
        period = re.search(
            r'\bPERIOD_PS\s*=\s*1000000(?:\.0*)?\s*/\s*FREQ_MHZ', body[1])
        if not fmt or not default or not period or not re.fullmatch(
                r'[^%]+=%[df]', fmt[1]):
            raise ValueError(
                f'unrecognized plusarg clock implementation in {path}')
        return [('FORMAT', fmt[1]), ('DEFAULT_FREQUENCY_MHZ', default[1])]
    raise ValueError(
        f'cannot locate original clock implementation for {module}')


def dtm_binding(filelist):
    """Select the registered Rocket wrapper or the older delayed Sodor wrapper."""
    if not filelist.is_file():
        raise ValueError(
            f'SimDTM requires its original HDL filelist: {filelist}')
    bodies = []
    for token in shlex.split(filelist.read_text(), comments=True):
        path = Path(token)
        if path.suffix not in ('.v', '.sv') or not path.is_file():
            continue
        body = re.search(r'\bmodule\s+SimDTM\b(.*?)\bendmodule\b',
                         path.read_text(), re.S)
        if body:
            bodies.append((path, body[1]))
    if len(bodies) != 1:
        raise ValueError(
            f'expected one selected SimDTM implementation, found {len(bodies)}'
        )
    path, body = bodies[0]
    rising = re.search(r'always\s*@\s*\(\s*posedge\s+clk\s*\)', body)
    falling = re.search(r'always\s*@\s*\(\s*negedge\s+clk\s*\)', body)
    registered = re.search(r'debug_req_valid_reg\s*<=\s*__debug_req_valid',
                           body)
    if rising and falling and registered:
        return 'SimDTM'
    input_delays = re.findall(r'\bwire[^\n]*#\s*0\.1\b', body)
    output_delays = re.findall(r'\bassign\s*#\s*0\.1\b', body)
    if rising and not falling and len(input_delays) == 4 and len(
            output_delays) == 6:
        return 'ArcLegacySimDTM'
    raise ValueError(f'unrecognized SimDTM scheduling contract in {path}')


def generate(layout_path, partition_path, output, filelist=None):
    model, = [
        m for m in json.loads(Path(layout_path).read_text())
        if m['name'] == 'ArcModel'
    ]
    part = json.loads(Path(partition_path).read_text())
    metadata_path = Path(partition_path).parent / 'rtl.json'
    metadata = json.loads(
        metadata_path.read_text()) if metadata_path.is_file() else []
    export_code, export_registration = generate_exports(metadata, part)
    analog_names = {n['wire'] for n in part.get('analog_nets', [])}
    states = {
        s['name']: s
        for s in model['states'] if s['type'] in ('input', 'output')
    }
    lines = [
        '// Generated native model bindings; no Verilator runtime or headers.',
        '#include "native/runtime.h"', '#include <cstdio>', 'extern "C" {',
        'extern ArcRuntimeModelInfo arcRuntimeModel_ArcModel;',
        'void ArcModel_eval(void *);'
    ]
    initial, final = model.get('initialFnSym'), model.get('finalFnSym')
    time_setter, wakeup_getter = model.get('timeSetFnSym'), model.get(
        'nextWakeupFnSym')
    for name in (initial, final):
        if name:
            lines.append(f'void {name}(void *);')
    if time_setter:
        lines.append(f'void {time_setter}(void *, uint64_t);')
    if wakeup_getter:
        lines.append(f'uint64_t {wakeup_getter}(void *);')
    lines += ['}'] + export_code
    lines += [
        'struct SignalSpec { const char *name; unsigned offset, bits; bool output; };',
        'struct PortSpec { const char *name, *wire; };',
        'struct ParameterSpec { const char *name, *value; };',
        'struct InstanceSpec { const char *module, *name; unsigned port_start, port_count, parameter_start, parameter_count; };',
        'struct ClockSpec { const char *held, *observed; };',
        'static const SignalSpec signal_specs[] = {'
    ]
    for name, state in states.items():
        lines.append(
            '  {' +
            f'{json.dumps(name)}, {state["offset"]}, {state["numBits"]}, ' +
            ('true' if state['type'] == 'output' else 'false') + '},')
    observations = {
        s['name']: s
        for s in model['states'] if s['type'] != 'memory'
    }
    observations.update(states)
    lines += ['};', 'static const SignalSpec trace_specs[] = {']
    for name, state in observations.items():
        lines.append(
            '  {' +
            f'{json.dumps(name)}, {state["offset"]}, {state["numBits"]}, false'
            + '},')
    lines += [
        '};', 'struct AnalogSpec { const char *name; unsigned bits; };',
        'static const AnalogSpec analog_specs[] = {'
    ]
    lines += [
        '  {' + json.dumps(n['wire']) + ', ' + str(n['bits']) + '},'
        for n in part.get('analog_nets', [])
    ] or ['  {nullptr, 0},']
    lines += ['};', 'static const ClockSpec clock_specs[] = {']
    for clock in part['phase_clocks']:
        lines.append('  {' + json.dumps(clock['held']) + ', ' +
                     json.dumps(clock['observed']) + '},')
    if not part['phase_clocks']:
        lines.append('  {nullptr, nullptr},')
    lines += ['};', 'static const PortSpec port_specs[] = {']
    port_specs, parameter_specs, instance_specs = [], [], []
    instances = list(part['instances'])
    for index, wire in enumerate(model.get('feedbackWires', [])):
        for direction, name in (('input', wire['input']), ('output',
                                                           wire['output'])):
            state = states.get(name)
            if not state or state['type'] != direction or state[
                    'numBits'] != wire['bits']:
                raise ValueError('invalid native feedback layout: ' + name)
        instances.append(
            dict(module='ArcFeedbackWire',
                 name='feedback_' + str(index),
                 ports=[
                     dict(name='q', wire=wire['input']),
                     dict(name='value', wire=wire['output'])
                 ],
                 parameters=[]))
    for inst in instances:
        start, param_start = len(port_specs), len(parameter_specs)
        module = inst['module']
        extra_parameters = []
        if module == 'SimDTM':
            module = dtm_binding(
                Path(filelist) if filelist else Path(partition_path).parent /
                'blackboxes.f')
        if module.startswith('ClockSourceAtFreqFromPlusArg'):
            extra_parameters = clock_parameters(
                module,
                Path(filelist) if filelist else Path(partition_path).parent /
                'blackboxes.f')
            module = 'ClockSourceAtFreqFromPlusArg'
        for port in inst['ports']:
            if port['wire'] not in states and port['wire'] not in analog_names:
                raise ValueError('native analog binding is not implemented: ' +
                                 inst['module'])
            port_specs.append('  {' + json.dumps(port['name']) + ', ' +
                              json.dumps(port['wire']) + '},')
        for param in inst['parameters']:
            match = re.fullmatch(r'([^:]+):\s*([^=]+)=\s*(.*)', param)
            if not match:
                raise ValueError('unrecognized native binding parameter: ' +
                                 param)
            name, ty, value = (s.strip() for s in match.groups())
            if value.startswith('"'):
                value = unquote(value)
            elif value in ('true', 'false'):
                value = str(int(value == 'true'))
            parameter_specs.append('  {' + json.dumps(name) + ', ' +
                                   json.dumps(value) + '},')
        for name, value in extra_parameters:
            parameter_specs.append('  {' + json.dumps(name) + ', ' +
                                   json.dumps(value) + '},')
        instance_specs.append(
            '  {' + json.dumps(module) + ', ' + json.dumps(inst['name']) +
            f', {start}, {len(port_specs)-start}, {param_start}, {len(parameter_specs)-param_start}'
            + '},')
    lines += port_specs or ['  {nullptr, nullptr},']
    lines += ['};', 'static const ParameterSpec parameter_specs[] = {']
    lines += parameter_specs or ['  {nullptr, nullptr},']
    lines += ['};', 'static const InstanceSpec instance_specs[] = {']
    lines += instance_specs or ['  {nullptr, nullptr, 0, 0, 0, 0},']
    lines += [
        '};',
        'std::unique_ptr<chipyard_arc::Runtime> chipyard_arc::create_model(int argc, char **argv) {',
        '  register_builtin_bindings();',
        '  auto owner = std::make_unique<Runtime>(argc, argv, arcRuntimeModel_ArcModel, ArcModel_eval, '
        + (initial or 'nullptr') + ', ' + (final or 'nullptr') + ', ' +
        (time_setter or 'nullptr') + ', ' + (wakeup_getter or 'nullptr') +
        ', ' + ('true' if model.get('hasTimeReaders') or any(i['module'] == 'ArcDelay' for i in instances) else 'false') + ', ' +
        json.dumps(part['top']) + ');', '  auto &runtime = *owner;',
        '  runtime.set_root_clock_used(' +
        ('true' if part.get('root_clock_used', True) else 'false') + ');',
        '  for (auto &s : signal_specs) runtime.add_signal(s.name, s.offset, s.bits, s.output);',
        *([
            '  runtime.add_sampling_signal(' +
            json.dumps(model['samplingInput']) + ');'
        ] if model.get('samplingInput') else []),
        '  for (auto &s : trace_specs) runtime.add_trace_signal(s.name, s.offset, s.bits);',
        f'  for (unsigned i = 0; i < {len(analog_names)}; ++i) runtime.add_analog_signal(analog_specs[i].name, analog_specs[i].bits);',
        f'  for (unsigned i = 0; i < {len(part["phase_clocks"])}; ++i) runtime.add_clock(clock_specs[i].held, clock_specs[i].observed);',
        f'  for (unsigned index = 0; index < {len(instance_specs)}; ++index) {{',
        '    auto &inst = instance_specs[index]; auto binding = make_binding(runtime, inst.module);',
        '    binding->instance_name = inst.name;',
        '    for (unsigned i = 0; i < inst.port_count; ++i) {',
        '      auto &p = port_specs[inst.port_start + i]; binding->ports.emplace(p.name, runtime.signal(p.wire));',
        '    }', '    for (unsigned i = 0; i < inst.parameter_count; ++i) {',
        '      auto &p = parameter_specs[inst.parameter_start + i]; binding->parameters.emplace(p.name, p.value);',
        '    }', '    runtime.add_binding(std::move(binding));', '  }',
        *export_registration, '  return owner;', '}',
        '#ifndef ARC_NATIVE_NO_MAIN', 'int main(int argc, char **argv) {',
        '  try {',
        '    auto runtime = chipyard_arc::create_model(argc, argv); return runtime->run();',
        '  } catch (const std::exception &e) { std::fprintf(stderr, "Native Arcilator: %s\\n", e.what()); return 1; }',
        '}', '#endif'
    ]
    Path(output).write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layout')
    parser.add_argument('partition')
    parser.add_argument('output')
    parser.add_argument('--filelist', type=Path)
    args = parser.parse_args()
    generate(args.layout, args.partition, args.output, args.filelist)
