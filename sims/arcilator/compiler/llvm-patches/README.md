# LLVM compiler algorithm patches

These patches target LLVM commit
`a17069fea5b271c6824ea773874b007116de9644`, the LLVM submodule of CIRCT
`firtool-1.162.0` (`fa4f662cbe710906347fac795c581a06c6b99757`). They reduce
redundant compiler work without removing optimization passes, changing model
partitioning, or adding runtime functions. They do not modify an installed SDK.
`manifest.json` records the original sources, patch order, and hashes.

Validation entries describe construction-time and isolated compiler checks.
Complete simulator build and runtime evaluations are separate reports with
their own frozen source, tool, model-flag, and workload manifests.

1. `0001` skips a scheduler DFS node that an earlier visit already processed.
   Reconvergent dependency paths can enqueue a node more than once. Skipping its
   repeated successor scan preserves reachability and the final visited set.
2. `0002` scans definitions directly when subregister tracking is disabled and
   the live interval has no subranges. Uses cannot affect this initialization
   step under that condition. MRI stores definitions before uses, and its
   definition iterator preserves their order. The original path remains for
   subregister tracking; subsequent interval construction is unchanged.
3. `0003` skips sorted slot indexes preceding the first live segment before
   testing interval intersection. Such indexes cannot intersect any segment.
   The public declaration now documents the existing requirement that slot
   indexes are sorted in non-decreasing order. Duplicates remain supported.
   Both callers in this LLVM version maintain sorted arrays: spill-weight
   calculation uses the ordered register-mask slots; stack coloring sorts its
   live-start arrays and preserves their order with `inplace_merge`.
4. `0004` records block membership in the existing LazyValueInfo callback
   handles. Value invalidation visits those blocks in the original ascending
   order instead of scanning every block. Lattice, overdefined, predecessor,
   and nonnull insertions all record membership. Block/edge invalidation can
   leave conservative IDs, which are rechecked; clearing the analysis destroys
   the handles and memberships. The callback is removed last. The inline
   handle grows from 40 to 72 bytes; larger memberships can allocate.
5. `0005` groups the existing ordered SLP alias-result cache into source rows.
   A memory-dependency scan acquires its row lazily and retains destination
   query order, direction, answers, and AA calls. The sole insertion callsite
   cannot grow the outer map while a scan uses its row pointer. Existing
   deferred instruction deletion still prevents address reuse. Destination
   buckets shrink from 24 to 16 bytes; each row has 272 bytes of inline
   storage. Storage capacity changes do not change optimization budgets.
6. `0006` collects physical-register uses from the circular MRI use-list tail,
   then reverses the appended buffer to retain their original order. This
   avoids scanning the preceding definitions when the register is physical
   and the lane mask is complete. Physical subregister indexes are illegal
   MIR, so those definitions cannot read the old value. Virtual registers and
   subranges retain the original traversal. Every use's kill-flag update,
   PHI predecessor endpoint, tied early-clobber slot, and live-range extension
   call remains in its original order, including duplicate extensions.
   The generic collection API is additive, nonvirtual and out of line; no
   class field, layout, or existing inline body changes. It uses a temporary
   operand-pointer buffer and introduces no threading or compiler flags.
7. `0007` answers scheduling reachability queries with a bidirectional search
   inside the current topological interval. It expands the smaller frontier;
   meeting proves a path, and exhausting either frontier proves no path.
   Queued-edge ordering and the existing negative-result cache retain their
   original invalidation rules. The original DFS remains for edge insertion,
   where its visited set determines the topological shift. Query scratch is
   private and every later operation that interprets it resets it first.
   There are no new class fields, compiler flags, threads, or persistent caches.

8. `0008` short-circuits edge-dominance queries using proven CFG conditions.
   A reachable non-entry block's immediate dominator must be the edge's source;
   otherwise that edge cannot dominate it. If the source has only that successor,
   the edge dominates the destination. Entry/unreachable and other cases retain
   the original predecessor scan, duplicate-edge behavior and query ordering.
9. `0009` computes eligible scalar virtual-register ranges in coarse independent
   batches, then commits results in original register order. Workers own their
   calculator, ranges and arenas; kill/dead flags and component splitting remain
   serial. Complex subregister/bundle cases use the original calculation. An
   actual component split invalidates later speculative results because MRI
   delegates can affect other registers. The hidden worker option defaults to 0,
   retaining the original serial loop. The qualified threaded mode uses four
   affinity-limited workers without changing public class layouts or model code.
