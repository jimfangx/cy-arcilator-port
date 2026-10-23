#!/usr/bin/env python3
"""Generate the DPI adapter and two-phase state commits from compiler layouts."""
import argparse
import json
from pathlib import Path


def generate(layout_path, partition_path, output):
    models = json.loads(Path(layout_path).read_text())
    model, = [m for m in models if m['name'] == 'ArcModel']
    # Observing ports also creates wire taps with the same names. Only the
    # actual IO entries describe the externally writable/readable storage.
    states = {s['name']: s for s in model['states'] if s['type'] in ('input', 'output')}
    part = json.loads(Path(partition_path).read_text())
    clocks = part['phase_clocks']
    foreign = [p for inst in part['instances'] for p in inst['ports']]
    inputs = [dict(p, wire=p['name']) for p in part['ports'] if p['direction'] == 'in'] + [p for p in foreign if p['direction'] == 'out']
    outputs = [dict(p, wire=p['name']) for p in part['ports'] if p['direction'] == 'out'] + [p for p in foreign if p['direction'] == 'in']
    outputs += [dict(wire=c['observed'], bits=1) for c in clocks]
    ports = inputs + outputs
    for p in ports:
        if states[p['wire']]['numBits'] != p['bits']:
            raise ValueError(f'width mismatch for {p["wire"]}')
    lines = ['// Generated from the compiler state layout; do not edit.',
             '#include <cstring>', '#include <vector>', '#include "native/evaluation.h"', '#include \"trace.h\"', '#include <svdpi.h>', '#include <verilated.h>',
             '#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"',
             'extern "C" {', 'extern ArcRuntimeModelInfo arcRuntimeModel_ArcModel;',
             'void ArcModel_eval(void *);']
    initial, final = model.get('initialFnSym'), model.get('finalFnSym')
    time_setter = model.get('timeSetFnSym')
    for sym in (initial, final):
        if sym:
            lines.append(f'void {sym}(void *);')
    if time_setter:
        lines.append(f'void {time_setter}(void *, uint64_t);')
    lines += ['}', 'static const ArcTraceSignal traceSignals[] = {']
    counts = {}
    for signal in model['states']:
        counts[signal['name']] = counts.get(signal['name'], 0) + 1
    for signal in model['states']:
        if signal.get('type') != 'memory':
            trace_name = signal['name'] + ('__' + signal['type'] if counts[signal['name']] > 1 else '')
            lines.append('  {' + json.dumps(trace_name) + f', {signal["offset"]}, {signal["numBits"]}' + '},')
    lines += ['};',
              'static uint64_t chipyard_arc_time_fs() {',
              '  uint64_t value = Verilated::threadContextp()->time();',
              '  int exponent = Verilated::threadContextp()->timeprecision() + 15;',
              '  if (exponent < 0) VL_FATAL_MT(__FILE__, __LINE__, "Arcilator", "Time precision is finer than femtoseconds");',
              '  for (; exponent > 0; --exponent) {',
              '    if (value > UINT64_MAX / 10) VL_FATAL_MT(__FILE__, __LINE__, "Arcilator", "Simulation time exceeds the Arc context range");',
              '    value *= 10;', '  }', '  return value;', '}',
              'struct ChipyardArcState {',
              '  ArcTrace trace{arcRuntimeModel_ArcModel.numStateBytes};',
              '  ArcState *current, *staged;', '  std::vector<uint8_t> previous = std::vector<uint8_t>(arcRuntimeModel_ArcModel.numStateBytes);', '  uint32_t epoch = 0;',
              '  bool pending = false;', '};', 'extern "C" {',
              'void chipyard_arc_assert_fail() { VL_FATAL_MT(__FILE__, __LINE__, "Arcilator", "RTL assertion failed"); }',
              'void chipyard_arc_finish() { VL_FINISH_MT(__FILE__, __LINE__, "Arcilator"); }',
              'void chipyard_arc_stop_fail() { VL_FATAL_MT(__FILE__, __LINE__, "Arcilator", "RTL stopped with failure"); }',
              'uint32_t chipyard_arc_guard(uint32_t condition) { return condition; }',
              'void *chipyard_arc_create() {', '  auto *s = new ChipyardArcState;',
              '  s->current = arcRuntimeAllocateInstance(&arcRuntimeModel_ArcModel, nullptr);',
              '  s->staged = arcRuntimeAllocateInstance(&arcRuntimeModel_ArcModel, nullptr);']
    if initial:
        lines.append(f'  {initial}(s->current->modelState);')
    lines += ['  arcRuntimeOnInitialized(s->current);',
              '  arcRuntimeOnInitialized(s->staged);', '  return s;', '}',
              'void chipyard_arc_destroy(void *ptr) {',
              '  auto *s = static_cast<ChipyardArcState *>(ptr);']
    if final:
        lines.append(f'  {final}(s->current->modelState);')
    lines += ['  arcRuntimeDeleteInstance(s->staged);',
              '  arcRuntimeDeleteInstance(s->current);', '  delete s;', '}']
    args = ['void *ptr', 'uint32_t epoch', 'unsigned char printf_enable', 'unsigned char stop_enable'] + [f'{"const " if i < len(inputs) else ""}svBitVecVal *p{i}' for i in range(len(ports))]
    lines += ['void chipyard_arc_eval(' + ', '.join(args) + ') {',
              '  auto *s = static_cast<ChipyardArcState *>(ptr);', '  if (!s) return;',
              '  const auto size = arcRuntimeModel_ArcModel.numStateBytes;',
              '  if (epoch != s->epoch) {',
              '    if (s->pending) {', '      chipyard_arc::SamplingPhase sampling;',
              *([f'      {time_setter}(s->staged->modelState, chipyard_arc_time_fs());'] if time_setter else []),
              '      arcRuntimeOnEval(s->staged);',
              *([f'      s->staged->modelState[{states[model["samplingInput"]]["offset"]}] = 1;'] if model.get('samplingInput') else []),
              '      ArcModel_eval(s->staged->modelState);',
              '      std::memcpy(s->current->modelState, s->staged->modelState, size);',
              *([f'      s->current->modelState[{states[model["samplingInput"]]["offset"]}] = 0;'] if model.get('samplingInput') else []),
              '    }',
              '    s->pending = false;', '    s->epoch = epoch;', '  }',
              '  auto *state = s->current->modelState;']
    lines += [f'  state[{states["arc_printf_enable"]["offset"]}] = printf_enable;',
              f'  state[{states["arc_stop_enable"]["offset"]}] = stop_enable;']
    for i, p in enumerate(inputs):
        lines.append(f'  std::memcpy(state + {states[p["wire"]]["offset"]}, p{i}, {(p["bits"]+7)//8});')
    if time_setter:
        lines.append(f'  {time_setter}(state, chipyard_arc_time_fs());')
    if model.get('samplingInput'):
        lines.append(f'  state[{states[model["samplingInput"]]["offset"]}] = 0;')
    lines += ['  bool stable = false;',
              '  for (unsigned delta = 0; delta < 1000; ++delta) {',
              '    std::memcpy(s->previous.data(), state, size);',
              '    arcRuntimeOnEval(s->current);', '    ArcModel_eval(state);',
              '    if (!std::memcmp(s->previous.data(), state, size)) { stable = true; break; }',
              '  }',
              '  if (!stable) VL_FATAL_MT(__FILE__, __LINE__, "Arcilator", "Native combinational logic did not converge");',
              '  s->trace.sample(state, traceSignals, sizeof(traceSignals)/sizeof(traceSignals[0]));']
    for i, p in enumerate(outputs, len(inputs)):
        lines += [f'  std::memset(p{i}, 0, {((p["bits"]+31)//32)*4});',
                  f'  std::memcpy(p{i}, state + {states[p["wire"]]["offset"]}, {(p["bits"]+7)//8});']
    changed = ' || '.join(f'(state[{states[c["held"]]["offset"]}] != state[{states[c["observed"]]["offset"]}])' for c in clocks) or 'false'
    lines += [f'  if ({changed}) {{',
              '    std::memcpy(s->staged->modelState, state, size);']
    for c in clocks:
        lines.append(f'    s->staged->modelState[{states[c["held"]]["offset"]}] = state[{states[c["observed"]]["offset"]}];')
    lines += ['    s->pending = true;', '  }', '}', '}']
    Path(output).write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('layout')
    parser.add_argument('partition')
    parser.add_argument('output')
    args = parser.parse_args()
    generate(args.layout, args.partition, args.output)
