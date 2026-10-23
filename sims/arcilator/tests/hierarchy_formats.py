#!/usr/bin/env python3
"""Check physical formatting scopes and reject incomplete source provenance."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    args = parser.parse_args()
    work = SIM / 'generated-src/hierarchy-format-guards'
    work.mkdir(parents=True, exist_ok=True)
    libraries = [
        plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
               SIM / 'generated-src/compiler', SIM / 'compiler' / name)
        for name in ('NativeContexts.cpp', 'HierarchyFormats.cpp')
    ]
    pipeline = (
        'chipyard-native-contexts{root=Root},'
        'chipyard-capture-hierarchy-formats{root=Root},'
        'hw-flatten-modules,chipyard-resolve-hierarchy-formats{root=Root}')

    def lower(name, text, error=None):
        source, target = work / (name + '.mlir'), work / (name +
                                                          '-result.mlir')
        source.write_text(text)
        result = subprocess.run([
            args.circt_opt, source,
            *['--load-pass-plugin=' + str(p) for p in libraries],
            '--pass-pipeline=builtin.module(' + pipeline + ')', '-o', target
        ],
                                capture_output=True,
                                text=True)
        (work / (name + '.log')).write_text(result.stderr)
        if error:
            assert result.returncode == 1 and error in result.stderr, result.stderr
            return
        assert result.returncode == 0, result.stderr
        return target.read_text()

    source = '''module {
      func.func private @inner() {
        %m=sim.fmt.hier_path {chipyard.sv_format_scope="Decl.leaf.fn.block"}
        sim.proc.print %m
        return
      }
      func.func private @outer() {
        func.call @inner() {chipyard.sv_call_lookup="lexical"}:()->()
        return
      }
      hw.module private @Leaf() attributes {chipyard.sv_source_scope="Decl.leaf"} {
        llhd.process {
          func.call @outer() {chipyard.sv_call_lookup="lexical"}:()->()
          llhd.halt
        }
        hw.output
      }
      hw.module @Root() {
        hw.instance "a" @Leaf()->()
        hw.instance "b" @Leaf()->()
        hw.output
      }
    }'''
    result = lower('transitive', source)
    assert result.count(
        'sim.proc.print') == 2 and 'func.call' not in result, result
    for name in ('a', 'b'):
        assert 'sim.fmt.literal "Root.' + name + '.fn.block"' in result, result
    assert lower('idempotent', result) == result
    # A non-returning single-block function must retain its terminator and
    # valid call-result SSA, including any unreachable continuation.
    noreturn = source.replace(
        '        return\n      }\n      func.func private @outer',
        '        ub.unreachable\n      }\n      func.func private @outer')
    nonreturning = lower('noreturn', noreturn)
    assert nonreturning.count('ub.unreachable') == 2, nonreturning
    assert 'func.call' not in nonreturning, nonreturning
    stopped = lower('fatal-noreturn', noreturn.replace(
        'sim.proc.print %m', 'sim.proc.print %m\n        sim.terminate failure, quiet'))
    assert 'ub.unreachable' not in stopped, stopped
    assert stopped.count('sim.terminate') == 2, stopped
    lower(
        'missing-lexical',
        source.replace(' {chipyard.sv_format_scope="Decl.leaf.fn.block"}', ''),
        'requires frontend lexical scope provenance')
    lower('wrong-declaration',
          source.replace('Decl.leaf.fn.block', 'Decl.leafish.fn.block'),
          'requires a physical function call scope')
    lower('missing-call',
          source.replace(' {chipyard.sv_call_lookup="lexical"}', ''),
          'requires call lookup provenance')
    lower(
        'recursive',
        source.replace(
            '        sim.proc.print %m', '''        sim.proc.print %m
        func.call @inner() {chipyard.sv_call_lookup="lexical"}:()->()'''),
        'recursive context SV functions require native call frames')
    escaped = source.replace('Decl.leaf.fn.block',
                             r'Decl.leaf.\\fn.dot .\\block.dot ')
    plain = lower('escaped-plain', escaped)
    assert 'sim.fmt.literal "Root.a.fn.dot.block.dot"' in plain, plain
    escaped = escaped.replace('sim.fmt.hier_path {',
                              'sim.fmt.hier_path escaped {')
    formatted = lower('escaped-SV', escaped)
    assert r'sim.fmt.literal "Root.a.\\fn.dot .\\block.dot "' in formatted, formatted
    print(
        'PASS: transitive physical formatter scopes, repeated instances, escaped '
        'lexical suffixes, idempotence and four provenance/recursion guards',
        flush=True)


if __name__ == '__main__':
    main()
