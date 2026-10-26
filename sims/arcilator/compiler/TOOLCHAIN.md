For full-source LLVM construction, filtered frontend PGO and optional ThinLTO,
see [FULL_TOOLCHAIN.md](FULL_TOOLCHAIN.md). That workflow uses fresh matching
profiles and separate qualification gates. Both plain and ThinLTO variants have
passed four-target exact-object/canonical-IR and real codegen-API qualification.
Their isolated stage measurements are separate evidence. Before activation,
require an accepted fresh simulator build/runtime report for the exact chosen
compiler pair, source, profile and flags. It adds no Arcilator/CIRCT patch; the existing optional driver
workflow below is unchanged.

This page reproduces the legacy SDK overlay recipes. Their 30-TU optimizer and
eight-TU backend use **only patches 0001–0007** from each recipe's explicit
`applies_to_patches` list. Use a separate pinned source checkout for these
recipes. The global full-series source hashes describe a different construction;
they must not replace the legacy per-file hashes. Applying later patches before
checking the old recipes is an error, not a reason to relax hash checks.

`sdk_overlay.py` builds private LLVM compiler executables from the qualified
CIRCT revision. Direct implementation objects replace their original members
in the SDK's static archives. It does not install compilers, select simulator
build defaults, add a simulator result cache, or change model optimization
levels. Keep `CIRCT_BIN` and `CIRCT_ROOT` at the reference SDK so the simulator
runtime, headers, libraries, and host objects remain unchanged.

Qualification records below describe compiler construction and isolated stage
checks. Fresh simulator build and runtime comparisons are retained separately
with their exact model flags, source snapshots, and workload inputs.

The source pins are CIRCT `fa4f662cbe710906347fac795c581a06c6b99757`
(`firtool-1.162.0`) and LLVM
`a17069fea5b271c6824ea773874b007116de9644`. `24.0.0git` alone is not a
sufficient version identifier. The helper requires the qualified CIRCT/LLVM
version pair and records exact source, header, SDK archive, and binary hashes.
Use patches based on these revisions and retain their source manifests.

The LLVM helper requires Python 3.10 or newer, the complete SDK with static
archives and headers, and a compatible C++17 compiler. PGO additionally needs
the profile runtime and `llvm-profdata` matching the instrumenting compiler;
for clang 18 use `llvm-profdata-18`, rather than the SDK's LLVM 24 tool.
ThinLTO additionally needs a matching linker, such as LLD 18 for clang 18.
Runtime execution of the helper uses only the Python standard library.

Run these examples from `sims/arcilator`. First fetch the original implementation
and driver into a private directory:

```sh
sdk_prefix="$HOME/.cache/arcilator/firtool-1.162.0"
python3 compiler/sdk_overlay.py fetch --work /tmp/llvm-overlay \
  llvm/tools/llc/llc.cpp llvm/lib/CodeGen/TwoAddressInstructionPass.cpp
python3 compiler/sdk_overlay.py plan \
  --sdk "$sdk_prefix" --work /tmp/llvm-overlay \
  --tool llc --compiler /usr/bin/clang++-18 \
  --source /tmp/llvm-overlay/source/llvm/lib/CodeGen/TwoAddressInstructionPass.cpp
python3 compiler/sdk_overlay.py build \
  --sdk "$sdk_prefix" --work /tmp/llvm-overlay \
  --tool llc --compiler /usr/bin/clang++-18 \
  --source /tmp/llvm-overlay/source/llvm/lib/CodeGen/TwoAddressInstructionPass.cpp
```

`fetch` writes an immutable-revision `source-manifest.json` and refuses to
replace modified source files. `plan` records commands without compiling.
`build` always compiles the supplied translation units; existing objects are
not used as a cache. Repeat `--source` for additional implementation files
and `--include` for private header directories. All source and header
versions must match the SDK. InstCombine also needs its generated
`InstCombineCLOptions.inc`, generated with the matching SDK's `llvm-tblgen`
from the pinned `.td` file using `-gen-opt-parser-defs`.

