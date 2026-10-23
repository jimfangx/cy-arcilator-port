#!/usr/bin/env python3
"""Check source VPI metadata against source-language facts and unchanged IR."""
import argparse
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
from types import SimpleNamespace


def walk(value):
    if isinstance(value, dict):
        yield value
        for item in value.values():
            yield from walk(item)
    elif isinstance(value, list):
        for item in value:
            yield from walk(item)


def check_types(data):
    assert data['schema'] == 'chipyard.arcilator.sv-source'
    assert data['version'] == 1
    assert data['scope'] == 'one-elaborated-compilation'
    assert data['symbol_identity'] == 'snapshot-local-slang-address'
    nodes = list(walk(data['design']))
    ids = {node['addr']: node for node in nodes if 'addr' in node}

    def resolve(value):
        return ids[int(value.split(' ', 1)[0])] if isinstance(value, str) else value

    def member(scope, name, kind):
        return next(n for n in scope['members']
                    if n.get('name') == name and n.get('kind') == kind)

    top = next(n for n in nodes if n.get('kind') == 'Instance' and n['name'] == 'Top')
    body = resolve(top['body'])
    left = member(body, 'left', 'Instance')
    right = member(body, 'right', 'Instance')
    assert left['addr'] != right['addr']
    leaf = resolve(left['body'])
    assert leaf['name'] == 'Leaf' and resolve(right['body'])['addr'] == leaf['addr']
    wider = resolve(member(body, 'wider', 'Instance')['body'])
    assert wider['name'] == 'Leaf' and wider['addr'] != leaf['addr']
    assert member(wider, 'a', 'Port')['type']['range'] == '[9:3]'
    assert member(wider, 'y', 'Port')['type']['range'] == '[0:6]'
    escaped = member(body, 'escaped.instance', 'Instance')
    assert resolve(escaped['body'])['addr'] == leaf['addr']
    assert escaped['addr'] not in [left['addr'], right['addr']]
    dead = member(leaf, 'dead_internal', 'Variable')
    assert dead['type']['range'] == '[2:6]'
    assert dead['type']['elementType']['isSigned'] is True
    mem = member(leaf, 'mem', 'Variable')
    assert mem['type']['kind'] == 'FixedSizeUnpackedArrayType'
    assert mem['type']['range'] == '[5:2]'
    assert mem['type']['elementType']['range'] == '[7:0]'
    alias = resolve(member(leaf, 'packet', 'Variable')['type'])
    assert alias['name'] == 'payload_t' and alias['kind'] == 'TypeAlias'
    struct = resolve(alias['target'])
    hi = member(struct, 'hi', 'Field')
    lo = member(struct, 'lo', 'Field')
    assert hi['type']['range'] == '[2:0]'
    assert hi['type']['elementType']['isSigned'] is True
    assert lo['type']['range'] == '[0:4]'
    assert lo['type']['elementType']['name'] == 'bit'
    generated = member(leaf, 'g', 'GenerateBlockArray')
    actual = {}
    for block in generated['members']:
        if block.get('kind') == 'GenerateBlock':
            index = int(member(block, 'k', 'Parameter')['value'])
            actual[index] = member(block, 'local_net', 'Variable')['type']['range']
    assert actual == {2: '[2:0]', 3: '[3:0]'}
    assert Path(dead['source_file']).name == 'vpi_source_metadata.sv'
    assert dead['source_line'] == 7
    scales = {n['name']: n for n in data['time_scales']}
    for name, expected in dict(P=(10**10,10**6), Bus=(10**6,10),
                               Leaf=(10**7,1000), Top=(10**9,100000),
                               Unused=(10**17,1)).items():
        record = scales[name]
        assert record['resolved'] and (record['timeunit_fs'], record['timeprecision_fs']) == expected
    assert scales['$unit']['resolved'] is False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frontend', type=Path)
    parser.add_argument('--circt-bin', type=Path)
    parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--control', type=Path)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve(); work.mkdir(parents=True, exist_ok=True)
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.frontend is None:
        if args.circt_bin is None:
            parser.error('set --frontend or --circt-bin to the matching SDK')
        sim = Path(__file__).resolve().parent.parent
        sys.path.insert(0, str(sim))
        from compiler.verilog.build import build
        args.frontend = build(SimpleNamespace(
            circt_verilog=args.circt_bin/'circt-verilog',
            cache=sim/'generated-src/compiler/verilog', jobs=args.jobs,
            circt_source=None, slang_source=None, slang_build=None))
    frontend = args.frontend.resolve()
    control = (args.control or args.frontend).resolve()
    fixture = Path(__file__).with_name('vpi_source_metadata.sv').resolve()
    report = dict(status='running', steps=[], cases=[])

    def limits():
        resource.setrlimit(resource.RLIMIT_AS, (4*1024**3,4*1024**3))
        resource.setrlimit(resource.RLIMIT_CORE, (0,0))

    def run(name, command, valid=True):
        command = list(map(str, command)); log = work/(name+'.log')
        row = dict(name=name, command=command, log=str(log))
        report['steps'].append(row)
        with log.open('w') as output:
            p = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                 start_new_session=True, preexec_fn=limits)
            try: code=p.wait(timeout=180)
            except subprocess.TimeoutExpired:
                if p.poll() is None: os.killpg(p.pid,signal.SIGKILL)
                p.wait(); raise RuntimeError(name+' exceeded180s')
        row['returncode']=code
        assert (code==0)==valid,(name,log.read_text()[-4000:])

    try:
        for mode in ['import-only', 'ir-moore', 'ir-llhd']:
            ir=work/(mode+'.mlir'); old=work/(mode+'-control.mlir')
            quiet=work/(mode+'-disabled.mlir'); metadata=work/(mode+'.json')
            flags=['--single-unit','--'+mode,'--top=Top',fixture]
            run(mode,[frontend,*flags,'--native-vpi-metadata='+str(metadata),'-o',ir])
            run(mode+'-control',[control,*flags,'-o',old])
            run(mode+'-disabled',[frontend,*flags,'-o',quiet])
            assert ir.read_bytes()==old.read_bytes()==quiet.read_bytes()
            check_types(json.loads(metadata.read_text()))
            report['cases'].append(dict(mode=mode,original_whole_IR_identical=True,
                                        typed_source_and_shared_identity_passed=True))
        for name,text,flags,expected in [
            ('unresolved','module Default; endmodule\n',[],False),
            ('command-default','module Default; endmodule\n',['--timescale=10ns/100ps'],True),
            ('source-only-program','program automatic Driver; timeunit 100ns; timeprecision 10ps; string message; endprogram\n',[],True)]:
            source=work/(name+'.sv');source.write_text(text);metadata=work/(name+'.json')
            run(name,[frontend,'--lint-only',*flags,source,'--native-vpi-metadata='+str(metadata)])
            data=json.loads(metadata.read_text());assert data['mode']=='lint'
            item=next(v for v in data['time_scales'] if v['kind'] in ['module','program'])
            assert item['resolved']==expected
            if name=='command-default':assert (item['timeunit_fs'],item['timeprecision_fs'])==(10**7,100000)
            if name=='source-only-program':assert item['kind']=='program' and item['timeprecision_fs']==10000
            report['cases'].append(dict(name=name,source_elaboration_metadata_passed=True))
        a=work/'a.sv';b=work/'b.sv'
        a.write_text('timeunit 10ns; timeprecision 1ps; module A; endmodule\n')
        b.write_text('timeunit 1us; timeprecision 100ps; module B; A a(); endmodule\n')
        metadata=work/'separate.json'
        run('separate-units',[frontend,'--lint-only',a,b,'--native-vpi-metadata='+str(metadata)])
        data=json.loads(metadata.read_text());units=[s for s in data['time_scales'] if s['kind']=='compilation_unit']
        assert len(units)==2 and {s['compilation_unit'] for s in units}=={0,1}
        modules={s['name']:s for s in data['time_scales'] if s['kind']=='module'}
        assert modules['A']['timeprecision_fs']==1000 and modules['B']['timeprecision_fs']==100000
        source=work/'decades.sv'
        source.write_text('\n'.join(
            f'module Scale{power}; timeunit 100s; timeprecision {mag}{unit}; endmodule'
            for power,(unit,mag) in enumerate((u,m) for u in
                ['fs','ps','ns','us','ms','s'] for m in [1,10,100]))+'\n')
        metadata=work/'decades.json'
        run('all18-decades',[frontend,'--lint-only',source,'--native-vpi-metadata='+str(metadata)])
        data=json.loads(metadata.read_text());scales={s['name']:s for s in data['time_scales']}
        for power in range(18):
            assert scales['Scale'+str(power)]['timeunit_fs']==10**17
            assert scales['Scale'+str(power)]['timeprecision_fs']==10**power
        bad=work/'bad.sv';bad.write_text('module Broken; int n=unknown_symbol; endmodule\n')
        sentinel=work/'sentinel.json';sentinel.write_text('unchanged')
        run('semantic-error',[frontend,'--lint-only',bad,'--native-vpi-metadata='+str(sentinel)],False)
        assert sentinel.read_text()=='unchanged'
        mlir=work/'input.mlir';mlir.write_text('module {}\n')
        for name,flags in [
            ('parse-only',['--parse-only',fixture]),
            ('preprocess',['-E',fixture]),
            ('MLIR',[mlir]),
            ('stdout',['--lint-only',fixture,'--native-vpi-metadata=-']),
            ('IR-collision',['--lint-only',fixture,'-o',sentinel]),
            ('input-collision',['--format=sv','--lint-only',sentinel]),
            ('missing-directory',['--lint-only',fixture,'--native-vpi-metadata='+str(work/'missing/facts.json')])]:
            prefix = [] if any(str(flag).startswith('--native-vpi-metadata=')
                               for flag in flags) else [
                                   '--native-vpi-metadata='+str(sentinel)]
            run('reject-'+name,[frontend,*prefix,*flags],False)
            assert sentinel.read_text()=='unchanged'
        assert not list(work.glob('*.tmp-*'))
        report.update(status='passed_typed_source_metadata_and_original_IR',
                      separate_compilation_units_preserved=True,
                      negative_output_and_mode_guards_passed=True)
        print('PASS: source hierarchy/types/ranges/shared IDs, time-scale provenance, unchanged original Moore/LLHD IR and invalid-output guards',flush=True)
    except BaseException as error:
        report.update(status='failed',error=repr(error));raise
    finally:(work/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
