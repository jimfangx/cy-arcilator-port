The shared functions.svh is compiled unchanged by the native fixture and the
Icarus oracle. reference-{0,1}.txt retain the original Icarus12 output verbatim.
All 38 records are checked byte for byte, including quiet-mode file output.

The packed integer 32'h41004243 exposes a simulator disagreement: Icarus12
returns " ABC", Verilator5.022 returns "A BC", and stock Slang returns "ABC".
IEEE1800-2017 sections6.16 and21.3.3 specify removal of NULs when constructing
string values. The independently linked stock Slang ScriptSession evaluator
also returns "ABC" with length3 for the original $sformatf expression.
expected-{0,1}.txt change only this field of the raw corpus. The checker verifies
that restriction before comparing every byte with native output. The original
SV function source, raw outputs and exact discrepancy remain preserved.

packed_oracle.cpp contains the independent evaluator; packed_oracle.json records
its source, library and executable hashes and all eight expression results.
It links only libsvlang.a and libfmt.a from the matching CIRCT SDK, with the
pinned Slang source/generated headers used by compiler/verilog/build.py:

    c++ -O2 -std=c++20 -I$SLANG_SOURCE/include -I$SLANG_BUILD/source \
      -I$SLANG_BUILD/_deps/fmt-src/include -I$SLANG_SOURCE/external \
      packed_oracle.cpp $CIRCT_SDK/lib/libsvlang.a $CIRCT_SDK/lib/libfmt.a \
      -o packed-oracle
    ./packed-oracle

The executable prints each expression, string length and exact hexadecimal
bytes. It contains no native Arcilator formatting code or runtime.
