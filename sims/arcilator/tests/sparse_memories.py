"""Preserve memory features that cannot yet use native sparse storage."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compiler.memories import lower_memories

body = '''
    %mem = seq.firmem 1, 1, old, port_order : <9 x 65, mask 5>
    seq.firmem.write_port %mem[%address] = %data, clock %clock : <9 x 65, mask 5>, i5
    %value = seq.firmem.read_port %mem[%address], clock %clock : <9 x 65, mask 5>
'''
for original in [
        body.replace('1, 1, old', '2, 1, old'),
        body.replace('1, 1, old', '1, 2, old'),
        body.replace('port_order :', 'port_order {init = "image.hex"} :'),
        body.replace('seq.firmem 1,', 'seq.firmem sym @preserved 1,'),
        body + '    other.operation %mem : !opaque\n',
        body.replace('read_port %mem', 'unknown_port %mem'),
]:
    externs = {}
    output = lower_memories(original, externs, 1)
    assert output.strip() == original.strip(), output
    assert not externs, externs
externs = {}
assert lower_memories(body, externs, 10000).strip() == body.strip()
assert not externs
externs = {'ArcSparseMemory_0': 'existing'}
output = lower_memories(body + '    %arc_memory_1_true = hw.constant true\n',
                        externs, 1)
assert externs['ArcSparseMemory_2'] == 'ArcSparseMemory'
assert '%arc_memory_2_true' in output and '%arc_memory_2_mask' in output
assert 'seq.firmem' not in output
print('PASS: sparse-memory latency, initialization, symbol, unknown-user, '
      'threshold and name-collision guards')