10. `0010` excludes hidden mangled `.local` COMDAT address aliases from LLVM's
    plugin-export extraction. Public functions remain exported. This is a pinned
    compiler construction correction, not an optimizer or runtime speedup.

11. `0011` removes MemorySSA DFS frames for leaves and exhausted parent tails.
    It preserves traversal order, parent outgoing state, visited behavior and
    every rename/setter call. The pinned dominator tree uses intrusive sibling
    iterators; the change relies on that exact iterator contract.
12. `0012` skips a complete dominated-edge use walk only when a multi-use value's
    edge destination has no PHIs and the edge does not dominate that destination.
    Then no use can qualify, including a PHI use elsewhere. The original callback
    and replacement path remains for every other case. The rejected callback/
    query-factoring experiment is excluded.
13. `0013` indexes the highest static source-order category within the scheduler's
    existing first-1000 ready-list prefix. It evaluates the original dynamic
    comparator on that category in original vector order, preserving its
    nontransitive fold and swap/pop behavior. Small queues and broad ties retain
    the original fold. CSE and queue lifecycle changes invalidate the private
    implementation index. `SourceOrderPrefix.h` is a new private LLVM-licensed
    helper under `llvm/lib`, not a public or installed header. No public class
    layout changes are introduced by `0011`–`0013`.

The conditions depend on compiler invariants and target capabilities. They have
no core, configuration, workload, or generated-symbol dispatch.

Choose the manifest recipe before applying patches to a fresh private checkout
of the pinned `llvm-project` revision. The legacy `optimizer_overlay` (30 TUs)
and `backend_overlay` (eight TUs plus the driver) explicitly apply **0001–0007
only**; retain their original per-file hashes.
`historical_full_source_combined_recipe_0001_0010` and its accompanying historical
source-hash maps preserve the earlier qualified full-source recipe.
`full_source_combined_recipe` now describes **0001–0013**. Fresh compiler
construction, profiles, four-model/plugin/API qualification and quiet stage
comparisons passed; complete simulator build/runtime acceptance is a separate
delivery gate. Its extended global hash maps apply only
to that new series. Do not activate it on the historical evidence. Do not apply all
patches and then relax legacy hash checks to accommodate changed sources.

From the checkout root, apply exactly the chosen recipe in manifest order:

```sh
python3 - /path/to/llvm-patches full_source_combined_recipe <<'PYCODE'
import hashlib, json, pathlib, subprocess, sys
patch_dir, recipe_name = pathlib.Path(sys.argv[1]), sys.argv[2]
manifest = json.loads((patch_dir / 'manifest.json').read_text())
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip() == manifest['revision']
names = manifest[recipe_name]['applies_to_patches']
by_name = {row['file']: row for row in manifest['patches']}
for name in names:
    patch = patch_dir / name
    assert hashlib.sha256(patch.read_bytes()).hexdigest() == by_name[name]['sha256'], patch
    subprocess.run(['git', 'apply', '--check', str(patch)], check=True)
    subprocess.run(['git', 'apply', str(patch)], check=True)
PYCODE
```

For a legacy overlay, replace the recipe argument with `optimizer_overlay` or
`backend_overlay` and use a separate fresh checkout. The manifest includes the
previously omitted documentation-only LiveInterval.h output hash from `0003`;
that comment documents the existing sorted-slot contract without changing ABI.

Rebuild the affected compiler from that checkout, or compile the modified
translation units into a separate SDK-linked tool. Keep the original SDK
unchanged. Do not apply these patches blindly to another LLVM revision.

For `0001`–`0003`, validation compared complete model objects against the
original SDK backend for Rocket, BOOM, Saturn, and Shuttle; all four were
byte-identical. Separate
algorithm oracles checked DFS traversal, subregister guards, and sorted-slot
intersection. Those validation programs are not included in this series.
These checks establish preservation of the tested generated runtime code.
They do not establish an end-to-end clean-build speedup or exhaustive target
coverage.

`0004` and `0005` were included in the accepted legacy compiler bundle. Its
four-target complete build/runtime evaluation is retained in the frozen
`native-compiler-only-20261026T010301Z` delivery. Their instrumented training
outputs matched the reference optimized bitcode on all four models. An isolated
BOOM optimizer suite used two runs per compiler, with all eight outputs exact:
the prior selected OPT median was 71.6076 seconds, LVI with the existing 26-TU
profile was 64.1399 seconds, and the combined 30-TU candidate was 56.1024
seconds (21.65% less optimizer time). This is an optimizer-stage result, not a
full-build, simulation-runtime, or all-configuration speed claim. The combined
result includes broader compiler IR-PGO coverage, so it is not attributed to
the patches alone.

