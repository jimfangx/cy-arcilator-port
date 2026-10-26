# Complete private LLVM compilers

`full_toolchain.py` constructs complete private `opt` and `llc` executables from an explicitly qualified LLVM source/SDK pair. It supports Release controls, fresh Frontend or IR profile generation, profile-guided reconstruction, and optional ThinLTO. It does not change Arcilator/CIRCT sources, simulator functions, model optimization flags or the installed SDK. Construction alone does not establish correctness or a performance improvement.

The selected experimental policy is **Frontend profiling, atomic counter updates, and the exact `frontend-implementation.profile-list` file**. Its generic header/include exclusions reduce instrumentation overhead while retaining implementation definitions, including methods defined in `.cpp` files and declared in headers. The policy is not complete coverage of all compiler code. Filtered IR profiling was rejected after a real mismatch; do not substitute IR profiles for Frontend profiles.

## Prerequisites and source qualification

Use the pinned source revision and reviewed patch order recorded by the supplied provenance. Keep that source tree immutable. The helper does not download, authenticate or apply a supplied archive; `record-source` hashes an already verified checkout/archive, its root files, all source components and provenance. A matching version number is insufficient for SDK compatibility.

A compatibility JSON must be reviewed for the exact patched source and installed SDK. Required fields are:

```json
{
  "status": "validated-pinned-sdk-source-pair",
  "llvm_revision": "<exact 40-hex revision>",
  "source_tree_sha256": "<source_inventory tree_sha256>",
  "public_header_layout_changes": false,
  "sdk_files": {"<each borrowed bin/include/lib file>": "<sha256>"},
  "sdk_symlinks": {"<each borrowed file symlink>": "<link text>"},
  "sdk_tree_sha256": "<sdk_inventory tree_sha256>",
  "compiler_flags": [],
  "requires_atomic_profile_updates": true
}
```

Use `source_inventory()` and `sdk_inventory()` from this helper to obtain the inventories. The latter hashes all borrowed bytes and link targets, rather than a sample of SDK headers. Keep evidence of pinned source provenance, patch hashes, generated-header equality, implementation/public-layout review, actual plugin construction/loading and known-output qualification alongside this record. A caller-authored JSON assertion does not replace that review. Changes to public class layouts or inline contracts require a new ABI decision and matching plugins/headers; this SDK-prefix workflow intentionally refuses the unqualified case.

This implementation is currently qualified on Linux with `/proc` and CPU affinity support. Other operating systems need separate process-control and SDK/source compatibility qualification.

The installed SDK must expose `llvm-config`, LLVM/MLIR plugin headers, CIRCT companions and libraries. Host Clang, Clang++, LLD and llvm-profdata must support the chosen profile format and ThinLTO. CMake/Ninja and normal LLVM build dependencies must already be installed. The helper derives target/triple, ABI, RTTI, exceptions, assertions, threading, statistics, export annotations and dependency settings from the SDK, then compares both generated public configuration headers before compiling. It builds only `opt` and `llc`: building an unrelated `llvm-config` driver can collide with a trained wrapper's `main` profile.

The following Bash variables are caller-selected absolute paths; they are not environment variables interpreted by the helper:

```bash
helper=/path/to/full_toolchain.py
source=/path/to/qualified/llvm-project
sdk=/path/to/qualified/circt-sdk
compatibility=/path/to/reviewed-compatibility.json
provenance=/path/to/pinned-source-and-patches.json
source_record=/path/to/fresh-source-record.json
cc=/path/to/clang
cxx=/path/to/clang++
lld=/path/to/ld.lld
cmake=/path/to/cmake
ninja=/path/to/ninja
profdata=/path/to/llvm-profdata
policy=/path/to/frontend-implementation.profile-list
instrument=/path/to/fresh-instrument-build
training=/path/to/fresh-training
pgo=/path/to/fresh-pgo-build
qualification=/path/to/fresh-qualification
training_argv=/path/to/training-cases.json
qualification_argv=/path/to/qualification-cases.json
jobs=8
lto_jobs=8
training_jobs=1
```

Preserve compiler/linker command spelling when it selects behavior: resolving `clang++` to `clang` can change the language driver. The helper records both the supplied executable path and resolved file/hash.

```bash
python3 "$helper" record-source --source "$source" \
  --revision '<exact pinned revision>' --provenance "$provenance" \
  --compatibility-manifest "$compatibility" --sdk "$sdk" \
  --output "$source_record"
```

## Fresh construction, training and qualification

Use a fresh directory for every stage. This Bash array fixes the common construction identity. For a non-LTO sequence change `--lto thin` to `--lto none` and use separate fresh instrumentation, training and PGO directories; do not share their profiles.

```bash
common=(--source "$source" --source-record "$source_record"
  --compatibility-manifest "$compatibility" --sdk "$sdk"
  --cc "$cc" --cxx "$cxx" --linker "$lld" --cmake "$cmake" --ninja "$ninja"
  --lto thin --lto-jobs "$lto_jobs" --jobs "$jobs" --link-jobs 1
  --profile-kind frontend --profile-update atomic --profile-list "$policy")

python3 "$helper" build "${common[@]}" --mode instrument --work "$instrument"

python3 "$helper" train --manifest "$training_argv" --work "$training" \
  --opt "$instrument/prefix/bin/opt" --llc "$instrument/prefix/bin/llc" \
  --llvm-dis "$sdk/bin/llvm-dis" --profdata "$profdata" --jobs "$training_jobs"

python3 "$helper" build "${common[@]}" --mode pgo --work "$pgo" \
  --profile-record "$training/profile.json" --unprofiled-warning warning

python3 "$helper" qualify --manifest "$qualification_argv" --work "$qualification" \
  --opt "$pgo/prefix/bin/opt" --llc "$pgo/prefix/bin/llc" \
  --llvm-dis "$sdk/bin/llvm-dis" --jobs "$training_jobs"
```

