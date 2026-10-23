#!/usr/bin/env python3
"""Keep every macro definition when the emitted hierarchy omits dead memories."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'split-mems-conf.py'


class SplitMemories(unittest.TestCase):
    def split(self, with_dut, lines):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            tree = dict(instance_name='TestHarness', module_name='TestHarness',
                        instances=[dict(instance_name='host', module_name='host_mem', instances=[])])
            if with_dut:
                tree['instances'].append(dict(instance_name='dut', module_name='ChipTop',
                                              instances=[dict(instance_name='ram', module_name='dut_mem', instances=[])]))
            (work / 'hierarchy.json').write_text(json.dumps(tree))
            (work / 'all.conf').write_text(''.join(lines))
            subprocess.run([sys.executable, SCRIPT, '--in-smems-conf', work / 'all.conf',
                            '--in-model-hrchy-json', work / 'hierarchy.json',
                            '--dut-module-name', 'ChipTop', '--model-module-name', 'TestHarness',
                            '--out-dut-smems-conf', work / 'dut.conf',
                            '--out-model-smems-conf', work / 'model.conf'], check=True, capture_output=True)
            return (work / 'dut.conf').read_text(), (work / 'model.conf').read_text()

    def test_dead_debug_description_is_retained(self):
        dut = 'name dut_mem depth 128 width 32 ports rw \n'
        host = 'name host_mem depth 64 width 8 ports rw \n'
        orphan = 'name unused_debug_ext depth 32 width 128 ports mwrite,read mask_gran 32 \n'
        out_dut, out_host = self.split(True, [dut, host, orphan])
        self.assertEqual(out_dut, dut + orphan)
        self.assertEqual(out_host, host)
        self.assertCountEqual((out_dut + out_host).splitlines(True), [dut, host, orphan])

    def test_no_dut_keeps_definition_in_model(self):
        orphan = 'name unused_ext depth 32 width 128 ports rw \n'
        host = 'name host_mem depth 64 width 8 ports rw \n'
        self.assertEqual(self.split(False, [orphan, host]), ('', orphan + host))

    def test_empty_memory_list(self):
        self.assertEqual(self.split(True, []), ('', ''))


if __name__ == '__main__':
    unittest.main()
