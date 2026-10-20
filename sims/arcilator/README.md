# Arcilator simulation

This experimental backend compiles Chisel-generated hardware with CIRCT's
Arcilator and LLVM. The default runtime is native: building and running it does
not invoke Verilator, link its runtime, or include its headers. The native
frontend defines `ARCILATOR`; some upstream collateral, including CVA6's bundled
preprocessed source, contains its own `VERILATOR` compatibility guards. Native
model bindings call the original selected C/C++ device
drivers through the DPI interfaces those drivers already expose.

```sh
source env.sh
cd sims/arcilator
make CONFIG=RocketConfig
make CONFIG=RocketConfig run-binary-fast \
  BINARY=/absolute/path/to/program.riscv
make CONFIG=RocketConfig debug
make CONFIG=RocketConfig run-binary-debug \
  BINARY=/absolute/path/to/program.riscv
make native-test
make integration-test ARC_TEST_CASES="dmi uart network"
python3 tests/coverage.py --runtime native
```

Native executables are named `simulator-native-<model-package>-<config>` and
`simulator-native-<model-package>-<config>-debug`. `make native` and
`make native-debug` are explicit aliases. The usual `CONFIG_PACKAGE`, `MODEL`,
`SUB_PROJECT`, `LOADMEM`, `TIMEOUT_CYCLES`, and shared run targets apply.

Use the shared `TOP` variable when the DUT module has a custom name, for example
`CONFIG=EmptyChipTopConfig TOP=EmptyChipTop`. Native builds track `TOP` and
`MODEL` in their collateral settings, refreshing cached hierarchy annotations
when the selected modules change.

Native NVDLA builds discover the selected large or small release's original
RAM and cell libraries automatically. Models are loaded on demand, preserving
the modules and macros in the selected preprocessed design. The
`rtl-libraries.json` dependency records the selected source and library contents;
editing, adding, or removing a library file refreshes RTL import. This also
tracks headers when a checkout preserves their size and timestamp.

Run `source env.sh` from the Chipyard root to activate the normal environment
and set `RISCV`. The standard setup (step 10 of `build-setup.sh`)
installs the full **firtool-1.162.0** suite pinned in `conda-reqs/circt.json`,
including the matching headers and runtime libraries. Source builds use the
same release's CIRCT and LLVM revisions and enable the Slang Verilog frontend
and Z3 support. The Makefile discovers
`arcilator` on `PATH` and uses `firtool`, `circt-verilog`, `circt-opt`, `opt`,
`llc`, `llvm-config`, and `mlir-translate` from the same installation.
No `CIRCT_BIN` argument is needed; it remains an optional override for an
alternate full SDK. Existing environments with the older CIRCT 1.75.0 must
rerun the CIRCT installation step; sourcing `env.sh` alone does not upgrade
installed tools. The base environment supplies `libz3` (Z3 4.x); setup connects
it to the SDK's runtime library search path. Native host
compilation uses C++17, GNU C11 for `.c` drivers, and zlib.

`ARCILATOR_FLAGS`, `ARC_FIRTOOL_FLAGS`, `ARC_RTL_FLAGS`, `ARC_OPT_LEVEL`,
`ARC_LLC_OPT_LEVEL`,
`ARC_CFLAGS`, and `ARC_BUILD_JOBS` control compilation. `ARC_RUNTIME_LIBS`
overrides the CIRCT runtime link libraries. Native artifacts live in
`generated-src/<model>.<config>/arcilator-native`, with observed compiler state
in `arcilator-native-debug`. Compiler outputs are renamed into place only after
successful compilation.
`ARC_OPT_LEVEL` selects LLVM IR optimization. Machine-code generation uses the
same level by default; `ARC_LLC_OPT_LEVEL` overrides it independently. For large
models, `ARC_OPT_LEVEL=1 ARC_LLC_OPT_LEVEL=0` keeps the IR optimizations while
using LLVM's faster machine-code pipeline.

The backend builds a small CIRCT pass against the installed SDK headers and
loads it into the matching `circt-opt`. It partitions the native hardware into
bounded modules before Arcilator analyzes dependency sets, keeping each memory
with its ports and copying immutable diagnostic literals into their users.
Before outlining, constant element and slice reads through cyclic array muxes
and constant-index updates resolve per observed lane. This separates assigned
lanes from unused retained lanes without inserting default values. Only dead,
pure combinational cones are removed after checking all remaining users;
whole-array observers, state, named symbols and side effects keep their inputs
live. Unresolved or dynamic feedback remains for the compiler to diagnose.
Slice result aliases retain their declared type through a layout-preserving
cast around canonical array construction.
Finite shifted OR/AND/XOR recurrences become acyclic parallel prefix networks
when every dependency advances toward a constant identity seed. The rewrite
supports both directions and positive constant strides. Zero, dynamic and
arithmetic shifts, non-identity padding and dependent bases retain their
original feedback for the compiler to diagnose.
`ARC_PARTITION_OPS` and `ARC_PARTITION_PORTS` set the budgets (4,096 operations and
16,384 ports). Arcilator still compiles the complete design into one model with
the original native binding interface. Arrays remain arrays for Arc's native
lowering; singleton concatenations are normalized to avoid a CIRCT type error.
The default `ARCILATOR_FLAGS` requests stock function splitting at 8,192
operations. This CIRCT release does not apply that option to the final model
evaluation routine. `compiler/model.py` resumes the SDK pipeline after arc
conversion. `compiler/StateProperties.cpp` preserves the SDK's enable/reset
inference while constructing correctly typed zeros for aggregate state; the
stock pass assumes an integer type and can crash on enabled arrays. It then
normalizes array concatenations immediately before singleton-type
removal, and exports the layout using the SDK's model-info implementation.
At that same boundary it removes zero-width operands from concats with live
results, preserving their other operands and attributes. The SDK otherwise
erases such concats while removing i0 types. A regression checks 32,768
leading, trailing, nested and interleaved empty-operand combinations at O0/O2,
including functions with an additional empty result.
The native i0-removal pass also converts every function block and forwarded
branch/switch operand. Singleton arrays retain their values across coroutine
CFG edges, nested aggregate constants convert recursively, and nonzero-width
singleton indices keep bounds checks. `tests/singleton_cfg.py` checks 65,536
independent results at each of O0/O2, including zero-width arguments and
preserved function attributes.
It also outlines large allocated-model procedures using `ARC_MODEL_OPS`
(4,096 operations; set 0 to disable outlining). Stack allocations and layout
anchors stay in their original frame; helpers retain the existing storage
offsets and remain separate during LLVM optimization. The compiler port budget
also applies to helper arguments and results. This path uses the stock CIRCT
executables and passes built against their matching SDK headers.
For a fresh large-model build, `ARC_MODEL_OPS=16384` can reduce the cost of
passing values between helpers. An isolated full `GemminiRocketConfig` model
with LLVM IR level 1 and machine-code level 0 reduced its model procedures from
198 to 97 while retaining a byte-identical state layout. Over the same measured
prefix of 34,000 evaluations, evaluator time fell from 56.13 s to 18.53 s;
both executions reached the same simulation time and scheduler call counts.
The complete original DMA workload then passed in 73,246 root cycles, matching
the 4,096-operation baseline. The timing measurement covers that prefix, and
does not establish a complete-workload or cross-config speedup. Reports are in
`output/development/native-gemmini-model-budget-benchmark-20261018T222206Z/report.json`
and
`output/development/native-gemmini-model-budget-complete-20261018T222624Z/report.json`.
Subsequent measurements with the byte-identical baseline executable varied
substantially, so benchmark the selected workload when tuning this budget.
Defaults remain 4,096 operations. Per-stage `*-settings.json` dependencies
record the effective frontend, filelist, partitioning, model, LLVM code-generation
and host compiler/link options. Changing those options or tool paths rebuilds
the affected stages and their consumers. Repeating the same settings preserves
timestamps; changing only `ARC_BUILD_JOBS` does not rebuild compiled artifacts.
Normal/debug and native/hybrid settings live in their respective build directories.
Existing builds without settings files rebuild once to establish these dependencies.
`tests/build_settings.py --work /path/to/new/verification` checks real incremental
Rocket builds, restored baseline artifacts, and CPU execution in a fresh isolated
checkout using the configured `CIRCT_BIN`, `RISCV` and `CXX`.

`compiler/LargeReturns.cpp` keeps private LLVM structure returns with at least
64 fields out of the machine calling convention. Eligible direct calls use one
aligned scratch buffer per caller. Loads capture each result immediately after
its call, so later calls can reuse the buffer even when the original field
extracts occur much later. Field offsets come from the target data layout,
including packed structures and nested aggregate fields. Public functions,
address-taken functions, must-tail calls, and whole-aggregate return users keep
their original signatures. The transformed helpers remain separate during
optimization, and memory attributes are adjusted for the buffer writes.
Before serializing LLVM IR, `compiler/CompactStructs.cpp` gives large literal
structure types shared names. This avoids repeating thousands of field types
at every aggregate operation. It preserves packed layouts, existing identified
type bodies and their reachable literal types. A regression compares literal,
named and buffered forms at O0/O2, including aggregate CFG edges, globals and
GEPs, and checks name collisions, idempotence and serialized size.
`compiler/LargeArguments.cpp` similarly places private helpers with at least
64 arguments behind an aligned input buffer. Target layout determines each
argument's offset, including pointers, floating-point values and nested packed
aggregates. Each caller reuses one buffer and fills it immediately before the
call. Public and address-taken functions, must-tail callers and callees, and
functions requiring special parameter ABI attributes keep their signatures.
`compiler/llvm.py` applies both passes before lowering wide arithmetic,
publishing the result only after every stage succeeds.
`make llvm-native-test` checks 49,152
numerical results across original and transformed O0/O2 builds, including
multiple return blocks, nested callers, late extracts, 65-bit fields, pointers,
floating-point bit patterns, and failure preservation. Another 16,384 results
check the combined input and return buffers, nested scratch reuse, pointer
effects, mixed argument types and preservation of ABI-sensitive functions.
`compiler/LowerArrays.cpp` adapts the SDK's array lowering for coroutine control
flow. Array values can cross switch and branch edges, and return buffers follow
the function signature across all return blocks. The regression executes
120,000 checks, including array block arguments in otherwise scalar functions
and independent buffers for multiple array results. Another 48,000 checks cover
mixed nested-value and flat-buffer signatures, with a 3-by-8 matrix crossing
branches and function calls. Return-buffer ownership tests cover scalar results
before arrays, borrowed references, and preservation of argument attributes
after new buffer arguments are inserted. Buffer slots follow the original
function signature, and LLVM lowering reconciles aggregate conversion casts.
Isolated `arc.execute` regions use internal helper functions with that same
buffer convention, including nested regions, loops and multiple exits.
Aggregate array constants initialize buffers through HW conversion instead of
LLVM integer constants. A numerical regression checks FPnew's format table,
nested struct/array fields, zero initialization, CFG returns and immutable
updates at LLVM O0/O2.
Effectful combinational DPI regions and bare unclocked calls become
input-sensitive LLHD processes before Arc conversion. Each region runs once at
startup, then on changes to any external value it reads. Branches and repeated
calls retain their original execution points; separate call sites keep separate
activation state. Aggregate, real and clock observations compare their packed
bits, including signed zero and NaN encodings. Disabled unclocked call results
use zero as Arc's two-state value for SV X, without invoking the callback or
retaining a previous output. Existing timed and clocked processes keep their
activation rules. Reference-bearing regions and mixed clocked regions remain
for explicit compiler diagnosis. A regression checks 2,562 evaluations at each
of O0 and O2 and through native runtime settling, including duplicate calls,
input/output/inout ABI, padding, unrelated inputs and pre-edge register sampling.
A separate 16-stage chain hides every intermediate DPI result from public
ports. It checks 512 cycles of simultaneous data/clock changes, synchronous
reset, stable-input callback counts and deferred post-edge activation.
`tests/run.py --variants dpi-chain` exercises the same chain through the shared
adapter and an independent SystemVerilog checker.
Procedural scalar DPI calls in Arc coroutines invoke the SDK's C ABI wrappers
at their original control-flow point. A compiler regression checks narrowed
return values, edge/enable gating, and exact callback counts over 100 cycles.
Original integer output formals receive a constant zero input before the SDK
builds its wrappers. Internally they use its inout copy-in path, which has the
same external C pointer ABI. This gives unwritten output bits a deterministic
native two-state value on every call. Real inouts keep their original value and
refs keep their original pointer; no previous actual-output dependency is added.
Original output indices are recorded before rewriting, and declaration aliases
are checked for compatibility while their original directions are still intact.
The regression checks 2,049 evaluations at O0, O2 and through the native runtime,
including partial 129-bit writes, Boolean outputs, interleaved formals, both
aliases, inout copy-in/out and local reference-buffer identity.
`tests/seeded_dpi_poison.py --circt-bin "$CIRCT_BIN"` also fills SDK integer
allocations with ones before their copy-in stores. The initialized model still
passes at O0 and O2; removing just output seeding fails deterministically at
both levels. This negative check uses defined buffer contents throughout.
Non-suspending SystemVerilog tasks inline at their original call sites before
signal-write splitting and process lowering. A fixed-point analysis proves the
entire call graph has no waits, halts, nested processes or unresolved calls.
Recursive or suspending task calls remain unchanged. Inlining preserves task
branches, multiple returns, output and inout arguments, automatic local storage,
and ordered side effects within each activation. Compiler tests execute nested
task callbacks at LLVM optimization levels 0 and 2. An imported SystemVerilog
fixture checks task outputs, inout copy-back and exact print activations through
the native runtime.
Dynamic `%f`, `%e` and `%g` trace formats use a native formatter with the
matching SDK's double-conversion and `snprintf` semantics. Mixed messages retain
the SDK's integer, character and literal formatting at the original print point,
including stream selection and activation guards. Compile-time format expression
trees remain in their original frame when outlining would hide their definitions.
The formatter regression generates 6,480 SDK constant-folding references and
checks 4,320 runtime formatter calls at each of optimization levels 0 and 2,
covering half/single/double values, rounding and padding, signed zero, NaNs,
infinities, subnormals, embedded NUL/UTF-8, and mixed 129-bit integers. An imported
SV fixture verifies 256 complete trace messages over 512 clock cycles, including
pre-edge state and print suppression.
String filenames support native `$fopen`, `$fclose`, `$fflush` and file-output
streams, including descriptor and multichannel writes, append modes and final
blocks. Packed integer `%s` arguments
retain their declared width and the SDK's byte order and zero-byte handling;
constant strings retain padding and alignment. File writes follow the RTL's
guards and run without `+verbose`; console diagnostics retain their existing
print controls. The imported file fixture compares complete binary outputs,
channel fanout and exact operation counts over 128 cycles.
Boolean DPI arguments and returns carry C ABI zero-extension on declarations
and call sites. The compiler regression also checks multiple output pointers,
Boolean results, and inout copy-in/copy-out against independent arithmetic.
Before the SDK creates C ABI wrappers, imports from separate RTL specializations
that name the same C function share one compatible declaration. Argument names
and locations may differ; types, directions, order and semantic attributes must
agree. Call sites remain separate, with their original control flow and effects.
Conflicting declarations produce an error identifying both source locations.
The RTL importer uses local canonicalization on LLHD regions: generic sparse
constant propagation can discard a yielded DPI result in the GPU design.
The FIRRTL frontend repairs omitted separators before synchronous-memory
read-under-write modes while preserving `new`, `old`, and `undefined`. It removes
Chisel enum metadata that this firtool release cannot resolve on memory ports;
memory initialization, blackbox, and preservation annotations remain intact.
Native waveforms display the underlying packed values.
Integer shifts wider than 1,024 bits call native C++ byte operations directly
from the compiled dataflow. `compiler/llvm.py` supplies always-inline LLVM
bridges with byte-pointer arguments, avoiding both LLVM's costly variable-shift
legalization and extra foreign ports or settling rounds. Width, sign extension,
padding, and oversized shift amounts remain exact. Both runtimes link the same
arithmetic helper; the hybrid regression compares it with an independent SV
reference.

