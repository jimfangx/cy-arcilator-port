"""Count physical partition ports independently of procedural CFG size."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

SIM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SIM))
from compiler.partition import plugin

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--circt-opt', type=Path, required=True)
p.add_argument('--reference-source', type=Path)
p.add_argument('--work',
               type=Path,
               default=SIM / 'generated-src/partition-boundary-tests')
a = p.parse_args()
a.work.mkdir(parents=True, exist_ok=True)
library = plugin(a.circt_opt, os.environ.get('CXX', 'c++'),
                 SIM / 'generated-src/compiler')
reference = plugin(a.circt_opt, os.environ.get('CXX', 'c++'), SIM /
                   'generated-src/compiler',
                   a.reference_source) if a.reference_source else None


def invoke(source, target, ports, implementation):
    return subprocess.run([
        a.circt_opt, source, '--load-pass-plugin=' + str(implementation),
        '--pass-pipeline=builtin.module(chipyard-partition{max-ops=1 max-ports='
        + str(ports) + '})', '-o', target
    ],
                          text=True,
                          capture_output=True)


report = dict(status='running', cases=[])
for count in (8, 128, 2048):
    source = a.work / ('region-' + str(count) + '.mlir')
    lines = [
        'module {',
        ' hw.module @ArcModel(in %clock:i1, in %x:i17, out y:i17) {',
        '  %one = hw.constant 1 : i17', '  llhd.process {', '   cf.br ^wait',
        '  ^wait:', '   llhd.wait (%clock:i1), ^event', '  ^event:'
    ]
    for index in range(count):
        previous = '%one' if not index else '%v' + str(index - 1)
        lines.append('   %v' + str(index) + ' = comb.xor %x, ' + previous +
                     ' : i17')
    lines += [
        '   %text = sim.fmt.dec %v' + str(count - 1) +
        ' specifierWidth 0 : i17', '   sim.proc.print %text', '   cf.br ^wait',
        '  }', '  hw.output %x : i17', ' }', '}'
    ]
    source.write_text('\n'.join(lines) + '\n')
    target = source.with_suffix('.parts.mlir')
    result = invoke(source, target, 2, library)
    assert result.returncode == 0, result.stderr
    text = target.read_text()
    ports = re.search(r'hw.module private @ArcModel_part_\d+\((.*?)\)', text,
                      re.S)
    assert ports and len(re.findall(r'\bin %', ports[1])) == 2, text
    assert text.count('comb.xor') == count
    result = invoke(source, source.with_suffix('.overflow.mlir'), 1, library)
    assert result.returncode != 0 and 'exceeds the compiler port budget' in result.stderr, result.stderr
    before_failed = None
    if reference:
        result = invoke(source, source.with_suffix('.before.mlir'), 2,
                        reference)
        assert result.returncode != 0 and 'exceeds the compiler port budget' in result.stderr, result.stderr
        before_failed = True
        # When the conservative estimate already fits, retain identical IR.
        before = source.with_suffix('.unchanged-before.mlir')
        after = source.with_suffix('.unchanged-after.mlir')
        limit = count * 8 + 100
        for impl, destination in [(reference, before), (library, after)]:
            result = invoke(source, destination, limit, impl)
            assert not result.returncode, result.stderr
        assert before.read_bytes() == after.read_bytes()
    report['cases'].append(
        dict(internal_operations=count,
             physical_ports=2,
             overflow_guard=True,
             reference_failed=before_failed))
report['status'] = 'passed'
(a.work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
message = (
    'PASS: procedural regions with 8/128/2048 internal operations and two actual ports; '
    'repeated capture deduplication, literal rematerialization and genuine overflow rejection'
)
if reference:
    message += '; before/after comparisons and unchanged previously admitted partitions'
print(message)
