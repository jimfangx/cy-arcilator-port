from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import sys

import argparse
import shlex

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from compiler.partition import plugin as build_plugin

parser = argparse.ArgumentParser(
    description=
    'Numerically check compact LLVM types using the large-return fixture and independent C++ reference'
)
parser.add_argument('--input', type=Path, required=True)
parser.add_argument('--reference', type=Path, required=True)
parser.add_argument('--circt-bin',
                    type=Path,
                    default=os.environ.get('CIRCT_BIN'),
                    required=not os.environ.get('CIRCT_BIN'))
parser.add_argument('--work',
                    type=Path,
                    default=SIM / 'generated-src/compact-struct-test')
args = parser.parse_args()
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
sdk = args.circt_bin.resolve()
sim = SIM
cxx = os.environ.get('CXX', 'c++')
plugin = str(
    build_plugin(sdk / 'circt-opt', cxx, sim / 'generated-src/compiler',
                 sim / 'compiler/CompactStructs.cpp'))


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


# Make the established heterogeneous-return fixture use literal LLVM types.
# The C++ reference formulas are independent of both LLVM representations.
text = args.input.read_text()
types = dict(re.findall(r'^%(\w+) = type (.+)$', text, re.M))
assert set(types) == {'Small', 'Big', 'Packed'}
text = re.sub(r'^%\w+ = type .+\n', '', text, flags=re.M)
text = re.sub(r'%(Small|Big|Packed)\b', lambda m: types[m[1]], text)
small = types['Small']
protected = '{' + ', '.join(['i32'] * 64) + '}'
text += f'''
%Keep = type {{ [2 x {protected}], i8 }}
%chipyard.tuple = type {{ i8 }}
define {protected} @keep_body(%Keep %x) {{
 %v = extractvalue %Keep %x, 0, 1
 ret {protected} %v
}}
define i8 @name_collision(%chipyard.tuple %x) {{
 %v = extractvalue %chipyard.tuple %x, 0
 ret i8 %v
}}
@tuple_storage = internal global {small} zeroinitializer
define i64 @global_driver(i64 %seed) {{
 %address = getelementptr {small}, ptr @tuple_storage, i32 0, i32 17
 store i64 %seed, ptr %address
 %loaded = load {small}, ptr @tuple_storage
 %v = extractvalue {small} %loaded, 17
 ret i64 %v
}}
define i64 @phi_driver(i64 %seed) {{
 %bit = and i64 %seed, 1
 %odd = icmp ne i64 %bit, 0
 br i1 %odd, label %left, label %right
left:
 %a = call {small} @small(i64 %seed)
 br label %merge
right:
 %other = xor i64 %seed, 123
 %b = call {small} @small(i64 %other)
 br label %merge
merge:
 %v = phi {small} [ %a, %left ], [ %b, %right ]
 %field = extractvalue {small} %v, 5
 ret i64 %field
}}
'''
(work / 'original.ll').write_text(text)
cpp = args.reference.read_text()
cpp = cpp.replace(
    'extern "C" uint64_t driver(uint64_t);',
    'extern "C" uint64_t global_driver(uint64_t);\n'
    'extern "C" uint64_t phi_driver(uint64_t);\n'
    'extern "C" uint64_t driver(uint64_t);')
needle = 'if(driver(seed)!=expected)'
assert cpp.count(needle) == 1
cpp = cpp.replace(
    needle,
    'if(global_driver(seed)!=seed)throw std::runtime_error("global/GEP type changed layout");++checks;\n'
    'if(phi_driver(seed)!=((seed&1)?seed:(seed^123))+72)throw std::runtime_error("CFG aggregate type changed values");++checks;\n'
    + needle)
(work / 'check.cpp').write_text(cpp)
run(sdk / 'mlir-translate', '--import-llvm', work / 'original.ll', '-o',
    work / 'before.mlir')