## Native models

Large canonical FIRRTL memories use native sparse pages rather than embedding
all words in the Arcilator state copied during settling. Native builds lift
arrays of at least 1,048,576 bits with read latency zero or one and write
latency one. Each port retains its clock and enable; masked writes, ordered
write conflicts, read-write ports, and old/new read-during-write modes are
preserved. Unwritten words use native two-state zero. Initialized memories,
externally referenced symbols, unsupported latencies, and unrecognized users
retain their original compiler representation. The hybrid build retains its
existing memory lowering. `python3 tests/run_native.py --variants sparse-memory`
checks synchronous and asynchronous reads, non-byte masks, multiple ports,
independent clocks, invalid addresses, and sparse capacity above a trillion
words at O0 and O2.

Conditional writes retain a reaching definition even when the process has no
later read of that register. This lets uninitialized registers with enabled
writes preserve their prior value while disabled. `write-only` checks 13/65-bit
registers, independent clocks and holds at normal/O0/O2 settings. Partitioning
also checks distinct captured inputs and escaping results before rejecting an
oversized operation bundle: internal CFG values do not consume hardware ports.
`tests/partition_boundary.py` checks the real limit and optional before/after
comparisons; previously accepted fixture and complete Rocket/ManyPeripherals
partition outputs remain byte-identical in the recorded equivalence run.

`native.py` generates connection tables from the actual compiler state layout
and `partition.json`. `native_build.py` compiles the selected original drivers
with `native/`, which provides the scheduler and compatibility interfaces.
`native/models.def` is shared by the C++ factory registration and RTL importer.
The bindings currently include:

- SimDRAM with the original simple-memory and DRAMSim2 implementations, SimTSI,
  SimUART, remote-bitbang SimJTAG, TestchipSimDTM/SimDTM, and SimBlockDevice.
- SimNetwork, TraceROM, TrafficEval ingress/egress, and the software branch
  predictor and BTB drivers.
- The older Sodor SimDTM variant, including its 100 ps input/output delays,
  Radiance memory trace readers/loggers and emulator/fuzzer wrappers, and
  Dromajo's initialization, commit, and trap interfaces.
- SpikeBlackBox, Cospike system/device/memory/CSR registration and instruction
  checking, and all three debug-ROB wrappers.
- Generated asynchronous-register banks, gated and plusarg-controlled clocks,
  clock flops and logic, digital IO cells, GPIO attach nets, and SimSPIFlashModel.
  SPI flash memory operations use the original `plusarg_file_mem` driver.
- Single-producer conditional process signals with immediate or nonblocking
  zero-time writes. Their native storage retains its previous value when the
  write strobe is false, preserves explicit packed initializers, and commits
  writes after peer sequential models sample their inputs.
- Remaining packed reference storage, including memories whose partial writes
  mix blocking and nonblocking assignments. `ArcSignalMemory` owns storage per
  instance and preserves initializers, indexed elements, slices and struct
  fields. Branches and loop backedges transport the selected storage handle,
  version and bounded range, including merges of independent memories and
  ranges that shift between iterations. Blocking writes update storage at their original execution point;
  queued nonblocking writes commit in order after sampling and update only
  their addressed bits. Out-of-range reads return native two-state zero and
  writes cannot spill into neighboring elements. Read-only memories retain
  change detection when merged with a memory written by the same combinational
  process. Unsupported reference escapes and nonzero physical delays retain
  explicit errors before any root of their reference component is changed.
  `python3 tests/run_native.py --variants cfg-memory` checks branch merges,
  loop-carried ranges, clipping, blocking/NBA order, peer sampling and implicit
  sensitivity at O0/O2, plus rejection without partial mutation.
- Transparent latches, including the original Ibex clock gate, settle before
  clocked models sample their inputs. Blocking and nonblocking latch writes
  retain their distinct scheduling, packed initializers, and hold behavior.
  Unconditional combinational signal feedback preserves its initializer before
  the SDK promotes signals to SSA.

Remaining integer wire cycles use native feedback bindings. The compiler first
tries to prove a finite, initializer-independent fixed point with the SDK's
LLVM/Z3 interface. Successful proofs produce acyclic hardware. Otherwise the
native scheduler iterates the exposed feedback wires, retaining their last
settled values between evaluations. Nonconvergent logic fails with an explicit
diagnostic. This path is selected only for the native runtime; hybrid compilation
keeps its existing loop diagnostics. Hardware literals are rematerialized inside
partitions so those proofs and ordinary canonicalization can see their values.