The optimizer cache oracles exercised actual LVI callback lifetimes and the
SLP lookup bodies; the latter used a mock AA backend to compare ordered calls.
Omitting nonnull membership and collapsing query direction were detected.
New source/profile/tool combinations still require full-model qualification.
The manifest records the complete
30 implementation sources, generated private-header identity, and validation
scope. See [../TOOLCHAIN.md](../TOOLCHAIN.md) for fresh profile construction.
No additional Arcilator/CIRCT source change, optimization-pass removal,
runtime boundary, or artifact/result cache is introduced by these two patches.

`0006` is the minimal scalar physical-use suffix change included in the accepted
legacy bundle. Six genuine LLVM MachineFunction oracles compared complete
live intervals and every machine-operand flag against the SDK, covering
repeated uses, early clobbers, virtual subregister lanes, PHI predecessors,
bundles, debug/undef uses, and physical definitions. The collection API also
preserved the existing destination-buffer prefix and original use order.
An isolated BOOM backend comparison preserved complete model-object bytes:
the baseline median was approximately 92.91 seconds and the minimal candidate
median was 90.1868 seconds, a 2.93% reduction in that backend stage. This is
not a full-build, simulation-runtime, or all-configuration speed claim.

The threaded planning, canonical-instruction shortcuts, last-instruction
reuse, and BOLT experiments are retained only as private evidence; they are
not part of `0006`. Rebuilding the selected backend needs the patched public
MRI header ahead of the SDK headers and the additional
`MachineRegisterInfo.cpp` object. The manifest records all eight implementation
sources and both original and patched header identities. The pinned SDK stays
unchanged. See the legacy nine-translation-unit backend recipe in
[../TOOLCHAIN.md](../TOOLCHAIN.md).

An isolated, balanced BOOM backend comparison of the `0007` prototype used two
runs per compiler. The prior selected backend median was 91.1112 seconds and
the bidirectional prototype median was 87.4711 seconds, a 4.00% reduction.
All six comparison objects, including a separate parallel prototype, were
byte-identical. The parallel prototype was slower and is excluded from this
series. Genuine SDK scheduling oracles compared complete query and ordering
traces, queued updates and reinitialization, unnotified edge removals, and
`GetSubGraph`/`WillCreateCycle` calls after queries. Deliberately reversing the
backward traversal incorrectly was detected. The default-on cleanup
passed the same scheduling oracles in three repeats and preserved complete
model-object bytes for Rocket, BOOM, Saturn, and Shuttle. Applying `0001` and
`0007` to the pinned source reproduced the compiled source exactly; the earlier
six patch files remain unchanged. The cleaned source was included in the
accepted legacy bundle. The prototype percentage does not measure the cleaned
tool in isolation; it is a backend-stage result, not a complete-build or
simulation-runtime claim.

The accepted legacy bundle preserved complete model objects and matched normal/
observed executable load images on Rocket, BOOM, Saturn and Shuttle. Both retained
scalar programs passed complete-buffer, ten-callback, log and cycle/counter
checks. That delivery used one fresh build per engine/config and two balanced
runtime repeats with warm tools and retained FIRRTL; Scala elaboration and
compiler construction were excluded. These checks establish the tested scope,
not universal configuration coverage or Saturn RVV qualification.

For `0008`, independent CFG reachability and SDK differential oracles accompany
four exact model objects. Eight balanced BOOM optimizer samples compared matched
Release controls: 73.4951 seconds versus 72.1726 seconds, 1.80% less stage wall
time. This isolates the algorithm in those controls, not a comparison against the
legacy selected PGO optimizer.

For `0009`, 986403 speculative ranges were compared with the original serial API
at their commit points; real MIR/delegate checks exercised fallback and split
invalidation. Four complete model objects remained exact. Two balanced BOOM
backend samples per mode measured 88.0939 seconds stock versus 86.7032 seconds
with four workers (1.58% less wall time). Compiler CPU rose 1.89% and peak RSS
rose 34.1MiB. Sixteen workers were not selected over four; their difference is not
claimed statistically significant. These isolated percentages are not additive.
Tests and rejected lookup/threading experiments remain outside this patch series.