run(sdk / 'circt-opt', work / 'before.mlir', '--load-pass-plugin=' + plugin,
    '--pass-pipeline=builtin.module(chipyard-compact-llvm-structs)', '-o',
    work / 'after.mlir')
run(sdk / 'circt-opt', work / 'after.mlir', '--load-pass-plugin=' + plugin,
    '--pass-pipeline=builtin.module(chipyard-compact-llvm-structs)', '-o',
    work / 'twice.mlir')
assert (work / 'after.mlir').read_bytes() == (work / 'twice.mlir').read_bytes()
run(sdk / 'mlir-translate', '--mlir-to-llvmir', work / 'after.mlir', '-o',
    work / 'compact.ll')
compact = (work / 'compact.ll').read_text()
assert re.search(r'%chipyard\.tuple\.\d+ = type', compact)
assert '%chipyard.tuple = type { i8 }' in compact
assert re.search(r'define \{ i32(?:, i32){63} \} @keep_body\(%Keep', compact)
assert re.search(r'define internal %chipyard\.tuple\.\d+ @small', compact)
shutil.copyfile(work / 'compact.ll', work / 'buffered.ll')
run(sys.executable, sim / 'compiler/llvm.py', work / 'buffered.ll',
    '--llvm-opt', sdk / 'opt', '--cache', sim / 'generated-src/compiler',
    '--cxx', cxx)
for name in ['original', 'compact', 'buffered']:
    for level in ['0', '2']:
        output = work / (name + '-O' + level)
        run(sdk / 'opt', '-O' + level, work / (name + '.ll'), '-o',
            str(output) + '.bc')
        run(sdk / 'llc', '-O' + level, '-relocation-model=pic',
            '-filetype=obj',
            str(output) + '.bc', '-o',
            str(output) + '.o')
        run(*shlex.split(cxx), '-O' + level, work / 'check.cpp',
            str(output) + '.o', '-o', output)
        run(output)
print(
    'PASS: 98304 numeric checks; literal/named/buffered O0/O2, packed/nested layout, floats, pointers, i65, globals/GEPs and aggregate CFG edges; existing identified bodies protected, name collisions and idempotence',
    flush=True)

# Large serialized tuple updates exercise the actual text-size problem without
# asking the host backend to compile a deliberately unused 4096-field function.
n = 4096
ty = '!llvm.struct<(' + ', '.join(['i64'] * n) + ')>'
lines = [
    f'!tuple = {ty}', 'module {', ' llvm.func @large(%seed:i64) -> !tuple {',
    '  %initial=llvm.mlir.undef : !tuple'
]
previous = '%initial'
for i in range(n):
    lines.append(f'  %v{i}=llvm.insertvalue %seed, {previous}[{i}] : !tuple')
    previous = f'%v{i}'
lines += [f'  llvm.return {previous} : !tuple', ' }', '}']
(work / 'large.mlir').write_text('\n'.join(lines) + '\n')
run(sdk / 'mlir-translate', '--mlir-to-llvmir', work / 'large.mlir', '-o',
    work / 'large-before.ll')
run(sdk / 'circt-opt', work / 'large.mlir', '--load-pass-plugin=' + plugin,
    '--pass-pipeline=builtin.module(chipyard-compact-llvm-structs)', '-o',
    work / 'large-after.mlir')
run(sdk / 'mlir-translate', '--mlir-to-llvmir', work / 'large-after.mlir',
    '-o', work / 'large-after.ll')
before, after = [(work / name).stat().st_size
                 for name in ['large-before.ll', 'large-after.ll']]
assert before > after * 100, (before, after)
report = dict(
    status='passed',
    numeric_checks=98304,
    large_tuple_fields=n,
    original_llvm_bytes=before,
    compact_llvm_bytes=after,
    reduction_factor=before / after,
    scope=
    'Numerical and serialization fixtures; full-system validation is separate.'
)
(work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report), flush=True)
