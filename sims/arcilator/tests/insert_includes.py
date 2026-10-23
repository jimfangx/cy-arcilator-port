#!/usr/bin/env python3
"""Compare include expansion with native SV preprocessing in all branch modes."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--circt-verilog', type=Path, required=True)
parser.add_argument('--work', type=Path, required=True)
parser.add_argument('--preprocessor',
                    type=Path,
                    default=ROOT / 'scripts/insert-includes.py')
parser.add_argument('--baseline', type=Path)
args = parser.parse_args()
work = args.work.resolve()
work.mkdir(parents=True, exist_ok=True)
fixtures = {
    'leaf.svh':
    '''`ifndef LEAF_SEEN
`define LEAF_SEEN
`define APPLY(x) ((x) ^ 8'h96)
`endif
''',
    'wrapper.svh':
    '''`ifndef WRAPPER_SEEN
`define WRAPPER_SEEN
`ifdef INNER
`include "leaf.svh"
`endif
`endif
''',
    'common.svh':
    '''`ifndef COMMON_SEEN
`define COMMON_SEEN
localparam logic [7:0] CommonValue = 8'hc0;
`endif
''',
    'source.sv':
    '''`define UNUSED \\
 `ifdef IN_A_MACRO \\
 `endif
module Top(output logic [7:0] response);
`include "common.svh"
`ifdef A
/* The directive below is a comment, not the end of branch A.
`endif
*/
`include "common.svh"
`include "wrapper.svh"
`elsif B
`include "wrapper.svh"
`else
`include "wrapper.svh"
`endif
`include "leaf.svh"
localparam logic [7:0] Result = `APPLY(CommonValue);
if (Result != 8'h56) begin : wrong_value
  UndefinedMacroResultMustFail invalid();
end
assign response = Result;
endmodule
'''
}
for name, content in fixtures.items():
    (work / name).write_text(content)
sources = {'source': work / 'source.sv'}
for kind, script in [('expanded', args.preprocessor),
                     ('baseline', args.baseline)]:
    if script:
        sources[kind] = work / (kind + '.sv')
        with (work / (kind + '-expansion.log')).open('w') as log:
            subprocess.run([
                sys.executable, script, work / 'source.sv', sources[kind], work
            ],
                           check=True,
                           stdout=log,
                           stderr=subprocess.STDOUT)
results = {}
for kind, source in sources.items():
    results[kind] = []
    for bits in itertools.product((False, True), repeat=3):
        mode = ''.join(str(int(bit)) for bit in bits)
        flags = [
            '-D' + flag for flag, enabled in zip(('A', 'B', 'INNER'), bits)
            if enabled
        ]
        result = subprocess.run([
            args.circt_verilog, '--import-only', '--top=Top', '-I', work,
            *flags, source, '-o', work / (kind + '-' + mode + '.mlir')
        ],
                                text=True,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        (work / (kind + '-' + mode + '.log')).write_text(result.stdout)
        # The generate guard fails elaboration if the macro has the wrong
        # numeric value. The original, unflattened source is the independent
        # preprocessor reference, including its comments and macro bodies.
        results[kind].append(dict(mode=mode, exit_code=result.returncode))
        expected = not (kind == 'baseline' and not (bits[0] and bits[2]))
        if (result.returncode == 0) != expected:
            raise RuntimeError(
                f'{kind} mode {mode}: unexpected SV elaboration result; '
                f'see {work / (kind + "-" + mode + ".log")}')
    print(kind, sum(item['exit_code'] == 0 for item in results[kind]),
          '/8 passed')

# The expanded output must be usable without the original headers. Also check
# tabs and an include-line comment spanning subsequent caller lines; the
# comment must not hide the included file's nested include directive.
(work / 'indirect.svh').write_text('`include "leaf.svh"\n')
caller = work / 'commented-include.sv'
caller.write_text('''module Top(output logic [7:0] response);
\t`include "indirect.svh" /* caller comment
`endif
*/
localparam logic [7:0] Result = `APPLY(8'hc0);
if (Result != 8'h56) begin : wrong_value
  UndefinedMacroResultMustFail invalid();
end
assign response = Result;
endmodule
''')
isolated = work / 'standalone'
isolated.mkdir(exist_ok=True)
expanded = isolated / 'expanded.sv'
with (work / 'commented-include-expansion.log').open('w') as log:
    subprocess.run([sys.executable, args.preprocessor, caller, expanded, work],
                   check=True,
                   stdout=log,
                   stderr=subprocess.STDOUT)
for kind, source, includes in [('source', caller, ['-I', work]),
                               ('standalone', expanded, [])]:
    subprocess.run([
        args.circt_verilog, '--import-only', '--top=Top', *includes, source,
        '-o', isolated / (kind + '.mlir')
    ],
                   check=True)
report = dict(status='passed',
              standalone_include_comment_and_tabs=True,
              modes=('A', 'B', 'INNER'),
              results=results,
              source_sha256={
                  str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in [
                      Path(__file__), args.preprocessor, args.circt_verilog,
                      *sources.values()
                  ]
              })
(work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(
    'PASS: include branch selection, nested headers, conditional-to-unconditional '
    'includes, dominating duplicate includes, comments and macro continuations'
)