See [../FULL_TOOLCHAIN.md](../FULL_TOOLCHAIN.md) for full-source compiler
construction, training and qualification with `full_toolchain.py`. The generic
`frontend-implementation.profile-list` excludes header/include definitions while
retaining implementation bodies, including tested `.cpp` methods declared in
headers. It reduces repeated inline-helper counters without function/core/program
selectors. Use identical frozen policy bytes in generation and use, fresh
frontend profiles and atomic counter updates for concurrent compiler analysis.
Changing source, policy, instrumentation, tuning or LTO mode requires matching new
stages. Recorded missing-file coverage warnings remain visible under the declared
policy; outdated/CFG/hash/COMDAT mismatches remain fatal. No profile is rewritten.

**Historical 0001–0010 full-source evidence:** Both plain and separately trained
ThinLTO filtered-frontend compilers passed
four-target exact canonical-IR/model-object qualification, fresh SDK-plugin
loading and actual production codegen API checks. A balanced BOOM OPT comparison
(two samples/tool, exact outputs) measured medians of 53.2402 seconds for the
legacy reference, 52.8396 seconds for plain PGO and 49.9821 seconds for ThinLTO PGO
(6.12% less optimizer wall time). This does not establish a full simulator-build
or all-configuration speedup. Select OPT and LLC independently; an accepted fresh
complete build/runtime delivery report must bind the chosen tools, source,
profiles and flags before activation. Construction/training costs and isolated
stage evidence remain separate from simulator-build measurements.

Initial individual `0011`–`0013` experiments provided the following evidence. Matched Release BOOM
controls, with two balanced samples per compiler and exact outputs, measured:

| Patch | Compiler stage | Control median | Candidate median | Stage wall reduction |
| --- | --- | ---: | ---: | ---: |
| `0011` | OPT | 72.5010 s | 71.5332 s | 1.33% |
| `0012` | OPT | 73.6049 s | 72.2069 s | 1.90% |
| `0013` | LLC | 86.5760 s | 82.3713 s | 4.86% |

These percentages are not additive. They compare individual algorithms in
matched Release tools, not the previously selected full PGO compiler or the
new combined compiler. They establish no fresh complete-build or runtime result.
The manifest binds the exact timing records and oracle/output evidence by hash.

`0011` passed 45 actual MemorySSA API cases, independent in-process traversal/
parent-state checks and 30 deterministic exact event/use-list streams. Three
incorrect traversal/setter mutations were detected. Its initial model check
covered BOOM; a separate later replay passed canonical IR and complete model
objects for the remaining three models. Together these records cover all four
MemorySSA prototype models, without qualifying the new combined full PGO tool.
Randomized global trace ordering across separate processes is not claimed
because original pointer-keyed Phi roots can themselves vary.
`0012` preserved actual API IR, use order and callback traces and all four
prototype objects/canonical IR. Removing its no-PHI guard was detected. The
callback-invalidation fault belongs to the rejected factoring variant and is
not counted as selected-patch mutation coverage.

`0013` preserved all four prototype objects with an actual-pop original-fold
verifier. Its private-header oracle checked 57,040 pops, nontransitive ties,
exact prefix boundaries, CSE and lifecycle changes; six unsafe faults were
caught. Because the actual-pop verifier executes the original fold first, fresh
verifier-off all-four output and plugin/API qualification is a required new
combined-compiler gate. The isolated BOOM timing runs disabled that verifier and
preserved complete objects. The packaged private header differs from the tested
prototype only by its LLVM license comment, as recorded by the patch replay.

The 0001–0013 source series requires new matching filtered frontend/atomic
profiles and independent compiler construction. Do not reuse 0001–0010 profiles,
objects, readiness or timing results. The fresh combined ThinLTO compiler passed four-model canonical IR/object
identity with the scheduler verifier disabled, plus fresh production
SDK-plugin/codegen integration. Separate balanced BOOM stage comparisons
against the accepted PGO tools selected the new OPT and LLC. The manifest
binds their construction, profiles and measurements. Complete simulator
build/runtime acceptance is a separate delivery report required before activation. Tests and rejected alternatives remain private;
no simulator artifact/result cache, runtime boundary, configuration dispatch or
Arcilator/CIRCT source patch is introduced by these patches.

The patches modify LLVM code licensed under Apache 2.0 with LLVM Exceptions.
`LICENSE.txt` is the license from the pinned LLVM revision; its source and hash
are recorded in the manifest. Existing LLVM copyright and SPDX notices remain
in the upstream source files.