Explicit private include directories precede the SDK include directory. This
also permits a qualified additive header extension in an implementation
overlay. The caller must preserve the ABI used by the linked SDK archives:
private overrides must not change class layouts or other ABI contracts.
Retain the actual dependency-header hashes and qualify generated output;
matching filenames or a successful link do not establish compatibility.

Use separate work directories for Release, instrumented, PGO, and LTO builds.
The helper links a temporary executable and checks its version before
publishing it. A failed build preserves the previous executable and retains
logs. Source changes during compilation reject publication. `--jobs` defaults
to the process's CPU affinity and limits independent C++ compilations.
`--native` targets only the compiler executable at the build host; it does not
change the simulator's target CPU or compiler command arguments.

Use explicit **IR-level PGO** to reproduce the legacy overlay profiles below.
The new full-source workflow has a different frontend policy; its profiles are
not interchangeable with these overlays. Fetch the pinned driver
into each output prefix and keep implementation source paths and compilation
settings identical across generation and use:

```sh
python3 compiler/sdk_overlay.py fetch --work /tmp/llvm-instrumented \
  llvm/tools/llc/llc.cpp
python3 compiler/sdk_overlay.py fetch --work /tmp/llvm-pgo \
  llvm/tools/llc/llc.cpp
python3 compiler/sdk_overlay.py build \
  --sdk "$sdk_prefix" --work /tmp/llvm-instrumented \
  --tool llc --compiler /usr/bin/clang++-18 \
  --source /tmp/llvm-overlay/source/llvm/lib/CodeGen/TwoAddressInstructionPass.cpp \
  --mode instrument --profile-kind ir
```

Supply a JSON list of compiler argv lists. Each begins with literal `{tool}`;
`{work}` expands to the training directory. Preserve the original compiler
pipeline, target flags, loaded plugins, and postpasses in every command.
For example:

```json
[
  ["{tool}", "-O3", "-relocation-model=pic", "-filetype=obj", "/inputs/first.bc", "-o", "{work}/first.o"],
  ["{tool}", "-O3", "-relocation-model=pic", "-filetype=obj", "/inputs/second.bc", "-o", "{work}/second.o"]
]
```

```sh
python3 compiler/sdk_overlay.py train \
  --tool /tmp/llvm-instrumented/bin/llc --work /tmp/compiler-training \
  --manifest training.json --profdata /usr/bin/llvm-profdata-18 --jobs 4
python3 compiler/sdk_overlay.py build \
  --sdk "$sdk_prefix" --work /tmp/llvm-pgo \
  --tool llc --compiler /usr/bin/clang++-18 \
  --source /tmp/llvm-overlay/source/llvm/lib/CodeGen/TwoAddressInstructionPass.cpp \
  --mode pgo --profile-kind ir \
  --profile /tmp/compiler-training/compiler.profdata
```

For `opt`, use its pinned driver and implementation files with `--tool opt`;
training argv must include the complete original optimization pipeline.
Training executes compilers, not simulators. Independent commands run
concurrently with separate logs and profile filenames. Parallel commands
require distinct explicit output files. Each training invocation uses a fresh
profile directory and removes inherited `LD_PRELOAD`,
`ARC_COMPILER_SAMPLE_PATH`, and `LLVM_PROFILE_FILE` before setting its own
profile path. Training wall times are diagnostic, not isolated benchmarks.
Compare every training output with its reference before accepting a profile.

The accepted legacy optimizer uses **30 implementation translation units**.
Its exact revision-relative file list and before/after source hashes are under
`optimizer_overlay` in `compiler/llvm-patches/manifest.json`. This retains the
earlier 26 files, applies the LVI/SLP patches `0004`/`0005`, and adds unchanged
`Dominators.cpp`, `MemorySSA.cpp`, `MemorySSAUpdater.cpp`, and
`ValueTracking.cpp` to compiler IR-PGO coverage. The pinned OPT main is fetched
separately by the helper. No Arcilator/CIRCT or simulator source change is
needed to reproduce this optimizer recipe.

