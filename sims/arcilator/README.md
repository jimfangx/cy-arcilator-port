# Arcilator simulation

This experimental backend compiles Chisel-generated hardware with CIRCT's
Arcilator and LLVM. The default runtime is native: building and running it does
not invoke Verilator, link its runtime, or include its headers. The native
frontend defines `ARCILATOR`; some upstream collateral, including CVA6's bundled
preprocessed source, contains its own `VERILATOR` compatibility guards. Native
model bindings call the original selected C/C++ device
drivers through the DPI interfaces those drivers already expose.

Native C/C++ compilation overlaps LLVM model compilation and shares the outer
`make -j` jobserver. Without an outer parallel jobserver, it uses
`ARC_BUILD_JOBS`. Compiler-generated dependency files track included headers;
source edits rebuild the affected objects, model-only changes relink, and
unchanged builds reuse objects. The standalone `native_build.py` one-shot
compile/link interface remains available.

`ARC_MLIR_BYTECODE=1` (the default) writes the partition-to-model handoff as
MLIR bytecode, retaining the `model.mlir` artifact name. The compiler accepts
this encoding directly. Use the matching `circt-opt model.mlir` to print it,
or set `ARC_MLIR_BYTECODE=0` to generate text. The encoding participates in
the existing build settings so changing it refreshes the grouped partition
outputs. This changes serialization without skipping compiler passes.

`ARC_FUSED_PIPELINE=1` (the default) runs array normalization, optional model outlining,
and LLVM lowering in one `circt-opt` process with the same pass order and
options. Arc optimization and state allocation keep separate process contexts.
Set it to `0` to retain individual stages for debugging. This option uses
the installed SDK's existing passes.

`ARC_CONVERSION_BYTECODE=1` also uses bytecode for the Arc conversion handoff.
It requires the matching Arcilator driver extension and defaults to `0` for
the stock SDK. The [driver build instructions](compiler/arcilator-driver/README.md)
describe the private build; keep the SDK compiler, headers, and runtime aligned.

Private, same-revision LLVM compiler builds can be evaluated with `LLVM_OPT`
and `LLC` overrides while retaining the installed SDK for `CIRCT_BIN` and
`CIRCT_ROOT`. See [compiler/TOOLCHAIN.md](compiler/TOOLCHAIN.md) and the
[LLVM compiler patches](compiler/llvm-patches/README.md). These tools optimize
compiler execution; simulator inputs and optimization settings remain explicit.

### Reproduce the optimized toolchain from this branch

From the Chipyard root on each machine:

```bash
source env.sh
./sims/arcilator/build-toolchain.sh --jobs "$(nproc)"
source sims/arcilator/generated-src/toolchain/env.sh
make -C sims/arcilator -j"$(nproc)" CONFIG=RocketConfig
```

This builds the compiler tools locally from pinned source and the checked-in
LLVM patch series. It builds the optional bytecode driver using its existing
pinned source patch, generates fresh native model inputs with the installed SDK,
builds instrumented LLVM, trains fresh filtered frontend profiles, and builds
ThinLTO/PGO `opt` and `llc`. All configured model objects and canonical LLVM IR
must match the locally generated SDK references. A fresh production plugin/API
check must also pass before `env.sh` is published. No earlier experiment output,
binary bundle, model result or profile is an input to this workflow.

The default data recipe, [compiler/toolchain-configs.json](compiler/toolchain-configs.json),
trains on Rocket/BOOM/Shuttle and holds Saturn out of training. Pass `--configs`
with another explicit recipe to change the model set. Per-case options, including
Shuttle's existing wide-store/vector-packing/lookup-table controls, are data in
that file. These affect the generated models; profile training itself only runs
compiler programs.

The source build needs the pinned `firtool-1.162.0` SDK exposed by `env.sh`,
Clang18/Clang++18, LLD18, llvm-profdata18, the matching Clang profile runtime,
CMake, Ninja, Git and normal LLVM build dependencies, including Z3 development
headers/library when enabled by the SDK. The Chipyard setup's CIRCT step installs
the required SDK; an environment still finding an older CIRCT needs that step
rerun. `--sdk` is an optional override. Use `--cc`, `--cxx`, `--linker` and
`--profdata` for host tools outside PATH. `--host-cc` and `--host-cxx` select the
ordinary native/plugin compilers separately. Check prerequisites without building:

```bash
python3 sims/arcilator/compiler/toolchain.py check
```

`--work /path/to/fresh-directory` selects another work area. The default is
`sims/arcilator/generated-src/toolchain`, which is ignored by Git. Failed logs
and stage records are retained. Explicit `--resume` resumes the same unfinished
construction with identical sources/tools/flags; completed setups are immutable.

If the generated configuration audit reports `LLVM_ENABLE_ZLIB` as SDK `1` and
candidate `0`, CMake could not find or link zlib. Install its development package
and expose the active environment to CMake before retrying:

```bash
conda install -c conda-forge zlib
export CMAKE_PREFIX_PATH="$CONDA_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
rm -f -- sims/arcilator/generated-src/toolchain/instrument/build/CMakeCache.txt
./sims/arcilator/build-toolchain.sh --resume --jobs "$(nproc)"
```

Clearing the instrumented build's CMake cache also clears any failed zlib probe
retained from before the dependency was installed. Adjust that path if using a
custom `--work` directory. Keep the original build arguments when resuming.
If the checked-in construction scripts changed, use a fresh work directory
instead. The constructor requires
zlib when the SDK enables it, so a missing dependency fails during CMake
configuration rather than silently producing incompatible headers.

