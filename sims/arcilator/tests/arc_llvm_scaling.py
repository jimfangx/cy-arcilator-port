#!/usr/bin/env python3
"""Compare cached Arc/Func LLVM conversion against the matching stock SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

SIM=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SIM))
from compiler.partition import plugin


def source(count=16384, forward=False):
    lines=['module {']
    lines += [f'func.func private @padding_{i}(i64) -> i64' for i in range(count)]
    leaf='func.func private @leaf(%x:i64, %y:i13, %a:!hw.array<2xi8>, %p:!arc.arrayref<4xi13>) -> (i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>) attributes {llvm.bareptr} {\n return %x, %y, %a, %p : i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>\n}'
    if not forward: lines.append(leaf)
    types='i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>'
    lines.append('func.func @driver(%x:i64, %y:i13, %a:!hw.array<2xi8>, %p:!arc.arrayref<4xi13>) -> ('+types+') {')
    previous='%x, %y, %a, %p'
    for i in range(count):
        lines.append(f'%v{i}:4=func.call @leaf({previous}) {{chipyard.test_order = {i} : i32}} : ({types}) -> ({types})')
        previous=', '.join(f'%v{i}#{n}' for n in range(4))
    lines += ['return '+previous+' : '+types,'}']
    if forward: lines.append(leaf)
    lines += ['\nfunc.func @indirect(%x:i64, %y:i13, %a:!hw.array<2xi8>, %p:!arc.arrayref<4xi13>) -> (i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>) {\n %fn = func.constant @leaf : (i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>) -> (i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>)\n %v:4 = func.call_indirect %fn(%x,%y,%a,%p) : (i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>) -> (i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>)\n return %v#0,%v#1,%v#2,%v#3 : i64,i13,!hw.array<2xi8>,!arc.arrayref<4xi13>\n}\nfunc.func @memref_identity(%a:memref<4xi32>) -> memref<4xi32> { return %a : memref<4xi32> }\nfunc.func @print() {\n %m = sim.fmt.literal "arc lookup check"\n sim.proc.print %m\n return\n}\narc.model @m io !hw.modty<> storageBytes 24 { ^bb0(%s:!arc.storage): }\narc.model @m_eval io !hw.modty<> storageBytes 24 { ^bb0(%s:!arc.storage): }\nmodule @nested {\n func.func @leaf(%a:i8) -> i8 {return %a : i8}\n func.func @driver(%a:i8) -> i8 {\n  %v=func.call @leaf(%a) : (i8) -> i8\n  return %v : i8\n }\n arc.model @m io !hw.modty<> storageBytes 24 { ^bb0(%s:!arc.storage): }\n arc.model @m_eval io !hw.modty<> storageBytes 24 { ^bb0(%s:!arc.storage): }\n}\n}']
    return '\n'.join(lines)+'\n'


def check(circt,cxx,build):
    build.mkdir(parents=True,exist_ok=True)
    library=plugin(circt/'circt-opt',cxx,SIM/'generated-src/compiler',SIM/'compiler/ArcToLLVM.cpp')
    reports=[]
    for name,count,forward in [('1024',1024,False),('4096',4096,False),('large',16384,False),('largest',32768,False),('forward',128,True),('model-names',0,False)]:
        before=build/(name+'.mlir');before.write_text(source(count,forward));results=[]
        for label,lower in [('stock','lower-arc-to-llvm'),('cached','chipyard-lower-arc-to-llvm')]:
            after=build/(name+'-'+label+'.mlir');start=time.monotonic()
            command=[str(circt/'circt-opt'),str(before),'--load-pass-plugin='+str(library),'--pass-pipeline=builtin.module(arc-infer-context,'+lower+',reconcile-unrealized-casts)','-o',str(after)]
            result=subprocess.run(command,capture_output=True,text=True)
            (build/(name+'-'+label+'.log')).write_text(result.stdout+result.stderr)
            assert result.returncode==0,(name,label,result.stderr)
            results.append(dict(implementation=label,elapsed_seconds=time.monotonic()-start,output_sha256=hashlib.sha256(after.read_bytes()).hexdigest()))
        assert results[0]['output_sha256']==results[1]['output_sha256'],name
        text=(build/(name+'-cached.mlir')).read_text()
        assert [int(n) for n in re.findall(r'chipyard.test_order = (\d+) : i32',text)]==list(range(count))
        assert text.count('llvm.func @m_eval(')==2 and text.count('llvm.func @m_eval_eval(')==2
        assert '@m_eval_0' not in text
        assert 'llvm.dereferenceable = 8 : i64' in text
        assert 'llvm.bareptr' in text and 'llvm.call %' in text
        reports.append(dict(case=name,symbols=count,ordered_calls=count,complete_LLVM_IR_byte_identical=True,executions=results))
    collision=build/'collision.mlir'
    collision.write_text('module { func.func private @m_eval(!arc.storage) arc.model @m io !hw.modty<> storageBytes 24 { ^bb0(%s:!arc.storage): } }\n')
    for label,lower in [('stock','lower-arc-to-llvm'),('cached','chipyard-lower-arc-to-llvm')]:
        result=subprocess.run([str(circt/'circt-opt'),str(collision),'--load-pass-plugin='+str(library),'--pass-pipeline=builtin.module('+lower+')','-o',str(build/('collision-'+label+'.mlir'))],capture_output=True,text=True)
        (build/('collision-'+label+'.log')).write_text(result.stdout+result.stderr)
        assert result.returncode!=0,(label,'accepted conflicting model eval symbol')
    (build/'report.json').write_text(json.dumps(dict(status='passed',cases=reports,conflicting_eval_symbol_rejected=True),indent=2)+'\n')
    print('PASS: 54400 ordered calls with exact stock LLVM IR, aggregates/arrayrefs, bareptr attributes, forward/indirect/shadowed calls, unchanged generated eval names and preserved collision rejection')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-bin',type=Path,default=os.environ.get('CIRCT_BIN'))
    parser.add_argument('--work',type=Path,default=SIM/'generated-src/arc-llvm-scaling')
    args=parser.parse_args()
    if args.circt_bin is None:parser.error('set CIRCT_BIN or supply --circt-bin')
    check(args.circt_bin,os.environ.get('CXX','c++'),args.work)