Use a fresh private LLVM-project checkout at the pinned revision. This helper
applies exactly the recipe's declared seven patches in manifest order, verifies
each patch hash, and rejects a missing or broader recipe scope. Run it from
`sims/arcilator`; it changes only the named private checkout:

```sh
apply_legacy_patches() {
  python3 - "$1" "$2" <<'PYCODE'
import hashlib, json, pathlib, subprocess, sys
root, recipe_name = pathlib.Path(sys.argv[1]), sys.argv[2]
patch_dir = pathlib.Path('compiler/llvm-patches').resolve()
manifest = json.loads((patch_dir / 'manifest.json').read_text())
recipe = manifest[recipe_name]
names = recipe['applies_to_patches']
assert [name.split('-')[0] for name in names] == [f'{i:04d}' for i in range(1, 8)]
assert subprocess.check_output(
    ['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip() == manifest['revision']
by_name = {row['file']: row for row in manifest['patches']}
for name in names:
    path = patch_dir / name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == by_name[name]['sha256'], path
    subprocess.run(['git', '-C', str(root), 'apply', '--check', str(path)], check=True)
    subprocess.run(['git', '-C', str(root), 'apply', str(path)], check=True)
PYCODE
}
```

The examples require the manifest version that explicitly scopes both legacy
recipes to 0001–0007; they do not fall back to applying every patch in a directory.
Generate the InstCombine option header with the matching SDK tool:

```sh
opt_source_root=/sources/llvm-project
opt_ir_work=/tmp/opt-30
mkdir -p "$opt_ir_work"
apply_legacy_patches "$opt_source_root" optimizer_overlay
"$sdk_prefix/bin/llvm-tblgen" -I "$sdk_prefix/include" -gen-opt-parser-defs \
  "$opt_source_root/llvm/lib/Transforms/InstCombine/InstCombineCLOptions.td" \
  -o "$opt_source_root/llvm/lib/Transforms/InstCombine/InstCombineCLOptions.inc"
python3 compiler/sdk_overlay.py fetch --work "$opt_ir_work/instrumented" \
  llvm/tools/opt/opt.cpp
python3 compiler/sdk_overlay.py fetch --work "$opt_ir_work/pgo" \
  llvm/tools/opt/opt.cpp

build_opt30() {
  python3 - "$opt_source_root" "$sdk_prefix" "$@" <<'PY'
import hashlib, json, pathlib, subprocess, sys
source, sdk, work, mode = sys.argv[1:5]
root = pathlib.Path(source)
manifest = json.loads(pathlib.Path('compiler/llvm-patches/manifest.json').read_text())
overlay = manifest['optimizer_overlay']
command = [sys.executable, 'compiler/sdk_overlay.py', 'build',
           '--sdk', sdk, '--work', work, '--tool', 'opt',
           '--compiler', '/usr/bin/clang++-18', '--jobs', '16',
           '--mode', mode, '--profile-kind', 'ir']
for row in overlay['implementation_sources']:
    path = root / row['path']
    assert hashlib.sha256(path.read_bytes()).hexdigest() == row['patched_sha256'], path
    command += ['--source', str(path)]
header = overlay['generated_private_header']
assert hashlib.sha256((root / header['path']).read_bytes()).hexdigest() == header['sha256']
for directory in overlay['private_include_directories']:
    command += ['--include', str(root / directory)]
if mode == 'pgo':
    command += ['--profile', sys.argv[5]]
subprocess.run(command, check=True)
PY
}
build_opt30 "$opt_ir_work/instrumented" instrument
python3 compiler/sdk_overlay.py train \
  --tool "$opt_ir_work/instrumented/bin/opt" --work "$opt_ir_work/training" \
  --manifest /inputs/opt-training.json --profdata /usr/bin/llvm-profdata-18 --jobs 4
build_opt30 "$opt_ir_work/pgo" pgo "$opt_ir_work/training/compiler.profdata"
```

