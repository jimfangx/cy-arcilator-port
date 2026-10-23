#!/usr/bin/env python3
"""Every uniqued DPI declaration retains its own source signature and scope."""
import argparse
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
SIM = HERE.parent
sys.path.insert(0, str(SIM))
from compiler.verilog.build import build
from partition import IDENT, unquote
from dpi_abi import imports
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin', type=Path, required=True)
    parser.add_argument('--frontend', type=Path)
    args = parser.parse_args()
    tool = args.frontend or build(
        SimpleNamespace(circt_verilog=args.circt_bin / 'circt-verilog',
                        cache=SIM / 'generated-src/compiler/verilog',
                        circt_source=None,
                        slang_source=None,
                        slang_build=None,
                        jobs=2))
    work = SIM / 'generated-src/dpi-instance-metadata'
    work.mkdir(parents=True, exist_ok=True)
    target = work / 'metadata.mlir'
    subprocess.run([
        str(tool), '--single-unit', '--ir-llhd', '--top=MetadataTop',
        str(HERE / 'dpi_instance_metadata.sv'), '-o',
        str(target)
    ],
                   check=True)
    text = target.read_text()
    table = imports(text)
    declarations = {
        unquote(m[1])
        for m in re.finditer(r'^\s*sim.func.dpi private @(' + IDENT +
                             r')\(', text, re.M)
    }
    assert len(declarations) >= 2, (declarations, table)
    assert declarations == set(table), (declarations, set(table))
    scopes = set()
    for name in declarations:
        info = table[name]
        assert info['version'] == 1
        assert info['declaration']['c_name'] == 'metadata_probe' and info[
            'declaration']['context']
        assert [a['kind'] for a in info['abi']] == ['int', 'int']
        assert [a['direction'] for a in info['abi']] == ['in', 'return']
        scopes.add(info['declaration']['scope'])
    assert any(s.endswith('.a')
               for s in scopes) and any(s.endswith('.b')
                                        for s in scopes), scopes
    assert 'chipyard.dpi_context_lookup = "lexical"' in text and 'chipyard.dpi_context_lookup = "absolute"' in text
    assert 'chipyard.sv_call_lookup = "lexical"' in text and 'chipyard.sv_call_lookup = "absolute"' in text
    for name in ('MetadataTop', 'MetadataTop.a', 'MetadataTop.b'):
        assert 'chipyard.sv_source_scope = "' + name + '"' in text
    print(
        'PASS: every uniqued DPI declaration has original ABI and per-instance declaration scope; lexical/absolute DPI and SV function call provenance survives Moore lowering',
        flush=True)


if __name__ == '__main__':
    main()
