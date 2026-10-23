#!/usr/bin/env python3
"""Verify output seeds override arbitrary stack contents, with a negative control."""
import argparse
import json
import os
from pathlib import Path
import resource
import shlex
import signal
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SIM = HERE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--work',
                        type=Path,
                        default=SIM / 'generated-src/seeded-dpi-poison-test')
    args = parser.parse_args()
    sdk, work = args.circt_bin.resolve(), args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    def run(*command):
        subprocess.run([str(arg) for arg in command], check=True)

    # This test-only SDK shim fills integer allocations immediately after they
    # are created. SDK copy-in stores follow it. No uninitialized C/LLVM reads
    # are needed in either the positive test or the missing-seed negative test.
    wrapper = work / 'poison-arcilator'
    wrapper.write_text('''#!/usr/bin/env python3
import json,re,subprocess,sys
from pathlib import Path
sdk = Path(''' + repr(str(sdk)) + ''')
subprocess.run([str(sdk/'arcilator'),*sys.argv[1:]],check=True)
output=Path(sys.argv[sys.argv.index('-o')+1])
text=subprocess.check_output([sdk/'circt-opt',output],text=True)
pattern=re.compile(r'^([ \\t]*)(%[-a-zA-Z$._0-9]+) = llvm\\.alloca .* x (i[0-9]+) :.*$')
lines=[];count=0
for line in text.splitlines():
 lines.append(line)
 match=pattern.match(line)
 if not match:continue
 indent,pointer,ty=match.groups();name=f'%chipyard_output_poison_{count}';count+=1
 lines.extend([f'{indent}{name} = llvm.mlir.constant(-1 : {ty}) : {ty}',
               f'{indent}llvm.store {name}, {pointer} : {ty}, !llvm.ptr'])
assert count>=4,count
subprocess.run([sdk/'circt-opt','-','--emit-bytecode','-o',output],
               input='\\n'.join(lines)+'\\n',text=True,check=True)
(output.parent/'poison-count.json').write_text(json.dumps({'allocations':count})+'\\n')
''')
    wrapper.chmod(0o755)
    negative = work / 'compiler'
    negative.mkdir(exist_ok=True)
    for name in ('Model.cpp', 'LowerArrays.cpp', 'StateProperties.cpp',
                 'partition.py'):
        link = negative / name
        if not link.exists():
            link.symlink_to(SIM / 'compiler' / name)
    original = (SIM / 'compiler/model.py').read_text()
    assert original.count(',chipyard-seed-dpi-outputs') == 1
    without_seeds = negative / 'model_without_seeds.py'
    without_seeds.write_text(original.replace(',chipyard-seed-dpi-outputs',
                                              ''))
    libs = shlex.split(
        subprocess.check_output([sdk / 'llvm-config', '--libs', 'support'],
                                text=True))
    for name, compiler in [('positive', SIM / 'compiler/model.py'),
                           ('negative', without_seeds)]:
        build = work / name
        build.mkdir(exist_ok=True)
        run(sys.executable, compiler, '--input', HERE / 'seeded_dpi.mlir',
            '--output', build / 'model.ll', '--state-file',
            build / 'model.json', '--arcilator', wrapper, '--circt-opt',
            sdk / 'circt-opt', '--mlir-translate', sdk / 'mlir-translate',
            '--cache', SIM / 'generated-src/compiler', '--max-ops', '2')
        model, = json.loads((build / 'model.json').read_text())
        (build / 'seeded_dpi_layout.h').write_text(''.join(
            f'constexpr unsigned {p["name"]}Offset = {p["offset"]};\n'
            for p in model['states'] if p['type'] in ('input', 'output')))
        for level in ('0', '2'):
            run(sdk / 'opt', '-O' + level, build / 'model.ll', '-o',
                build / 'model.bc')
            run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
                '-filetype=obj', build / 'model.bc', '-o', build / 'model.o')
            run(*shlex.split(os.environ.get('CXX', 'c++')), '-O' + level,
                '-std=c++17', '-I' + str(build),
                '-I' + str(sdk.parent / 'include'), HERE / 'seeded_dpi.cpp',
                build / 'model.o', '-L' + str(sdk.parent / 'lib'),
                '-lCIRCTArcRuntime', *libs, '-lz', '-lpthread', '-ldl', '-o',
                build / 'test')
            result = subprocess.run([build / 'test'],
                                    capture_output=True,
                                    text=True)
            (build / ('O' + level + '.log')).write_text(result.stdout +
                                                        result.stderr)
            if name == 'positive':
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout, end='', flush=True)
            else:
                assert result.returncode == -signal.SIGABRT, result.returncode
                assert 'DPI output buffer was not initialized on this call' in result.stderr
                print(
                    f'PASS: removing output seeding fails with defined poisoned buffers at O{level}',
                    flush=True)


if __name__ == '__main__':
    main()