`/inputs/opt-training.json` must contain explicit original OPT argv lists, such
as `{tool}`, `-passes=default<O3>,verify`, the original triple/CPU flags, an
original complete model `.ll`, and a distinct `-o` output. Include every
required plugin and postpass. Compare all training outputs byte-for-byte to
their references before accepting the profile or compiler. Do not reuse the
old 26-TU profile after changing sources or coverage. Generate and use the new
profile with identical source paths, include directories, compiler, and build
settings; retain the strict mismatch checks described below.

The isolated BOOM optimizer-stage medians were 71.6076 seconds for the prior
selected OPT and 56.1024 seconds for the combined 30-TU candidate (21.65% less
time), with exact optimized bitcode in all eight suite runs. LVI alone with
26-TU coverage measured 64.1399 seconds. All four instrumented training outputs
were also exact. These historical optimizer-stage results were followed by
accepted complete
four-configuration build and runtime qualification of the legacy compiler bundle
in the frozen `native-compiler-only-20261026T010301Z` delivery. Complete model
objects and matched normal/observed executable load images were exact on Rocket,
BOOM, Saturn and Shuttle; both retained scalar programs passed full-buffer,
ten-callback, log and cycle/counter checks. This establishes the tested bundle's
code/semantic preservation, not universal configuration coverage. That evaluation
used one fresh build per mode/config and two balanced runtime repetitions with
warm tools and retained FIRRTL; Scala elaboration and compiler construction were
excluded. Saturn's scalar workloads do not qualify RVV. The historical 21.65%
optimizer result includes broader compiler profiling and cannot be attributed to
the two data-structure patches alone. It is not the new full-source workflow's
build result. The legacy patches implement no within-function parallel analysis.

The selected backend overlay uses eight implementation sources plus the pinned
LLC main. It retains patches `0001`–`0003`, the minimal physical-use suffix
algorithm in `0006`, and bidirectional scheduling reachability in `0007`.
It uses Release clang 18 without compiler PGO, LTO, native
host targeting additions, threaded planning, or experimental LLC options.
The physical-register/full-lane-mask guard is automatic; virtual/subregister
cases retain the original traversal. All original live-range extension calls
remain, including duplicates. This changes compiler work, not model flags.
The scheduling query expands the smaller forward/backward frontier inside
the valid topological interval. Edge insertion retains the original DFS and
visited-set ordering, and the existing negative-result cache is unchanged.

Use a separate fresh pinned checkout and the same `apply_legacy_patches`
function above with `backend_overlay`. Its patch list is also exactly 0001–0007.
The private source header must precede installed SDK headers because it declares
the additive out-of-line MRI collection method; its existing class layout is
unchanged. The helper's repeated `--include` options have that precedence. Run
from `sims/arcilator`:

```sh
llc_source_root=/sources/llvm-project-backend
llc_release_work=/tmp/llc-backend
apply_legacy_patches "$llc_source_root" backend_overlay
python3 compiler/sdk_overlay.py fetch --work "$llc_release_work" \
  llvm/tools/llc/llc.cpp
python3 - "$llc_source_root" "$sdk_prefix" "$llc_release_work" <<'PY'
import hashlib, json, pathlib, subprocess, sys
root, sdk, work = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
manifest = json.loads(pathlib.Path('compiler/llvm-patches/manifest.json').read_text())
overlay = manifest['backend_overlay']
command = [sys.executable, 'compiler/sdk_overlay.py', 'build',
           '--sdk', sdk, '--work', work, '--tool', 'llc',
           '--compiler', '/usr/bin/clang++-18', '--jobs', '16',
           '--mode', 'release']
for row in overlay['implementation_sources']:
    path = root / row['path']
    assert hashlib.sha256(path.read_bytes()).hexdigest() == row['patched_sha256'], path
    command += ['--source', str(path)]
header = overlay['patched_public_header']
assert hashlib.sha256((root / header['path']).read_bytes()).hexdigest() == header['patched_sha256']
for directory in overlay['private_include_directories']:
    command += ['--include', str(root / directory)]
subprocess.run(command, check=True)
PY
```