ThinLTO's exact missed-loop-unrolling warning is visible and recorded in
`profile-warning-audit.json`. It does not reject construction; profile mismatch
and unexpected warnings still do. See [the construction diagnostic
policy](compiler/FULL_TOOLCHAIN.md#fresh-construction-training-and-qualification).

Use the generated `env.sh` in subsequent shells after Chipyard's `env.sh`.
The activation script supports Bash and Zsh. To repair a completed setup whose
older activation script incorrectly looks for `toolchain.py` in the current
directory, update this branch and regenerate only that script:

```bash
python3 sims/arcilator/compiler/toolchain.py env \
  --work sims/arcilator/generated-src/toolchain --write-script
source sims/arcilator/generated-src/toolchain/env.sh
```

Use the original `--work` path if different. This checks the completed setup's
artifacts and qualification records before writing `env.sh`; it does not rebuild
the compilers or change their recorded construction identity.

`--jobs` controls compilation concurrency, `--lto-jobs` fixes ThinLTO workers,
`--link-jobs` defaults to one, and `--training-jobs` controls concurrent model
compiler cases. This setup can consume substantial memory and disk space.

Activation selects the qualified common stock-O3/native preparation settings,
four liveness workers and host CPU targeting. It disables state partitioning,
model outlining, shared procedures and ABI buffers. Wide-store distribution and
vector packing default to zero and Arcilator flags to empty; explicit Make
arguments override the environment. To reproduce the Shuttle recipe, additionally
pass `ARC_DISTRIBUTE_WIDE_STORES=1 ARC_VECTOR_PACKING=1
ARCILATOR_FLAGS=--lookup-tables=false`.

All source pins, patches, configuration data and setup code are versioned.
Generated references, profiles, tools and activation paths are local to each
machine. The command reproduces the construction recipe and checks model-code
preservation; it does not promise identical tool binary bytes across build paths
or the same performance on another host. See
[compiler/FULL_TOOLCHAIN.md](compiler/FULL_TOOLCHAIN.md) for construction guards
and the lower-level interfaces.

Native416 checks this build graph on the original full Rocket/Shuttle release
models and all ten original foreign drivers per model. With 16 available CPUs,
warm compiler/library dependencies and fresh SV, models and objects, complete
builds from the original shared FIRRTL took these seconds:

| Original config | Arcilator, two runs | Verilator 5.022, two runs |
| --- | --- | --- |
| RocketConfig | 56.23, 56.43 | 56.84, 57.23 |
| ShuttleConfig | 62.24, 62.44 | 56.04, 60.64 |

The prior native build took 58.05/64.44 seconds respectively. Common Scala
elaboration is separate: 6.61/6.81 seconds from warm Scala dependencies. Cached
main builds take 0.60 seconds. Model bitcode, objects, full metadata and generated
bindings match the qualified402 model; rebuilding at the original source paths
also reproduces all 30 native objects and the complete executables byte for byte.
The incremental-build regression covers real C/C++ drivers, source/header/flag
changes, quoted paths, cached builds and failed-compilation output preservation.

The default LLVM pipeline and runtime remain unchanged. Experimental pass
omissions were rejected when full fresh-build runtime checks found regressions;
their faster compile numbers describe archived experiments. The measured
Rocket builds beat the measured reference, while Shuttle still misses the
compile-time target. Broader configuration qualification remains open. Evidence
and retained experimental results are in
`output/development/native-compile-integration416-20261024T224229Z/`.

## Experimental state-aware partitioning

`ARC_STATE_PARTITIONING=1` enables state-aware model scheduling and outlining
before LLVM flattening. The first pass records state-object provenance before
allocation. The second constructs a dependence graph for each supported effect
epoch using SSA dependencies and exact physical-state read-after-write,
write-after-read and write-after-write dependencies. A deterministic scheduler
co-locates producers and consumers while preserving these edges. Calls, unknown
aliases, array references, storage lifetimes and unsupported control/effects
form barriers. Supported conditional bodies keep their predicates.

A costed prefix selector chooses legal low-interface cuts within bounded
windows, rather than always cutting at a fixed operation count. Admission must
improve the estimated partition/interface/frontier costs without worsening the
other limits. Epochs without admissible partitions retain their original order.
After scheduling, the pass rebuilds dominance and state-version information.
An old state value crosses the helper boundary as a snapshot unless reloading
it at that boundary is proven to observe the same physical range and version.
Allocation order, physical layout, clocks and foreign callback order remain
part of the original model.

The setting defaults to `0` and participates in model and codegen cache keys.
Enabled builds write `state-partition-report.json` (schema 2) and
`state-partition-report.plan.json` beside the model. Reports include dependency
edges, accepted/rejected scheduling decisions, selected cut costs, interface
sizes, helper counts and original operation ordinals.

With `ARC_CODEGEN_PIPELINE=stock` and more than one build job, enabled builds
split independent LLVM definitions before optimization and use the matching
SDK's ThinLTO summary/import pipeline to optimize and generate code in
parallel. This retains LLVM's cross-module analysis. Optimizer/target options
also reach the final worker pipeline; the SDK resolves the host CPU alias.
CIRCT's redundant integer-alignment rules are removed only after proving ABI
and preferred alignment unchanged, then the complete LLVM layout must equal
the target layout before its canonical spelling is used. Native ABI/storage/vector
passes run after the final optimizer. Original private symbol bindings are
restored after relocatable linking. Unsupported symbol resolutions, assembly,
targets or missing matching tools fall back to the ordinary whole-module
optimizer. `codegen-report.json` records the strategy, fallback reason and
stage timings. On the ThinLTO path, `model.bc` is the verified pre-partition
seed; the report identifies this explicitly. `ARC_BUILD_JOBS=1` keeps serial
whole-module optimization. Simulation execution remains single-threaded.

Run `make native-state-partition-test` with the matching CIRCT SDK. Independent
O0/O3 oracles cover full state bytes, old/new versions, derived clocks, opaque
callback mutations, array/partial aliases and conditional writers. The suite
also removes actual dependency/version/epoch guards to check that the oracles
detect errors. Cycle, cancellation, malformed-certificate and no-admissible-cut
fallbacks preserve their required semantics. LLVM tests cover public mutable
globals, private bindings, supported ThinLTO execution, ordinary fallback and
preservation of existing outputs after a failed backend.

Keep this opt-in until complete matched build/runtime measurements establish
its value for the intended configuration. Scalar Dhrystone and Towers runs do
not qualify vector instruction execution on Saturn or every possible model.


## Native callback schedules

The runtime builds ordered lists for combinational, sampling and timed binding
callbacks. An explicit contract owned by the exact constructed C++ type lets it
omit inherited base no-ops and timer polls that always return `never`. Base
commits still run whenever writes or internal changes are pending. Overridden
callbacks retain their order and DPI scope. Unannotated types, inherited
contracts and erased factories retain all callbacks. Use
`+arc-no-callback-schedule` to disable this optimization.

Built-in bindings declare their reviewed contracts through
`native_callback_contract()` and use `make_binding_instance<T>`. A contract is
an assertion that member lookup exposes the final virtual overriders; review
callback `using` declarations and inheritance before adding one. Subclasses
must declare their own contract to opt in. This avoids treating hidden base
member names as proof that a virtual override is absent.

Native325 qualifies the schedules on the existing full RocketConfig and
ShuttleConfig models, using the original Dhrystone500 and Towers7 programs,
selected DPI drivers and DRAMSim2. Two balanced runs per variant/program on
CPU 15 with seed 1, an 8 MiB stack and no preload measured the following median
CPU times against maintained native322:

| Configuration | Program | Native322 | Callback schedules | CPU reduction |
| --- | --- | ---: | ---: | ---: |
| RocketConfig | Dhrystone500 | 17.738 s | 15.219 s | 14.2% |
| RocketConfig | Towers7 | 1.338 s | 1.169 s | 12.6% |
| ShuttleConfig | Dhrystone500 | 15.548 s | 12.808 s | 17.6% |
| ShuttleConfig | Towers7 | 1.354 s | 1.136 s | 16.1% |

Dhrystone measurements come from the byte-identical native324 executables;
native325 corrects the generic contract and independently passes inheritance
counterexamples. All sixteen workload logs, guest counters and root cycles
match the original results. Separate checks match every final Dhrystone state
byte, model/SDK counts, lifecycle events and six decoded Shuttle results.
Standalone tests compare complete callback sequences, DPI scopes, picosecond
and femtosecond timers, pending commits, late bindings, state bytes and cached
signal addresses with scheduling disabled. Native326 rebuilds the maintained
runtime and factory objects against the new headers before inheriting those
results. The full and stable hardware model objects remain unchanged.

These measurements cover these models and programs on this host. General VPI
and all-config qualification remain incomplete. Commands, hashes, logs and
state evidence are in
`output/development/native-declared-callbacks325-20261023T011052Z/`.

## Compiler-certified stable evaluation

`ARC_STABLE_EVALUATOR=1` enables a second native evaluator for models that pass
this backend's existing idle-clock, storage-alias, effect and observation proofs.
When clock histories equal their held inputs, all certified clocked regions are
inactive. The compiler copies every unconditional operation and store into a
separate function; ordinary dead-code elimination removes calculations used
only by the inactive regions. Outputs are still recomputed. The original full
model entry remains available to the SDK and native runtime.

The native runtime selects the smaller evaluator after its SDK evaluation hook
and a fresh stability query. Sequential sampling, unsafe bindings, exported
callbacks, observations and invalidated clock contracts retain full evaluation.
The required whole-state `idle_next` comparison and current/staged storage stay
in place, preserving cached `Signal` addresses. Use
`ARC_STABLE_EVALUATOR=0` to omit the extra function, or
`+arc-no-stable-evaluator` to retain the full evaluator at runtime. The compiler
flag participates in normal and debug model-build settings.

Native321 qualifies this path on the existing complete RocketConfig and
ShuttleConfig models with the original Dhrystone500 and Towers7 programs, RTL,
DPI drivers, DRAMSim2, clock/reset behavior and diagnostics. Two balanced runs
per variant/program on CPU 15, seed 1, the normal 8 MiB stack and no preload
measured these median CPU times against maintained native313:

| Configuration | Program | Native313 | Stable evaluator | CPU reduction |
| --- | --- | ---: | ---: | ---: |
| RocketConfig | Dhrystone500 | 20.663 s | 17.826 s | 13.7% |
| RocketConfig | Towers7 | 1.599 s | 1.353 s | 15.4% |
| ShuttleConfig | Dhrystone500 | 17.707 s | 14.547 s | 17.8% |
| ShuttleConfig | Towers7 | 1.537 s | 1.271 s | 17.3% |

All sixteen uninstrumented runs match the original complete logs, guest
counters and root cycles. Separate diagnostics match every final Dhrystone
state byte, the original SDK hook and lifecycle counts, and all six independently
decoded Shuttle dirty-L1 results. Full plus stable kernel calls equal the
original full-kernel count. Original selected driver objects are byte-for-byte
unchanged; feature-off compilation reproduces the previous normalized LLVM.
Standalone tests cover opt-outs, callback feedback, unreported commits, dirty
clock bytes, post-SDK query invalidation, state bytes, waveforms and pointer
stability. Compiler tests compare declared output bits and all other state
bytes at O0/O3, including partial-width register payloads.
Partial-width evaluations use valid input/register padding and check complete
output bytes against an independent expected value. Deliberately dirty payload
padding is exercised only by read-only queries. This follows the pinned LLVM
load contract; newer unpinned LLVM documentation may describe different rules.

These results cover the named models, programs and host controls. They do not
establish all-config runtime performance or a new Verilator comparison; general
VPI and all-config qualification remain incomplete. Commands, hashes, timings
and full-state evidence are in
`output/development/native-runtime-stable321-20261022T233015Z/`.

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
`ARC_BUILD_JOBS` defaults to the available CPU count and controls LLVM machine
code generation, host C/C++ compilation, and the patched Verilog frontend build.
Set it explicitly to limit concurrency when memory is constrained. LLVM keeps
whole-model optimization by default, then balances independent functions across
compiler processes and combines their objects. State-aware stock builds use
parallel ThinLTO optimization as described above. `ARC_BUILD_JOBS=1` uses the
original serial pipeline. `codegen-report.json` records stage timings and partition costs; a
single dominant function can limit parallelism. Ordinary parallel builds retain
the whole optimized module in `model.bc` and add instruction-selection boundaries
separately in each partition; state-aware ThinLTO builds retain the verified seed. Module or inline assembly and
non-ELF targets use one code generator. This setting does not parallelize the
simulation evaluator.
`ARC_ABI_BUFFERS` defaults to 1 and controls the private large-argument and
large-result rewrites before and after LLVM optimization. Set it to 0 to
retain those private LLVM ABIs; the original external DPI interfaces and native
wide-arithmetic ABI adapter are unchanged. This option is independent of
`ARC_SHARED_PROCEDURES`.

`ARC_EARLY_INLINE` defaults to 1 and runs standard LLVM inlining before the
arithmetic ABI is fixed, exposing small forwarded values to native lowering.
Set it to 0 to retain the previous order; the setting is tracked in the model
cache. The compiler CLI provides the matching `--no-early-inline` switch.

`ARC_CODEGEN_PIPELINE` defaults to `bounded`. Selecting `stock` uses the stock
LLVM optimization pipeline and one `llc` process, retaining ABI buffering when
`ARC_ABI_BUFFERS=1`. It omits aggregate-store materialization, vector guards,
instruction-selection boundaries and parallel model partitioning. The requested
`ARC_BUILD_JOBS` still controls host and frontend compilation; the codegen report
records both requested and actual jobs. Both options participate in build cache
invalidation.

`ARC_SPARSE_PREPARATION=1` bulk-copies unchanged canonical hardware text while
retaining the existing semantic rewriters for clocks, memories, callbacks, and
foreign instances. Inputs with unsupported newline or range layouts use the
ordinary reader. This changes compiler preparation, not the generated runtime.

`ARC_NATIVE_PREPARATION=1` prepares the owned wide-arithmetic ABI and private
procedure ABIs with a typed LLVM pass and passes binary IR to optimization.
Preparation and O3 use separate LLVM contexts, preserving the original early
inlining context and discarding its transient compiler state. The temporary
binary handoff is rebuilt for every model compilation and removed afterward.
`ARC_EARLY_INLINE`, `ARC_SHARED_PROCEDURES`, and `ARC_ABI_BUFFERS` keep their
existing roles. In this mode, `model.ll` contains LLVM before arithmetic
preparation and `model.bc` contains the optimized, prepared model. State
partitioning must be disabled.

`ARC_LLC_FLAGS` passes additional options to the selected LLVM backend and is
tracked in code-generation settings. Compiler-specific worker options require
a backend that implements them; `ARC_BUILD_JOBS` alone cannot parallelize a
single dominant LLVM function in the stock compiler.

`ARC_LATE_WIDE_ARITHMETIC` defaults to 1. Proven-pure generated wide shifts
remain typed CIRCT arithmetic through Arc optimization, and surviving shifts
above 1,024 bits become typed native helper calls afterward. The restoration and
DPI sensitivity passes share one strict external-function/type/provenance check;
ordinary, enabled, clocked or mismatched calls retain their existing handling.
Set `ARC_LATE_WIDE_ARITHMETIC=0` (or `compiler/model.py
--late-wide-arithmetic=false`) to retain the earlier arithmetic boundary. This
option participates in model cache invalidation. Native device-driver ABIs and
runtime scheduling are unchanged.

The minimal native-compatible compiler control uses:

```sh
make native CONFIG=RocketConfig ARC_MODEL_OPS=0 ARC_SHARED_PROCEDURES=0 \
  ARC_ABI_BUFFERS=0 ARC_CODEGEN_PIPELINE=stock
```

Use the same overrides on subsequent run targets; changing them refreshes the
cached model.

This retains native correctness and compatibility lowering, model metadata,
clock/reset/diagnostic behavior, original device drivers and certified runtime
optimizations. The complete Rocket/Dhrystone control measured 45.27 s against
73.91 s for the default in the same paired sequence, with identical retirement
traces, six checked final globals and evaluation/helper/hook counts. This
measurement covers Rocket; other configurations may need the default compiler
scalability protections. Evidence is in
`output/development/native-minimal-backend201-20261021T201519Z/findings.json`
and `measurement.json` there.

Native202 integrates these build controls. The default pipeline passes all 91
native variants and the compiler/frontend, RTL-library and coverage-integrity
gates. Eight O0/O3 mode checks cover 294,912 independent results, aliased
buffers, delayed captures, effects, output bounds and failed-build preservation.
The workspace Rocket builds for default, ABI-buffer-disabled and minimal modes
match their qualified model objects, metadata and checked code/data sections;
identical repeat builds leave artifacts untouched. Their complete original
Towers logs match, as do default MegaBoomV3 and Shuttle builds. The workspace
minimal Rocket executable also reproduces the full Dhrystone retirement trace
and all six checked final globals. Rocket's default settings are restored after
validation. Commands, hashes and actual-main results are in
`output/development/native-compiler-controls202-20261021T201939Z/findings.json`,
`promotion.json`, `normal-make-report.json` and `normal-program-validation.json`
there. All-config runtime qualification remains incomplete.

`ARC_OPT_LEVEL` selects LLVM IR optimization. Machine-code generation uses the
same level by default; `ARC_LLC_OPT_LEVEL` overrides it independently. For large
models, `ARC_OPT_LEVEL=1 ARC_LLC_OPT_LEVEL=0` keeps the IR optimizations while
using LLVM's faster machine-code pipeline.
`ARC_OPT_FLAGS` passes additional, shell-quoted arguments to LLVM `opt` and
defaults to empty. Its value participates in codegen cache invalidation and
appears as parsed arguments in `codegen-report.json`. Host target/CPU attributes
emitted by `opt` survive serial and parallel machine-code generation.

On the measured x86-64 host, this selects the build machine's instruction set:

```sh
make native CONFIG=RocketConfig ARC_MODEL_OPS=0 ARC_SHARED_PROCEDURES=0 \
  ARC_ABI_BUFFERS=0 ARC_CODEGEN_PIPELINE=stock \
  ARC_OPT_FLAGS='-mtriple=x86_64-unknown-linux-gnu -mcpu=native'
```

Use a compatible host CPU selection when moving the executable to another
machine. Paired full Rocket/Dhrystone controls measured 47.27 s for the minimal
baseline, 44.65 s with the host triple and 40.64 s with host-CPU selection.
All retirement traces, six checked final values and evaluation/helper/hook
counts match. This tuning changes the compiled model; original device-driver
objects and runtime remain identical. Results cover this host and workload.
Evidence is in
`output/development/native-host-target203-20261021T205822Z/findings.json`
and `measurement.json` there.

For the existing packed-cache unit fixture, host targeting alone exhausted the
2 GiB compiler admission limit in SLP vectorization. Adding
`-vectorize-slp=false` to `ARC_OPT_FLAGS` passes all 1,536 cache images within
the 2 GiB/45 s budget using stock LLVM. This unit result does not establish a
full-workload speedup or justify dropping default scalability protections across
configurations. The empty `ARC_OPT_FLAGS` default preserves the existing path.

Native204 integrates this option after all 91 default native variants and the
compiler/frontend, RTL-library and coverage-integrity gates pass. Focused target
tests check 589,824 integer/ABI results, 65,536 floating-point cases and 1,536
packed-cache images, including separate rounding, signed zero, subnormals,
infinities and NaNs. Actual Makefile builds reproduce the qualified default,
minimal, host-triple and host-CPU model objects and checked code/data sections.
Flag-only changes preserve the model/glue stages; quoted arguments and identical
repeat builds behave correctly. The workspace host-CPU Rocket binary reproduces
the full Dhrystone retirement trace and all six checked globals. Complete
original Towers logs match for tuned Rocket and default Rocket, MegaBoomV3 and
Shuttle. Rocket's empty-flag default is restored after validation. Evidence is in
`output/development/native-opt-flags204-20261021T211528Z/findings.json`,
`promotion.json`, `normal-make-report.json` and `normal-program-validation.json`
there. All-config runtime qualification remains incomplete.

Native205 compares two existing upstream options independently with the same
complete Rocket/Dhrystone host-tuned minimal pipeline. Two balanced,
uninstrumented runs per control measured:

| Existing compiler option | Median elapsed time |
| --- | ---: |
| Host-tuned minimal baseline | 40.77 s |
| Arcilator lookup tables disabled | 40.02 s |
| LLVM SLP vectorization disabled | 41.77 s |

Every full retirement trace, six checked final values and evaluation/helper/hook
count matches. Physical metadata remains identical; regenerated native glue and
the original frozen runtime/device-driver objects retain the same interface.
The small lookup-table improvement covers this workload. SLP disabling remains
a scaling option for the packed-cache unit rather than a Rocket runtime gain.
Defaults remain unchanged. The lookup-table control uses
`ARCILATOR_FLAGS='--split-funcs-threshold=8192 --lookup-tables=false'` with the
same minimal/host-tuning overrides; the SLP control appends
`-vectorize-slp=false` to `ARC_OPT_FLAGS`. Full commands, hashes, exact counts
and results are in
`output/development/native-upstream-options205-20261021T214751Z/findings.json`
and `measurement.json` there.

Native206 checks the same host-tuned minimal controls on the original
ShuttleConfig hardware with complete, unchanged Towers and Dhrystone programs.
Two balanced, uninstrumented runs per engine on CPU 15 with seed 1 and the
standard 8 MiB stack measured:

| Engine/compiler control | Towers median | Dhrystone median |
| --- | ---: | ---: |
| Native default | 5.60 s | 64.19 s |
| Host-tuned minimal, SLP enabled | 3.12 s | 35.55 s |
| Host-tuned minimal, SLP disabled | 3.18 s | 36.56 s |
| Matched Verilator reference | 1.87 s | 20.02 s |

Every native run reproduces the complete original UART log, guest counters and
root-cycle count. Separate diagnostics retain identical model-evaluation,
idle-helper, stability-helper and SDK-hook counts. A read-only host wrapper
also checks all six final Dhrystone results in valid matching L1 D-cache lines;
each value resides in a dirty line, so this check does not use backing DRAM.
Debug locations identify the existing cache allocations while requiring
regenerated LLVM and metadata to match the original models. The actual model,
runtime and original device-driver objects remain unchanged. All eight complete
final tag/data memory images match across the three native controls. Corrupted
results, invalid or duplicate tags, and truncated state fail the oracle checks.

The tuned native builds remain slower than the matched Verilator reference.
These results cover this host and the two named workloads; defaults remain
unchanged and all-config runtime qualification remains incomplete. The existing
host-tuning command above selects this optional path with `CONFIG=ShuttleConfig`;
append `-vectorize-slp=false` to compare SLP disabling. This study establishes
native numerical results; the historical Verilator six-value oracle gap is
retained. Commands, hashes and results are in
`output/development/native-shuttle-fast206-20261021T215958Z/findings.json`,
`measurement.json`, `numerical-validation.json` and `cache-oracle-provenance.json`
there.

Native207 checks the same host-tuned minimal controls on the original
MegaBoomV3Config hardware with complete, unchanged Towers and Dhrystone programs.
Two balanced, uninstrumented runs per engine on CPU 15 with seed 1 and the
standard 8 MiB stack measured:

| Engine/compiler control | Towers median | Dhrystone median |
| --- | ---: | ---: |
| Native default | 11.08 s | 83.54 s |
| Host-tuned minimal, SLP enabled | 6.52 s | 49.22 s |
| Host-tuned minimal, SLP disabled | 6.47 s | 49.06 s |
| Matched Verilator reference | 7.17 s | 54.14 s |

Every native run reproduces the complete original UART log, guest counters and
root-cycle count. Separate diagnostics retain identical model-evaluation,
idle-helper, stability-helper and SDK-hook counts. A read-only host wrapper
also checks all six final Dhrystone results in valid matching L1 D-cache lines;
each value resides in a dirty line, so this check does not use backing DRAM.
Debug locations identify the existing cache allocations while requiring
regenerated LLVM and metadata to match the original models. The actual model,
runtime and original device-driver objects remain unchanged. All eighteen
complete final tag/data memory images match across the three native controls.
Corrupted results, mismatched read copies, invalid or duplicate tags, and
truncated state fail the oracle checks. Both metadata copies and all sixteen
data arrays are checked.

The Verilator reference retains its original compiler settings; the native
controls use host-CPU selection. These are results for the two named programs.
Stock LLVM codegen took 178 s and 165 s for SLP enabled/disabled, respectively,
within this study's 4 GiB/180 s compiler admission. Prerequisite compiler caches
were warm. These results cover this host and the two named workloads; defaults
remain unchanged. All-config runtime qualification remains incomplete. The
existing host-tuning command above selects this optional path with
`CONFIG=MegaBoomV3Config`;
append `-vectorize-slp=false` to compare SLP disabling. This study establishes
native numerical results; the historical Verilator six-value oracle gap is
retained. Commands, hashes and results are in
`output/development/native-mega-fast207-20261021T221938Z/findings.json`,
`measurement.json`, `numerical-validation.json` and `cache-oracle-provenance.json`
there.

Native209 caches the five port handles of each generated asynchronous-register
bank at initialization. Each sample still reads the current values, detects the
same rising clock/reset edges and stages its output in the same nonblocking
queue. The captured byte vector moves into that queue instead of being copied
again. Handles point to stable current-model storage; settling continues to copy
state into that storage. All model evaluations, SDK hooks, foreign callbacks,
clock/reset behavior and original device drivers remain intact.

Two balanced runs per cell on CPU15, seed 1 and the default 8 MiB stack measured
the existing complete original programs with the optional host-CPU compiler path:

| Configuration | Program | Previous binding | Cached bank ports |
| --- | --- | ---: | ---: |
| RocketConfig | Towers | 3.11 s | 2.75 s |
| RocketConfig | Dhrystone | 40.75 s | 36.32 s |
| ShuttleConfig | Towers | 3.15 s | 2.84 s |
| ShuttleConfig | Dhrystone | 35.91 s | 32.82 s |

Every timing run matches the complete original log, guest counters and completion
cycle count. Focused actual-main qualification also preserves Rocket's entire
retirement trace, all six Dhrystone values and exact model/SDK evaluation counts.
Shuttle preserves the entire final model-state image, six values read through
valid dirty-cache tags and data, and exact evaluation counts. The 2051-bit integer,
array and nested-structure async fixture adds 1,024 independently checked mixed
clock/reset sequences alongside its existing asynchronous/synchronous cases.
These are focused checks for this binding change; the preceding all-91-variant
checkpoint is native204. Compiler defaults and the original Verilator reference
settings remain unchanged, and Rocket/Shuttle are still slower than that reference.
All-config runtime qualification remains incomplete. Evidence is in
`output/development/native-runtime-profile209-20261021T231903Z/promotion.json`,
`banks-measurement.json`, `banks-build.json` and `findings.json` there.

Native210/211 tested two additional inlining controls on the same complete
original Rocket/Shuttle Dhrystone programs with the native209 bank-cache binding.
Lowering LLVM's `-inline-threshold=0` leaves more helper calls but shrinks each
main evaluator by only about 1.5%. Two balanced runs per engine measured Rocket
at 35.93 s baseline versus 39.09 s for the control, and Shuttle at 32.45 s versus
34.38 s. The setting is slower on both measured programs.

The earlier upstream Arc `--inline=false` option also preserves all listed signal
layouts, but the native compiler can no longer prove the idle-clock certificate.
The conservative runtime remains active. One complete Dhrystone screening pair
measured Rocket at 36.18 s versus 51.50 s, and Shuttle at 32.73 s versus 44.60 s.
All 12 complete program runs across these two studies match original logs, guest
counters and completion cycles. Both controls were rejected; no compiler default,
runtime, original driver or existing Verilator-reference setting changed. Slower
controls did not trigger additional Towers or broad numerical/configuration
qualification. These are measured controls for the named programs, not universal
performance claims. Commands, hashes and exact scopes are in
`output/development/native-inline-control210-20261021T234124Z/findings.json` and
`output/development/native-arc-inline211-20261021T235108Z/findings.json`.

Native212 kept native-only C++ link-time optimization private: paired full
Dhrystone medians improved about 0.3% on Rocket and 1.8% on Shuttle, without a
material gain that justified changing defaults. The LLVM model and original
device-driver objects were unchanged. Its build/probe/measurement reports are in
`output/development/native-cxx-lto212-20261021T235742Z/findings.json`.

Native213 separates complete-word wide-shift loops from bounded partial-word
copies, retaining both helper ABIs, high-amount checks, sign extension and padding
masks. It also fills only bytes outside the overwritten result region. Generic
O3 compiles the two interior loops into 16-byte SIMD with runtime alias checks;
there are no new ISA flags, model, clock, reset, scheduling or driver changes.
The original independent helper test passes 119,880 O0/O3 checks up to 65,537 bits;
generated LLVM ABI tests pass 8,640 vectors, their protected-page checks and
intentional truncation-fault controls. Additional private arithmetic/guard-page
checks cover boundary words, unaligned payloads and read-only inputs.

Two balanced complete-program runs per engine/cell, using the existing optional
host-CPU LLVM path, CPU15, seed1 and the default 8 MiB stack measured:

| Configuration | Program | Previous helper | Interior-word helper |
| --- | --- | ---: | ---: |
| RocketConfig | Towers | 2.72 s | 2.61 s |
| RocketConfig | Dhrystone | 35.95 s | 34.41 s |
| ShuttleConfig | Towers | 2.95 s | 2.73 s |
| ShuttleConfig | Dhrystone | 32.65 s | 31.02 s |

Every timing matches the full original log, guest counters and completion cycles.
Shuttle's first Towers baseline was noisier than the other timing runs; the
Dhrystone gains are about 4% on Rocket and 5% on Shuttle. Rebuilt actual-main
binaries have the timed candidates' code/data sections. Rocket's entire original
retirement trace and six globals match; Shuttle's entire final state and six
dirty-cache values match. Exact model evaluations, SDK hooks and helper counts
match in both cases. These are focused checks for this pure arithmetic change;
the historical all-91-native checkpoint remains native204. Rocket/Shuttle remain
slower than the existing original Verilator reference, and all-config runtime
qualification remains incomplete. Commands and hashes are in
`output/development/native-wide-interior213-20261022T001252Z/findings.json`,
`measurement.json`, `focused-tests.json`, `guard-page.json` and `promotion.json`.

Native214 reads one-byte scalar ports directly and masks their unused high bits.
Zero-width reads still return zero without touching storage; reads above 64 bits
still reject scalar access. Larger scalar reads use the previous implementation.
The existing run-loop fixture now checks dirty one-bit clocks, other partial
bytes, zero width, an unaligned nine-bit fallback and input immutability. Both
baseline and candidate pass 129,664 independent integer/protected-page/error
checks at O0/O3; all 18 complete waveform cases match for both headers and the
rebuilt actual main. The physical-clock certificate's raw-byte guards remain intact.

Fresh baseline/candidate C++ builds use identical source paths and generic O3
flags. Fresh baseline code/data sections match the qualified native213 binaries,
and rebuilt actual-main sections match the timed candidate. LLVM models and
original device-driver objects remain byte-identical. Two paired full-program
runs per engine/cell, CPU15, seed1 and the default 8 MiB stack measured:

| Configuration | Program | Previous scalar read | Masked one-byte read |
| --- | --- | ---: | ---: |
| RocketConfig | Towers | 2.66 s | 2.57 s |
| RocketConfig | Dhrystone | 34.96 s | 33.58 s |
| ShuttleConfig | Towers | 2.77 s | 2.70 s |
| ShuttleConfig | Dhrystone | 31.62 s | 31.00 s |

All 16 timings match full original logs, guest counters and completion cycles.
Actual-main Rocket preserves the entire retirement trace and all six Dhrystone
results; Shuttle preserves its entire final model state and six valid dirty-cache
results. Exact model, SDK-hook and helper counts match for both cores. Compiler
settings, clocks/reset, diagnostics and original drivers remain unchanged. These
are focused checks, not an all-native/configuration sweep; the historical 91-case
checkpoint remains native204. Rocket/Shuttle remain slower than the original
Verilator reference, and all-config qualification remains incomplete. Evidence is
in `output/development/native-signal-byte214-20261022T003103Z/findings.json`,
`measurement.json`, `signal-unit.json`, `fixture.json`,
`baseline-equivalence.json` and `promotion.json` there.

Native215–218 checked general model-codegen controls while retaining native214.
An optimized-IR audit and 2,016 independent arithmetic checks showed that explicit
vector forms gave the same instruction counts as scalar wide-bitwise forms in
the isolated cases; the backend already uses SIMD. LLVM `select-optimize` and O2
produced unchanged normalized optimized IR for both sampled models, so redundant
codegen/firmware runs were skipped. Standard instruction sinking changed code but
paired Dhrystone runs gained only about 1.7% on Rocket and were flat on Shuttle;
all eight full logs, guest counters and cycles matched. No control was promoted.
The controls apply uniformly to IR/compiler properties; named cores and programs
identify validation cases, without design/workload-specific implementation rules.
No clock/callback proof or conservative fallback was weakened. These limited
controls do not establish all-config support or a universal performance result.
Evidence is in `output/development/native-model-op-audit215-20261022T005413Z/findings.json`,
`output/development/native-select-control216-20261022T010415Z/findings.json`,
`output/development/native-ir-sink217-20261022T010454Z/findings.json` and
`output/development/native-O2-control218-20261022T011455Z/findings.json`.

Native219 tested constant-width/mode C++ wrappers for the existing pure shift
helper, derived only from generated IR types. Both models passed independent
helper/guard checks and eight complete Dhrystone timing runs matched original
logs, counters and cycles. Balanced medians measured Rocket at 33.66 s baseline
versus 34.10 s candidate, and Shuttle at 30.71 s versus 30.35 s. The control was
rejected without changing defaults. Evidence is in
`output/development/native-helper-specialize219-20261022T012329Z/findings.json`.

Native220 passes a proven small unsigned value directly to a pure left-shift
helper, avoiding a materialized wide input buffer. Only constants fitting 64 bits
or direct zero extensions from at most 64 bits qualify, and the callee must have
the existing generated arithmetic declaration. Full shift amounts retain the
same low-word/high-word/partial-tail checks and output masks. Other expressions,
right shifts, occupied wrapper names and ordinary DPI calls retain their existing
paths. Both generic helper ABIs remain available. Selection depends on IR types
and values, with no core, instance, signal or workload identity checks. Lowering
streams the module and retains only qualifying zero extensions for each function.

The new regression checks conservative fallbacks, large decimal constants,
foreign-call preservation, symbol collisions, atomic ABI errors and idempotence.
Independent tests pass 23,520 direct scalar-data vectors and 3,640 protected-page
checks at O0/O3, plus 33,600 generated LLVM vectors across stock and bounded
codegen at O0/O3. Existing generic-helper checks pass at O0/O3; existing generated
wide-ABI checks retain their high-bit fault controls and boundary cases.

Two balanced complete-program runs per engine/cell on CPU15, seed1 and the
default 8 MiB stack, using the existing optional host-CPU compiler path, measured:

| Configuration | Program | Previous arithmetic | Scalar-input shifts |
| --- | --- | ---: | ---: |
| RocketConfig | Towers | 2.58 s | 2.49 s |
| RocketConfig | Dhrystone | 33.94 s | 32.43 s |
| ShuttleConfig | Towers | 2.71 s | 2.62 s |
| ShuttleConfig | Dhrystone | 30.89 s | 29.83 s |

All 16 runs match full original logs, guest counters and completion cycles. The
diagnostic candidate preserves Rocket's entire original retirement trace and
six globals, Shuttle's entire final model state and six valid dirty-cache values,
and exact model/helper/SDK-hook counts for both. Fresh actual-main lowering
produces the exact timed LLVM inputs; freshly compiled helper/model objects and
linked executables match the qualified candidate's loaded code/data sections.
Original driver objects, physical layouts, clock/reset behavior, callbacks,
diagnostics and compiler defaults remain unchanged. These are focused checks;
the historical all-91-native checkpoint remains native204. Rocket/Shuttle still
trail the original Verilator reference, and all-config/general-VPI qualification
remains incomplete. Commands, hashes and scopes are in
`output/development/native-scalar-shift220-20261022T013222Z/measurement.json`,
`qualification.json`, `promotion.json`, `unit.json` and `findings.json` there.

Native221 reprofiled the integrated native220 Rocket Dhrystone executable using
33,670 self-samples with no unmapped samples and the entire original program log
unchanged. The evaluator accounts for about 60% of samples; scalar-input and
remaining generic shift helpers account for about 3.7% and 2.8%. This diagnostic
profile is not a timing comparison. Evidence is in
`output/development/native-main-profile221-20261022T015020Z/findings.json`.

Native222 runs LLVM's standard CGSCC inliner before procedure-storage, private
ABI and arithmetic lowering. Ordinary forwarding calls then expose their narrow
zero-extended values to the existing native220 arithmetic contract. In the two
sampled complete models, 86 additional left-shift sites qualify; all 324 left
shifts use the scalar-input path. Selection follows normal LLVM inlining and IR
types/values, without core, signal, instance or workload-specific analysis. No
new arithmetic implementation, compiler dependency or native C++ change is needed.

`ARC_EARLY_INLINE=1` enables the stage by default. `ARC_EARLY_INLINE=0`, or the
compiler CLI's `--no-early-inline`, retains the previous lowering order. The flag
is recorded in model settings so changes invalidate the model cache. Real Make
checks accept 0/1, reject other values, and verify that repeating the same setting
preserves its timestamp. With no `--llvm-opt`, standalone arithmetic lowering
retains its existing tool-free behavior.

Two balanced full-program runs per engine/cell on CPU15, seed1 and the default
8 MiB stack, using the existing optional host-CPU compiler path, measured:

| Configuration | Program | Native220 | Early LLVM inlining |
| --- | --- | ---: | ---: |
| RocketConfig | Towers | 2.59 s | 2.23 s |
| RocketConfig | Dhrystone | 33.25 s | 29.49 s |
| ShuttleConfig | Towers | 2.54 s | 2.35 s |
| ShuttleConfig | Dhrystone | 29.52 s | 26.85 s |

All 16 runs reproduce full original logs, guest counters and completion cycles.
Independent diagnostics preserve Rocket's entire retirement trace and six globals,
Shuttle's entire final state and six valid dirty-cache values, and exact model,
helper and SDK-hook counts. Fresh actual-main default CLI output has the timed
candidate's normalized LLVM; the disabled CLI output matches native220. Fresh
model objects and linked executables match the qualified candidate's loaded
code/data sections. All native C++, original drivers, physical metadata, clocks,
reset, callbacks and diagnostics remain unchanged.

The new forwarding fixture checks 33,600 independent integer vectors across the
enabled/disabled paths and both codegen pipelines. Existing compiler-mode checks
pass 294,912 integer/ABI results and 32,768 floating-point cases, including aliases,
side effects, delayed captures and failure preservation. Existing shared-procedure
checks pass 6,291,456 words, ABI/storage guards, CLI defaults/overrides and failed
transaction preservation. These are targeted checks, not an all-native/config
sweep; the historical 91-case checkpoint remains native204. Rocket/Shuttle still
trail the original Verilator reference. All-config and general VPI work remains
incomplete. Commands, hashes and exact scopes are in
`output/development/native-early-inline222-20261022T015212Z/findings.json`,
`probe.json`, `measurement.json`, `qualification.json` and `promotion.json` there.

Native223 profiled the integrated native222 full Rocket Dhrystone program with
30,953 self-samples, no unmapped samples and the original log unchanged. A
read-only LLVM DemandedBits audit224 then found that all 84 remaining right-shift
results observe at most four bits; 42 results are unused. The audit adds no
production pass and does not specialize to those observed widths. It identified
missing optimizer information at the generated arithmetic boundary.

Native225 marks the generated integer-valued arithmetic wrappers `nounwind`,
`willreturn` and `memory(none)`. Their bounded deterministic C++ implementation
accesses private nonescaping scratch buffers and has no driver/SDK callbacks or
stateful operations. LLVM permits local alloca reads/writes under
[`memory(none)`](https://llvm.org/docs/LangRef.html#function-attributes).
Pointer C ABI declarations retain their argument-memory effects; ordinary DPI
contracts remain intact. Scalar-input wrappers inherit the same generated
contract. No new arithmetic implementation, custom optimization pass, compiler
dependency or runtime C++ change is needed.

Stock LLVM optimization reduces scalar-helper call sites from 324 to 108 and
general word-helper sites from 85 to 35 in each sampled model. These counts are
static diagnostics. Two balanced complete-program runs per engine/cell on CPU15,
seed1 and the default 8 MiB stack, using the existing optional host-CPU path,
measured:

| Configuration | Program | Native222 | Explicit arithmetic contract |
| --- | --- | ---: | ---: |
| RocketConfig | Towers | 2.32 s | 1.89 s |
| RocketConfig | Dhrystone | 30.20 s | 24.70 s |
| ShuttleConfig | Towers | 2.41 s | 1.94 s |
| ShuttleConfig | Dhrystone | 27.65 s | 22.11 s |

All 16 full logs, guest counters and completion cycles match. Independent
qualification preserves Rocket's complete retirement trace and six globals,
Shuttle's entire final model state and six valid dirty-cache values, and exact
model/helper/SDK-hook counts. Fresh actual-main normalized LLVM and freshly
compiled model/executable loaded sections match the qualified timed candidate.
All native C++, original device-driver objects, physical metadata, clocks/reset,
foreign callbacks, diagnostics and compiler tuning settings remain unchanged.

The generated ABI fixture adds 432 independent unused-result cases covering full
and scalar data, all shift modes, aliases, unsigned wraparound and a separate
ordinary counter-writing call. The optimized code drops unused arithmetic and
keeps that effect. Existing 8,640 full-ABI and 33,600 scalar-input LLVM vectors,
O0/O3 protected-page checks and high-bit fault controls pass. Original CIRCT tests
pass 10 pure-helper provenance guards and 25,344 compiled value/stable/sampling
checks with legacy sensitivity controls. This is targeted qualification; the
historical all-91-native checkpoint remains native204. Current results do not
reproduce the magnitude reported in the upstream 2023 talk, and no cause for that
gap is assumed from harness overhead alone. Existing original Verilator controls
are retained, without new configurations/workloads or reference jobs. All-config
and general VPI work remains incomplete. Evidence is in
`output/development/native-pure-contract225-20261022T021745Z/findings.json`,
`measurement.json`, `qualification.json`, `promotion.json` and `unit.json`.
Profile/audit evidence is in
`output/development/native-main-profile223-20261022T021244Z/findings.json` and
`output/development/native-demand-audit224-20261022T021434Z/findings.json`.

Native226 reprofiled native225 on the complete original Rocket Dhrystone
program: 25,768 self-samples, no unmapped samples and the whole original log
unchanged. The evaluator accounts for about 59% of samples. PBQP control227
exceeded the controlled 4 GiB address-space budget for both models; the plain
allocator comparison and initial extra-verifier attempt are archived separately.
Greedy-priority control228 produced identical loaded executable code/data, and
additional closed C-helper effect facts229 produced identical machine objects.
Those controls remain private and triggered no firmware timings. Evidence is in
`output/development/native-main-profile226-20261022T024000Z/findings.json`,
`output/development/native-pbqp227-20261022T024225Z/findings.json`,
`output/development/native-greedy-priority228-20261022T024700Z/findings.json` and
`output/development/native-helper-effects229-20261022T025155Z/findings.json`.

Native230 returns zero directly when a scalar input to a pure logical left shift
is zero. This identity holds for every shift amount. The comparison uses LLVM
[`freeze`](https://llvm.org/docs/LangRef.html#freeze-instruction), preserving the
defined case where poisoned data is ignored by an overshift. The original value
still feeds the nonzero path, and output allocation stays in the entry block.
Full-width shift amounts, pointer helper memory effects and existing fallbacks
remain intact. Selection uses integer values/types rather than core, signal,
instance or workload identity; native C++ and device-driver objects are unchanged.

Two balanced complete-program runs per engine/cell on CPU15, seed1 and the
default 8 MiB stack, using the existing optional host-CPU path, measured:

| Configuration | Program | Native225 | Zero-input fast return |
| --- | --- | ---: | ---: |
| RocketConfig | Towers | 1.91 s | 1.81 s |
| RocketConfig | Dhrystone | 24.85 s | 23.94 s |
| ShuttleConfig | Towers | 1.99 s | 1.93 s |
| ShuttleConfig | Dhrystone | 23.09 s | 21.91 s |

All 16 runs match full original logs, guest counters and completion cycles.
Independent diagnostics preserve Rocket's complete retirement trace and six
globals, Shuttle's entire final state and six valid dirty-cache values, and exact
model/helper/SDK-hook counts. Fresh actual-main normalized LLVM and freshly
compiled model/executable loaded sections match the qualified timed candidate.
Runtime scheduling, state snapshots, clock/reset behavior, callbacks, diagnostics,
hardware metadata and compiler tuning settings remain unchanged.

The scalar fixture now checks 144 defined poison-data overshift and effectful
amount-producer cases at O0/O3 with both codegen pipelines. An isolated LLVM
helper double models the defined overshift edge; real arithmetic uses the native
implementation and independent Python integer vectors. Existing 33,600 scalar
LLVM vectors, full-ABI vectors, boundary/alias guards and ordinary-effect checks
pass. A private mutation experiment incorrectly required one optimizer outcome
for undefined control flow; that expectation was removed, and the corrected
safe semantic cases and actual-main fixtures pass. Undefined variants are never
executed or linked to firmware.

These are targeted checks, not an all-native/config sweep; the historical 91-case
checkpoint remains native204. No new configurations, firmware workloads or
Verilator reference jobs were added. Current evidence does not reproduce the
upstream speedup magnitude or establish all-config/general-VPI support. Commands,
hashes and scopes are in
`output/development/native-zero-shift230-20261022T030304Z/findings.json`,
`measurement.json`, `qualification.json`, `promotion.json`, `unit.json` and
`poison-edge.json` there.

Native231 profiles the integrated native230 executable on the complete original
Rocket Dhrystone program: 25,194 samples, none unmapped, with its entire original
log unchanged. Model evaluation accounts for 59.8% of samples, runtime settling
7.7%, and libc 18.4%. This is diagnostic evidence rather than a timing comparison.

Three focused controls remain private. Conditional byte-exact state copyback232
passes 9,120 independent byte-array cases and 104 protected-page cases at O0/O3,
but reduces Dhrystone time only 1.5% on Rocket and 0.9% on Shuttle. Host-targeted
native C++233 preserves all 18 freshly rebuilt generic baseline object sections
per core; its Dhrystone reduction is about 3.0% on Rocket and 0.8% on Shuttle.
Those flags affect only the private native C++ objects, so the experiment is not
equivalent to setting the Makefile's current flags for all original drivers.
A local source search overlapped part of the Rocket comparison; no positive
performance claim rests on those timings. Control234 changes only the child
process's [glibc copy threshold](https://www.sourceware.org/glibc/manual/latest/html_node/Hardware-Capability-Tunables.html)
from the loader-confirmed 2,112 bytes to 1 MiB, using identical native230
executables; both cores regress. The small or inconsistent gains do not justify
integration or extra controls. Across these experiments, all 28 complete original
timing runs retain their full logs, counters and completion cycles. No additional
Towers or numerical/count runs were started after failed admission thresholds.

Native230 remains the integrated implementation. Model/driver code, clocks/reset,
callbacks, build defaults and the parked compatibility work are unchanged. No new
configurations, firmware workloads or Verilator reference jobs were added. The
all-config and general-VPI goal remains incomplete. Evidence and exact commands:
`output/development/native-main-profile231-20261022T033826Z/findings.json`,
`output/development/native-copyback232-20261022T033956Z/findings.json`,
`output/development/native-host-cpp233-20261022T034625Z/findings.json`,
`output/development/native-libcopy234-20261022T035057Z/findings.json`.

Native235 and native236 test exposing scalar-input logical-left-shift arithmetic
directly to standard LLVM optimization. Both retain the existing generated-pure
type/provenance selection and generic fallback. They freeze control probes and
shift amounts, keep finite in-range guards and local scratch, and use bounded
host-word operations. Variant235 bounds its final partial-word loop explicitly;
variant236 allocates eight additional private bytes so a word and carry-byte
store fit without that loop. Only the original result bytes are read back.

Each private emitter passes the original independent scalar/full-ABI integer,
ordinary-effect and boundary tests under stock/bounded LLVM at O0/O3, including
144 defined poison/ordinary-argument cases. Additional exact-storage fixtures
check 32,928 byte-exact protected-boundary cases per candidate at O0/O3, including
unaligned destinations and read-only amount buffers. Deliberately enlarged
writes must produce an actual SIGSEGV/SIGBUS in the guard child. These tests use
the exact generated scratch body with external storage only in the isolated
fixture; no test double or mutation is linked to firmware.

Fresh full-model builds remain within the controlled 4 GiB/180 s budget per
compiler process. Native C++, original device-driver objects, full hardware and
ELFs, model layout, clock/reset behavior, diagnostics and SDK callbacks are
unchanged. Two balanced full Dhrystone runs per engine/core on CPU15, seed1 and
the default 8 MiB stack measured:

| Candidate | Configuration | Native230 baseline | Candidate |
| --- | --- | ---: | ---: |
| Inline235 | RocketConfig | 24.52 s | 25.25 s |
| Inline235 | ShuttleConfig | 22.19 s | 23.26 s |
| Padded236 | RocketConfig | 24.58 s | 25.23 s |
| Padded236 | ShuttleConfig | 22.04 s | 22.71 s |

All 16 complete logs, guest counters and completion cycles match the originals,
but both candidates regress. Their larger model objects correlate with those
results without establishing a cause. Both remain private; no Towers or further
numerical/count qualification was justified, and native230 stays integrated.
No extra tool dependency, build-default, runtime, configuration/workload or
Verilator-reference change was made. These are focused little-endian x86-64
host results, not all-config/general-VPI qualification. Evidence:
`output/development/native-inline-scalar235-20261022T035758Z/findings.json` and
`output/development/native-padded-scalar236-20261022T040540Z/findings.json`,
with `unit.json`, `internal-guards.json`, `build-report.json` and
`measurement.json` in each directory.

Native237 audits the installed standard O3 pipeline on exactly the native230,
inline235 and padded236 inputs. It already runs early CSE before forced inlining;
all three reduce 324 scalar-helper calls to 108 per core at that point. Lost
pre-inlining call elimination therefore does not explain the inline regressions.
Native238 checks every SSA use of the surviving generic right-shift results:
all 35 per core are scalar truncations after standard normalization, to observed
widths 1, 4 or 64. The corrected diagnostic covers fastcc calls and cross-checks
its call count against237. These are read-only audits, not a performance claim.

Private native239 prototypes a bounded low-word C helper and conservative
truncation-only LLVM result lowering. It checks complete uses, types, known pure
provenance, reserved-name collisions and helper ABI; extra uses and quoted local
identifiers retain the full ABI. The helper passes 69,840 independent integer
vectors and 10,080 read-only protected-page vectors at O0/O3, including full upper
shift amounts, signed extension, partial words and dirty padding. Stock/bounded
LLVM at O0/O3 passes 42,000 additional vectors and 36 poison/ordinary-effect
cases, plus alias, idempotence, collision and atomic ABI-failure controls. A valid
renamed-argument case verifies the conservative owned-signature fallback.

The prototype keeps existing scalar-input lowering first, then uses the same
one-iteration InstCombine configuration as standard O3, then narrows eligible
results. This removes 162 unused scalar calls and preserves the remaining 162;
42 wide-result calls per core qualify before the rest of O3. The earlier order
lost some scalar-input recognition and was not used for timings. Initial tooling
and fixture corrections are archived and detailed in the report.

Fresh models and the new arithmetic object use unchanged O3/native-CPU/llc-O3
settings; other native objects, original device drivers, full hardware/ELFs,
layout, clocks/reset, diagnostics and SDK callbacks remain frozen. Two balanced
complete Dhrystone runs per engine/core on CPU15, seed1 and an 8 MiB stack measured
Rocket 24.86 -> 24.39 seconds and Shuttle
22.00 -> 21.98 seconds. All eight complete logs, counters and completion cycles match.
The gain is too small/inconsistent for integration, so native230 remains current.
No Towers, extra numerical/count qualification, new configuration/workload,
Verilator reference or production/default change followed the failed admission.
The all-config/general-VPI goal remains incomplete. Evidence:
`output/development/native-phase-audit237-20261022T041325Z/findings.json`,
`output/development/native-result-audit238-20261022T041530Z/findings.json` and
`output/development/native-low-result239-20261022T041826Z/findings.json`.

Native240 retains proven-pure wide arithmetic through Arc optimization and
lowers surviving large shifts afterward. A shared ownership check requires both
unit provenance markers, the exact generated name/mode/width, compatible integer
arguments/result and an external declaration. Enabled/clocked calls and foreign
or malformed contracts retain their handling; colliding unknown late symbols
retain their arithmetic operation. Selection is based on IR/type/effect contracts,
with no core, signal, instance or workload identity checks. The existing native
C++ arithmetic, device drivers, clocks/reset and diagnostics are unchanged.

Fresh controls reproduce the old metadata and native230 normalized LLVM exactly.
Candidate bindings are generated from their actual new metadata. Two balanced
complete-program runs per engine/core/program on CPU15, seed1 and the default
8 MiB stack measured the existing optional stock/native-CPU path:

| Configuration | Program | Native230 | Late arithmetic | Less elapsed time |
| --- | --- | ---: | ---: | ---: |
| RocketConfig | Dhrystone | 24.58 s | 20.59 s | 16.2% |
| RocketConfig | Towers | 1.89 s | 1.59 s | 16.1% |
| ShuttleConfig | Dhrystone | 21.94 s | 19.45 s | 11.3% |
| ShuttleConfig | Towers | 1.95 s | 1.69 s | 13.0% |

All 16 timing runs match complete original logs, guest counters and completion
cycles. Independent diagnostics preserve Rocket's entire retirement trace and
six final globals, Shuttle's six valid dirty-L1 results, and exact model
evaluation, idle/stability-helper and SDK-hook counts. These comparisons use the
original complete Dhrystone500 and Towers7 ELFs and unchanged original drivers.
The measured compile settings are reproducible with:

```sh
make native CONFIG=RocketConfig ARCILATOR_FLAGS= ARC_MODEL_OPS=0 \
  ARC_SHARED_PROCEDURES=0 ARC_ABI_BUFFERS=0 ARC_CODEGEN_PIPELINE=stock \
  ARC_LATE_WIDE_ARITHMETIC=1 \
  ARC_OPT_FLAGS='-mtriple=x86_64-unknown-linux-gnu -mcpu=native'
```

Use the same overrides on run targets; substitute `CONFIG=ShuttleConfig` for the
other measured core. Empty `ARCILATOR_FLAGS` selects the exact measured pipeline.
These timings compare native230 with native240 on this host, not a new matched
Verilator comparison or a universal/default-pipeline speedup claim.

Rocket's model buffer changes from 199,441 to 199,377 bytes; Shuttle's changes
from 189,713 to 189,617. Interval accounting shows identical declared storage
sizes and fewer alignment-gap bytes. Old state offsets are therefore not reused.
Source-location-only diagnostic builds reproduce the timed LLVM and metadata
exactly. Their static typed location/allocation-order correspondence compares all
4,099 Rocket and 4,774 Shuttle allocation fields, including every logical memory
row, and all 858/861 public ports. Repeated source-location groups use fixed IR
order, never runtime-value matching. Logical value bits and the SDK context
prefix match; alignment gaps and unused padding bits are not numerical values.
Fresh source-tag-derived cache offsets locate Shuttle's valid resident results.
Corruption, swapped distinct repeated-group fields, invalid tags and duplicate
valid tags are detected by the independent oracles.

Actual main passes 50,688 compiled arithmetic/stable/reset/sampling checks under
stock/bounded O0/O3 with legacy controls, 22 provenance/collision/clock/enable
guards, and the ordinary-DPI sensitivity suite. A new early/late O0/O3 fixture
checks 12,384 defined zero-result bytes and 20 exact ordinary callbacks: poisoned
data is ignored by a defined overshift, and an observable amount producer still
runs when zero-data arithmetic makes its result unused. Fresh actual-main LLVM,
metadata, generated factory/model objects and executable loaded sections match
the timed, numerically qualified candidate. The shared provenance refactor also
reproduces the disabled control. Actual Make checks verify 0/1 validation,
invalidation, unchanged repeat settings, worker-count independence and default1.

This is targeted qualification on the two authorized cores/programs, not an
all-native/configuration sweep; the historical 91-case checkpoint remains
native204. No new configurations, workloads or Verilator reference jobs were
added, and the private time-compatibility change remains parked. General VPI and
all-config qualification remain incomplete; upstream advertised speedup magnitude
is not established. Exact commands, hashes and scopes are in
`output/development/native-late-arithmetic240-20261022T043844Z/findings.json`,
`measurement.json`, `qualification.json`, `promotion.json`, `state-layout.json`,
`state-map.json`, `layout-padding-proof.json` and `Make-flags.json` there.

Native251's general immutable-byte-table hoist passed 660,256 original/hoisted
byte-value and callback-order checks, plus 29 fallback/collision guards. Removing
constant stack initialization still made Rocket 3.1% slower and reduced Shuttle
time by only 1.0%, so that prototype remains private.

Native252 instead checks the existing `--lookup-tables=false` option on the same
optional stock/native-CPU pipeline. Fresh controls reproduce native240's metadata
and normalized LLVM exactly. Candidate factory objects are regenerated from
their actual metadata and have identical allocated code/data to the qualified
factory objects. All remaining native/runtime/math and original driver objects stay
fixed. Two balanced complete-program runs per engine/core/program measured:

| Configuration | Program | Native240 | Lookup tables disabled | Elapsed change |
| --- | --- | ---: | ---: | ---: |
| RocketConfig | Dhrystone | 20.69 s | 21.05 s | 1.8% slower |
| ShuttleConfig | Dhrystone | 19.57 s | 18.47 s | 5.6% less time |
| ShuttleConfig | Towers | 1.71 s | 1.62 s | 5.5% less time |

All 12 timing runs reproduce the complete original logs, guest counters and
completion cycles. Rocket's failed Dhrystone admission did not trigger Towers
or further numerical runs. The admitted Shuttle setting additionally matches
the original log, exact model/SDK counts, all 4,774 retained allocation fields
(16,703 logical rows), 861 ports, the complete 189,617-byte final state and all
six valid dirty-cache numerical results. Source-location-only diagnostics
reproduce the timed LLVM and metadata and prove that every field/port offset is
unchanged; repeated source groups retain the fixed IR-order correspondence.
Corrupted values/state, invalid tags and duplicate valid tags are detected.
Shuttle's original log has no instruction-by-instruction retirement trace.

The measured Shuttle setting is reproducible with:

```sh
make native CONFIG=ShuttleConfig ARCILATOR_FLAGS='--lookup-tables=false' \
  ARC_MODEL_OPS=0 ARC_SHARED_PROCEDURES=0 ARC_ABI_BUFFERS=0 \
  ARC_CODEGEN_PIPELINE=stock ARC_LATE_WIDE_ARITHMETIC=1 \
  ARC_OPT_FLAGS='-mtriple=x86_64-unknown-linux-gnu -mcpu=native'
```

Use the same overrides on run targets. This is an existing-option tuning result
for the two original Shuttle workloads on this host; common settings remain
unchanged. It is not a new paired Verilator comparison or an all-config/general
VPI qualification. No configurations, workloads or reference jobs were added.
Evidence, exact commands and hashes are in
`output/development/native-constant-tables251-20261022T064606Z/findings.json` and
`output/development/native-lookup-disabled252-20261022T065931Z/findings.json`,
`measurement.json`, `qualification.json`, `state-layout.json` and `state-map.json`
there.

`ARC_DISTRIBUTE_WIDE_STORES=1` enables optional LLVM store distribution after
ordinary optimization and before codegen block boundaries. It removes a large
integer PHI when a closed, single-use expression leads to a full-width store.
The expression and store execute on the selected incoming edge; conditional
edges are split where necessary. External operands must already be available
on every edge. The store crosses only pure instructions, proven-disjoint
ordinary loads and the end of a distinct scratch lifetime. Other calls, writes,
volatile/atomic accesses and uncertain lifetimes or aliases retain the original
code. Source loads and lifetime markers stay in place, and every retained state
bit is preserved. The option defaults to `0`; eligibility uses LLVM types,
control flow and memory effects.

Native272 measured this option against the qualified native240 Rocket and
lookup-disabled native252 Shuttle executables on the same host, using two
balanced repeats of each admitted original program:

| Configuration | Program | Control | Store distribution | Elapsed change |
| --- | --- | ---: | ---: | ---: |
| RocketConfig | Dhrystone | 20.47 s | 20.24 s | 1.1% less time |
| ShuttleConfig | Dhrystone | 18.47 s | 17.65 s | 4.4% less time |
| ShuttleConfig | Towers | 1.606 s | 1.556 s | 3.1% less time |

Rocket's result was below admission, so no additional Rocket workload or
numerical runs were launched. All twelve timing logs, guest counters and
completion cycles match their original references. Shuttle additionally matches
all 189,617 final state bytes, exact model/SDK counts, all retained fields and
ports, and the six independent dirty-cache Dhrystone results. Corrupted state
and values, invalid tags and duplicate valid tags are detected. Native273
checks the actual integrated compiler with the option off and on against the
qualified control and measured candidate model objects. Repository tests cover
byte values, aliases, callbacks, protected memory boundaries, edge execution,
and restricted lifetime/poison controls through both codegen pipelines.

To reproduce the admitted Shuttle setting, add `ARC_DISTRIBUTE_WIDE_STORES=1`
to the native252 command above and to its run targets. This is a host tuning
result for the original programs; it does not establish every configuration,
general VPI support, or a new paired Verilator comparison. Evidence is in
`output/development/native-wide-stores272-20261022T121658Z/measurement.json`,
`qualification.json`, `unit.json` and `controls.json`, and
`output/development/native-wide-store-integration273-20261022T125014Z/integration.json`.

`ARC_VECTOR_PACKING=1` enables optional fixed-vector representation of pure
integer packing DAGs after ordinary optimization and store distribution. It
handles bitwise operations, zero extension, scalar-conditioned selection and
constant logical shifts within one basic block. Target vector legality and the
estimated cost of every new operation, conversion, shuffle and mask determine
eligibility. Rejected temporary instructions are removed. Left shifts restore
the original integer width before reuse; per-lane shift amounts stay in range.
Scalar memory access types and positions, native ABIs, state layout, freezes,
PHIs and foreign calls retain their existing handling. Big-endian targets,
unsupported types, flagged or dynamic shifts, small DAGs and unprofitable
conversions retain scalar code. The setting defaults to `0`, participates in
codegen cache invalidation, and has a matching `compiler/codegen.py
--vector-packing` switch for both codegen pipelines.

Native281 measured the original complete programs against the qualified
native273 builds on the same host, with two balanced repeats per variant:

| Configuration | Program | Control CPU time | Packing CPU time | CPU change |
| --- | --- | ---: | ---: | ---: |
| RocketConfig | Dhrystone | 20.99 s | 20.91 s | 0.4% less |
| ShuttleConfig | Dhrystone | 18.10 s | 17.14 s | 5.3% less |
| ShuttleConfig | Towers | 1.576 s | 1.512 s | 4.1% less |

Rocket remains on its previous settings because its improvement was below
admission. All twelve timing logs, guest counters and completion cycles match
their original references. Shuttle also matches all 189,617 final-state bytes,
exact model/SDK counts, retained fields and ports, and six independently decoded
dirty-cache Dhrystone results. Corrupted values and state, invalid tags and
duplicate valid tags are detected. Native282 verifies whole model objects and
executables from the maintained CLI against the qualified controls and measured
candidates with the option off and on. Tests cover values, aliases, callback
order, protected memory boundaries, fallback paths and failed-build preservation
through stock and bounded codegen. Dirty-byte fixtures use byte-sized loads and
ordinary truncation so those inputs have defined semantics; these are targeted
checks rather than a complete LLVM refinement proof.

The admitted Shuttle setting is:

```sh
make native CONFIG=ShuttleConfig ARC_MODEL_OPS=0 ARC_SHARED_PROCEDURES=0 \
  ARC_ABI_BUFFERS=0 ARC_CODEGEN_PIPELINE=stock \
  ARC_DISTRIBUTE_WIDE_STORES=1 ARC_VECTOR_PACKING=1 \
  ARC_OPT_FLAGS='-mtriple=x86_64-unknown-linux-gnu -mcpu=native' \
  ARCILATOR_FLAGS='--lookup-tables=false'
```

Use the same settings for run targets. These measurements qualify tuning for
the original workloads on this host. All-config and general VPI qualification
remain incomplete. Evidence is in
`output/development/native-vector-packing281-20261022T145919Z/measurement.json`,
`qualification.json`, `unit.json`, `packing-unit.json` and `findings.json`, and
`output/development/native-vector-packing-integration282-20261022T152250Z/integration.json`.

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
(16,384 operations by default; set 0 to disable outlining). Stack allocations and layout
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
The default is 16,384 operations. Per-stage `*-settings.json` dependencies
record the effective frontend, filelist, partitioning, model, LLVM code-generation
and host compiler/link options. Changing those options or tool paths rebuilds
the affected stages and their consumers. Repeating the same settings preserves
timestamps; changing only `ARC_BUILD_JOBS` does not rebuild compiled artifacts.
Normal/debug and native/hybrid settings live in their respective build directories.

`ARC_SHARED_PROCEDURES=1` forwards values between eligible private
procedures through one invocation-local workspace. Each input is stored once,
and consumers can read producer results directly. Public, address-taken,
constrained-ABI, GC, `noipa` and `optnone` functions retain their signatures.
Modules containing assembly retain their original ABIs as well.
Additional workspace storage is capped at 1 MiB per caller; groups exceeding
that limit retain their original ABI. The option defaults to 1 and participates
in model-build settings, so toggling it rebuilds the affected artifacts.
Set `ARC_SHARED_PROCEDURES=0` to disable sharing. The compiler CLI enables it
when `--llvm-opt` is supplied; `compiler/llvm.py --no-share-procedures` disables
it explicitly, and `--share-procedures` enables it explicitly.
For example, after the normal environment setup:

```sh
make native CONFIG=RocketConfig ARC_MODEL_OPS=16384 ARC_SHARED_PROCEDURES=1
```

On the complete cached Shuttle hello workload, a fresh current-compiler and
native-runtime build at 16,384 operations measured 19.33 s without sharing and
18.73 s with sharing. Both completed the original program in 30,716 cycles,
using the original selected C/C++ drivers, DRAMSim2, default clocks, O3 and the
default 8 MiB stack. Verilator measured 2.21 s in the paired comparison.
This covers cached hardware rather than fresh Scala elaboration, and does not
establish a speedup for every configuration. Compiler/source fingerprints and
commands are recorded in
`output/development/native-shared-main170-20261021T062908Z/work/full-main-Shuttle/report.json`.
`tests/shared_procedures.py` checks 6,291,456 independent words, nested and
packed results, pointers, aliases, side effects, multiple returns, ABI and
workspace-limit fallbacks, and failed-input preservation at O0/O3. Selected
native regressions enable the pass by default; compare the original ABI with
`tests/run_native.py --no-share-procedures --variants arrays sensitized-dpi scheduled-dpi latches`.
Existing builds without settings files rebuild once to establish these dependencies.

Large Boolean lookup tables with at least 256 entries and at least 75% of one
constant value use a byte fill followed by the original differing and dynamic
stores. Their storage remains local to the model invocation. The compiler keeps
small tables and other element types on the existing path.
`tests/boolean_arrays.py --circt-bin "$CIRCT_BIN"` checks 188,416 independent
entries and adjacent memory guards at O0/O3. On the same complete cached Shuttle
workload, this change measured 18.03 s against 18.62 s for the baseline, with
both completing in 30,716 cycles. This is a workload-specific measurement;
the report is in
`output/development/native-boolean-fill172-20261021T072908Z/work/full-Shuttle/report.json`.

The model compiler also skips phase-only observation-cache updates for generated
combinational DPI coroutines during sequential sampling. Actual input changes
retain their pending activation, including transitions that return to an earlier
value while sampling is held. Startup and unsupported coroutine shapes use the
existing behavior, and runtime fixed-point convergence remains unchanged.
`compiler/model.py --freeze-sampling=false` disables this optimization.
`tests/sampling_phases.py --circt-bin "$CIRCT_BIN"` compares every output and
callback trace over 800 steps at O0/O2, including initial sampling values,
transient inputs, clocks, aggregates and real values. The isolated optimization
measured 15.73 s against 18.54 s on the complete cached Shuttle workload, with
both completing in 30,716 cycles. Evidence is in
`output/development/native-sampling-preserve173-20261021T073756Z/work/full-Shuttle/report.json`.

With both optimizations, the native174 compiler and freshly built native
runtime completed the same cached Shuttle workload in 15.22 s, compared with
18.53 s for the shared-procedure baseline and 2.21 s for Verilator. Completion
remained 30,716 cycles; model evaluations fell from 153,991 to 127,350.
The benchmark snapshot matched all 37 top-level compiler sources at that checkpoint.
Ten selected native fixture bodies and the standalone Boolean, sampling-phase
and procedure-ABI checks also passed. These results cover cached Shuttle
hardware and focused regressions; full configuration coverage remains incomplete.
Reports are in
`output/development/native-combined-main174-20261021T074442Z/work/full-Shuttle/report.json`
and its sibling `main-native-checks/report.json` and `final-source-checks.json`.

Native settling uses compiler-certified write-only output slots to avoid
repeating a model evaluation solely because an unread output changed. Every
private state, input, context and padding byte remains in the convergence
comparison. The compiler checks overlapping storage aliases and records its
certificate in `model.json`; native bindings must separately opt in to the
combinational feedback contract. Unknown bindings, output readback, opaque DPI
imports, context/exports, time-dependent models and compiler observation modes
retain the full-state comparison. This affects settling comparisons; sequential
sampling, nonblocking publication, analog resolution and snapshots keep their
existing behavior.

`ARC_OUTPUT_CONVERGENCE=1` enables this optimization by default.
`ARC_OUTPUT_CONVERGENCE=0` disables it and participates in model-build settings;
the equivalent compiler option is `--output-convergence=false`.
`tests/convergence.py --circt-bin "$CIRCT_BIN"` checks storage alias/readback
proofs, source effects, metadata validation, opt-out/observation modes, private
state, feedback, unknown bindings, late exports and independently owned runtime
layouts. `tests/build_settings.py` additionally checks real Rocket rebuilds,
unchanged repeated settings, restoration and execution with the option disabled.

The integrated native184 sources pass all 88 default and three optional native
variants, the frontend and LLVM ABI/code-generation gates, RTL-library checks
and coverage-integrity checks. The convergence regression covers 17 allocated
layout/readback cases, 12 source-effect cases, 13 metadata cases, four compiler
modes and 20 runtime cases. Fresh main-tree default-O3 Rocket hello, UART
transmit/receive and JTAG register/memory tests pass at 32,896, 192,596 and
37,226 cycles with the original device drivers and Verilator unavailable.
These checks cover the recorded fixtures and Rocket workloads; qualification
across every configuration remains incomplete. Reports are in
`output/development/native-output-convergence-main184-20261021T094819Z/report.json`
and its `work/integration-final/report.json`.

On the complete cached Shuttle hello workload, the final native184 compiler
certifies all 568 outputs without changing the complete generated LLVM or state
layout. Fresh native runtime/device-driver builds measured 11.22 s enabled and
15.53 s disabled, with byte-identical complete logs and 30,716 completion cycles.
Model evaluations with the optimization totaled 92,468 (61,671 current-state
and 30,797 staged evaluations). The paired Verilator reference measured 2.11 s;
Arcilator remains slower on this workload. These timings use O3, default clocks,
the complete original ELF, original selected drivers and the default 8 MiB
stack. They cover cached Shuttle hardware rather than fresh Scala elaboration.
The benchmark report is the native184 report directory's
`work/full-Shuttle-final/report.json`.

Compiler-generated wide shifts carry a separate pure-arithmetic contract.
The sensitivity pass validates the declaration, width, mode, signature and
contract before leaving a bare helper call in the combinational dataflow.
An ordinary DPI call, a familiar symbol name or a no-readback contract alone
keeps event-sensitive lowering. This removes coroutine observation state for
calculations implemented by the backend's own byte arithmetic helper.
`tests/pure_wide_shift.py --circt-bin "$CIRCT_BIN"` checks ten provenance
cases and 25,344 independently computed values at O0/O3, including six
widths, chained shifts, dirty padding, stable evaluations, clock/reset sampling
and two model instances. Unmarked declarations exercise the legacy sensitivity
path as a control. All 91 native variants and the frontend, LLVM ABI/codegen,
RTL-library and coverage-integrity gates pass with these sources.

On the complete cached Shuttle workload, native185 reduces model state from
616,833 to 189,713 bytes and raw LLVM from 25.6 MB to 13.5 MB. Fresh default-O3
builds measured 8.22 s against 11.23 s for native184 and 2.21 s for Verilator,
with identical complete native logs and 30,716 completion cycles. O1/llc0 also
completes the original program with the identical full log; its code-generation
stage took 6.67 s and its program run 25.70 s. Those O1 measurements are a
separate correctness run, not a paired performance comparison. These results
cover cached Shuttle hardware; they do not establish fresh Scala elaboration,
every configuration or faster-than-Verilator runtime. Evidence is in
`output/development/native-pure-wide185-20261021T102658Z/work/full-qualified/report.json`,
`work/pure-wide-tests/report.json`, `full-native-report.json` and
`work/O1-llc0/report.json` within that same report directory.

Fresh RocketConfig, MegaBoomV3Config and ShuttleConfig builds also complete the
unchanged installed Dhrystone (500 iterations) and Towers (seven discs)
benchmarks with DRAMSim2 and the original drivers. Two-run median wall times
for native185 and the matched Verilator builds are:

| Configuration | Benchmark | Native | Verilator |
| --- | --- | ---: | ---: |
| RocketConfig | Dhrystone | 142.31 s | 20.46 s |
| RocketConfig | Towers | 10.74 s | 1.57 s |
| MegaBoomV3Config | Dhrystone | 137.88 s | 54.29 s |
| MegaBoomV3Config | Towers | 18.22 s | 7.17 s |
| ShuttleConfig | Dhrystone | 116.74 s | 20.90 s |
| ShuttleConfig | Towers | 10.17 s | 1.86 s |

Printed benchmark output and positive `mcycle`/`minstret` values match across
both engines and repeats for every case. Towers checks its results internally.
Separate Rocket retired-store traces validate six final Dhrystone scalar
values on both engines and match at 519,926 simulation cycles. Default
MegaBoom and Shuttle lack the required source-register trace, so their
Dhrystone numerical checks remain unavailable; successful completion alone
does not prove those values. Timing runs omit verbose traces, use CPU 15 and
seed 1, and exclude build activity. Rocket's native default-seed-1 timings
were retained while its initially auto-seeded Verilator references were
replaced afterward, so the primary Rocket timings did not alternate engines.
Build costs are recorded separately, including failed attempts and deliberate
pause intervals. The actual warm MegaBoom native build took 1,301.59 s after
subtracting those pauses; this is not a cold-build comparison. All three
native builds used an unavailable Verilator path and their executables have
no Verilator dynamic-library or unresolved-symbol dependencies. Full commands,
hashes, logs, limits and costs are in
`output/development/native-three-core186-20261021T110034Z/first-pass-findings.json`.

LLVM block limiting counts the selected payload of aggregate `insertvalue` and
`extractvalue` instructions. Charging the entire SSA container at every field
update had expanded one scalar-heavy MegaBoom function from 194 blocks to
5,001; the corrected limiter produces 226 blocks. Actual aggregate memory
operations and wide arithmetic retain their full payload costs. The regression
`tests/aggregate_codegen.py` checks 2,097,152 independent lane values at O0/O3,
including disabled writes and aliased input/output buffers. Its aggregate
fixture falls from 289 artificial boundaries to two, while the wide-arithmetic
control retains 97 boundaries.

With the same complete MegaBoom LLVM input and default O3 settings, a fresh
native187 LLVM stage measured 342.62 s: optimization took 329.07 s and parallel
machine-code generation took 11.93 s. The optimized bitcode remains
byte-identical to native185. Relinking the original 28 C++ driver objects and
running the unchanged full Towers ELF reproduces the entire native185 log.
The saved problematic partition alone compiles at O3 in 6.42 s with about
293 MiB peak RSS; these are unpaired compiler diagnostics. All 91 native
variants and the frontend/LLVM, RTL-library and coverage-integrity gates pass.
The actual main compiler also rebuilds Rocket with Verilator unavailable and
reproduces the original complete Towers log at 39,426 simulation cycles, with
unchanged model inputs, optimized bitcode and metadata. Evidence is in
`output/development/native-aggregate-blocks187-20261021T120424Z/promotion.json`,
`full-native-report.json`, `required-gates-report.json` and
`work/full-MegaBoom/report.json` within that report directory. The six-case
runtime table above remains the native185 comparison; this compiler fix has
not received a new paired runtime benchmark pass.

Large single-use aggregate update chains are materialized at their original
ordinary store before LLVM optimization. Field values keep their original
evaluation points, shared intermediate snapshots remain available, and nested
overlapping updates retain their order. Volatile stores and scalable-vector
structures retain the original path. This avoids constructing an aggregate
lattice for every intermediate field update while keeping the complete O3
pipeline enabled.

On the same complete MegaBoom input, native189's fresh LLVM stage measured
56.58 s, including 42.32 s of optimization and 12.42 s of machine-code
generation, with about 10.8 GiB peak compiler RSS. The preceding native187
stage measured 342.62 s and about 21.3 GiB. These measurements cover fresh LLVM
compilation of the saved hardware model; the original 28 C++ driver objects
were relinked. The unchanged full Towers ELF reproduces the entire native185
log. `tests/aggregate_stores.py` checks 487,424 lane values and 1,280 ordered
callbacks at O0/O3, covering aliased and unaligned buffers, intermediate
snapshots, PHI roots, nested overlapping fields, idempotence, volatile/scalable
guards, large unsigned array indices and pointer address spaces. All 91 native
variants and the required frontend/LLVM, RTL-library and coverage-integrity
gates pass. The installed main compiler also reproduces the complete Rocket
Towers log at 39,426 cycles with unchanged hardware input and metadata.
Evidence is in
`output/development/native-aggregate-stores189-20261021T124904Z/promotion.json`,
`full-native-report.json`, `required-gates-report.json` and
`work/full-MegaBoom/report.json` within that report directory. Its optimized
LLVM representation changes; the independent value/callback tests and full
program checks establish the recorded correctness scope.

Controlled Rocket runtime decomposition (native191) uses the same complete
RocketConfig and unchanged installed 500-run Dhrystone ELF, with the original
DPI drivers, DRAMSim2, clocks, asynchronous resets and diagnostics. Single
uninstrumented runs with seed 1, CPU 15 and the default 8 MiB stack measured:

| Native compiler/runtime variant | Wall time | Model evaluations per primary DUT cycle |
| --- | ---: | ---: |
| Current native189 | 139.54 s | 6.001 |
| Current compiler, conservative whole-state convergence | 183.32 s | 8.267 |
| Stock SDK compiler, conservative whole-state convergence | 127.05 s | 8.267 |
| Current compiler without procedure sharing | 137.80 s | 6.001 |
| Current compiler without custom model outlining | 113.54 s | 6.001 |

The matched Verilator control measured 19.67 s. All controls reproduce the
entire native output, guest counters (232,125 `mcycle`, 213,024 `minstret`)
and 519,926 TestDriver root cycles. The primary 500 MHz clock has 259,963
rising edges; its cycles differ from the unused 1 ns TestDriver root clock.
Separate retired-store traces validate six final Dhrystone values for the
current, stock, no-sharing and no-outlining builds. These first-pass controls
use one uninstrumented timing each; diagnostic profiling runs are separate.

The current profile attributes 86.5% of runtime to model evaluation, 6.0% to
state copies/comparisons and 6.2% to binding combinational/sample/commit work.
Evaluation includes generated helper calls: CPU self samples place 16.6% in
the native wide-shift helper and 63.8% in compiled-model object symbols.
Unresolved libc samples cannot be assigned uniquely to model memory operations
or scheduler copies. Under identical conservative convergence, the current and
stock compilers execute the same 2,149,117 evaluations; their diagnostic mean
evaluation costs are 76.9 us and 48.6 us. Output-convergence filtering reduces
the current compiler's evaluations by 27.4% and uninstrumented wall time by
23.9%. Counts and per-evaluation costs are measured independently.

The stock control successfully uses matching SDK Arcilator, `opt -O3` and
`llc -O3` on the identical full native-partitioned model input. It retains the
required byte-pointer wide-value ABI bridge and native metadata-based glue;
it uses no custom LLVM pass plugins or late model outlining. The upstream
[arc-tests driver](https://raw.githubusercontent.com/circt/arc-tests/main/rocket/rocket-main.cpp)
uses two evaluations per cycle and reports time around model evaluation.
Its custom AXI memory model, reset/diagnostic settings and evaluation-only
frequency differ from this complete Chipyard harness and wall-time scope.
This does not establish a correct two-evaluation replacement scheduler for
our multi-clock/DPI model. It also does not qualify every Chipyard configuration.
Commands, hashes, compiler costs, profiles and numerical checks are in
`output/development/native-upstream-gap191-20261021T141741Z/findings.json` and its referenced reports.

Native192 evaluates partial-byte wide shifts one 64-bit word at a time while
preserving the byte-pointer ABI, padding and arithmetic sign extension. On the
same full RocketConfig/Dhrystone workload and default compiler settings, two
balanced runs per engine measured median wall times of 139.98 s before the
change and 133.84 s afterward, a 4.4% reduction. The matched Verilator median
was 19.85 s. All output, 519,926 root cycles, guest counters and 1,560,026 model
evaluations remain identical. Six final Dhrystone values pass the independent
retired-store oracle. The helper still accounts for 14.5% of sampled CPU time;
this change does not resolve the remaining performance gap.

All 91 native variants pass with the new helper. Independent Python-integer
checks cover 29,970 vectors across 111 widths at O0, O3 and with the undefined
behavior sanitizer; 6,660 additional cases place all three buffers against
inaccessible pages. These checks cover signed/unsigned shifts, overshifts,
dirty padding, unaligned buffers, input preservation and bounded accesses.
The actual main Rocket executable is byte-identical to the timed candidate,
builds with Verilator unavailable and reproduces the entire original Towers
log at 39,426 cycles. Its LLVM, object and state-layout artifacts remain
byte-identical. Commands, source hashes, profiles and qualification evidence
are in `output/development/native-word-shifts192-20261021T151618Z/findings.json`
and `promotion.json` in that report directory.

Native194 passes compact shift amounts across the LLVM/C++ boundary. LLVM
reduces the complete upper words to a nonzero flag and passes that flag with
the low word and partial tail. C++ masks the partial word before choosing the
bounded shift, preserving declared-width behavior even with dirty padding.
Known narrow sources reduce to a scalar low word and zero upper pieces;
no full amount buffer crosses the C ABI. Previously compiled models retain
the original byte-array amount entry point.

With the same full RocketConfig/Dhrystone model and default settings, two
balanced runs per engine measured medians of 135.31 s for native192 and
105.65 s for native194, a 21.9% elapsed-time reduction. Verilator measured
19.97 s. All output, guest counters, 519,926 root cycles and 1,560,026 model
evaluations match. Six final Dhrystone values pass the retired-store oracle.
The native helper accounts for 6.0% of CPU self samples, down from 14.5% in
the preceding profile. Compiled-model evaluation remains the dominant cost;
this workload still takes about 5.3 times Verilator's wall time.

All 91 native variants and the frontend/LLVM, RTL-library and coverage-integrity
gates pass. Independent tests cover 59,940 byte/compact-ABI vectors at O0, O3
and with the undefined-behavior sanitizer, 8,640 generated-LLVM value checks,
672 compiled-wrapper protected-page checks and 6,660 native-helper
protected-page cases. Deliberately dropping high amount bits fails the oracle.
The actual main Rocket binary is byte-identical to the timed candidate.
Fresh main Rocket, MegaBoomV3 and Shuttle builds reproduce their entire original
Towers logs with unchanged hardware inputs and model metadata, and have no
Verilator link dependency. These are the recorded functional/performance
scopes; all-config runtime qualification remains incomplete. Full commands,
hashes and checks are in
`output/development/native-scalar-shift194-20261021T155931Z/findings.json`,
`promotion.json` and `matrix-qualification.json` in that directory.

Native196 adds an optional compiler-certified idle-clock helper. The compiler
derives its private clock-history slots from allocated Arc operations, proves
that every clocked region is inactive under the helper's edge checks, and
checks that all other unguarded output writes are independent of the changed
phase inputs and histories. The versioned JSON certificate names the helper
and its exact phase-clock inputs; native binding generation validates those
inputs against the original partition metadata. Unknown effects, storage
aliases, sampled procedural DPI controls, time/context/export dependencies and
observation modes retain full evaluation. No RTL, diagnostics, clock/reset
behavior or original device-driver interface is removed.

The runtime preserves every foreign combinational/sample/commit call, analog
resolution, staged snapshot/copyback, delta iteration and SDK evaluation hook.
Bindings without the combinational reporting contract retain full evaluation.
After an eligible sample, the next model body is an identity only when no
commit reports a change, no immediate wakeup is pending, and the complete
post-commit model state still matches the staged snapshot. Unreported commit
writes therefore force ordinary evaluation. Use `ARC_IDLE_CLOCKS=0` to compile
without the helper, or `+arc-no-idle-clocks` to retain full evaluation in an
otherwise certified native executable.

Two balanced, uninstrumented runs per engine on the same complete
RocketConfig/Dhrystone workload measured medians of 104.35 s for native194 and
76.90 s for native196, a 26.3% elapsed-time reduction. Verilator measured
20.01 s; native remains about 3.8 times slower on this workload. A separate
counter run records 1,040,020 full model evaluations instead of 1,560,026,
while preserving all 1,560,026 SDK evaluation hooks. Entire program logs,
guest counters and all six checked Dhrystone globals match.

All 91 native variants and the frontend/LLVM, RTL-library and coverage-integrity
gates pass. Dedicated checks cover 29 compiler-proof cases, 17 metadata cases,
five compiler CLI modes, 44 runtime comparisons and ten invalid runtime
contracts, plus independent O0/O3 state comparisons. Runtime comparisons
include callback values/order/counts, SDK hooks, commit/combinational/analog
feedback, direct sample writes, unreported commit writes, falling-clock reset,
native waveforms and recreation. Intentionally removing the post-commit guard
fails the callback oracle. Actual main Rocket, MegaBoomV3 and Shuttle builds
preserve their hardware inputs, partition metadata and physical state layouts,
match the qualified private model objects and checked code/data sections, and
reproduce the complete original Towers logs without a Verilator link dependency.
Actual main Rocket also reproduces the entire Dhrystone retirement trace and
passes its six-value numerical oracle. All-config runtime qualification remains
incomplete. Evidence is in
`output/development/native-idle-clock196-20261021T174210Z/findings.json`,
`promotion.json`, `measurement.json` and `main-numerical-validation.json` there.

Native198 adds an optional read-only stability query to the same compiler
certificate. When the query proves that a combinational evaluation leaves
non-output state unchanged, the runtime omits that evaluation's convergence
snapshot and comparison. It still executes the model, all foreign callbacks,
SDK evaluation hooks and delta iterations. Phase snapshots and the post-commit
equality guard remain in place. `+arc-full-state-snapshots` disables this
snapshot optimization; `+arc-no-idle-clocks` disables both certified helpers.
Both helpers now inspect the physical bytes of held clocks and private
histories: noncanonical bytes force full evaluation without changing storage.
This closes a padding case found while qualifying the earlier idle helper.

Two balanced, uninstrumented runs per engine on the complete Rocket/Dhrystone
workload measured medians of 77.68 s for native196 and 74.04 s for native198,
a 4.7% elapsed-time reduction. The same native198 binary with full convergence
snapshots measured 79.62 s. Verilator measured 20.34 s; native remains about
3.6 times slower. A separate diagnostic run retains 1,040,020 model evaluations
and all 1,560,026 SDK evaluation hooks, accepting 1,040,018 stability queries.
Whole Dhrystone retirement traces and the six checked final globals match.

All 91 native variants and the frontend/LLVM, RTL-library and coverage-integrity
gates pass. Dedicated checks include 60 runtime comparisons, 393,216 read-only
query checks at O0/O3, 524,256 dirty-byte fallback checks per helper and 131,072
partial-width private-register checks. Deliberately accepting every query or
removing the post-commit guard fails the regression oracle. Fresh workspace
Rocket, MegaBoomV3 and Shuttle builds reproduce their original complete Towers
logs with unchanged physical state layouts and no Verilator link dependency.
Their model objects and checked code/data sections match the qualified private
builds. The workspace Rocket binary also passes the complete Dhrystone
retirement-trace comparison and six-value oracle. Evidence is in
`output/development/native-stable-snapshots198-20261021T183336Z/findings.json`,
`promotion.json`, `measurement.json` and `main-numerical-validation.json` there.
All-config runtime qualification remains incomplete.

An isolated native199 control compiles the identical complete Rocket hardware
input with the matching stock Arcilator, stock `opt -O3` and `llc -O3`, plus
the native wide-arithmetic C ABI adapter. It retains the native frontend and
hardware partition. A second control changes only that compiled model object
to the current custom compiler's object, using the exact same native glue,
model metadata, runtime and original device-driver objects. Both use
conservative full-state convergence.

Two balanced, uninstrumented runs per control on the original complete
Rocket/Dhrystone workload measured:

| Compiler backend | Median elapsed time | Diagnostic full evaluations / SDK hooks |
| --- | ---: | ---: |
| Stock SDK plus native wide C ABI adapter | 84.28 s | 2,149,117 / 2,149,117 |
| Current custom compiler, same conservative runtime | 139.55 s | 2,149,117 / 2,149,117 |

Separate diagnostic runs reproduce the complete retirement trace and all six
checked final globals with equal evaluation counts. Stock therefore takes
39.6% less elapsed time in this compiler comparison. The current default,
including its certified runtime optimizations, measured 73.65 s in the same
balanced sequence. These controls establish a compiler-cost gap on this
workload; they do not establish that every native compatibility pass can be
removed across Chipyard configurations. Independent outlining, ABI-buffering
and vector-guard comparisons follow below.
The six timed runs were preserved after correcting a diagnostic-only Python
oracle namespace error; the saved stock trace was rechecked and the remaining
custom diagnostic completed. Commands, hashes, raw results and the required
versus optional pass inventory are in
`output/development/native-stock-minimal199-20261021T193753Z/findings.json`,
`measurement.json`, `controls-build-report.json` and `upstream-pass-audit.json`
there.

Native200 measures those three optional compiler features independently on
the same complete Rocket/Dhrystone model. Every candidate retains the exact
native metadata, clock/output/stability certificates, runtime, original device
drivers, hardware, reset behavior and diagnostics. ABI buffering disables both
large-return and large-argument rewrites before and after LLVM optimization;
the shared-procedure workspace remains enabled. Removing vector guards also
removes their matching attribute-restoration pass.

| Native compiler variant | Median elapsed time | Change from default |
| --- | ---: | ---: |
| Current default | 73.65 s | — |
| Custom outlining disabled | 61.15 s | 17.0% shorter |
| Private ABI buffering disabled | 54.52 s | 26.0% shorter |
| Wide-vector guards disabled | 76.66 s | 4.1% longer |

Each median covers two balanced, uninstrumented runs at CPU 15 with seed 1
and the default 8 MiB stack. Separate instrumented runs reproduce the entire
retirement trace, all six checked final globals, and every evaluation/helper/hook
count from native198: 1,040,020 full evaluations and 1,560,026 SDK hooks.
Both ABI-buffering runs measured 54.52 s. These results identify useful optional
pass removals on Rocket; production defaults retain the existing scalability
protections. The packed-cache regression motivating the vector guard also
remains relevant to wider models. Full commands, hashes, compiler resources,
exact counters and logs are in
`output/development/native-optional-ablations200-20261021T195806Z/findings.json`,
`build-report.json` and `measurement.json` there. All-config runtime qualification
remains incomplete.

Further complete-Shuttle measurements with explicit outlining budgets measured
14.82 s at 32,768 operations and 14.32 s at 65,536, against about 15.12 s at
16,384. Both retained the identical state layout, 127,350 model evaluations and
30,716 completion cycles. The requested default remains 16,384; an individual
build can select another budget with `ARC_MODEL_OPS=65536`. These measurements
cover the cached Shuttle workload. Reports are in
`output/development/native-budget32768-178-20261021T081149Z/work/full-Shuttle/report.json`
and
`output/development/native-budget65536-179-20261021T081714Z/work/full-Shuttle/report.json`.

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
Native builds compile scalar digital IO cells and clock inversion/OR/NOR logic
into the hardware model to avoid extra settling across C++ boundaries.
`EICG_wrapper` retains its transparent low-level enable latch through `ArcLatch`;
the clock AND stays in the compiled model. Dynamic enable and test-enable
changes while the input clock is high cannot open or close the gate.
Nonstandard interfaces and analog connections retain their native bindings.
The partition CLI enables this with `--native-combinational-models`; the native
Makefile supplies it automatically, while the hybrid path keeps its existing
boundaries. `tests/run_native.py --variants compiled-combin` checks chained
gates, IO enables, inverted clocks, asynchronous resets and edge counts against
an independent oracle, including enable changes during the high clock phase.

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

Native source filelists expand separated `-f FILE` and `-F FILE` references
without an HDL simulator. `-f` entries use the invocation directory; `-F`
entries use the referenced list's directory. Quoted paths, shell escapes and
`#`, `//`, and `/* ... */` comments are accepted; recursive or missing lists
fail explicitly. These are source-selection lists: frontend and compiler flags
continue to use their existing Make arguments. RTL import, clock/DTM contract
selection, library discovery and C/C++ driver compilation share this reader.
Make fingerprints listed HDL and driver inputs separately, so edits in nested
lists invalidate the relevant build stages. Run `make native-filelist-test` for
path/comment/recursion, incremental Make and real native C/C++ driver-link tests.
C drivers inherit shared `-I`, `-D` and `-U` options in both joined and
separated forms, retaining quoted paths and macro values as single arguments.
C++ language-standard options stay on C++ compilations; missing shared-option
operands fail before compilation or output replacement.

The DPI compatibility layer implements the scalar/vector/open-array operations
used by the ported drivers. Native VPI provides argument/product queries through
`vpi_get_vlog_info`, `cbStartOfSimulation` and `cbEndOfSimulation` through
`vpi_register_cb`, cancellation through `vpi_remove_cb`, and error reporting
through `vpi_chk_error`. An embedded `Runtime(0, nullptr, ...)` exposes a valid
empty command line: its argument query succeeds with zero `argc` and null
`argv`. `make native-vpi-test` checks this with the original UART driver as
well as empty/nonempty argument transitions and borrowed-pointer identity.
A linked, null-terminated C `vlog_startup_routines`
array runs once per process before binding initialization. Start callbacks run
after binding initialization and before RTL initial blocks and SDK initialization
hooks. End callbacks run after RTL finalization, while model storage and DPI
scopes are still alive. Both phases run once per initialized model. With callbacks
registered, `run()` finalizes and delivers end callbacks before reporting its
result; a callback exception makes that run fail. Manual initialization/advance
users receive end callbacks on model destruction. Recreated models need new
registrations; startup routines are not replayed.

Lifecycle registration copies the reason, function and user-data pointer. The
caller retains ownership of user data. Object, time, value and index fields are
ignored for these two reasons and are zero/null on delivery. Callbacks execute
in registration order; new registrations for the current phase run in a later
wave, and pending callbacks can be cancelled, including during delivery.
Independent registrations have no count limit; a self-scheduling chain that
exceeds 1,000 waves fails convergence. Delivered, cancelled and destroyed-model
handles expire. VPI delivery does not establish an implicit DPI scope; a client
calling a scope-dependent export must select its scope explicitly. No callback
dispatch is added to the per-cycle scheduler, and the unused registry is lazy.

Run `make native-vpi-test`, or `python3 tests/vpi_lifecycle.py` with `CIRCT_BIN`
set, for the standalone C/C++ ABI, cancellation/lifetime/error, real SDK Runtime
and complete run-loop waveform checks. The callback-enabled full original
Shuttle Dhrystone qualification preserves all 189,617 final state bytes, the
whole original log, evaluation/SDK counts and six independently decoded numerical
results. Balanced Rocket/Shuttle Dhrystone and Towers comparisons with callbacks
unused found no material runtime regression. Evidence is in
`output/development/native-vpi-lifecycle286-20261022T163240Z/qualification.json`
and `measurement.json` there. Those comparisons do not establish a VPI speedup.
The maintained native287 integration rebuilds all selected runtime and original
driver sources through `native_build.py` and reproduces both recommended
Rocket/Shuttle executables byte for byte. Its callback-enabled Shuttle executable
also matches the qualified native286 executable exactly. The focused Make target
passes; this checkpoint does not repeat the full configuration/fixture matrix.
See `output/development/native-vpi-lifecycle-integration287-20261022T170118Z/integration.json`
and `native-build-CLI.json` there.

Arbitrary VPI hierarchy traversal, value access, timed/value/synchronization
callbacks, force/release and dynamic plugin loading are not implemented.
Global SV time precision and original typed hierarchy must be retained before
flattening to implement those APIs correctly; the runtime's femtosecond clock
alone does not supply that metadata.

`ARC_SOURCE_METADATA=1` optionally retains source metadata for each imported RTL
boundary. The patched frontend's `--native-vpi-metadata=FILE` option writes a
versioned JSON snapshot after successful Slang elaboration and before IR
optimization. It contains Slang's detailed source AST, original packed/unpacked
ranges, signed types, generated scopes and resolved time-scale facts for
compilation units, packages and module/interface/program declarations, including
unused declarations. Snapshot-local Slang address IDs preserve links to shared
types and instance bodies. These IDs are opaque within one snapshot and cannot
serve as runtime pointers or VPI handles across imports or rebuilt models.
Unspecified time scales remain explicitly unresolved.

`rtl.json` records each sidecar's path/hash, frontend command, input hashes,
generated wrapper/stub sources and omitted `TestDriver` files. The snapshots
describe individual frontend compilations; they do not establish the assembled
model's global time precision or optimized storage correspondence. The normal
default is off, and these records do not enable the unfinished VPI APIs.

Run `make native-source-metadata-test CIRCT_BIN=/path/to/sdk/bin`; the test builds
the matching patched frontend if needed. `ARC_SOURCE_METADATA_FRONTEND=/path/to/arcilator-verilog`
selects an already-built matching frontend. The focused fixtures check source
types/ranges/shared links, all 18 time-precision decades, source-only program
scopes, separate compilation units, invalid output paths and unchanged original
Moore/LLHD IR. The existing three-boundary native RTL fixture also preserves its
whole final imported IR with metadata enabled. Evidence and commands are in
`output/development/native-vpi-source-integration315-20261022T221147Z/` and its recorded parent. Unsupported callback reasons fail with a
VPI error. Additional native bindings can use the `Binding`/`register_binding`
API.
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
flattening. JTAG's 100 ps input/output delays retain femtosecond event origins.
Each of the five output assignments keeps an independent inertial deadline:
an unchanged pin retains its pending transition across driver calls, and a
short pulse returning to its delivered value is cancelled. The 32-bit exit
value is one delayed assignment. Run `make native-jtag-test` for standalone
real-runtime checks of independent outputs, fractional delays, TDO pulses,
reset history, clock division, sticky initialization, DPI scopes and model
recreation at O0/O3/UBSAN, including timing fault controls.

The older Sodor `SimDTM` binding also retains femtosecond origins for its
100 ps input and output delays. The `dtm` variant of `tests/run_native.py`
checks callbacks immediately before and after fractional input deadlines,
all six delayed outputs, registered reset, output width masking and model
recreation with a compiled CIRCT fixture and the original `debug_tick` ABI.

Run `make native-predictor-test` to check `BranchPredictorHarness` and
`BTBHarness` against BOOM's unchanged `predictor_sw.cc` and `btb_sw.cc` drivers.
The standalone SDK Runtime tests 64-bit PC/history/target arguments, prediction
before updates, saturating learning, BTB hits, reset/enable behavior, DPI scopes,
deferred output commits and model recreation at O0/O3/UBSAN. Fault controls
reject reversed callbacks, truncated PCs and premature output writes. This
checks the native bindings directly without elaborating a hardware configuration.
`make native-predictor-model-test` runs the same original-driver checks through
a compiled MLIR fixture: hardware partitioning, Arc/LLVM lowering, O0/O3 model
code generation and generated native port mappings. It checks the generated
64-bit interface metadata and rejects deliberately swapped PC/history mappings.
The two targets share their behavioral oracle and recreate both model contexts.

`make native-block-device-test` compiles an MLIR fixture with the original
`SimBlockDevice.cc`/`blkdev.cc` and FESVR context library. O0/O3 checks cover
tag parameters, 32/64-bit arguments, stalled responses, interleaved tagged
writes, queue/partial-write retention across reset, disabled mode and complete
2 KiB disk-image comparisons. Fault controls reject truncated data, swapped
tags and ignored response backpressure. The test uses FESVR from `RISCV` or
Chipyard's local conda toolchain; it builds no firmware or hardware configuration.

`make native-network-test` compiles an MLIR fixture with the original
`SimNetwork.cc`/`device.cc`/`switch.cc` and FESVR context library. An empty
device name selects the original switch's local path, which opens no TAP
interface. O0/O3 tests send two packets to the driver's actual random MAC and
compare every received word and last-flit flag against independently constructed
packet bytes. Checks cover partial packets and queued receive data across reset,
64-bit transmit arguments, the 48-bit MAC port, rate/burst defaults and overrides,
DPI scopes and deferred output commits. Fault controls reject truncated transmit
data, callbacks during reset and a corrupted MAC. It uses the same FESVR discovery
as the block-device test and builds no firmware or hardware configuration.

`make native-debug-rob-test` compiles all three DebugROB bindings with the original
Rocket Chip `debug_rob.cc` driver. O0/O3 checks cover all eight 64-bit payload
words, full-width tags, two hart queues, waiting entries, out-of-order writeback,
already supplied data and exception/interrupt fields. Push callbacks run on rising
edges; pop callbacks run on falling edges and publish on the next rising edge.
Reset retains the last popped value and suppresses the first falling-edge pop
after release, matching `debug_rob.v`. The test compares every byte of all 27
published traces and rejects payload truncation, tag truncation, the wrong pop
edge and missing reset history. It builds no firmware or hardware configuration.

`make native-traffic-test` compiles TrafficEval ingress and egress with the
original Constellation `TrafficEval.cpp`, `netrace.c` and `queue.c` drivers.
O0/O3 checks cover the embedded multiline configuration, `+eval_params` file
override, packet head/tail and unique IDs, backpressure, reset retention, deferred
outputs, success/throughput-failure flags and timeout cycles above 32 bits.
Independent byte traces and expected driver measurements check the complete
single-flow run. Fault controls reject corrupted IDs, swapped completion flags,
truncated cycles and clearing retained ingress payload during reset. This uses
the original random-traffic path; netrace file playback is not covered. It builds
no firmware or hardware configuration.

`make native-clock-test` extracts the original inline clock sources from Rocket
Chip and Chipyard. It imports Rocket Chip's fixed clock as RTL and connects the
harness and plusarg clock bindings through the compiled native model. O0/O3
checks distinguish assignment-delay sampling from delay-before-assignment
sampling while power and gate change between edges, including fractional
picosecond input times. Default and overridden frequencies, all clock levels,
peer register counts and complete event traces are checked just before, at and
after deadlines. Fault controls move the original assignment delay and ignore
the native clock gate; both must fail. It builds no firmware or configuration.

`make native-memtrace-test` compiles the Radiance trace reader/logger bindings
with their original C++ drivers. The initialization parameters are narrowed to
the declared DPI `bit` inputs: `HAS_SOURCE=2` selects no source column, while
`IS_RESPONSE=3` passes one to the logger. O0/O3 checks cover both file formats,
three packed lanes, full 64-bit cycles/addresses/data, stalled reads, reset
retention, EOF and logger cycle reset. Independent oracles compare every output
and logger-file byte. Fault controls restore either flag-conversion bug, ignore
read stalls or reverse lane order; all must fail. It builds no firmware or
hardware configuration.

`make native-declaration-dpi-test` imports and compiles read-only declaration
initializers containing context-free DPI function calls. Closed dependency graphs
run once before time-zero processes, preserve callback order and share results
without repeating effects. Frontend provenance retains initialization when an
unused declaration is removed. O0/O3 tests check 64-bit values, dependent `chandle`
calls, unused effects, independent instances, model recreation, and every output
byte across clock, reset and held evaluations. Ordinary initial and clocked DPI
calls retain their execution points. Duplicate-callback and wrong-observer faults
must fail. `tests/signal_initializers.py` retains diagnostics for input-dependent,
unbound contextual, task, cyclic, escaping and effect-reordering cases. The
standalone snapshot pass also rejects writable DPI values when native storage
lowering is not enabled.

`make native-context-declaration-dpi-test` extends declaration initialization to
bound contextual DPI functions. Native scope binding outputs are initialized
before the model's startup function and can be captured there. The compiler
checks their provider, width, use and partition contracts, then lets the matching
SDK lower state with normal verification. Ordinary model inputs retain their
startup diagnostics. O0/O3 tests cover local and package scopes, exported SV
state read and written during initialization, scope selection and restoration,
user data, registry cleanup and model recreation. Complete output traces match;
a fault that swaps physical scope bindings must fail. No runtime change is
needed for these startup captures.

`make native-writable-declaration-dpi-test` checks declaration DPI values that
subsequent RTL or exported SV functions can modify. The compiler writes each
initializer result into native signal storage before dependent callbacks run.
A later initializer resamples that storage after an intervening callback writes
it through an SV export. Native scope and memory-handle inputs are checked
against their initialized binding providers before SDK state lowering; ordinary
model inputs remain unsupported startup dependencies. O0/O3 tests cover full
64-bit values, 129-bit word chunks, synchronous updates, asynchronous reset
pulses, context restoration, and independent model recreation. Every output byte
matches an independent trace oracle. Fault controls detect duplicate callbacks,
wrong observer data, swapped scopes, omitted startup writes, and stale reads.
This supports closed, straight-line declaration dependency graphs; general
runtime initialization and cyclic or input-dependent graphs remain open.

`make native-global-declaration-dpi-test` checks closed package/global
initializers containing context-free DPI functions. Global values use the same
shared native allocation in every imported specialization and physical instance.
Each allocation claims its initializer once per model. The complete expression
and its assignment run under that claim before module declaration startup;
skipped instances execute neither callbacks nor floating calculations. Shared
keys also validate an initializer fingerprint, so conflicting definitions cannot
silently select whichever instance initializes first. Dependency cycles,
callback reordering, unbound context, tasks and unsupported effects retain
diagnostics. Structured startup regions keep their single-block form until SDK
state lowering; ordinary procedural SCF uses the matching SDK conversion rules.

O0/O3 tests compare every byte of three-instance traces over shared writes,
held events and two model recreations. They cover dependent globals, slices of
129-bit storage, real and shortreal values, module startup after global startup,
and unused initializer effects. A floating exception check detects evaluation
of a skipped initializer. Six faults break claims, assignments, the final
partial word, sharing, fingerprints or observer data; all must be detected.
Contextual global DPI and general conditional/runtime startup remain open.

Asynchronous registers are grouped by clock/reset into banks of at most 1,024
bits. The native binding samples both clock and asynchronous-reset edges.
Registers wider than a bank are split and reassembled with their exact reset
values. Packed arrays and nested structures use CIRCT bitcasts to preserve their
layout across the bank interface. No SystemVerilog process is needed to execute
these banks.

GPIO and SPI pins resolve enabled drivers before further combinational
settling. The native model is two-state: floating and conflicting bits resolve
to zero, and uncontested driven bits retain their values. Scalar drivers cover
nets through 64 bits. Wider nets use `Binding::drive(name, value, mask)` with
little-endian byte vectors containing exactly the port's byte count. Each mask
bit enables one driven bit; unused high bits are ignored. Reusing the same
binding and port replaces that driver's value and mask. Cached `Signal`
addresses stay stable through net registration, settling and binding cleanup.
`AnalogConst` retains its parameterized `WIDTH`, including signed constant
extension above 64 bits. Integer literals are reduced modulo the destination
width, including literals larger than 64 bits that drive narrow ports. Constants
are parsed during configuration and narrow drivers retain a scalar value.
Wide storage is allocated only for wide nets; the
existing scalar resolver is unchanged. More complex electrical analog behavior
is unverified.

Run `make native-analog-test CIRCT_BIN=/path/to/sdk/bin`, or
`python3 tests/analog_nets.py --circt-bin=/path/to/sdk/bin --work=/tmp/analog-test`.
The focused tests use the real SDK runtime, 21,923 checks at O0/O3, independent
per-bit resolution and integer oracles, and verified HW partition/factory/VCD
fixtures at widths 7, 64, 65, 73 and 257, including 128-bit parameters on narrow
ports. They also exercise the existing VPI lifecycle
and all 18 run-loop waveforms, without Scala elaboration or Verilator.

Whole original Rocket/Shuttle Dhrystone and Towers passed 16 balanced runs;
complete Dhrystone final states, diagnostics, model/SDK counts and VPI lifecycle
also matched. The two-repeat CPU medians were 0.3–0.6% lower for Shuttle and
1.2–1.5% higher for Rocket; this establishes no speedup claim. Evidence and exact
commands are in `output/development/native-wide-analog-integration313-20261022T214937Z/` and its recorded parent.
General VPI and verification across every configuration remain unfinished.

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

The native `plusarg_reader` accepts integer `%d`, `%h`, `%x`, `%o`, and `%b`
conversions in either case, zero modifiers such as `%0H`, and escaped `%%`
characters in argument prefixes. Values and defaults retain the configured bit
width, including widths greater than 64 bits.

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

The `block_model` case runs the complete repository `tests/blkdev.c` program on
`BlockDeviceModelRocketConfig`. It writes and reads four sectors, checks all 512
data values, and requires the program's `All correct` result. The `block` case
exercises the original SimBlockDevice C++ driver on `SimBlockDeviceRocketConfig`.

```sh
python3 tests/integration.py --runtime native --cases block_model --timeout 0
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
exported SRAM APIs. General nonconstant declaration initialization remains incomplete. Closed DPI
declaration graphs, including verified native context scopes and writable
native storage, are supported as described above. Closed package/global
initializers with context-free DPI functions are also supported there.

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

Native callback scopes use a private inline guard for the same thread-local
exchange and restoration as `DpiContext`; its public constructor/destructor ABI
remains available. `tests/idle_clock.py` also runs `tests/scope_guard.py`, which
checks nested and throwing callbacks, public ABI calls, registry cleanup, thread
isolation, and late-loaded shared libraries. The scope test needs only C++.
