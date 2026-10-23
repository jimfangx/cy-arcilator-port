#!/usr/bin/env python3
"""Execute original scope oracles and physical context-DPI calls without Verilator."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin
from native_hw import prepare_rtl


def digest(path):
    with path.open('rb') as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
        return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work', type=Path)
    args = parser.parse_args()
    sdk = args.circt_bin.resolve()
    (SIM / 'generated-src').mkdir(exist_ok=True)
    work = args.work or Path(
        tempfile.mkdtemp(prefix='scope-frames-', dir=SIM / 'generated-src'))
    work.mkdir(parents=True, exist_ok=True)
    fixtures = HERE / 'scope_frames'
    cxx = os.environ.get('CXX', 'c++')

    def run(*command):
        subprocess.run([str(arg) for arg in command], check=True)

    frontend = Path(
        subprocess.check_output([
            sys.executable, SIM / 'compiler/verilog/build.py',
            '--circt-verilog', sdk / 'circt-verilog', '--cache',
            SIM / 'generated-src/compiler/verilog', '--jobs=1'
        ],
                                text=True).strip())
    libraries = {
        name:
        plugin(sdk / 'circt-opt', cxx, SIM / 'generated-src/compiler',
               SIM / 'compiler' / name)
        for name in ('RTL.cpp', 'NativeContexts.cpp', 'HierarchyFormats.cpp')
    }
    verify = work / 'verify.cpp'
    verify.write_text('''#include "native/runtime.h"
#include <string>
int main(int argc,char **argv) {
 auto m=chipyard_arc::create_model(argc,argv);
 m->signal("arc_printf_enable").set(argc>1&&std::string(argv[1])=="+quiet"?0:1);
 m->initialize();m->advance(0,[]{});
}
''')
    sources = {
        p: digest(p)
        for p in (SIM / 'native').rglob('*')
        if p.suffix in ('.cpp', '.h', '.def')
    }
    sources.update({p: digest(p) for p in fixtures.iterdir() if p.is_file()})
    cases = []
    report = dict(
        status='running',
        cases=cases,
        frontend=str(frontend),
        scope='Original independent SV print values/paths/counts at O0/O2; '
        'concurrent process order unrestricted. Actual context-DPI scope handles, '
        'names, callback counts and returned values checked separately. No Verilator.'
    )

    def save():
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')

    references = {
        r['case']: r
        for r in json.loads((fixtures / 'references.json').read_text())
    }
    tops = dict(package='PackageTop',
                unit='UnitTop',
                physical='PhysicalTop',
                sibling='SiblingTop',
                relative='RelativeTop',
                nested='NestedTop',
                upward='UpTop',
                bare_calls='BareTop',
                context='FrameTop',
                task_abi='TaskAbiTop')
    common = {}
    save()
    for name, top in tops.items():
        build = work / name
        build.mkdir()
        source = fixtures / (name + '.sv')
        if name in references:
            assert digest(source) == references[name]['source_sha256']
        raw, resolved = build / 'source.mlir', build / 'resolved.mlir'
        run(frontend, '--single-unit', '--infer-clock-ports=false',
            '--ir-llhd', '--timescale=1ns/1ps', '--top=' + top, source, '-o',
            raw)
        pipeline = (
            'builtin.module(chipyard-restore-dpi-abi,'
            'chipyard-native-dpi-tasks,chipyard-inline-instant-tasks,chipyard-native-contexts{root='
            + top + '},inline,chipyard-capture-hierarchy-formats{root=' + top +
            '},hw-flatten-modules,chipyard-resolve-native-contexts{root=' +
            top + '},chipyard-resolve-hierarchy-formats{root=' + top +
            '},symbol-dce)')
        run(sdk / 'circt-opt', raw,
            *['--load-pass-plugin=' + str(lib) for lib in libraries.values()],
            '--pass-pipeline=' + pipeline, '-o', resolved)
        if name not in ('context', 'task_abi'):
            expected = (fixtures /
                        (name + '.expected')).read_text().splitlines()
            text = resolved.read_text()
            assert 'sim.fmt.hier_path' not in text
            assert text.count('sim.proc.print') == len(expected)
            for path in set(re.findall(r'\[([^]]+)\]', '\n'.join(expected))):
                assert 'sim.fmt.literal "' + path + '"' in text, (name, path)
        prepare_rtl(resolved, build / 'design.mlir', sdk / 'circt-opt',
                    libraries['RTL.cpp'])
        run(sys.executable, SIM / 'partition.py', '--input',
            build / 'design.mlir', '--output', build, '--top', top,
            '--circt-opt', sdk / 'circt-opt', '--cxx', cxx,
            '--native-sparse-memories')
        run(sys.executable, SIM / 'compiler/partition.py', '--input',
            build / 'partitioned.mlir', '--output', build / 'outlined.mlirbc',
            '--circt-opt', sdk / 'circt-opt', '--cache',
            SIM / 'generated-src/compiler', '--cxx', cxx, '--max-ops=8',
            '--emit-bytecode', '--native-feedback')
        run(sdk / 'circt-opt', build / 'outlined.mlirbc', '--symbol-dce',
            '--canonicalize', '-o', build / 'model.mlir')
        run(sys.executable, SIM / 'compiler/model.py', '--input',
            build / 'model.mlir', '--output', build / 'model.ll',
            '--state-file', build / 'model.json', '--arcilator',
            sdk / 'arcilator', '--circt-opt', sdk / 'circt-opt',
            '--mlir-translate', sdk / 'mlir-translate', '--cache',
            SIM / 'generated-src/compiler', '--cxx', cxx)
        run(sys.executable, SIM / 'compiler/llvm.py', build / 'model.ll',
            '--llvm-opt', sdk / 'opt', '--cache',
            SIM / 'generated-src/compiler', '--cxx', cxx)
        run(sys.executable, SIM / 'native.py', build / 'model.json',
            build / 'partition.json', build / 'model.cpp')
        for level in ('0', '2'):
            assert all(digest(p) == sha for p, sha in sources.items())
            run(sdk / 'opt', '-O' + level, build / 'model.ll', '-o',
                build / ('model' + level + '.bc'))
            model = build / ('model' + level + '.o')
            run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
                '-filetype=obj', build / ('model' + level + '.bc'), '-o',
                model)
            executable, objects = build / ('test-O' + level), build / (
                'objects-O' + level)
            flags = [
                '-O' + level, '-std=c++17', '-DARC_NATIVE_NO_MAIN',
                '-I' + str(SIM), '-I' + str(SIM / 'native/include'),
                '-I' + str(sdk.parent / 'include')
            ]
            links = [
                '-L' + str(sdk.parent / 'lib'), '-lCIRCTArcRuntime',
                '-lLLVMSupport', '-lLLVMDemangle', '-lz', '-lpthread', '-ldl'
            ]
            if name in ('context', 'task_abi'):
                links += ['-Wl,--wrap=chipyard_arc_stop_fail']
            driver = fixtures / (name + '.cpp') if name in (
                'context', 'task_abi') else verify
            if level not in common:
                filelist = build / 'blackboxes.f'
                # This object pool serves later hand-lowered context/task
                # cases too. Include their completion API through the same
                # foreign-source filelist accepted by the production builder.
                filelist.write_text(str(driver) + '\n' +
                                    str(SIM / 'native/control.cpp') + '\n')
                run(sys.executable, SIM / 'native_build.py',
                    '--filelist=' + str(filelist),
                    '--main=' + str(build / 'model.cpp'),
                    '--model=' + str(model), '--output=' + str(executable),
                    '--build=' + str(objects), '--cxx=' + cxx,
                    '--cxxflags=' + shlex.join(flags),
                    '--ldflags=' + shlex.join(links), '--jobs=1')
                # The original builder's first object is the generated main;
                # object17 is this test's verifier. All other runtime objects
                # have identical flags and guarded source bytes across cases.
                common[level] = sorted(p for p in objects.glob('*.o')
                                       if not p.name.startswith(('0-', '17-')))
                assert len(common[level]) == 18
                assert sum(p.name.endswith('-control.o')
                           for p in common[level]) == 1
            else:
                objects.mkdir()
                main_object, verifier = objects / 'model-main.o', objects / 'verify.o'
                run(*shlex.split(cxx), *flags, '-c', build / 'model.cpp', '-o',
                    main_object)
                run(*shlex.split(cxx), *flags, '-c', driver, '-o', verifier)
                run(*shlex.split(cxx), main_object, verifier, *common[level],
                    model, *links, '-o', executable)
            result = subprocess.run([executable],
                                    capture_output=True,
                                    text=True,
                                    check=True)
            (build / ('run-O' + level + '.log')).write_text(result.stdout +
                                                            result.stderr)
            if name == 'context':
                assert 'PASS: 18 direct/relative/root/nested/task context-DPI callbacks' in result.stdout
            elif name == 'task_abi':
                assert 'PASS: imported task C int status' in result.stdout
                assert result.stdout.count(
                    'ERROR: native DPI task returned a disable status') == 1
            else:
                assert sorted(
                    result.stdout.splitlines()) == sorted(expected), (
                        name, result.stdout)
                quiet = subprocess.run([executable, '+quiet'],
                                       capture_output=True,
                                       text=True,
                                       check=True)
                assert not quiet.stdout, quiet.stdout
            assert 'Verilated::' not in subprocess.check_output(
                ['nm', '-C', executable], text=True)
            cases.append(
                dict(case=name,
                     level=level,
                     status='passed',
                     source_sha256=digest(source),
                     simulator_sha256=digest(executable),
                     records=len(result.stdout.splitlines()),
                     verilated_symbols_absent=True))
            save()
    report.update(status='passed',
                  sources_unchanged=all(
                      digest(p) == sha for p, sha in sources.items()))
    save()
    print(
        'PASS: eight unchanged scope oracles, quiet printing and18 physical context-DPI '
        'callbacks, task ABI/invalid-status guards atO0/O2; ' +
        str(work / 'report.json'),
        flush=True)


if __name__ == '__main__':
    main()