This always recompiles the nine translation units; no prior compiler objects
or simulator outputs are needed. The build record must list the patched MRI
header among its actual dependencies. Select the resulting executable with
`LLC="$llc_release_work/bin/llc"`, retaining the reference SDK for CIRCT and
the simulator runtime. The isolated BOOM backend median decreased from about
92.91 to 90.1868 seconds with identical model objects. The legacy backend bundle
subsequently passed the accepted four-configuration
code/semantic qualification described above. This isolated suffix measurement
is not an end-to-end or simulation-runtime speed claim.

The later bidirectional scheduling prototype reduced the isolated BOOM
backend median from 91.1112 to 87.4711 seconds (4.00%), with all comparison
objects byte-identical. Its default-on cleanup passed scheduling oracles and preserved complete
model-object bytes on all four models. Its cleaned source was included in the
accepted legacy delivery described
above. The prototype's isolated percentage is not a measured isolated speedup
of that cleaned bundle. The attempted within-query parallel traversal was slower
and is not included in the recipe. No experimental LLC flags,
thread pool, or new persistent memoization is needed by `0007`.

`sdk_overlay.py --profile-kind frontend` retains its historical default; the
legacy 30-TU recipe above explicitly selects IR. An earlier overlay frontend
profile was rejected when SLPUtils and SLPVectorizer coalesced a weak
`std::not_equal_to` instantiation with different frontend hashes (`0x820620` and
`0x20620`). That failed profile remains rejected. Legacy IR PGO uses
`-fprofile-generate` and `-fprofile-use`, enables weak/COMDAT mismatch diagnostics,
and treats compiler warnings as errors. Frontend PGO rejects outdated and
unprofiled-file diagnostics. Do not discard these guards to force a build.

The new full-source workflow instead evaluates **filtered frontend PGO with
atomic counter updates**. Its frozen `frontend-implementation.profile-list`
excludes header suffixes and include-directory definitions by source location,
without function, core or workload selectors. This limits repeated counters in
inline LLVM/stdlib helpers while retaining implementation-defined methods. A
Clang 18 two-TU fixture retained a `.cpp` method declared in `.h`, excluded header
helpers, and passed strict frontend profile consumption. Reduced fixture counter
sites are evidence about coverage, not a full compiler-training speed claim.

A positive implementation-extension allowlist alone is insufficient: Clang 18
can fall back to the main TU filename for unmatched header locations. A catch-all
`!src:*` cannot be overridden by positive implementation rules. Use the frozen
explicit exclusion policy rather than either construction. Some unusual header
locations can remain covered, while implementation files under include directories
are excluded. Header filtering also reduces PGO detail; validate the chosen tools
and outputs rather than assuming equivalent optimization quality.

Keep the policy bytes/path bound in both generation and use, with fresh matching
profiles and atomic instrumentation for concurrent compiler workers. Filtered IR
was not selected: its small validation fixture failed a retained implementation
CFG/hash check. Full frontend, filtered frontend and IR profiles are distinct
policies and cannot be reused across one another. See
[FULL_TOOLCHAIN.md](FULL_TOOLCHAIN.md) for the full workflow's commands,
provenance guards, SDK-plugin qualification and current acceptance scope.

