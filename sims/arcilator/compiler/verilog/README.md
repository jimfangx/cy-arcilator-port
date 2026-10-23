# SystemVerilog frontend compatibility

This frontend fixes interface-array imports in the `firtool-1.162.0`
CIRCT SDK. It flattens each array element into directed ports, follows Slang's
rebased connection order, and resolves member accesses by interface instance.
Scalar interface signatures are preserved. The patch also predeclares interface
arrays and handles connections from sibling instances with canonicalized bodies.
It also preserves fractional delays before conversion to femtoseconds and rounds
time-to-integer conversions in the local time unit without overflowing on the
rounding addition. The normal native importer selects this cached frontend for
that SDK; an explicit `CIRCT_VERILOG` override takes precedence. Fixed-size
hierarchical `$bits` queries do not create live reference ports or function
captures. The native importer enables Slang's hierarchical-constant extension,
which the selected vendor RTL requires for these queries.
Output variable port declaration initializers are preserved as variable
initializers. Slang stores these on the port symbol; the SDK importer previously
skipped them. Input connection defaults retain their existing handling.
Instance output connections retain their declared source signedness when
extended to a wider destination. The SDK previously zero-extended both signed
and unsigned outputs. The native importer elaborates generated SV boundary
wrappers so source input defaults and width conversions run through these rules.
Post-conversion canonicalization processes uses before definitions. The SDK's
default traversal can repeatedly reassociate arithmetic in an unreachable
induction loop after pruning its constant entry branch, exhausting memory on
CVA6's CSR logic. The alternate traversal retains ordinary canonicalization
without a rewrite cap. `tests/run_native.py --variants cfg-loops` checks both
RV32 and RV64 specializations of a reduced CSR-style reproducer numerically,
including disabled branches, locked lanes and pre-edge register sampling.
One-bit pre/post increment and decrement also write their new value back to
the operand. The SDK computed the correct inverted bit but omitted the
assignment in that special case. CVA6's two-entry LSU FIFO consequently kept
both pointers at zero and eventually lost a store completion. The numerical
regression checks 16,384 combinations of values and side effects, conditional
execution, signed and unsigned one-bit `bit`/`logic` operands, and wider
controls. The previous frontend fails the first enabled one-bit case.
Fixed unpacked-array assignments also reshape equal-width packed elements,
recursively preserving nested array positions and converting bit/logic domains
through the existing packed conversion rules. This covers Caliptra AES's
single-element array of 128-bit values assigned to packed 4-by-4-by-8 elements.
The numerical regression checks 16,384 vectors at O0/O2, including nested arrays,
packed structs, singleton dimensions, signed types, explicit domain casts, and
rebased/reversed bounds. Width-changing leaf conversions remain unsupported.

Static procedural locals are allocated in their enclosing module instance or
package before lowering bodies. Module functions receive those references as
captures, including through transitive calls. This prevents local-variable
promotion from replacing retained state with the declaration initializer on
every invocation. Separate module instances still own separate storage.
`tests/run_native.py --variants static-storage` exercises these lifetimes through
native models at O0/O2. Globals retain their original qualified source name,
including function-local statics, so independently imported variants identify
the same storage without relying on declaration-order suffixes. The native
signal-memory pass binds constant-initialized globals using that identity.
Compilation-unit static locals are declared before top-level subroutines,
using the same global identities and per-model storage. Exported coroutine
metadata survives MooreToCore so non-suspending tasks can retain their ABI and
captured storage while becoming native callable functions. Nonconstant
static/global initialization still needs additional lowering.

The DPI ABI patch preserves explicit C-name aliases and records the original
Slang argument types before Moore lowering. Metadata distinguishes scalar atoms
from same-width packed bit/logic vectors, normalizes typedefs and enums to their
underlying ABI, and retains packed bounds, state domains, argument directions,
qualifiers, source type spellings, declaration scopes and time units. A module
attribute carries this information through MooreToCore; the native preparation
pass restores it onto DPI declarations and call sites before flattening.
`tests/dpi_abi.py --circt-bin "$CIRCT_BIN"` checks this provenance and the native
ABI admission guards. Recording a type does not imply its native bridge is
implemented; unsupported signatures still produce explicit diagnostics.

DPI exports retain their original C signature and captured-reference identities.
A private opaque reference anchor keeps export storage alive through generic
promotion and dead-code elimination, including variables used only from C.
Per-instance scope markers also retain stateless exports. Native signal-memory
lowering removes these anchors only while binding the corresponding storage
and compiled function entries. The export ABI metadata remains on those entries
for C wrapper generation; unsupported representations are diagnosed.

From `sims/arcilator`, with the full matching SDK installed:

```sh
python3 compiler/verilog/build.py \
  --circt-verilog "$CIRCT_BIN/circt-verilog" \
  --cache generated-src/compiler/verilog
```

