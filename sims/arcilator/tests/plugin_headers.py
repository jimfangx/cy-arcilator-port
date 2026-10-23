#!/usr/bin/env python3
"""Verify that changing a nested local header rebuilds the executable CIRCT pass."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

SIM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SIM))
from compiler.partition import plugin


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--circt-opt', type=Path, required=True)
    args = parser.parse_args()
    (SIM / 'generated-src').mkdir(exist_ok=True)
    work = Path(
        tempfile.mkdtemp(prefix='plugin-headers-', dir=SIM / 'generated-src'))
    source = work / 'Pass.cpp'
    source.write_text('''#include "Outer.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Tools/Plugins/PassPlugin.h"
using namespace mlir;
struct HeaderPass : PassWrapper<HeaderPass, OperationPass<ModuleOp>> {
 MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(HeaderPass)
 StringRef getArgument() const override { return "header-value"; }
 void runOnOperation() override {
  getOperation()->setAttr("chipyard.header_value", IntegerAttr::get(IntegerType::get(&getContext(),32),HEADER_VALUE));
 }
};
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo mlirGetPassPluginInfo() {
 return {MLIR_PLUGIN_API_VERSION,"HeaderValue","1",[]{PassRegistration<HeaderPass>();}};
}
''')
    (work / 'Outer.h').write_text('#include "Value.h"\n')
    (work / 'source.mlir').write_text('module {}\n')
    libraries = []
    for value in (17, 29):
        (work / 'Value.h').write_text('#define HEADER_VALUE ' + str(value) +
                                      '\n')
        library = plugin(args.circt_opt, os.environ.get('CXX', 'c++'),
                         work / 'cache', source)
        libraries.append(library)
        result = subprocess.check_output([
            args.circt_opt, work / 'source.mlir', '--load-pass-plugin=' +
            str(library), '--pass-pipeline=builtin.module(header-value)'
        ],
                                         text=True)
        assert 'chipyard.header_value = ' + str(value) + ' : i32' in result, result
    assert libraries[0] != libraries[1]
    assert plugin(args.circt_opt, os.environ.get('CXX', 'c++'), work / 'cache',
                  source) == libraries[1]
    print(
        'PASS: nested local header changes produce a new executable plugin; unchanged inputs reuse it',
        flush=True)


if __name__ == '__main__':
    main()
