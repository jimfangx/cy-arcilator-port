from pathlib import Path
from types import SimpleNamespace
import sys, subprocess, json

sim = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(sim))
import partition

w = sim / 'generated-src/partition-stream-test'
w.mkdir(parents=True, exist_ok=True)
work = w / 'guards'
work.mkdir(exist_ok=True)
# A rejected normalization must preserve even an input named like its output.
source = work / 'normalized.mlir'
payload = b'module {\n  sv.verbatim.module @"unterminated\n}\n'
source.write_bytes(payload)
try:
    partition.normalize_input(source, work)
    raise AssertionError('bad extern accepted')
except ValueError as error:
    assert 'foreign module declaration' in str(error)
assert source.read_bytes() == payload
assert not list(work.glob('normalized-*.mlir'))
# A late prefix decoding error must not replace a previous partition artifact.
source = work / 'bad-prefix.mlir'
source.write_bytes(
    b'module {\n\xff\n  hw.module @Guard() {\n    hw.output\n  }\n}\n')
target = work / 'partitioned.mlir'
target.write_bytes(b'previous valid output')
fds = len(list(Path('/proc/self/fd').iterdir()))
try:
    partition.partition_flat(source, work, 'Guard', {})
    raise AssertionError('bad prefix accepted')
except UnicodeDecodeError:
    pass
assert target.read_bytes() == b'previous valid output'
assert not list(work.glob('partition-text-*')) and not list(
    work.glob('partitioned-*.mlir.tmp'))
assert len(list(Path('/proc/self/fd').iterdir())) == fds
# The outer module may have attributes containing the words "module {".
source = work / 'attributes.mlir'
source.write_text(
    '''module attributes {test.note="module {", test.unicode="λ"} {
  hw.module @Guard(in %clock:i1,out q:i1) {
    hw.output %clock:i1
  }
}
''')
args = SimpleNamespace(input=source,
                       output=work,
                       top='Guard',
                       circt_opt=sys.argv[1],
                       cxx=__import__('os').environ.get('CXX', 'c++'))
partition.prepare(args)
subprocess.run([args.circt_opt, target, '-o', work / 'verified.mlir'],
               check=True)
text = target.read_text()
assert 'test.note = "module {"' in text and 'chipyard_arc_assert_fail()' in text
report = dict(status='passed',
              cases=[
                  'in-place normalization failure preserves input',
                  'temporary output cleanup',
                  'descriptor cleanup on prefix failure',
                  'outer module attributes and opaque strings'
              ])
(w / 'guards-report.json').write_text(json.dumps(report, indent=2) + '\n')
print('PASS:', ', '.join(report['cases']), flush=True)
