#!/usr/bin/env python3
"""Check FIRRTL compatibility without changing memory or annotation semantics."""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import subprocess


def main():
    sim = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firtool', type=Path, required=True)
    parser.add_argument('--build', type=Path, default=sim / 'generated-src/frontend-test')
    args = parser.parse_args()
    args.build.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('arc_frontend', sim / 'frontend.py')
    frontend = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(frontend)
    base = '''FIRRTL version 3.3.0
circuit MemoryRuw :
  module MemoryRuw :
    input clock : Clock
    input address : UInt<2>
    input data : UInt<8>
    output q : UInt<8>
    smem mem : UInt<8>[4], MODE
    write mport w = mem[address], clock
    connect w, data
    read mport r = mem[address], clock
    connect q, r
'''
    assert frontend.normalize_chirrtl(base) == (base, 0)
    assert frontend.strip_enum_metadata(base) == (base, 0)
    annotations = [{'class': 'chisel3.experimental.EnumAnnotations$' + kind,
                    'target': 'MemoryRuw.NotARealTarget'} for kind in
                   ('EnumComponentAnnotation', 'EnumDefAnnotation', 'EnumVecAnnotation')]
    retained = {'class': 'firrtl.transforms.DontTouchAnnotation',
                'target': '~MemoryRuw|MemoryRuw>q'}
    annotations.append(retained)
    annotated = base.replace('circuit MemoryRuw :',
                            'circuit MemoryRuw :%[' + json.dumps(annotations) + ']')
    stripped, count = frontend.strip_enum_metadata(annotated)
    assert count == 3 and 'DontTouchAnnotation' in stripped and 'EnumAnnotations' not in stripped
    match = re.search(r':%\[', stripped)
    parsed, _ = json.JSONDecoder().raw_decode(stripped, match.end())
    assert parsed == [retained]
    for mode in ('new', 'old', 'undefined'):
        outputs = []
        for spelling in ('canonical', 'emitted'):
            text = annotated.replace('MODE', mode)
            if spelling == 'emitted':
                text = text.replace('], ' + mode, '] ' + mode)
            source = args.build / f'{mode}-{spelling}.fir'
            output = source.with_suffix('.mlir')
            source.write_text(text)
            subprocess.run(['python3', str(sim / 'frontend.py'), '--input', str(source),
                            '--output', str(output), '--top', 'MemoryRuw',
                            '--firtool', str(args.firtool), '--', '--parse-only'], check=True)
            ir = output.read_text()
            assert re.search(r'chirrtl.seqmem.* ' + mode.capitalize() + r'\s+:', ir)
            assert 'DontTouchAnnotation' in ir
            outputs.append(ir)
        assert outputs[0] == outputs[1], f'{mode} memory semantics changed'
    print('PASS: frontend memory modes, enum metadata, retained annotations, and canonical IR parity', flush=True)


if __name__ == '__main__':
    main()