For a control, `build ... --mode release --work <fresh-control>` uses the same qualified source/configuration without profile generation/use. A control requires its own qualification. No host-native tuning or simulator-specific dispatch is added. Build jobs may change between stages, but fixed ThinLTO backend jobs remain part of the producer/consumer identity. Account for both concurrent cases and each compiler's internal worker count when allocating CPUs and memory.

Training sets a unique `%p-%m` raw-profile destination for every declared case/step, removes inherited profiling/preload variables, and merges only `train-*.profraw` in the fresh stage. Startup/build-tool profiles are excluded. Each expected output must pass before merging; any merge warning prevents publication. Only a successfully completed training stage publishes `profile.json`. Profiles cannot cross source bytes/path, SDK, compiler/linker, helper version, profile kind, atomic policy, filter path/bytes, ABI flags, LTO mode or fixed LTO worker-count changes. There is no reuse of prior partial profiles.

Every PGO C/C++ compile retains `-Werror`, explicit hash/out-of-date and weak-COMDAT mismatch diagnostics. The selected **consumer-only** policy adds exactly `-Wno-error=profile-instr-unprofiled`: expected missing-file coverage warnings remain visible and counted in `profile-warning-audit.json`, including on failed construction. It does not suppress diagnostics or relax stale/hash/COMDAT errors. Consumer severity is separately recorded from profile generation identity. `--unprofiled-warning error` restores fully fatal missing-file coverage. Assembly-only commands are classified as nonconsumers; requiring C++ profile-use flags on `.S` commands is invalid.

## Opaque compiler case manifests

The helper has no core, generated-symbol or workload rules. The caller supplies exact pipelines, target flags, plugins and known outputs. Every input/plugin/reference needs an absolute path and SHA256. Independent cases can run concurrently; steps within a case run serially in that case's fresh directory.

```json
{
  "inputs": [
    {"path": "/absolute/input.bc", "sha256": "<sha256>"},
    {"path": "/absolute/Pass.so", "sha256": "<sha256>"}
  ],
  "cases": [{
    "id": "opaque-case",
    "steps": [
      {"tool": "opt", "args": ["/absolute/input.bc", "-O3", "-load-pass-plugin=/absolute/Pass.so", "-o", "{work}/optimized.bc"]},
      {"tool": "llc", "args": ["{work}/optimized.bc", "-O3", "-relocation-model=pic", "-filetype=obj", "-o", "{work}/model.o"]}
    ],
    "outputs": [
      {"path": "{work}/optimized.bc", "comparison": "llvm-ir", "reference": {"path": "/absolute/reference.bc", "sha256": "<sha256>"}},
      {"path": "{work}/model.o", "comparison": "bytes", "reference": {"path": "/absolute/reference.o", "sha256": "<sha256>"}}
    ]
  }]
}
```

The example's options are illustrative, not a default pipeline. Keep the real original optimization/verification/CPU options and ordered postpasses verbatim. `bytes` requires exact output identity. `llvm-ir` uses the pinned SDK decoder and ignores only its `; ModuleID = ...` comment; instruction, attribute, metadata and source-file differences remain visible. Raw output hashes and byte identity are also retained. Response files and unbound file-valued options are refused. Case outputs may not use helper-owned `result.json`/`result.json.new`, numeric `.log` names, or the `.canonical-` prefix; these collisions are rejected before compiler launch. Keep a separate held-out manifest to assess training coverage; the helper does not invent a hold-out split.

## Publication, integration and interruption

Only a successful construction publishes `prefix`, after generated-config checks, actual command audits, complete source/SDK rechecks and startup validation. The prefix contains newly built regular `opt`/`llc` files and references the pinned SDK's other tools, headers and libraries. `build-manifest.json` explicitly says `built_requires_output_qualification`; it is not an acceptance marker. Preserve the matching completed stage record and manifest mirror. Before selection, independently build/load the port's real plugins using this prefix and run the complete required compiler/output qualification, followed by fresh simulator build/runtime validation under the project's policy.

Use explicit `LLVM_OPT=<prefix>/bin/opt` and `LLC=<prefix>/bin/llc` in the port only after that qualification. For the qualified coarse-liveness experiment, also pass `ARC_LLC_FLAGS='-parallel-virtreg-liveness-jobs=4'`; retain the caller's original model and other LLVM flags. The selected worker option is an LLVM compiler option and does not split the simulator. The supplied SDK's Arcilator/CIRCT tools remain unchanged. Do not change model optimization options while comparing compilers. No simulator result lookup, ccache integration or ThinLTO incremental cache is introduced.

SIGINT/SIGTERM request cancellation. The Linux implementation tracks process start identities and owned descendants, including Ninja-created process groups, then stops only those descendants. It does not provide a pause command. Keep failed-stage records and logs. `build --resume` requires the same helper/source/profile/tool/configuration/severity identity and no remaining owned live jobs; it resumes that compiler construction, not a fresh timed build. Completed stages are immutable. Training/qualification require fresh directories; a failed attempt is never relabelled successful. Use the construction's frozen `producer.py` if the working helper has subsequently changed.

Retain configure/compile/link commands, warning/source counts, tools, source/SDK inventories, raw/merged profiles, references, output comparisons and stage status. Compiler construction/training costs are setup costs, separate from simulator compilation time. Performance claims require a separately reserved quiet measurement with fresh simulator outputs and correctness gates. Concurrent development timings cannot establish speedups.