The isolated BOOM OPT comparison used two balanced samples per tool with exact
outputs: the accepted baseline median was 53.2402 seconds, plain filtered frontend
PGO 52.8396 seconds, and separately trained ThinLTO PGO 49.9821 seconds (6.12% less
optimizer wall time). ThinLTO had the lowest median in that isolated OPT comparison; this is not a
full-build or all-configuration speed claim. Choose OPT and LLC independently,
then use the accepted complete delivery report to identify their exact pair,
backend flags and activation scope. The accepted 682-file legacy delivery
provides the comparison reference.

The full workflow records missing-file profile-coverage warnings from unexecuted
compiler sources as a visible, explicitly permitted coverage category. That does
not relax outdated-profile, CFG/hash/COMDAT mismatch or other diagnostic guards.
Use the exact declared coverage policy; do not hide warnings or rewrite profiles.

`--lto --linker /private/path/ld.lld-18` enables ThinLTO across compiler overlay
objects. The reference SDK archives remain native objects: this is not
whole-LLVM LTO. Use matching LTO settings for profile generation and use;
prelink differences can invalidate profiles. ThinLTO adds compiler construction
CPU and memory cost, and its linker has a separate worker pool from `--jobs`.
PGO adds instrumented training runs and profile storage. Neither changes the
model's optimization level or simulator runtime flags. Qualify each new
combination independently before selecting it.

Each private build exposes SDK headers, libraries, `llvm-config`, and needed
LLVM companions through symlinks. `layout.json` records their targets and
identities. Compiler outputs are preserved; unrelated existing files or links
cause an error. For an earlier build, install only these links with:

```sh
python3 compiler/sdk_overlay.py layout \
  --sdk "$sdk_prefix" --work /tmp/llvm-overlay --tool llc
```

Assemble explicitly selected compiler candidates in a separate prefix with
`bundle --sdk SDK --work PREFIX --opt OPT --llc LLC`. This records both input
hashes and creates symlinks; it does not choose a winner or copy simulator
artifacts. Evaluate overrides while retaining the reference SDK:

```sh
make native CONFIG=YourConfig BINARY=your-program.riscv \
  LLVM_OPT=/tmp/private-opt/bin/opt LLC=/tmp/private-llc/bin/llc \
  CIRCT_BIN="$sdk_prefix/bin" CIRCT_ROOT="$sdk_prefix"
```

The optional `compiler/arcilator-driver` CMake project builds an Arcilator
driver against the same SDK archives. It requires CMake, Ninja, a C++17
compiler, and external pinned source; it does not fetch or vendor CIRCT.
`ARCILATOR_SOURCE` names the pinned original or qualified patched
`tools/arcilator/arcilator.cpp`. For example:

```sh
cmake -S compiler/arcilator-driver -B /tmp/private-arcilator -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCIRCT_SDK="$sdk_prefix" \
  -DARCILATOR_SOURCE=/sources/circt/tools/arcilator/arcilator.cpp
cmake --build /tmp/private-arcilator --parallel
make native CONFIG=YourConfig BINARY=your-program.riscv \
  ARCILATOR=/tmp/private-arcilator/bin/arcilator \
  CIRCT_BIN="$sdk_prefix/bin" CIRCT_ROOT="$sdk_prefix"
```

The driver defaults retain SDK JIT support, disable FST, and use C++11 ABI 1.
`arcilator-driver-config.json` records the accepted configure inputs;
`arcilator-driver-build.json` records the completed binary's identity. Keep
the reference SDK runtime even when overriding this driver.

Build, layout, and training manifests record `builder_format` and the helper
SHA. Build manifests additionally record exact flags, source/dependency hashes,
all linked SDK archive hashes, and the merged profile hash when used. Layout
manifests record SDK and symlink identities; training manifests record compiler,
argv, profile, and merged-profile identities. Historical manifests
retain the helper SHA used for their build; formatting the helper does not
rewrite old artifact provenance. Successful linking alone is not qualification:
check complete output identity and isolated fresh compiler timings before
claiming a speedup, then qualify the combined simulator build.
