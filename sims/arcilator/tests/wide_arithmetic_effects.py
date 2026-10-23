#!/usr/bin/env python3
"""Preserve defined poison overshifts and observable ordinary amount producers."""
from pathlib import Path
import argparse
import json
import os
import signal
import subprocess
import sys
import datetime

SIM = Path(__file__).resolve().parents[1]


def check(circt, work, cxx):
    main = SIM
    sdk = Path(circt).resolve().parent
    w = Path(work)
    w.mkdir(parents=True, exist_ok=True)
    out = w / 'report.json'
    report = {'status': 'running', 'runs': []}
    def save():
        out.write_text(json.dumps(report, indent=2) + '\n')
    def run(name, cmd, d):
        command = list(map(str, cmd))
        report['active_step'] = name
        save()
        with (d / (name + '.log')).open('w') as log:
            p = subprocess.Popen(['/usr/bin/prlimit', '--as=4294967296', '--core=0', '--', *command], stdout=log, stderr=subprocess.STDOUT, start_new_session=True, env=os.environ.copy())
            try:
                code = p.wait(timeout=180)
            except BaseException:
                if p.poll() is None:
                    os.killpg(p.pid, signal.SIGKILL)
                    p.wait()
                raise
        assert code == 0, (name, code)
    save()
    width = 1025
    text = r'''module {
 func.func private @ordinary_amount(i32)->i32
 func.func private @chipyard_arc_wide_shift_0_1025(i1025,i1025)->i1025 attributes {chipyard.no_output_readback,chipyard.native_pure_wide_shift}
 func.func private @chipyard_arc_wide_shift_1_1025(i1025,i1025)->i1025 attributes {chipyard.no_output_readback,chipyard.native_pure_wide_shift}
 hw.module @ArcModel(in %tick:i32,out left:i1025,out right:i1025,out effect:i1025) {
  %poison=ub.poison : i1025
  %big=hw.constant 1025:i1025
  %zero=hw.constant 0:i1025
  %high=hw.constant 0:i993
  %small=sim.func.dpi.call @ordinary_amount(%tick):(i32)->i32
  %amount=comb.concat %high,%small:i993,i32
  %left=sim.func.dpi.call @chipyard_arc_wide_shift_0_1025(%poison,%big):(i1025,i1025)->i1025
  %right=sim.func.dpi.call @chipyard_arc_wide_shift_1_1025(%poison,%big):(i1025,i1025)->i1025
  %effect=sim.func.dpi.call @chipyard_arc_wide_shift_0_1025(%zero,%amount):(i1025,i1025)->i1025
  hw.output %left,%right,%effect:i1025,i1025,i1025
 }
}'''
    source = w / 'input.mlir'
    source.write_text(text)
    for mode in ['early', 'late']:
        d = w / mode
        d.mkdir(exist_ok=True)
        run(mode + '-model', [sys.executable, main / 'compiler/model.py', '--input', source, '--output', d / 'model.ll', '--state-file', d / 'model.json', '--arcilator', sdk / 'bin/arcilator', '--circt-opt', sdk / 'bin/circt-opt', '--mlir-translate', sdk / 'bin/mlir-translate', '--cache', main / 'generated-src/compiler', '--cxx', cxx, '--max-ops', '0', '--late-wide-arithmetic', str(mode == 'late').lower()], d)
        run(mode + '-ABI', [sys.executable, main / 'compiler/llvm.py', d / 'model.ll'], d)
        model, = json.loads((d / 'model.json').read_text())
        ports = {s['name']: s for s in model['states']}
        initial = model.get('initialFnSym')
        header = '#include <cstdint>\n' + ''.join(('constexpr unsigned ' + n + 'Offset=' + str(ports[n]['offset']) + ';\n' for n in ['tick', 'left', 'right', 'effect'])) + (f'extern "C" void {initial}(void*);\ninline void initialize(void*p){{{initial}(p);}}\n' if initial else 'inline void initialize(void*){}\n')
        (d / 'layout.h').write_text(header)
        code = r'''#include "circt/Dialect/Arc/Runtime/ArcRuntime.h"
#include "layout.h"
#include <cstring>
#include <vector>
#include <stdexcept>
#include <cstdio>
extern "C" void ArcModel_eval(void*);
extern "C" ArcRuntimeModelInfo arcRuntimeModel_ArcModel;
static std::vector<uint32_t> calls;
extern "C" uint32_t ordinary_amount(uint32_t tick) {calls.push_back(tick);return tick%2 ? 1025 : 0;}
int main() {
 auto *state=arcRuntimeAllocateInstance(&arcRuntimeModel_ArcModel,nullptr);
 initialize(state->modelState);arcRuntimeOnInitialized(state);
 unsigned checks=0;
 for(uint32_t tick: {0u,0u,1u,1u,0u,2u,2u,3u}) {
  std::memcpy(state->modelState+tickOffset,&tick,4);
  arcRuntimeOnEval(state);ArcModel_eval(state->modelState);
  for(unsigned offset: {leftOffset,rightOffset,effectOffset})
   for(unsigned byte=0;byte<129;++byte) {
    unsigned value=state->modelState[offset+byte];if(byte==128)value&=1;
    if(value)throw std::runtime_error("defined poison overshift/zero-data result changed");
    ++checks;
   }
 }
 if(calls!=std::vector<uint32_t>({0,1,0,2,3}))throw std::runtime_error("ordinary DPI activation/call sequence changed");
 arcRuntimeDeleteInstance(state);std::printf("PASS: %u zero-result bytes,5 exact ordinary callbacks, stable input reuse\n",checks);
}
'''
        (d / 'test.cpp').write_text(code)
        for level in ['0', '3']:
            run(mode + '-opt' + level, [sdk / 'bin/opt', '-O' + level, d / 'model.ll', '-o', d / 'model.bc'], d)
            run(mode + '-llc' + level, [sdk / 'bin/llc', '-O' + level, '-relocation-model=pic', '-filetype=obj', d / 'model.bc', '-o', d / 'model.o'], d)
            run(mode + '-link' + level, [cxx, '-O' + level, '-std=c++17', '-I' + str(sdk / 'include'), '-I' + str(d), d / 'test.cpp', main / 'native/wide.cpp', main / 'native/evaluation.cpp', d / 'model.o', '-L' + str(sdk / 'lib'), '-lCIRCTArcRuntime', '-lLLVMSupport', '-lLLVMDemangle', '-lz', '-lpthread', '-ldl', '-o', d / 'test'], d)
            run(mode + '-run' + level, [d / 'test'], d)
            report['runs'].append({'mode': mode, 'level': level, 'defined_result_byte_checks': 3096, 'exact_ordinary_callbacks': 5})
            save()
            print(mode, level, 'poison/ordinary effects passed', flush=True)
    report.update(status='passed', completed_at=datetime.datetime.now(datetime.timezone.utc).isoformat())
    save()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path,
                        default=SIM / 'generated-src/wide-arithmetic-effects')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    check(args.circt_bin, args.work, args.cxx)


if __name__ == '__main__':
    main()