The builder prints the resulting executable path. Select it with
`make CONFIG=... CIRCT_VERILOG=/printed/path/arcilator-verilog`. The builder
downloads pinned CIRCT importer sources and Slang headers, verifies the importer
source hashes, and links against the installed SDK's libraries. CMake 3.28 or
newer, Ninja, a C++20 compiler, Python, and `patch` are required. The build is
cached and locked so concurrent callers cannot use a partly linked executable.
An existing matching checkout can be supplied with `--circt-source` and
`--slang-source`; `--slang-build` reuses generated headers from that checkout.

Run the numerical regression with:

```sh
python3 compiler/verilog/test.py \
  --circt-verilog /printed/path/arcilator-verilog \
  --circt-bin "$CIRCT_BIN" \
  --work generated-src/interface-array-tests
python3 compiler/verilog/increments_test.py \
  --circt-verilog /printed/path/arcilator-verilog \
  --circt-bin "$CIRCT_BIN" \
  --work generated-src/increment-tests
python3 compiler/verilog/unpacked_reshape_test.py \
  --circt-verilog /printed/path/arcilator-verilog \
  --circt-bin "$CIRCT_BIN" \
  --work generated-src/unpacked-reshape-tests
```

`make frontend-test` runs all frontend numerical regressions against the
automatically selected cached frontend. `make native-test` includes that target.

Packed `case`, `casex` and `casez` operands use their bit representation, including
structs, arrays and unions. `packed_cases_test.py` checks all 4,096 inputs at O0
and O2. It requires `--union-library` pointing to the `UnionBits.cpp` pass built
against the matching SDK; `make frontend-test` supplies this automatically.

The pinned Moore-to-Core conversion also retains constant and dynamic bit/range
references into packed structs and unions as native reference descriptors.
The native memory pass consumes these descriptors with the original index width
and bounds. Packed types containing `time` retain the SDK's conversion rules.
The build verifies the converter source hash and links the patched converter
into the frontend alongside the importer.

The regression executes LLVM models against 1,000 independent input vectors
per case: scalar, singleton, pair, rebased and reversed ranges, multidimensional
arrays, forwarded ports, sibling instances, and interfaces without modports.
The last case promotes reference signals after flattening hierarchy.
An additional 1,000-vector regression checks signed input conversion and signed
versus unsigned output extension. The complete numerical suite has 9,000 vectors.

`tests/run_native.py --variants procedural` exercises fractional delays, integer
context time, constant strings, clocked state and diagnostics through the normal
native importer. `tests/procedural_reference.py --work PATH` checks that source
independently with Verilator. The separate `compiler/RTL.cpp` pass prepares
processes for the SDK's register and coroutine lowering. Full Radiance simulation
is not established by the frontend patch; unresolved signals and unsupported DPI
still fail explicitly in the native importer.

`nonoverlap.patch` corrects next-cycle implication and followed-by: the extra
cycle belongs after the antecedent, as `concat(antecedent, delay(true, 1))`.
The pinned importer instead delayed the antecedent itself, changing the sampled
condition. `nonoverlap_test.py` checks 20,480 source-level truth values against
forward-indexed traces, including a multi-cycle antecedent. It is included in
`make frontend-test`.

`unknown-comparisons.patch` resolves case equality and inequality with literal
X/Z masks before Moore-to-Core converts runtime values to the native two-state
representation. This keeps `$isunknown` from turning into an even-parity test
and preserves `$onehot0(0)`. Constant operands are evaluated before importing
case comparisons, retaining masks in replicated, concatenated, cast, and
arithmetic constant expressions. Comparisons between two four-state constants
retain their exact X/Z distinction. `$isunknown` on a two-state `bit` value
returns false directly, keeping the Moore operands well typed.
The converter follows packed bitcasts, concatenations, replication, and integer
extensions to recover constant masks. Explicit conversion to `bit` clears X/Z
before comparison. Case-statement patterns use Slang's constant evaluator;
dynamic patterns retain their evaluation order and captured-variable updates.
`unknown_comparisons_test.py` exhaustively checks known inputs through 13 bits
and checks wide inputs through 129 bits, including X/Z literal comparisons.
The native `bit-builtins` fixture also exercises packed aggregates, `bit`
storage, and function calls at the normal, O0, and O2 compilation settings.
The `case-effects` fixture checks that the first match stops later calls and
that identical pattern arguments still perform separate counter updates.

The native timing patches preserve inertial continuous assignments before LLHD
canonicalization, accept single-expression parenthesized delays, and preserve
fractional literals until lexical precision rounding. The real-to-integer
conversion patch supplies the rounding required by Moore's operation contract.
`compiler/FloatMath.cpp` converts that rounding operation to an LLVM intrinsic
before the pinned Arcilator input reader. Frontend cache identities include all
patch bytes, and the production LLVM model rule depends on the rounding pass.