Other ordinary RTL blackboxes are compiled from their selected Verilog sources
with `circt-verilog`. `native_hw.py` specializes each parameter combination,
namespaces imported hierarchy, adapts bit/clock and packed aggregate ports, and
incorporates the resulting HW into the same native model. A generated
SystemVerilog wrapper connects the declared Chisel ports by name, letting source
elaboration apply omitted-input defaults, truncation and signed/unsigned output
extension. The matching frontend patch preserves the declared output signedness
during those conversions. Constant string parameter comparisons fold byte for
byte; dynamic string operations retain their ordinary lowering. Selected sources share
a compilation unit and receive the `ARCILATOR` macro. Native builds take FPnew
packages from the selected blackbox resources, avoiding duplicate global
Radiance package definitions while retaining the FPU preprocessor defines.
`tests/package_resources.py` checks this selection, the original vendor package
constants, and the package flags used by the hybrid and Verilator flows.
Interface reference signals
are promoted after inlining; clocked state is inferred without removing its
diagnostics. Timed and effect-only processes use Arc coroutines, including
explicitly carried process outputs across suspension points. Before register
inference, static signal references are split at their procedural write
boundaries so independent bit, packed-array lane, and nested-structure writers
retain their original ranges. Reads reconstruct the packed value; write delays,
enables, initial values, and overlapping drivers are preserved. Dynamic or escaped
references remain for the ordinary lowering to handle. Aggregate bitcasts are
expanded with the SDK's element operations so constant-index reads retain
precise dependencies for Arc's loop checks. `rtl.json` records each imported source
and specialization. The GCD integration runs its original Verilog implementation.
Repeated constant drivers are coalesced only when the entire driver set agrees
on one unconditional immediate constant. Native import also coalesces immediate
unconditional drives from inferred registers when their input values, clocks,
reset controls, presets and attributes match. The registers start equal under
native two-state initialization and keep the same transfer function. Other
driver sets remain intact. Undriven, unescaped signals resolve to their explicit
constant initializer, or to zero under the native two-state policy; aliased
signals and dynamic initializers retain their ordinary lowering.
Struct updates at a control-flow merge can move to incoming branches when the
merged value has no other readers. Only pure, speculatable field computations
are copied. This exposes complete field assignments to normal canonicalization
and removes false dependencies on old local storage. Partial assignments and
side effects remain observable.
Writes left inside combinational regions use the SDK's drive hoister before
control-flow removal, retaining their values, enable conditions and delays.
Finite loops in startup processes are expanded using the RTL's original integer
width, signed comparisons, and wrapping arithmetic, within the SDK's budget of
fewer than 1,024 iterations. Constant memory contents exposed by those loops
become presets on the inferred register. Aggregate presets use a packed integer
register with layout-preserving conversions around it. Conditional, delayed, or
additional nonconstant drivers remain intact; conflicting presets fail explicitly.
When RTL instantiates a Chisel module already present in the HW design, the
importer supplies its interface during elaboration, externalizes that interface
before inlining, and connects it to the original native implementation. Packed
ports and clocks are adapted in both directions; instances retain separate state.
Native hardware bodies retain their implementation when their names match a
built-in binding.
Ordinary DPI function imports also compile into native C ABI bridges. The
frontend records the original argument types, directions, C-name aliases,
qualifiers, declaration scopes and timescales before Moore lowering erases those
distinctions. Bridges support signed/unsigned byte, shortint, int and longint
atoms; scalar bit/logic; packed two-state and four-state vectors, structures,
unions and integer arguments; real/shortreal values; strings; and opaque chandles. Packed arguments use
32-bit words, with interleaved aval/bval words for four-state values. Native
unknown bits become zero. Output buffers start at zero and inouts copy in their
logical values. Scalar and floating returns use their C calling convention.
String inputs use immutable C strings. Returned and output/inout strings are
copied into the model's string pool before subsequent C calls can overwrite
their backing buffers; null pointers become empty strings. The native storage
handle remains separate from the original SystemVerilog ABI metadata.
`tests/run_native.py --variants string-imports` checks C-name aliases, pure and
effectful calls, UTF-8 and empty strings, borrowed input pointers, mutable C
buffers, reset/enable gating, retained values and model recreation at normal/O0/O2.
Opaque `chandle` inputs, outputs, inouts and returns use `void *` in C, as
specified by the [DPI type mapping](https://www.accellera.org/images/downloads/standards/sce-mi/SCE_MI_v20-3-22-2007.pdf).
The compiler transports their pointer bits through 64-bit native storage on
the supported 64-bit host. C retains ownership of the pointed-to objects.
Null handles, equality tests, conditional transport and captured handles in
SV exported functions preserve their values across calls.
`tests/run_native.py --variants handles` checks the public C ABI in both
directions, pointers above 32 bits, C-name aliases, mixed string/handle calls,
reset/enable gating and model recreation at normal/O0/O2.
Context function imports bind to their physical declaration instance before RTL
flattening. This includes hardware and initial processes, shared module definitions,
hierarchical SV function calls, generated blocks, escaped instance names, and
package declarations. The importer retains source hierarchy and lexical versus
hierarchical lookup through Moore-to-Core conversion. A compiler pass carries a
scope handle separately from the original DPI signature; the C function receives
its original arguments. Context calls restore the selected scope after C returns,
including callbacks that select another scope for nested SV calls. Compiled SV
exports and imports share the same native scope objects.

`tests/run_native.py --variants context-scopes` checks two copies of a shared RTL
hierarchy, generated/escaped leaves, initial callbacks, C-name aliases, package
calls, private SV functions, nested C/SV callbacks, reset/enable behavior, scope
user data and model recreation at normal/O0/O2. Compiler regressions check late
uniqued declarations, preserved frontend provenance, nested function inlining,
hierarchy mapping and idempotence. Recursive context SV functions still require
native call frames and fail explicitly. Imported tasks, packed returns,
and integral `time` formals still require a native binding.
Missing ABI provenance and unsupported RTL constructs fail explicitly.

Fixed unpacked-array imports now marshal integral and packed-vector elements
through contiguous native C buffers. Nested dimensions follow declaration order;
packed vectors use canonical 32-bit words, including separate value/unknown words
for four-state vectors. These C buffers account for per-element padding independently
of the compact SV storage. Original array bounds and element metadata remain on the
DPI declaration. Output and inout arrays are copied back before RTL continues.
Aliases may use different SV bounds with the same C element type; conflicting C
pointer element types or constness fail explicitly.

`tests/run_native.py --variants unpacked-imports` checks scalar arrays, negative
and descending bounds, 2D 65/129-bit vectors, padding, unknown-bit masking,
input/output/inout, context scope, exact activations and model recreation at
normal/O0/O2. `tests/native_unpacked.py` checks shape/type/direction admission,
C prototype compatibility, preserved metadata and idempotence.

Open-array imports accept fixed unpacked actual arrays of these same element
representations, including mixed fixed/open dimensions. Each call retains its
actual bounds and uses an opaque `svOpenArrayHandle` at the C boundary. The
runtime implements dimension/bound queries, contiguous C storage, element
pointers, scalar/vector accessors, and packed select helpers. Output and inout
handles refer to temporary C buffers whose values are copied back before RTL
continues. Source formal metadata stays separate from the actual-shape transport
metadata. Existing SimDRAM descriptors keep their byte payload behavior.

`tests/run_native.py --variants open-arrays` checks negative/descending bounds,
1D–3D arrays, mixed fixed/open dimensions, C aliases with different actual bounds
and lengths, 65/129-bit vectors, scalar logic and unknown masking, partial output
initialization, context scope and model recreation at normal/O0/O2.
`tests/native_open.py` checks provenance and prototype guards and the standalone
runtime API, including generic 4D indexing and selects across word boundaries.
Fixed and open unpacked-array imports also support `chandle` and `string`
elements. Chandles use native pointer entries; C retains ownership of the
pointed-to objects. String entries use C string pointers, and output/inout
strings are copied into the model pool before subsequent C callbacks can
change borrowed buffers. Nested handle/string storage becomes native 64-bit
array elements while the original element kinds and actual bounds remain in
separate ABI metadata. Constant default array constructors are materialized
before state inference; effectful initialization expressions remain runtime data.

`tests/run_native.py --variants pointer-arrays` checks fixed and open arrays,
2D chandles, 3D strings, UTF-8 and empty/null strings, partial output defaults,
borrowed mutable C buffers, shifted-bound aliases, context scope, reset/enable
behavior and model recreation at normal/O0/O2. `tests/native_pointer_arrays.py`
checks recursive ABI marking, prototype/context provenance, typed default
initializers and retained initializer effects.
Dynamic actual arrays, open packed dimensions, and other unsupported element
representations still need compiler support.

Imports of one C function retain separate wrappers when their context, purity
or task qualifiers differ. Equivalent declarations still share their bridge;
combining declarations cannot remove a required scope check.

The DPI compatibility layer implements the scalar/vector/open-array operations
used by the ported drivers. Its VPI support currently provides argument/product
queries through `vpi_get_vlog_info`; arbitrary VPI hierarchy traversal, callbacks,
and force/release are not implemented. Additional native bindings can use the
`Binding`/`register_binding` API.
Native callbacks expose DPI scopes through `svGetScope`, `svSetScope`,
`svGetNameFromScope`, and `svGetScopeFromName`. Names use the original top name
and partition instance path, separated by `/`. Context is restored between
callbacks, including nested calls and exceptions, and is local to the host
thread. Scope lookup entries expire with their owning model or binding.
Both explicit `advance()` calls and the standalone `run()` loop establish the
scope for timer queries and timed callbacks.
The context binding pass preserves declaration scope through flattening; physical
scope names follow the native hierarchy, including CIRCT's generated instance
names. `svPutUserData` and `svGetUserData` associate caller-keyed data with these
scope objects and discard it on destruction. The optional `svGetCallerInfo`
query returns zero because source caller information is not provided.

Builds place the matching CIRCT SDK include directory ahead of toolchain
includes. RISC-V toolchains can ship a different LLVM header version, which
must not define types used by the linked CIRCT/LLVM runtime.

For the tested SDK, native RTL import builds and caches a
[patched frontend](compiler/verilog/README.md) for interface arrays and timing
conversions. This first build requires CMake 3.28 or newer, Ninja, `patch`, and a
C++20 compiler, and downloads verified sources at pinned revisions. An explicit
`CIRCT_VERILOG` override uses that executable directly. `ARC_FRONTEND_PATCHES=auto`
or `off` overrides the patch-selection policy. Fixed-size hierarchical `$bits`
queries are elaborated as type queries, without routing their operands as live
signals. Numerical regressions cover
array dimensions, directions, forwarding, and sibling instances. Full Radiance
configuration runtime remains unverified.

## Scheduling and waveforms

The native scheduler supplies Arc's context time and services coroutine wakeups
between clock and foreign-device events. Its internal time is in femtoseconds;
the existing binding `time()` and `advance()` interfaces use picoseconds, while
`time_fs()` exposes the exact current time. Compiler-generated accessors use the
SDK's time operations so the host does not hardcode context storage offsets.
Their names are recorded in the layout JSON alongside the unchanged SDK state
layout. Models that read context time produce VCD files with a 1 fs timescale;
other models retain the 1 ps timescale.
Timestamp reads used within one process activation become graph inputs for
register inference. Direct uses across a wait retain the earlier sampled time.
Physical time arguments formed by integer-to-time conversions are carried as
64-bit femtoseconds across waits so coroutine frames have a defined layout.
Timestamped diagnostics keep their original conditions, values and print counts.

Combinational logic settles with native clocks held at their committed levels.
Convergence includes internal coroutine state, so a chain can settle even when
none of its intermediate results are public outputs. Sensitized combinational
DPI processes receive an internal sampling control threaded through the hardware
hierarchy. A process activated during native register sampling keeps its previous
result and waits for that control to clear before calling C code. Normal waits
observe only the original RTL inputs, preventing extra callbacks on clock phases.
On an observed edge, the scheduler snapshots settled inputs, lets every foreign
sequential model sample pre-edge values, then commits native and foreign next
state. Clock aliases share phase inputs; probe references are resolved before
flattening. Timed clocks and JTAG delays use physical picosecond events.

Asynchronous registers are grouped by clock/reset into banks of at most 1,024
bits. The native binding samples both clock and asynchronous-reset edges.
Registers wider than a bank are split and reassembled with their exact reset
values. Packed arrays and nested structures use CIRCT bitcasts to preserve their
layout across the bank interface. No SystemVerilog process is needed to execute
these banks.

GPIO and SPI pins resolve enabled drivers before further combinational
settling. The native model is two-state: floating and conflicting bits resolve
to zero, and uncontested driven bits retain their values. The current pin
resolver supports nets up to 64 bits. More complex analog behavior is unverified.

`debug` preserves native registers, wires, and ports. `+vcdfile=PATH` or
`+arc-vcdfile=PATH` writes a native VCD with physical timestamps and flattened
signal names. `+dump-start=N` starts snapshots at root clock cycle N. Normal
builds expose boundary ports; debug builds also expose observed internal state.
Native memory contents are omitted. Waveform generation uses no Verilator code.
Imported RTL state names include the generated port wrapper's `rtl` instance
level; the wrapper source and top name are recorded in `rtl.json`.

RTL assertions and failing native stops fail the simulation; successful stops
finish it. Single-cycle LTL properties lower to Boolean checks while retaining
the clock edge and enable. This includes nested implication, negation,
conjunction/disjunction, intersect/concat of single-cycle sequences, exact zero
delay and exact one repetition. Positive, negative and both-edge assertions
sample settled values before registers update. Finite sequence antecedents also
compile into native sampled monitors. These support bounded delays and positive
repetitions, concatenation, sequence AND/OR/intersection, nested implications and
property conjunction with pointwise consequents. Each endpoint checks its
consequent on the original clock edge. Asynchronous disable clears pending
activations; `$past` keeps sampling on its own clock independently of assertion
enables. Simulation checks clocked assumptions through the assertion callback.
Temporal monitors currently support positive or negative edges, at most 4,096
cycles, and a 100,000-expression construction budget. Unbounded operators,
zero-count repetitions, general temporal consequents/negation/disjunction,
both-edge temporal monitors and explicitly clocked subproperties remain intact
for compiler diagnosis. The frontend corrects the pinned SDK's placement of the
one-cycle delay in non-overlapping implication and followed-by operators.
`+verbose` enables generated diagnostics after reset. The native
standalone driver currently uses the standard 1 ns root clock and 777.7 ns reset
delay; custom `CLOCK_PERIOD`, `RESET_DELAY`, `PRINTF_COND`, and `STOP_COND` macro
overrides still require parity work. State initialization is deterministic zero.

## Validation

`make test` (also `native-test`) compiles and executes fixtures without Verilator. They cover
2,051-bit integer, array, and nested-structure asynchronous registers, resets
asserted between clock edges,
synchronous state, the original TraceROM driver with byte masks/final-beat and
backpressure checks, RTL parameter specializations and nested hierarchy,
bit/clock conversions, edge sampling, and normal/debug VCD timestamps and dump
start behavior. Additional fixtures check the older Sodor DTM's edge and delay
semantics, original Radiance trace and C++ emulator/fuzzer wrappers, wide packed
lanes, reset/backpressure, unused root-clock scheduling, arbitrary-width integer
plusargs, reset-gated print/stop controls, 33,000 input ports, shared diagnostic
literals, singleton and repeated array concatenation, and array state/read/reset
semantics. The repeated-array case reproduces the BOOM4 type error in the stock
pipeline and checks all resulting bits after clock edges and resets.
A cyclic-array fixture checks observed lanes, opaque fallback arrays, both mux
branches, update order, resets and registered sampling over 512 cycles. Separate
guards check declared alias types, nonzero slice offsets and lane order, preserve real scalar and
array feedback, partial/dynamic/out-of-bounds accesses, whole-array observers and
DPI effects, and reproduce the original false-loop failure without the rewrite.
The imported SystemVerilog procedural fixture checks native state together with
one diagnostic per edge, constant string formatting, 1.5 ps autonomous delays,
integer `$time` rounding, packed arrays/structures, and unconnected output ports.
The optional SV comparison runs that same source in Verilator. The native
fixture builds and runs without Verilator.
Another fixture checks a native-to-RTL-to-native pipeline with two independent
instances, packed return structures, hierarchical type queries, and state
sampled before the clock edge.
The conditional-signal fixture checks a nonzero 129-bit output-port initializer,
reset, write enable, timed suspension, retention between writes, and a native
flip-flop sampling the old value on the same edge as a nonblocking write.
The latch fixture checks 3,072 steps through the original Ibex clock gate,
high-phase hold, test enable, packed/structure initializers, transparent data,
nonblocking publication and simultaneous peer sampling. A feedback fixture
checks 512 cycles of explicit scalar/129-bit initialization, set/clear/hold,
native wire feedback and peer sampling, then requires an oscillator diagnostic.
Another fixture checks 4,096 cycles of solver-proved 13-bit feedback. Structural
guards preserve genuine state, unsupported operators and effectful dependencies,
and test literal ownership at three partition sizes.
A scope fixture checks two native instances through initialization,
combinational evaluation, sampling, timer queries and timed events, including
lookup identity, nested calls, exception restoration, thread isolation and
cleanup when scopes are destroyed.
A pointwise-property fixture checks exact assertion callback counts on positive,
negative and both clock edges, pre-edge register values, reset and enable
suppression, and failing termination through the standalone run loop. Separate
truth tables check 160 Boolean property results while preserving assertion
edges, enables and labels; guard cases retain genuine temporal cones and shared
temporal users.
A signal-coherence fixture checks five equivalent register drivers, a 44-bit
counter with wrapping feedback, pre-edge sampling, reset, 257-bit and aggregate
undriven outputs, partially driven nets, a nonzero 129-bit initializer, and
complete struct assignments across four branches. Guard checks preserve
different clocks, data, resets, presets, conditional or delayed writes, escaped
references, partially assigned fields, shared reads and opaque effects. Removing
struct-update motion reproduces the unresolved local aggregate signal.
A separate partial-write fixture checks bit banks, packed array lanes, nested
struct fields, two independent clocks, conditional enables, repeated reset,
feedback, and dynamic reads between edges against an independent state model.
It also checks a combinational prefix tree with separate packed-array writers.
Startup tests check nonzero scalar and memory initialization before the first
clock, including 13-bit lanes, 8- and 17-entry memories, wide induction variables,
wrapping loops, independent clocks and instances, dynamic writes, enables, and
synchronous resets. Every memory location is checked between clock edges.
Port-connection tests check two widths and parameter specializations, input and
output truncation, signed and unsigned extension, nonzero omitted-input defaults,
floating inputs, omitted outputs, escaped keyword ports and pre-edge sampling.
Another native case checks Boolean, signed-integer, real, escaped-string and
UTF-8 parameters. Constant string tests cover all six comparison predicates,
prefixes, unsigned bytes and embedded nulls while retaining dynamic comparisons.
A CSR-style loop fixture checks 1,024 cycles of RV32 and RV64 specializations,
constant-disabled loop bodies, locked lanes, dynamic partial writes and
pre-edge sampling. Its source reproduces an SDK canonicalizer allocation
runaway; post-conversion cleanup now processes uses before definitions.
The vendor scan regression checks 18 actual `VX_scan` specializations across
widths 5, 9 and 17, XOR/AND/OR operations, and both directions against independent
bitwise prefix calculations. A separate fixture checks two generated banks of
conditional dynamic bit writes, nested branches, input changes between edges,
and registered sampling. It reproduces unresolved combinational writes when the
drive-hoisting extension is removed.
The shifted-prefix fixture checks 66 recurrences over 1- through 129-bit values,
including OR/AND/XOR, both directions, stride 1/3 and registered sampling.
Independent truth tables cover 2,180 cases with nonunit and full-width strides;
guards retain unresolved feedback and operation attributes. The original
`VX_rr_arbiter` regression checks 14 specializations over widths 1/2/3/4/5/8/17,
with lock/unlock, idle behavior, reset and pre-edge sampling.
Run `python3 tests/run_native.py --variants scan radiance-cache rr-arbiter` for the vendor
regressions; these require the Radiance/Vortex sources to be checked out.
`python3 tests/vortex_dpi.py --circt-bin "$CIRCT_BIN"` links native LLVM models
against the original Vortex `util_dpi.cpp` for XLEN 32 and 64. It checks
signed/unsigned arithmetic, zero divisors and signed overflow, UUID state and
reset, narrowed returns, and exact clock/enable callback counts over 1,024 cycles
per width. Disabled arithmetic is checked directly against caller-owned storage.
`--unwritten-outputs` additionally invokes those callbacks with a false input
enable through the compiled model. It checks that their unwritten outputs use
fresh zero-seeded storage, while a clocked call suppressed by its call enable
retains its previous result. The original driver source remains unchanged.
The same regression checks the original delayed assertion's native scope name
and failing termination. `--opt-level 0` also exercises the unoptimized ABI.
The procedural DPI regression switches between two aliases for each of its
stateful scalar and mixed-output C functions. Both aliases run with exact call
counts at optimization levels 0 and 2. Declaration guards check all argument
directions, renamed formals, implicit C names, independent nested scopes and
conflicts in types, argument order, directions, arity and semantic attributes.
Input-sensitive lowering is verified for the tested combinational and bare
unclocked calls. The native ABI regression elaborates actual SV imports and
executes their C functions through the normal importer and native scheduler.
It checks C-name aliases, signed/unsigned atoms, scalar versus one-bit packed
arguments, 32/33/65/129-bit vectors, output initialization, inouts, four-state
word layout, and IEEE payloads. Stable evaluations and changes confined to
unused input padding must preserve exact callback counts. Partial observation
words are masked through an opaque legal-width helper before coroutine
snapshots, so padding cannot become an RTL input event. The provenance
regression checks typedefs/enums, packed ranges and state domains, qualifiers,
scopes, timescales, bridge retention, and explicit unsupported-signature guards.
Full GPU runtime behavior remains unverified.
Wide-shift tests compare 132 vectors with Python integers and a separate SV
reference, including padding bits and shift amounts above the port width. A
registered result checks synchronous reset and sampling at the clock edge.
The byte-ABI helper also has 8,280 Python-reference checks across 92 widths,
including byte and word boundaries, arithmetic shifts, and dirty padding.
Dromajo's SDK and the Radiance Rust interfaces use ABI mocks in the main
fixture suite. Separate tests exercise all three interfaces with their real
libraries; full emulator workloads remain unverified.
The separate `tests/cyclotron.py` regression links the original `SimMemFuzzer.cc`
to the real Cyclotron Rust library. It passes 500,009 callbacks and 126 reset
edges, exercises backpressure, and drains balanced request/response counts on
all three packed lanes. It also checks that outputs update only on falling
edges. Frozen sources, the library and executable, and the full compressed log
are in `output/development/native-cyclotron-real-20261017T101829Z/report.json`.
Run it with `--circt-bin "$CIRCT_BIN" --library /path/to/libcyclotron.so`.
Adding `--kind emulator` compares 9,728 outputs over 512 cycles against direct
calls to the original emulator driver in an independent process. Both paths
make exactly 493 calls, preserve reset guards, and update only on positive
edges. The reference exercises 167 distinct instruction addresses. The combined
real-library evidence is in
`output/development/native-cyclotron-both-20261017T102518Z/report.json`.
`cargo build --lib` in `generators/radiance/cyclotron` builds that library.
The `memfuzz` integration case exercises `RadianceFuzzerConfig`; pass the library
and its runtime search path through `EXTRA_SIM_LDFLAGS` when building that config.
An earlier full-config run passes with DRAMSim2 in 1,000,802 root cycles. Its report is
`output/integration/20261017T104302696882Z-n1q7hcek/report.json`. The original
one-million-cycle attempt remains in its report: the driver's unchanged
500,000-tick stopping point needs a little over one million 1 ns root cycles
with its 2 ns device clock, reset and response draining.

`tests/dromajo.py` links the original TestchipIP wrapper to the real Dromajo SDK.
An independent Python RV64 oracle supplies 434 steps across three retirement
slots, check masks, reset gaps, integer arithmetic, a store/load, a machine timer
interrupt and ECALL handling. The original driver and native binding both accept
the complete trace. A corrupted result is rejected after 299 callbacks; the
native case checks that SDK status 8191 reaches the standard runtime's failure
status. Frozen inputs, sources, SDK, executables and logs are in
`output/development/native-dromajo-real-20261017T105812Z/report.json`.
The test uses SDK revision `7062d33ad00ebc1a1f71fe8b13f1f5a53c99a12f`, which retains
the `--mmio_range` option required by TestchipIP. Pass `--circt-bin`, `--riscv`,
`--sdk-source` and `--library` to the test. Build the SDK with a matching compiler;
the local GCC build also needs `gcc-ranlib` to index its LTO archive.

The native latch/feedback development run records 38 passing native variants
and passing Rocket/DRAMSim2/TSI, UART, and JTAG workloads. All checked executables
have no Verilated runtime symbols. Source snapshots, artifact hashes, logs, and
the failed Ibex RV32 workload are retained in
`output/development/native-feedback-20261017/report.json`. These results cover
the recorded fixtures and workloads; they do not establish every Chipyard config.

The subsequent CFG/zero-width development run passes all 39 native variants,
including the RV32/RV64 loop fixture, original Radiance cache, vendor scan and
round-robin arbiter. Its report is
`output/development/native-cfg-final-20261017/report.json`, with source snapshots,
artifact hashes and archived executables. Every executable has no Verilated
runtime symbols. The matching frontend also passes 9,000 independent interface
and signed-port-conversion vectors.

The memory/aggregate run passes all 41 native variants, including mixed-write
reference storage and native file output. Compiler preflights also check nested
execute regions and aggregate constants at O0/O2. Its frozen sources, logs,
123 artifact hashes and executables are recorded in
`output/development/native-memory-aggregate-20261017T100241Z/report.json`.
All archived executables have no Verilated runtime symbols. Full-system reruns
are recorded separately; the fixture suite does not establish every config.
The matching backend also passes Rocket, Ibex, asynchronous-reset Rocket,
multiclock Rocket, UART and JTAG in
`output/integration/20261017T100328860275Z-n72voxef/report.json`.
The complete RadianceClusterConfig compiles through LLVM IR generation, including
1,602 combinational DPI regions and 256 seeded DPI outputs. That compiler-stage
result is recorded in
`output/config-sweep/20261017T100818204882Z-ha9p89yf/report.json`; full GPU runtime
remains unverified.
The original eight-core `RadianceFP16ClusterConfig` also passes native RTL
import after static task formals and their startup initializers are preserved.
The complete original generated RTL and flags retain their input hashes.
The reduced failures, independent Icarus oracle, O0/O2 runtime checks and original
config import are retained in
`output/development/native-static-formals-prototype-71-20261019T015825Z/report.json`.
A fresh build through the normal production frontend completes the full hardware
partition and LLVM generation on native71. It outlines 2,713,021 operations into
685 HW modules and emits 1,653 model procedures, retaining all eight cores and
both vendor caches. The reports are
`output/development/native-radiance-fp16-hw-71-20261019T022103Z/report.json` and
`output/development/native-radiance-fp16-llvm-71-20261019T024531Z/report.json`.
Machine-code generation and native simulator linkage also complete with the
original selected C++ drivers and no Verilated symbols. The simulator-stage
report is
`output/development/native-radiance-fp16-simulator-71-20261019T030452Z/report.json`.
GPU runtime remains unverified; the original RV32 ADD workload is running with
an explicit check of the original SimTSI software exit signal, so a vendor RTL
`$finish` alone cannot establish a pass.

Native72 also preserves strings inside nested structs and arrays, including
member references, whole-struct copies and constant default initialization.
All 71 native variants pass, alongside the standard frontend/LLVM checks and
Rocket hello/JTAG workloads. The evidence is in
`output/development/native-string-struct-promotion-72-20261019T032611Z/report.json`.
The original `SmallNVDLARocketConfig` now completes native RTL import and hardware
partitioning with its shared `$timeformat` state retained. Its original native71
failure and successful hardware build are recorded in
`output/development/native-string-struct-prototype-72-20261019T032402Z/report.json`.
The subsequent LLVM build stops at an unlowered original `$test$plusargs` call;
SmallNVDLA simulator linkage and runtime remain unverified.

Native73 lowers literal `$test$plusargs` queries through the existing native VPI
argument interface, preserving prefix matching and the original assertion
controls. All 72 native variants pass, including initial and clocked queries
at normal/O0/O2 settings; 13 separate cases also match Icarus. Standard
frontend/LLVM checks, Rocket hello and JTAG pass. Sources, executable hashes,
logs and earlier failures are retained in
`output/development/native-test-plusargs-promotion-73-20261019T034616Z/report.json`.
The original SmallNVDLA LLVM build then reaches an unsupported `%m` hierarchy
formatter in its diagnostic prints; its full runtime remains unverified.

Native76 resolves `%m`/`%M` source paths through imported-RTL and whole-design
flattening, including generated/escaped instance names and named block, task
and function scopes. Runtime constant folding retains function print effects;
physical function inlining applies native print/stop controls and ends a
non-returning fatal/finish process before returning to the native scheduler.
All 75 native variants pass. The unchanged three-message SV scope test matches
Icarus byte for byte at O0/O2; expanded lexical and function-effect tests pass
at normal/O0/O2, including quiet printing and stop-callback gating. Nested
blocks inside tasks/functions follow the separate stock diagnostic: Icarus 12
omits those suffixes. That diagnostic is optional; native builds use no
Verilator. Standard frontend/LLVM checks, Rocket hello (32,896 cycles) and JTAG
(37,226 cycles) pass. Evidence and earlier failure logs/source snapshots are in
`output/development/native-hierarchy-function-effects-promotion-76-20261019T050937Z/report.json`.
The full original SmallNVDLA hardware pipeline also passes on this source.
The same design also passes LLVM lowering, ABI normalization, O1 optimization,
machine-code generation and linkage with the original drivers. The simulator
contains no Verilated symbols; evidence is retained in
`output/development/native-nvdla-simulator-76-20261019T062436Z/report.json`.
The same native76 simulator passes CPU hello in 32,896 cycles with process
exit zero and the original SimTSI exit signal equal to one. This workload uses
`+loadmem` preload with the original UART/DRAMSim2/TSI drivers; it does not prove
serial TSI loading or NVDLA accelerator arithmetic. Source and original-input
guards, the exact executable and software, and runtime logs are retained in
`output/development/native-nvdla-cpu-boot-76-20261019T081012Z/report.json`.
This is historical76 evidence, not validation of later backend revisions.
The older isolated74 result remains in
`output/development/native-nvdla-cpu-boot-74-20261019T061340Z/report.json`;
74 has a separately reproduced lexical scope limitation.
On76, package and relative other-instance formatter calls, and
recursive or suspending contexts, still require further call-frame work.

Native79 preserves resolved call scopes through function and task inlining.
Package and compilation-unit formatters keep their static paths; relative child,
upward sibling and explicit root calls resolve to the correct physical instance,
including repeated copies of one canonical module and calls without parentheses.
The same frames bind context DPI imports before flattening. Synchronous imported
DPI tasks now carry their C completion status through a private return value;
invalid nonzero completion ends the physical process through native stop controls.
General task disable/suspension and recursive context frames remain unsupported.
All 76 native variants and 20 additional O0/O2 executions pass: eight unchanged
SV scope oracles produce 150 exact records, context callbacks check 18 physical
calls per level, and task ABI tests cover signed scalars, 65-bit vectors,
output/inout copy-back and invalid-status continuation/restart guards.
The nested-header cache test executes two separately rebuilt plugin versions.
Standard main frontend/LLVM checks, Rocket hello (32,896 cycles) and JTAG
(37,226 cycles) pass with no Verilator. Native runtime sources and original
drivers are unchanged. Sources, logs, executables, earlier failures and the
previously tested snapshot are retained in `output/development/native-call-scopes-task-abi-promotion-79-20261019T071420Z/report.json`.

Native81 supports clocked, non-suspending C/SV task callback chains and the
C `int` completion ABI for exported tasks. Relative child and fixed-root
context calls inside an export select the correct physical instance. Private
printing functions retain format expressions through CFG merging while native
print/stop controls remain active. Successful `$finish` from an exported task
preserves simulator success and halts the importing SV process, including when
C discards the completion status. Finish state belongs to the model's scope
lifetime and clears on recreation. General task disable, suspension and recursive
context frames remain unsupported.
All 78 native variants and the prior 20 scope/task executions pass with unchanged
source/test guards. Callback tests check two canonical instances, 28 exact print
records, both original fatal messages and print controls, output/status handling,
scope restoration and failed-task continuation/restart. Finish tests check normal
preflight followed by successful finish, no SV continuation, and four model
recreations at normal/O0/O2. Existing export tests pass 12,288 cycles per level.
Both callback and finish programs also pass through the unchanged production
builder, selecting the new `native/control.cpp` through the foreign-source list.
Existing runtime/build/export sources and original peripheral drivers are
unchanged. Main frontend/LLVM checks, Rocket hello (32,896 cycles), and JTAG
(37,226 cycles) pass without Verilator. The original finish failure, harness-link
failure, source snapshots, logs and exact executable archives are retained in
`output/development/native-task-callback-promotion-81-20261019T085503Z/report.json`.
The same executable subsequently passes the original UART transmit `A`/receive
`Z` and quiet-diagnostic workload in 192,596 cycles, with unchanged hardware,
implementation and simulator hashes. Its evidence is retained in
`output/development/native-uart-runtime-refresh-81-20261019T090551Z/report.json`.
A separate unchanged string-return `$sformatf("%m")` source matches eight stock
Icarus records at O0/O2, including nested calls, two instances and quiet controls.
At native81, the runtime-argument version was unsupported: native string admission rejected
`sim.string.format_to_string` and its dynamic formatter consumer. The original
source, stock oracle, successful executables and corrected-pipeline failure are
retained in `output/development/native-pure-return-scope-probe-82-20261019T093119Z/report.json`.

The isolated native83 candidate adds dynamic format-to-string storage and carries
formatted values through control-flow joins as native string handles. The prior
78 variants and 20 scope/task executions pass with unchanged guards; the original
runtime-argument scope test matches all eight stock records at O0/O2. Conditional
returns pass independent value, scope, side-effect, print-control and recreation
checks at normal/O0/O2, including the original production builder. Qualification
remains incomplete: the unchanged packed integer with an embedded NUL prints
`ABC` natively, ` ABC` in Icarus 12 and `A BC` in Verilator 5.022. Strict stock
file comparisons remained failed, so native83 was not promoted. Sources, the unapplied
review patch, logs and exact executable recovery records are retained in
`output/development/native-format-controlflow-checkpoint-83-20261019T102118Z/report.json`.

Native84 lowers runtime `$sformatf` returns and dynamic string fields using
model-owned string storage and the SDK formatter. Nested and conditional returns
carry rendered string handles through CFG joins. Print controls retain nonpure
argument effects and file output. The original shared SV functions remain
unchanged. All 79 native variants and 20 scope/task executions pass with unchanged
source/test guards. The original runtime-argument scope source matches all eight
stock records at O0/O2, including quiet controls. Formatting tests compare 20
stdout records and 38 file records per level at normal/O0/O2, with mixed 65-bit
signed/unsigned values, real/binary/octal/character fields, padding/empty strings,
physical scopes, clock/reset/enable behavior and model recreation. The original
production builder selects additive `native/format_strings.cpp` automatically;
existing native runtime/build/export sources and original drivers are unchanged.
The raw Icarus corpus is retained. Its packed integer embedded-NUL field was an
incorrect reference: independently linked unmodified Slang returns `ABC` with
length 3, consistent with the string-value rules in IEEE1800-2017 sections6.16
and21.3.3. The checker verifies that only this field changes and still compares
every byte. Icarus12's ` ABC` and Verilator5.022's `A BC` outcomes remain recorded.
Main frontend/LLVM checks, Rocket hello (32,896 cycles), JTAG (37,226) and the
original UART transmit `A`/receive `Z`/quiet workload (192,596) pass without
Verilator. All three workloads use the same native executable. Exact source,
oracle, executable and recovery evidence is retained in `output/development/native-format-strings-promotion-84-20261019T105310Z/report.json`.

Native85 adds the missing `compiler/FormatStrings.cpp` prerequisites to native
RTL import and normal/debug model compilation. Editing that pass now invalidates
all four relevant Make targets. GNU Make's actual dependency database verifies
both the omission and the corrected graph. Original Rocket hello (32,896 cycles)
and JTAG (37,226) pass again with exactly the native84 executable; compiler,
runtime, drivers, ABI and model bytes are unchanged. The dependency patch and
validation evidence are in `output/development/native-format-build-dependencies-85-20261019T113616Z/report.json`.

The standalone run loop caches its diagnostic-control signal handles and the
immutable `+verbose` option. It resolves `io_success` at its first post-reset
check, retaining bindings that register that signal during initialization or
before reset ends. Every root edge and scheduled event still runs. The native
regression checks 18 combinations of root-clock use, timed evaluation, verbose
output, late signal registration, completion, timeout, and missing success
signals against independently generated complete waveforms in ps and fs.

Native86 records time-dependency metadata before Arc state lowering adds
coroutine wakeup checks. Processes whose yielded wakeup is always `UINT64_MAX`
can use input and device events without polling an unused TestDriver clock.
Explicit Arc, LLHD, and formatted time reads, finite or unknown wakeups, and
nested coroutine calls retain conservative time handling and femtosecond
waveforms. Exporting a model without this provenance retains the previous
lowered-time-read scan. Native runtime and original device-driver sources do
not change. The timing regressions compare exact DPI calls and returns,
completion/reset timing and complete waveforms, continuous time outputs, and
1 fs autonomous wakeups without source time reads.

Native88 bounds LLVM code generation for large packed hardware values. A
matching-SDK LLVM pass temporarily suppresses vectorization in functions with
integers wider than 1,024 bits, then removes the temporary attributes before
instruction selection. This avoids the 134,217,728-bit vectors observed in the
original Vexii cache update. Basic blocks are split using a cost that includes
operand widths, with retained block boundaries to bound LLVM's DAG scheduler.
Optimization and machine-code generation remain at O3. Native model bindings,
runtime, device drivers, and hardware parameters do not change.

The production codegen regression compares 4,608 complete 4,096-byte cache
images at O0/O2/O3, covering all 256 write addresses, both enables, independent
old/base images, aliased and unaligned buffers, original attributes, explicit
floating-point operations, constrained entry/tail calls, and failed-build
preservation. All 82 native variants, the original scope/task preflight and
standard frontend/LLVM checks pass. The unchanged two-lane RV64/F/D/A/C
`VexiiRiscvConfig` passes hello (33,856 cycles), double addition (11,646), double
division (12,126), single addition (11,656), atomics (10,706), and compressed
instructions (11,646) with DRAMSim2 and the original SimTSI driver. The compiler
and test evidence is in `output/development/native-bounded-codegen-promotion-88-20261019T143119Z/report.json`.
These workloads do not establish universal configuration or VPI support.

The later Ibex run passes all 37 upstream RV32 ADD cases through normal TSI
loading and DRAMSim2 in 161,616 cycles, matching the stock simulator. Its report
is `output/development/native-vector-bootrom-20261017/report.json`.
The shared boot ROM now supplies a 256-byte-aligned table for direct and
vectored interrupts. Cached generator jars track changes to the embedded ROM
images. `tests/bootrom.py --riscv "$RISCV"` checks both images, all 32 vector
slots, compressed-code variants and exact appended-DTB placement.
Ibex rounds trap addresses down to 256-byte boundaries. `tests/ibex_isa.py`
aligns the upstream test environment's existing handlers and optional-CSR
recovery labels, retaining all arithmetic checks, privilege setup and CSR
probes. `--check-reference` validates the workload in Spike and requires an
injected ADD-to-SUB fault to fail test 3. The unadjusted environment and old
boot ROM failures remain in the reports. Other configs' ROM regressions are
recorded separately as they complete.

The native workload runs have passed Rocket and BOOM hello with DRAMSim2, normal
TSI loading on TinyRocket, dual-core Rocket, four DRAM channels, asynchronous
uncore reset, multiclock Rocket, physical UART TX/RX, remote-bitbang JTAG debug
register access, DMI loading, disk read/write comparisons, NIC DMA loopback,
software branch prediction, TraceGen with Axe WMO checking, and IceNet unit tests.
The native tethered multichip hello run completes after **9,352,876** root cycles,
matching the Verilator reference. The I2C-config run checks hello boot with the
stock tied-off harness; I2C transfers remain untested.
An additional Sodor5StageConfig RV32 instruction workload passes using normal
TSI loading, completing after 97,836 root cycles.

Earlier native integration runs also pass the GCD hardware comparison, GPIO inputs/registers,
SPI flash reads and six persisted single/quad write variants, read-only flash
and extended addresses, Spike instruction execution, Shuttle/Cospike, checkpointing
Cospike, and TrafficEval throughput checks. These runs explicitly set Verilator
to an unavailable path and check that the native executable has no Verilated symbols.
Use the JSON reports for the precise implementation hashes and current status.
The integration runner also provides `rv32`, `shuttle_plain`,
`shuttle_three_wide`, `gcd_tl`, `gcd_external_clock`, `init_zero`, `sim_axi`,
`ring_noc` and `mesh_noc` selections for additional named configurations.
They use the existing ISA, hello and GCD programs. `sim_axi` uses normal TSI
loading into the config's pure Chisel memory, with a ten-million-cycle allowance
for loading and execution. These selections are regression
definitions; consult their runtime reports for completed validation.
The frozen native69 ring and mesh NoC hello runs pass in 53,406 and 38,036 root
cycles. The mesh report is
`output/development/native-additional-mesh_noc-69-20261018T234706Z/report.json`.
The complete `SimAXIRocketConfig` hello run passes in 1,038,936 root cycles using
the original TSI driver and the config's pure Chisel AXI RAM, with no SimDRAM or
fast memory loading. Its report is
`output/development/native-simaxi-complete-69-20261018T234852Z/report.json`.
The earlier one-million-cycle limit expired while the CPU was waiting in boot
ROM; that failure and the original binary are retained with the completed run.

The `default_clock`, `single_clock`, `pll_clock`, `chip_like`, `flat_chip_like`,
`no_axi_fast` and `no_axi_serial` selections exercise the original clocking and
chip-like configs, retaining their reset synchronizers, dividers and PLL
settings. Native70 hello passes for `DefaultClockingRocketConfig` in 32,896 root
cycles and `SingleClockBroadcastRocketConfig` in 119,816. Their reports are
`output/development/native-clocking-default_clock-70-20261019T000357Z/report.json`
and `output/development/native-clocking-single_clock-70-20261019T001019Z/report.json`.
`ChipLikeClockingRocketConfig` also boots in 120,346 cycles with its original
clock mux/divider and reset synchronizers; its report is
`output/development/native-clocking-pll_clock-70-20261019T002521Z/report.json`.
These boot runs do not establish software-controlled PLL switching.
The `sodor1`, `sodor2`, `sodor3`, `sodor3_single`, `sodor5` and `sodor_ucode`
selections use the complete upstream RV32 ADD program and normal TSI loading;
their runtime validation is recorded separately.
Native70 `Sodor1StageConfig` passes in 97,836 root cycles; its report is
`output/development/native-sodor-sodor1-70-20261019T002244Z/report.json`.
`Sodor2StageConfig` also passes in 97,836 cycles, recorded in
`output/development/native-sodor-sodor2-70-20261019T003345Z/report.json`.
`Sodor5StageConfig` passes in 97,836 cycles and `SodorUCodeConfig` in 103,116;
their reports are
`output/development/native-sodor-sodor5-70-20261019T010321Z/report.json` and
`output/development/native-sodor-sodor_ucode-70-20261019T010322Z/report.json`.
Both three-stage variants fail a TileLink inflight assertion. Independent stock
Verilator builds reproduce the assertion with identical generated RTL and the
same original ADD program and normal TSI loading. The failures remain unresolved;
the comparison does not establish architectural equivalence or passing configs.
Evidence is retained in
`output/development/native-sodor3-reference-failures-70-20261019T013624Z/report.json`.

`tests/gemmini_software.py` builds the unchanged upstream `mvin_mvout`,
`matmul_os` and `matmul_ws` programs with a private include snapshot. Supply the
`gemmini_params.h` generated for the hardware being tested; the shared header
may describe a different config after another elaboration. The script requires
a new work directory, records source/header/binary hashes, and does not change
the shared Gemmini headers or enable the workloads' `FAST` selection:

```sh
python3 tests/gemmini_software.py --riscv "$RISCV" \
  --parameters /path/to/hardware-generated/gemmini_params.h \
  --work /path/to/new/gemmini-software
make CONFIG=GemminiRocketConfig run-binary-fast \
  BINARY=/path/to/new/gemmini-software/mvin_mvout.riscv
```

Use `--cases` to select individual programs. Compilation alone does not verify
the hardware or its parameter match. The native integer `GemminiRocketConfig`
DMA run passed after 73,246 root cycles with the original DRAMSim2/TSI drivers;
its report is
`output/development/native-gemmini-mvin-68-20261018T220537Z/report.json`.
The native69 `GemminiShuttleConfig` also passes hello in 30,716 root cycles and
the same original DMA program in 50,306 cycles, checking all eight 16x16 int8
matrices. Its private elaboration-generated parameter header exactly matches
the firmware header, and every shared Gemmini include file retains its
pre-build hash. The DMA report is
`output/development/native-gemmini-shuttle-dma-69-20261018T224102Z/report.json`.
Original output-stationary and weight-stationary matrix tests remain separate
runtime checks.
`FPGemminiRocketConfig` on native70 also boots hello in 32,876 root cycles.
Its private generated header describes the config's 4x4 array with float
elements/accumulators and 4,096 scratchpad/accumulator rows; the original DMA
and matrix programs compile against that exact header. The CPU boot report is
`output/development/native-fp-gemmini-hello-70-20261018T235356Z/report.json`.
This boot result does not establish accelerator computation; those workloads
are recorded separately.
The unchanged FP32 move-in/move-out program subsequently passes in 20,586 root
cycles, checking eight 4x4 matrices (128 float elements) through the original
DRAMSim2/TSI drivers and accelerator DMA. Its report is
`output/development/native-fp-gemmini-dma-70-20261018T235604Z/report.json`.
The complete original output-stationary matrix program also passes in 424,056
root cycles, retaining its activation/shift combinations, preloads, accumulated
results and golden-value comparisons with the matching FP32 parameter header.
Its report is
`output/development/native-fp-gemmini-matmul-os-70-20261019T003514Z/report.json`.
The complete original weight-stationary matrix program passes in 341,826 cycles.
The combined hello/DMA/OS/WS archive is
`output/development/native-fp-gemmini-complete-70-20261019T011917Z/report.json`.
A separate build using `ARC_MODEL_OPS=65536` passes the full DMA/OS/WS programs
with the same cycle counts and byte-identical state layout; its report is
`output/development/native-fp-gemmini-budget65536-complete-70-20261019T013624Z/report.json`.
These results do not establish a performance improvement or full-state equivalence.

`python3 tests/run_native.py --variants radiance-cache` runs an additional
vendor RTL test when the Radiance/Vortex sources are present. It imports the
original cache through the normal native RTL pipeline, using the Chisel boundary
and a three-cycle memory responder, and checks all data and tags for 64 reads
with 16 cold misses and 48 hits.

`tests/integration.py` selects actual Chipyard configs and device workloads.
Omitting `ARC_TEST_CASES` runs the full case list. `ARC_TEST_TIMEOUT` sets the wall
timeout per command (default 1,800 seconds). Set it to `0` to allow builds and
simulations to run without a wall deadline; the simulator's cycle limit still
applies. The direct runner accepts `--max-cycles` to override that limit and
repeatable `--make-setting NAME=VALUE` options for `ARC_*` build settings and
compiler tools. Each case records those overrides and both limits:

```sh
python3 tests/integration.py --cases rocket jtag --timeout 0 \
  --max-cycles 1000000 --make-setting ARC_OPT_LEVEL=1 \
  --make-setting ARC_LLC_OPT_LEVEL=0
```

The `ara_rocket_4096`, `ara_rocket_8192`, `ara_shuttle_4096`, and
`ara_shuttle_8192` cases select the full two-lane or four-lane Ara configs,
including the 128-bit bus in both 8192-bit configs. They enable the vendor
filelist and its frontend flags, verify the imported unit's VLEN, lane count,
and bus width, and run 257 integer and 257 floating-point elements. Expected
results use scalar arithmetic with compiler vectorization disabled. For example:

```sh
python3 tests/integration.py --runtime native --cases ara_rocket_8192 --timeout 0
```

These case definitions do not establish a configuration pass; each run must
complete its hardware build and numerical checks. The installed Spike reference
rejects VLEN above 4096 bits, so its software checks cannot validate the full
8192-bit configuration.

The `saturn`, `saturn_masked`, `saturn_transpose`, and `saturn_sgemm` cases use
the original `REFV256D128RocketConfig`, with VLEN 256, a 128-bit vector datapath,
and vector floating point. The architecture diagnostic checks `vlenb`, LMUL
limits, and 111 integer64/FP64 results against precomputed host bit patterns.
The other cases retain the upstream Saturn kernels, startup code, warmups,
datasets, and checkers: 1,000 masked elements, a 32-by-32 transpose, and a
71-by-71 SGEMM. Their software builds write only to each case's output directory.
Use `--timeout 0` for long vector workloads. An independent software check is
available through `tests/saturn_software.py --riscv "$RISCV" --work /tmp/saturn-check
--benchmark architecture --check-reference`; it uses Spike's `zvl256b` ISA
extension and also requires an injected FP64 error to fail. The masked-load and
SGEMM selections provide mask and FMA negative controls with the same option.

Reports in `output/integration/<timestamp>/report.json` contain backend,
software, simulator and partition hashes, required-model checks, cycle counts,
and separate build/run logs. Native reports verify that executables contain no
Verilated runtime symbols and record imported RTL separately from device bindings.
Cases reject source changes during execution.
The no-wall-timeout and explicit-build-setting options pass full native70
Rocket hello (32,896 cycles) and JTAG register checks (37,226), with the expected
failure for a short cycle allowance. Their report and retained verifier
timestamp-assumption failure are in
`output/development/native-integration-options-70-20261019T002904Z/report.json`.

The refreshed seven-workload run passes Rocket, Ibex, asynchronous-reset Rocket,
multiclock Rocket, UART, JTAG, and the real Cyclotron/DRAMSim2 fuzzer. The fuzzer
finishes in 1,000,800 root cycles. Executables, programs, logs, source hashes,
9,000 frontend interface vectors and references to the 41-variant, real-library,
and full CVA6 proofs are archived in
`output/development/native-unary-drivers-20261017T122623Z/report.json`.
A later unchanged-source run passes all 17 selected system workloads,
including DMI, block devices, network loopback, GPIO and SPI flash. Its original
source hashes and logs are archived in
`output/development/native-seventeen-systems-60-20261018T145040Z/report.json`.
Subsequent compiler/runtime changes carry their own validation checkpoints.
The complete frozen native68 matrix also passes all 21 selected workloads,
including normal TSI Ibex loading, Cospike, software branch prediction and
Shuttle. Its combined report is
`output/development/native-twentyone-complete-68-20261019T003302Z/report.json`.
That archive retains the original disk-full interruption and missing-source
Ibex preparation failure alongside the successful recovery results.

The UART check verifies transmitted `A`, received `Z`, and quiet-mode diagnostics.
JTAG checks IDCODE, DTMCS, DMCONTROL and DMSTATUS through the original socket
server. SPI tests check every command and persisted payload, and require the ROM
image to remain unchanged. TraceGen checks its actual memory trace with Axe.
Stock TraceGen with DRAMSim2 triggers a TileLink opcode assertion in both
backends, so its integration case uses the original simple-memory driver.

`tests/coverage.py --runtime native` distinguishes current, historical, and absent
runtime evidence. `tests/sweep.py --runtime native` compiles the named main
configuration catalog, including configs moved into generator plugins, and records successes and failures separately from runtime
results. `--stage hw`, `--stage llvm`, and `--stage simulator` select compilation
depth; `--skip-integration` leaves runtime-selected configs to the workload runner.
The sweep inherits each selected config's integration build defaults, including
Ara's source file list and Slang flags. Explicit `--make-setting` values take
precedence, and the report records the effective per-config settings.
Use `--timeout 0` for no wall-clock deadline and repeat `--make-setting NAME=VALUE`
to record compiler settings for both preparation and config builds. This uses
the integration runner's compiler-setting whitelist; hardware/runtime overrides
and `MAKEFLAGS` are rejected.
Other subprojects and composed configs are reported separately. Compilation alone
does not establish runtime correctness. Support for every config remains
**unverified**, including additional external IP and Chisel 7 layers.

A hardware-stage sweep of the 180 catalog entries outside the integration
selection is recorded in
`output/config-sweep/20261017T122943158404Z-qzf7869c/report.json`. It rotates
through generator families before the largest replicated designs; successful
hardware compilation does not establish LLVM compilation or runtime success.

The sweep exposed a shared include-expansion bug: a header emitted in one
preprocessor branch was incorrectly removed from its sibling branches. The
expander now removes duplicates only when the earlier include dominates the
current branch, preserving surrounding comments and macro continuations.
Caliptra's resource build also tracks the expander and header dependencies.
`python3 tests/insert_includes.py --circt-verilog "$CIRCT_BIN/circt-verilog"
--work generated-src/include-tests` compares all eight branch modes with native
SV preprocessing and checks a standalone expanded file. Frozen evidence is in
`output/development/native-conditional-includes-20261017T130059Z/report.json`.
The default frontend now reshapes equal-width packed elements inside fixed
unpacked arrays, including nested dimensions. Its regression checks 16,384
vectors at O0/O2 across packed arrays/structs, singleton dimensions, domains,
signed types, and rebased/reversed bounds. Together with native reference-component
lowering, it completes AES RTL import while preserving the original key-expansion
logic. `AES256ECBRocketConfig` also compiles through LLVM using the standard Make
pipeline. The original generated cipher driver and Caliptra RTL pass 28 AES-256
ECB encrypt/decrypt comparisons with OpenSSL at each of O0/O2, including reset,
key/mode changes and output backpressure. Reproduce this with
`make CONFIG=AES256ECBRocketConfig caliptra-aes-test CIRCT_BIN=/path/to/circt/bin`.
This harness uses the actual generated driver and its reachable helpers; the
full CPU RoCC/DMA workload remains unverified.

Packed structs and unions support constant/dynamic bit and range writes through
native reference descriptors. Indices retain their full width, and nested
projections clip writes to the available parent range. Union values use a common
overlapping bit representation across module signatures, calls, CFG arguments,
arrays and structs. The native suite includes `packed-struct`, `packed-union`
and `packed-cfg` fixtures with blocking/nonblocking ordering and O0/O2 execution.
`tests/union_bits.py` separately checks module/extern signatures, 65-bit values,
nonzero union-field offsets and zero-width fields. Packed `case`, `casex` and
`casez` comparisons are included in `make frontend-test`.

Native import uses `compiler/NativeMem2Reg.cpp`, based on the pinned CIRCT LLHD
memory-promotion pass. Conditional drives have separate mutable definitions:
equal data/enable operands in different branches can still require different
incoming values and muxes. The SDK's interning of these definitions can produce
invalid SSA or reuse a mux from a sibling block. `tests/mem2reg.py` reproduces the
stock SDK dominance failure and checks 65,536 vectors at each of O0/O2 across
scalar and packed aggregate values with different incoming values and enables.

Foreign RTL source discovery excludes `TestDriver`, matching the files actually
sent to the native importer. This prevents a testbench reference to `TestHarness`
from creating a circular native boundary containing its analog ports.
`tests/rtl_source_scope.py` checks this while retaining a real RTL-to-Chisel
helper boundary. Full-system GPIO and SPI-flash workloads pass in
`output/integration/20261017T143108207651Z-ekbrpmhn/report.json`.

Frozen evidence for the packed union/reference and conditional-drive changes is
in `output/development/native-early-conditional-defs-45-suite-20261018T031103Z/report.json`.
All 45 native fixture variants and all five frontend numerical suites pass.
The same backend source snapshot passes the original AES driver at O0/O2 and a
full CPU loopback-network workload using the original SimNetwork/DRAMSim2
drivers. This does not establish support for the entire configuration catalog.
At that checkpoint, Ara's exported SRAM DPI functions were unsupported.
Subsequent native export work covers those functions; complete Ara vector
workloads remain under validation.

Automatic references inside combinational regions use private LLVM stack
storage. Each declaration initializes a fresh allocation; CFG edges carry a
pointer and a bounded bit range. Components can merge roots of different sizes
and retain references to earlier loop allocations. This storage does not escape
the invocation. Delayed writes, escaping references and components mixed with
global storage stay unchanged for a later lowering or an explicit diagnostic.
`tests/run_native.py --variants local-memory` checks 16,384 independent vectors,
including 1/65/91-bit storage, 129-bit projected references, wide invalid indices,
partial writes, loop-carried ranges and live references to prior allocations.
It also checks exact DPI callback values, order and counts inside the loop at
O0/O2, through the normal native runtime.

The optional `tests/ara_functions.py` regression imports the original Ara
package functions and checks their shuffle/deshuffle tables at O0/O2 for all five
lane counts and all element widths. It takes `--circt-bin`, `--ara-root`,
`--filelist` (the complete SV dependency manifest), and `--work`. It does not
require modifications to Ara's source files or establish full-system support.

The stack-storage and original-package results are frozen in
`output/development/native-local-memory-46-suite-20261018T034032Z/report.json`.
All 46 native fixtures pass, along with 81,920 original Ara function checks and
the original AES driver at O0/O2. The same report preserves a failing static
storage case: after writing 17, disabling the writer should retain 17, but the
pinned SV importer restores the declaration initializer, 291. This report
records the frontend before the static-storage fix described below.

The default frontend now hoists static procedural locals into storage owned by
their module instance. Function calls capture that storage, including through
wrapper functions; repeated calls share a static local and separate instances
retain independent state. Automatic locals retain their invocation lifetime.
`tests/run_native.py --variants static-storage` checks 4,096 cycles at O0/O2,
including explicit statics in automatic functions, implicit statics in default
functions, conditional block statics and fresh automatic block locals. The
previous frontend fails the repeated-call case on cycle 1.
Static value formals also retain storage: inputs copy in at call entry, outputs
copy out at every return, and output-only formals keep their preceding value
until assigned. Default arguments are evaluated by callers. Pure declaration
initializers snapshot startup state instead of following later argument writes;
compilation-unit/package dependencies use the same startup rule and shared
global identity. `tests/run_native.py --variants static-formals` checks defaults,
input/output/inout retention, reset hold, two instances, compilation-unit task
storage and model recreation at O0/O2. `tests/signal_initializers.py` checks
nonzero chained startup values and diagnoses cyclic or effectful dependencies,
including unused initializer effects.
The main implementation passes all 70 native regression variants and the
original Rocket hello/JTAG checks in
`output/development/native-static-formals-promotion-71-20261019T020321Z/report.json`.
That checkpoint retains the normal pinned-source frontend build, exact source
and test hashes, executables, earlier failures and the separate Radiance
RTL-import evidence.
Package globals with constant initializers now use native signal memories keyed
by their original qualified name. Imported module variants share each global's
contents and version, while each simulator model owns its storage. Blocking
updates become visible immediately and nonblocking writes commit together.
`tests/run_native.py --variants global-storage` checks 12,288 cycles per
executable, including separately imported parameter variants, package-function
statics, passive readers, wide partial writes, nonblocking array writes, invalid
indices and model recreation. It also checks conflicting widths/initial values
and guards against dropping initializer side effects or cloning unsupported
global references into independent registers.
The `global-startup` variant additionally reads and writes the shared globals
from an SV `initial` block, including a shared function call and both blocking
and nonblocking writes before the first clock edge.
Initializers requiring runtime calls or effects remain unsupported and are
diagnosed; general runtime initialization ordering remains open. Subsequent
export support is described below.

The static-storage results are frozen in
`output/development/native-static-storage-47-suite-20261018T041421Z/report.json`.
All 47 native fixtures, five frontend compatibility suites, 81,920 original Ara
helper comparisons and the original AES driver at O0/O2 pass. The report also
preserves the previous frontend's failing lifetime test and the failures for
mutable package globals and DPI-valued initializers before global storage was
added. At that checkpoint, full Ara import had no remaining local signals;
four drives in its three exported SRAM APIs still required native bindings.

The shared-global results are frozen in
`output/development/native-global-storage-49-suite-20261018T043929Z/report.json`.
All 49 native fixtures, five frontend suites, original Ara helper checks, the
original AES driver and seven full-system integration cases pass on this
implementation. At that checkpoint, full Ara still stopped at its three
exported SRAM APIs. Nonconstant declaration initialization remains unsupported.

The native importer now compiles integral, string, real, shortreal, and scalar
realtime DPI exported functions
and registers their C entry points against physical instance scopes. The
original SV bodies read and write the same signal-memory storage as RTL;
exports are not implemented as peripheral-specific replacements. `rtl.json`
records each compiled entry, its original ABI and captured storage contract.
C callers select a scope with `svSetScope(svGetScopeFromName(path))`. Export
scope paths use the same slash-separated physical hierarchy as native binding
names, including generated boundary wrappers. `svGetNameFromScope` returns
that path; original dotted SV hierarchy aliases are not reconstructed.
Stateless exports retain per-instance specialization, and storage used only by
C exports remains live. Native C callbacks can invoke these entries during
model settling without recursively evaluating the whole design. Outside an
active evaluation, the runtime settles initial work before the call and
publishes its blocking or captured-memory NBA writes before returning to C.
Scalar atoms, bit/logic scalars, packed vectors, strings, chandles and output/inout formals use
the original DPI C ABI; unknown input bits become zero in this two-state model.
`tests/run_native.py --variants exports` checks these paths at O0/O2 across
13/65-bit memories, 512-bit arguments, two specialized instances and model
recreation. Temporary output/inout references reject NBA writes whose update
would outlive the call frame.
`tests/run_native.py --variants real-exports` checks native `real` and
`shortreal` C ABI values, returns, output/inout arguments, captured module
storage, and automatic function locals. It compares exact IEEE bit patterns,
including signed zero, subnormals, infinities and NaN payloads, and verifies
arithmetic/rounding, separate instance scopes, model recreation and state
coherence between RTL and C++ at O0/O2/O3.

Scalar `realtime` imports and exports use the standard `double` C ABI and IEEE
f64 storage. `$realtime` converts the model's physical clock into the declaration's
lexical time unit. Exported bodies and their SV helper functions read that same
native clock; integral time and scheduler clocks retain their existing
representation. `tests/run_native.py --variants realtime-exports` runs 41,059
checks at each of normal/O0/O2/O3 settings. These cover exact IEEE identities,
context callbacks, defaults and typed constants, output/inout arguments, null
rejection, captured state and RTL coherence, model recreation, and independent
1ns/100ps declaration time units. This coverage applies to scalar formals.

Native108 passes the 82 default native variants plus the optional `scan`,
`radiance-cache`, and `rr-arbiter` resource regressions (85 total), and the
standard frontend/LLVM gates. Fresh default-O3/llc-O3 Rocket, JTAG and UART
workloads pass with the original drivers and unchanged whole program ELFs;
all 22,965 complete CPU trace records match Native105. Source snapshots, the
initial failures, four realtime executables, and full qualification logs are in
`output/development/native-realtime-promotion-108-20261020T120107Z/report.json`. These results do not establish every
Chipyard configuration or general VPI support.

Fixed unpacked-array export formals marshal integral atoms, bit/logic scalars,
packed bit/logic vectors and aggregates, strings, and chandles through their
original C pointer ABI. C elements follow declaration order across nested,
ascending, descending, and rebased dimensions. Each packed element uses its own
canonical 32-bit words; SV storage remains compact across non-byte widths.
Outputs start at zero and inouts preserve unwritten elements. Unknown input
bits become zero, output unknown masks and C padding are cleared, and returned
string storage belongs to the model. Null array arguments are rejected.
A stable compiler layout ID selects the physical instance's bridge, including
parameterizations with the same total SV width but different C element padding.
This selection does not depend on distinct machine-code function addresses.
Exported scalar atoms use the public DPI C types `char`, `short`, `int`, and
`long long`, including their unsigned forms. Fixed string/handle input arrays
retain the C header's pointer qualifications. The array regression compiles both
the generated bridge and its client with a portable public DPI header, and also
checks signed/unsigned scalar edge values and 64-bit array output/inout data.

`tests/run_native.py --variants array-exports` checks two specialized instances,
1D/2D arrays, 13/65/129/193-bit values, mixed bounds, partial outputs and inouts,
scalar unknown encoding, string/handle captures, high pointer bits, model
recreation, and state coherence between RTL and C++. Equal 195-bit layouts
require nine versus ten C words and retain surrounding canaries. Normal/O0/O2/O3
executions each check 1,902,084 values; a one-bit expected-value fault is rejected.
Dynamic and open-array export formals still require additional lowering.

Non-suspending exported tasks use the same native call frames, including void
and zero-argument tasks, output formals and captured instance storage. Tasks
that wait or retain uninlined coroutine calls are rejected; they need scheduler
call frames. String inputs, outputs, inouts and returns use the standard C
string ABI. Their contents are copied into an immutable, per-model string pool;
returned pointers stay valid until the model is destroyed. Native string
operations include length, byte indexing, comparisons, concatenation, ASCII
case conversion and packed integer conversions. Original string ABI metadata
remains dynamic; the separate native storage representation uses handles.

`$readmemh` and `$readmemb` load fixed unpacked arrays of packed words,
including ascending/descending declarations, nonzero/negative bounds, optional
start/finish ranges, address directives, comments and underscore separators.
Holes preserve previous values; malformed input or invalid enum words stop the
load after retaining earlier valid writes. X/Z file bits become zero in this
two-state runtime. `readmem` checks the compiled original SV tasks at O0/O2,
including the `simutil_memload` signature, 13/65-bit words and enum validation.
Multidimensional loads visit numeric indices in ascending order within each
row, with the rightmost unpacked dimension varying fastest. Address directives
select the highest dimension and restart at its first subword; reverse ranges
reverse that dimension while preserving the order inside each row. The
compiler records the declared direction of every unpacked dimension so the
runtime can address the packed native storage correctly. Partial files and
row jumps retain all words they do not write. These rules follow
[the SystemVerilog array file-format specification, sections 23.17–23.18](https://ece.uah.edu/~gaede/cpe526/SystemVerilog_3.1a.pdf#page=354).
`readmem-multi` checks compiled SV exports with mixed-direction 2D/3D arrays,
negative bounds, 9/13/65/129-bit words, slices, indexed subarrays, binary/hex
files and partial failures at normal/O0/O2 settings.
Queue loads and runtime string formatting still need lowering. At that checkpoint the original Ara
import retained all three SRAM exports, and hardware preparation stopped at
temporal LTL assertions. Its complete CPU workload remains unverified. Runtime
nonconstant declaration initialization and general VPI hierarchy/callback
support also remain open. Context function imports are supported as described
in the DPI section; recursive contexts still require native call frames.
The earlier frozen reports above
cover their recorded implementations; the export implementation has its own
regression results.

`tests/ara_sram.py --circt-bin "$CIRCT_BIN"` also compiles the unmodified Ara
`tc_sram.sv` with all three original exports enabled. Its comparisons cover
13/65-bit memories, file loading, C setters/getters, invalid indices, masked
RTL writes, pre-write read data, registered read latency and model recreation.
It uses the production native build script at O0/O2 and checks for absent
Verilated symbols.

The partition preparation now streams the normalized and flattened assembly
instead of retaining whole bodies, line lists and unused global SSA sets.
`FileLines` supplies repeatable passes and temporary output files are removed
when a pass completes or fails. Outer module attributes and opaque strings
survive declaration insertion. Byte-for-byte comparisons cover the 50 original
native fixtures and the full flow for procedural scopes, asynchronous registers
and exported functions. The real 64-core BOOM input (2,832,011,290 bytes) also
produced identical partition IR, bindings JSON and adapter SV at a measured
47,124 KiB maximum Python RSS. That measurement covers Python partitioning;
CIRCT compilation and complete CPU runtime are separate requirements.
Compilation-unit static locals are now declared as shared globals before their
function bodies lower. `unit-static` adds O0/O2 checks for retained values and
independent model recreation. Static initializers that depend on task arguments
still need proper formal storage and startup initialization; Radiance trace
tasks currently expose that remaining gap.

The task/context changes are frozen in
`output/development/native-task-context-51-20261018T070358Z/report.json`.
That checkpoint passed all 51 native fixtures, five frontend suites, 81,920
original Ara function checks and 28 AES-driver comparisons at each of O0/O2.
It includes scope restoration after callbacks change the selected scope or
throw, and compiler checks that preserve distinct DPI import qualifiers.
The report identifies the earlier five-system snapshot separately; these
fixture results do not establish universal configuration or runtime support.

The subsequent string/readmem and singleton-CFG implementation is frozen in
`output/development/native-strings-sram-cfg-52-20261018T082814Z/report.json`.
It records 52 native fixtures, the frontend/Ara/AES helper suites, 131,072
singleton-CFG comparisons and 33,792 comparisons against the unmodified Ara
SRAM at O0/O2. Its six loader/export executables and the original-SRAM
executables contain no Verilated symbols. The current full-system matrix is
tracked separately in the report; full Ara and universal config support remain
incomplete. The completed five-system run is preserved in
`output/development/native-five-systems-52-20261018T084458Z/report.json`:
Rocket (32,896 cycles), async-reset Rocket (31,386), multiclock Rocket (31,386),
UART (192,596), and JTAG (37,226), including actual debug-register reads.
That archive records the exact implementation and three executable hashes.

The temporal checkpoint is frozen in
`output/development/native-temporal-53-20261018T085452Z/report.json`. It passed all 53 native
fixtures and six frontend suites. Its SV regression checks for finite
antecedents, exact/ranged delays, repetitions, sequence conjunction/disjunction
and intersection, nested implications, `$past`, independent positive/negative
edges, asynchronous cancellation and stop gating. It compares exact assertion
callback counts against a separate C++ trace checker at O0/O2. Thirteen guard
cases ensure unsupported cones cannot become partial monitors. A separate
20,480-result source-level truth table checks next-cycle SVA operators; the
previous frontend produces 2,816 differing results. Ara now passes hardware
partitioning with 476 pointwise assertions and 138 finite temporal monitors;
complete CPU execution is still pending.

## Hybrid reference

`make ARC_RUNTIME=hybrid` or `make hybrid` selects the prior mixed simulator.
It runs the generated hardware with Arcilator and delegates foreign SV/event
scheduling to Verilator. Its executables retain the `simulator-...` names and
artifacts use `arcilator` / `arcilator-debug`. This optional reference requires a
Verilator timing-capable C++ compiler. `make hybrid-test` runs its nine compiled boundary
fixtures against independent SystemVerilog references; these include analog
attach, parameters, probes, 5,000 returned bits, delayed-event feedback, and
TraceROM. `tests/integration.py --runtime hybrid` selects hybrid workload tests.
The full prior 23-case hybrid matrix passed, with I2C and software prediction also
passing separately. These results are kept separate from native evidence.

## Known shared build failures

The [debug-ROB driver](../../generators/rocket-chip/src/main/resources/csrc/debug_rob.cc)
now initializes all DPI outputs on empty/waiting entries. Previously undefined
interrupt flags could fail Cospike in both simulators. The shared generator build
also tracks embedded DPI C/C++ and headers so driver changes refresh resources.

An isolated `USE_CHISEL7=1` build fails in shared Scala projects before hardware
elaboration, including FixedPoint API mismatches and missing `midas.targetutils`
imports. The same errors occur through the Verilator makefile. The config sweep
also reproduced these shared failures in Verilator:

| Config | Failure before simulation compilation |
| --- | --- |
| EmptyChipTopConfig | Annotation targets absent `ChipTop` |
| AbstractTraceGenConfig | No inward system-bus connection |
| CustomIOChipTopRocketConfig | Missing `CanHavePeripheryTLSerial` key |
| GCDHLSRocketConfig | `vitis_hls` unavailable |
| MultiLinkSymmetricChipletRocketConfig | Uninitialized serial-TL harness inputs |
| RocketCoreChipletConfig | Uninitialized second serial-TL harness inputs |
| NonBlockingTraceGenL2RingConfig | Clock-group combiner requirement |
| TutorialStarterConfig | No-tile subsystem still requests a PLIC |
| TutorialNoCConfig | TileLink NoC edge/node count requirement |

Failures remain in the reports. `Cloned64MegaBoomV3Config` initially exceeded a
one-hour build limit while canonicalizing its 2.8 GiB flattened partition.
Independent runtime diagnostic guards avoid quadratic SCF condition propagation;
the revised partition passes that canonicalization in about seven minutes.
Bounded hardware modules now pass Arc conversion and loop splitting with the
stock compiler. Native Cloned64RocketConfig builds have completed hello after
56,026 root cycles. Allocated-model function outlining and direct wide arithmetic
are now part of the standard compiler flow. Larger standalone experiments remain
separate from the standard Make regression evidence. BOOM4 also exposed a late
array-concatenation type error, now covered by the staged compiler regression.
Ibex's clock-gating latch and combinational feedback compile and run natively.
Its aligned RV32 ADD workload passes in the same cycle count as the stock
simulator. CVA6's duplicate FPnew package inputs have been removed. Its full
RTL frontend now completes after fixing a canonicalizer allocation runaway.
Native bindings now lower its 36 SRAMs with mixed blocking/nonblocking partial
writes and its return-address-stack array-slice references. The memory fixture
checks two independent instances, masked/overlapping writes, invalid indices,
pre-edge sampling and packed stack operations with 208,908 comparisons. Later
compiler fixes cover array-valued execute regions, trace files and FPnew's
struct-valued constant tables. Full CVA6 now completes hello with the unchanged
native C++ runtime and production compiler stages, using isolated split LLVM
code generation. It completes in **71,646 root cycles**, with all **4,288
retirements** matching the stock reference's PCs, encodings and cycle numbers.
Its executable contains no Verilated symbols. Frozen sources, executable and
traces are in
`output/development/native-cva6-runtime-20261017T121750Z/report.json`; the report
references the archived LLVM model by hash. The later standard Make CVA6 run
also passes in 71,646 cycles with `ARC_OPT_LEVEL=1 ARC_LLC_OPT_LEVEL=0`; its
report is `output/development/native-cva6-cpu-66-20261018T204027Z/report.json`.

This required fixing a frontend defect: one-bit increment/decrement computed
the new bit without writing it back, preventing the LSU FIFO pointers from
advancing. The patched
frontend passes 16,384 numerical checks; the previous frontend fails the
first enabled one-bit case. All 41 native regression variants pass with this
fix; frozen evidence is in
`output/development/native-unary-suite-20261017T114958Z/report.json`.
A separate test of the original CVA6 SRAM passes 16,384 comparisons for each
of its Xilinx and Altera implementations, covering byte enables,
read-before-write, idle hold, reset, optional output registers and peer sampling.
That isolated test selects the same upstream compatibility branch as CVA6's
bundled source. The hello test also requires its HTIF mailboxes in the uncached
`0x80001000..0x80001080` window defined by `CVA6Tile.scala`. Its linker layout
now reserves that window, and the stock reference completes in 71,646 cycles.
A separate candidate with cached latch port handles and byte copies instead
of temporary vectors completes in the same cycle count and matches the entire
retirement trace. Frozen sources, LLVM, executable and traces are in
`output/development/native-cva6-candidate-20261017T121047Z/report.json`.
The cached latch-copy implementation is now part of the native runtime. The
archived 10,000-cycle probe reduced CPU time from 59.3 to 36.9 seconds while
matching 2,001 complete port snapshots, excluding process-specific memory
handles. Current validation also checks dirty padding and overlapping storage
against a bit-level snapshot oracle.
The SDK importer also formats default `%t` diagnostics in local time units;
global-precision scaling remains unfinished.
The stock compiler crashes while splitting loops for the original flat
Cloned64RocketConfig. A separate compiler experiment reproduced truncated
argument indices on a 33,000-input regression. The default module-partitioning
pass avoids those limits, and that regression checks every output with the
stock compiler. The experimental compiler patches are not needed by this path.

Upstream: [Arcilator](https://github.com/llvm/circt/tree/main/tools/arcilator),
[Arc dialect](https://circt.llvm.org/docs/Dialects/Arc/),
[Arc tests](https://github.com/circt/arc-tests).

Dynamic indices in procedural writes to packed arrays retain their individual
write ranges before LLHD drive hoisting. Bounded array selections become
index-qualified writes to the existing signal pieces; nested struct fields,
original write enables and delays are preserved. This prevents separate
processes from overwriting or retaining another process's bits in Ara's request
matrix. The `dynamic-writes` native regression checks concurrent matrix columns,
disjoint struct fields, non-power-of-two arrays, invalid indices and reads whose
index depends on another probe, at LLVM optimization levels 0 and 2.

Native transparent latches cache their stable runtime port addresses during
initialization. Enabled copies use overlap-safe byte moves with a separately
captured final partial byte, preserving padding cleanup and exact change
detection without temporary vectors. The latch fixture includes 11,308
bit-level snapshot comparisons for 1..4096-bit ports, both overlap directions,
identical storage, dirty padding and disabled transparency, in addition to
the original clock-gating and peer-sampling checks.

Native continuous-assignment delays retain inertial pulse rejection. Pure LLHD
propagation delays retain every input transition through native transport
queues. Both use physical femtosecond event times, including events between the
existing peripherals' picosecond interfaces. `Runtime::advance_fs` drains pending
events at the requested instant and applies its external drive callback once.
Real delay expressions round once to lexical `timeprecision`; fractional time
literals and expressions remain real until that conversion. Real-to-integral
casts round to nearest with ties away from zero using LLVM rounding intrinsics.

The `delayed-signals`, `delay-precision`, and `delay-transport` regressions compare
12,001 samples against frozen, independently generated stock-SV records. They
exercise wide values, chains, constant and procedural delays, short pulses,
delayed clocks, exact deadline ties, precision rounding, real casts, dense and
coarse advances, and model recreation at normal/O0/O2/O3 settings. The transport
reference uses delayed nonblocking assignments to retain short pulses. The
normal native build and regression run require no stock-SV simulator.

Hierarchical calls route their implicit captured variables through the same
module boundaries as explicit hierarchical references. The
`hierarchical-captures` fixture checks independent arrays in distinct and
identical-parameter subbanks, static task arguments, automatic read functions,
and multiple outer instances. Unsupported reference escapes and assignment
forms retain explicit diagnostics.

Native117 passes all 86 default native variants plus the three optional resource
variants (89 total), the standard frontend/LLVM gates, and fresh default-O3
Rocket/JTAG/UART workloads with unchanged complete ELFs and all 22,965 CPU
records matching Native108. Full original LargeNVDLA hardware import passes
with 841 timed cells, 592 procedural memories, and 214 asynchronous register
banks. Its LLVM/runtime control remains separate. Qualification sources and
closed evidence are in `output/development/native-timing-promotion-117-20261020T144119Z/report.json`.
These results do not establish every configuration or general VPI support.

The native production file list includes the original TOP and MODEL memory
files produced by Chipyard's MacroCompiler. This resolves custom blackbox SRAM
references such as Radiance's `rf_bank` through the original generated RTL.
The default TOP/MODEL and hybrid input list are unchanged. The source paths
participate in file-list settings and dependencies so selecting different
generated memory files refreshes the import.

Native119 passes standard full hardware builds for BigLittle synthesis,
4CFP16 synthesis, non-synthesis 4CFP16, and Rocket without manually supplying
RAM models or RTL flags. Fresh default-O3 Rocket/JTAG/UART runs retain the
original complete ELFs, cycle counts, and all 22,965 CPU records. Production
file-list checks cover older source-path changes and exact hybrid117 list
equivalence. Compiler/runtime/test code remains byte-identical to the qualified
117 checkpoint and its 89-variant suite; that unchanged suite was not rerun
for this Makefile change. Evidence is in
`output/development/native-generated-mem-promotion-119-20261020T154550Z/report.json`.

Native121 qualifies automatic original NVDLA library discovery with full
standard Large NVDLA, Small NVDLA and Rocket hardware builds, no manual library
flags, the frontend/LLVM gates and all 89 native variants. Fresh default-O3
Rocket/JTAG/UART runs retain the complete original ELFs, cycle counts and all
22,965 CPU records. This is hardware compilation evidence for NVDLA; its full
runtime qualification remains separate. Evidence is in
`output/development/native-nvdla-library-promotion-121-20261020T161135Z/report.json`.

Native123 caches symbol tables in procedural DPI lowering and source-time
dependency lookup, and visits each shared wrapper once for existing scalar
ABI attributes. Symbol definitions and call order are preserved. Complete
lowered IR matches native121 byte-for-byte on six comparison inputs, including
32,768 ordered calls and repeated coroutine instances with shadowed symbols.
On that isolated case, the two passes took 48.47 s before and 1.13 s after;
this measurement does not establish an end-to-end simulator speedup.
Fresh frontend/LLVM checks, all89 native variants and original default-O3
Rocket/JTAG/UART pass with identical complete ELFs, cycle counts and 22,965
CPU records. Full original NVDLA compiler-stage controls and its current-source
runtime qualification are separate; no uncompleted runtime receives credit.
Evidence is in `output/development/native-dpi-symbol-promotion-123-20261020T163704Z/report.json`.

CustomIOChipTopRocketConfig uses the current SerialTL trait class as its
IO-cell map key. The stale package string prevented elaboration; the corrected
lookup keeps every original IO cell, port, connection and configuration value.
The Scala source participates in coverage fingerprints. Native124 passes its
full standard hardware build and default-O3 hello, UART transmit/receive and
JTAG register/memory checks, matching ordinary Rocket cycle counts, complete ELFs
and all 22,965 CPU records. Run the first-class cases with
`make integration-test ARC_TEST_CASES="custom_io custom_io_uart custom_io_jtag"`.
All original 60 case tuples and runtime selection decisions are preserved.
Compiler/runtime C++ and the 89 native fixture sources remain identical to
qualified123; that unchanged suite is inherited, not reported as rerun here.
Evidence is in `output/development/native-custom-io-promotion-124-20261020T171405Z/report.json`.

Native126 indexes root formatting marker declarations once before classifying
model calls. Callback type checks, call order, unused-format cleanup and
unsupported-consumer errors remain in place. Six complete lowered-IR outputs
match native124 byte-for-byte; a 32,768-call isolated pass measurement fell
from 10.22 s to 0.43 s. This does not measure end-to-end simulation speed.
Fresh frontend/LLVM/source-time checks, all89 native variants and original
default-O3 Rocket/JTAG/UART pass with identical full ELFs, cycle counts and
22,965 CPU records. The full current NVDLA array/runtime/format compiler stage
passed in 445 s; its remaining compiler and runtime work is tracked separately.
Evidence is in `output/development/native-format-index-promotion-126-20261020T174625Z/report.json`.

Native132 uses the matching SDK Arc-to-LLVM conversion patterns with the
SDK-supported scoped function-symbol cache. The adapter preserves the original
types, layout, options and calling-convention rules. It excludes disappearing
Arc model names from the lookup cache, preserving generated evaluation names
even for models named `m` and `m_eval`, and rejects real symbol collisions.
`compiler/ArcToLLVM.cpp` retains the upstream license and conversion source from
CIRCT `fa4f662cbe710906347fac795c581a06c6b99757` (LLVM
`a17069fea5b271c6824ea773874b007116de9644`). The final DPI ABI attribute pass
also caches scoped symbol lookup without changing its ABI rules.
Six complete stock/cached LLVM IR comparisons and a permanent 54,400-call
regression cover aggregates, array references, forward/indirect/shadowed calls,
model names and conflicting symbols. Fresh frontend/LLVM checks, all89 native
variants and original default-O3 Rocket/JTAG/UART pass; complete ELFs, cycle
counts and 22,965 CPU records match native126. The complete current NVDLA
model now lowers and translates to LLVM IR; machine code and runtime results
are tracked separately and remain uncredited until they pass.
Evidence is in `output/development/native-arc-lookup-promotion-132-20261020T185629Z/report.json`.

Coverage requires an exact implementation fingerprint for both runtime and
compilation evidence. A private candidate that records additional changed
sources is historical, even if every main-tree file matches. Missing or changed
fingerprints also remain historical; incomplete successful-run records are
rejected. Run `python3 tests/coverage_integrity.py` to exercise these rules with
isolated synthetic metadata. Those fixtures never count as hardware or runtime
qualification. This analyzer-only correction leaves the qualified native132
compiler and runtime sources unchanged.


Native135 buffers private LLVM values according to their allocation size as well
as parameter and tuple-field counts. It handles scalar, array and aggregate
results, including PHI results and whole-aggregate consumers, with aligned
scratch storage and results captured before scratch reuse. Public, address-taken,
always-inline and constrained ABI functions retain their signatures. Must-tail
calls and their callers retain their required calling conventions.
The value ABI passes run again after the complete O3 pipeline and before
instruction-selection block boundaries are pinned. This prevents argument
promotion from recreating very wide machine arguments. Make tracks the two
ABI-pass sources as object-compilation dependencies.
The regression compares 1,247,040 bytes independently at each of O0/O2/O3,
including 4,099-, 8,207- and 32,771-bit values, packed layouts, arrays, PHIs,
side effects, delayed results, constrained calls and output bounds. Existing
frontend/LLVM/time checks, all89 native variants and the original default-O3
Rocket/JTAG/UART workloads pass, preserving complete ELFs, cycle counts and
all 22,965 CPU records. The complete original chiplet model continuation also
passes default O3/llcO3 and the default 8 MiB stack, with its original hello ELF,
50,406 cycles and all 12,792 CPU records matching the qualified127 reference.
That continuation retains the separately qualified127 private harness fix;
it does not establish a fresh main-tree chiplet hardware build. The joint
Scala corrections and broader configuration qualification remain separate.
These checks establish the recorded correctness results; they do not measure
an end-to-end speedup or establish every configuration or general VPI support.
