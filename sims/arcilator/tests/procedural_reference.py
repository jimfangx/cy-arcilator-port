#!/usr/bin/env python3
"""Check the native procedural fixture against its original SystemVerilog."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--verilator',
                    default=os.environ.get('VERILATOR', 'verilator'))
parser.add_argument('--work', type=Path, required=True)
args = parser.parse_args()
here = Path(__file__).resolve().parent
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
subprocess.run([
    args.verilator, '--binary', '--timing', '--assert', '-Wno-fatal', '-j',
    '4', '--top-module', 'ProceduralReference', '--Mdir',
    str(work),
    str(here / 'native_procedural.sv'),
    str(here / 'procedural_reference.sv')
],
               check=True)
result = subprocess.run([work / 'VProceduralReference'],
                        text=True,
                        capture_output=True,
                        check=True)
markers = [
    line for line in result.stdout.splitlines()
    if line.startswith('NATIVE_PROCEDURAL ')
]
if markers != ['NATIVE_PROCEDURAL 0 3', 'NATIVE_PROCEDURAL 10 6']:
    raise RuntimeError('SV procedural reference prints changed: ' +
                       result.stdout)
print(result.stdout, end='')
