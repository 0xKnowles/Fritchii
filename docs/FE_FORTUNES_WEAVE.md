# Fire Emblem: Fortune's Weave (`04007EF01F8AE000`) — bring-up notes

Base: `nx2-20261031` (`60ef6d275`), branch `fe-fortunes-weave`. Linux build (CachyOS,
GCC 16, radv 26.99.99 / RX 9070 XT), `eden-cli`, loose ExeFS dump with update `131072` (2.0.0).

## Shader implementation update (2026-11-02)

Windows validation below uses nx2 commit `4285e58d06`, Clang 22.1.8 Release,
with the local patch in this worktree. The earlier bring-up notes below are historical.

### Uniform reuse and command-frontier follow-up

This pass retains the throughput build and implements the next bounded changes:

- Both graphics and compute uniform streaming download newer GPU-written bytes before
  reading guest memory. This also covers the unaligned-binding path, which previously
  selected streaming without excluding GPU-modified data.
- Small Vulkan uniforms reuse immutable uploads after comparing freshly read bytes. Keys
  include stage, bank, size and scheduler tick; compute has its own stage namespace. Tick
  changes force a fresh allocation. Changed bytes never overwrite an in-flight upload.
  This replaces the experimental graphics-only `EDEN_NX2_UBOREUSE` gate with a default path,
  using normal stream allocations rather than per-change deferred staging allocations.
- Exact duplicate storage bindings share synchronization and dirty marking within one
  binding pass. Descriptor entries are still emitted individually. Read-to-write upgrades
  still mark the buffer written; collisions fall back to full setup, and multi-range
  bindings invalidate the small per-pass reuse table. No state survives a draw/dispatch.
- Command fetches wait through the last possibly memory-writing ring entry instead of an
  inert trailing run of non-executable method-sink appends or engine switches. Every direct
  macro write, multi-method, executable method and deferred callback remains a hazard.
  In particular, direct state writes that can flush deferred draws are not exempted. The
  historical GPU-written-page check remains after synchronization, including the first
  queued write. Ordinary drains, parameter waits and submission fences are unchanged.

The new diagnostic counters report uniform hits/misses, duplicate storage setup reuse and
command state-only tails. A tail count is not a count of eliminated waits: the prior writer
may still need completion, and the consumer may acknowledge several entries together.
This does not remove all command-read serialization or establish a GPU hardware bottleneck.

Separate executable: `build-fe-shaders/emulator-nx2-4285e58d/bin-buffer-reuse-fixes/eglantine.exe`.
Timing launcher: `build-fe-shaders/Run-buffer-reuse-fixes-with-timing.cmd`.
The user's running game and previous executable are left untouched. Gameplay correctness
and any FPS improvement remain unverified until the user's retest.

Validation: `build-fe-shaders/buffer-reuse-build.log` reports BUILD READY, Clang 22.1.8
Release, 12 jobs. The full suite passed 398 cases (17,694,753 assertions); a separate focused
run passed 28 cases (8,578 assertions). `git diff --check` passed. The previous throughput
executable still has SHA-256 `35431A5FAAEC7E801C9B5202A319DD363B2591D2F113CE5A53FEC8C404FB7F76`.
New executable SHA-256: `2E8B842EF7B995D573BAD3EF8F1444F293C6FB4E3C7CDACDFE862CC86E9C2D87`.
No emulator/game or GPU test was launched; the existing user game remained running.

### Throughput follow-up

The user reported 15-17 FPS with the CPU/GPU audit build. This follow-up retains those
correctness fixes and adds three bounded optimizations:

- Native Vulkan storage bindings cache the first device-contiguous span by address space,
  guest address and requested extent. Map/remap/unmap invalidations cover the entire query,
  including holes beyond the returned span. OpenGL remains uncached because its mapping
  notification path does not provide the required invalidation guarantee.
- Graphics descriptors sharing a texture-handle table reuse an owned snapshot within one
  draw. Each descriptor root is still validated; every new draw synchronizes and reloads.
  The cache holds at most eight tables, with a noncached overflow path.
- Direct compute dispatches with a zero-sized grid skip pipeline/resource setup and false
  storage-buffer dirtiness, retaining flushes and the pre-dispatch memory barrier. Indirect
  dispatches always read their real dimensions and are not skipped by this check.

Command-fetch drains remain unchanged. Optional timing now attributes drains to command
fetches, parameter synchronization, submission fences and ring pressure, and distinguishes
historically GPU-written command pages from actual dirty flushes. Compute timing is split
into pipeline, lock, storage, texture, buffer, descriptor and recording work. These nested
zones are exclusive CPU wall time; sum them with residual `compute` when comparing the
previous aggregate. They do not measure hardware GPU execution time.

The separate output is `build-fe-shaders/emulator-nx2-4285e58d/bin-throughput-fixes/eglantine.exe`.
`build-fe-shaders/Run-throughput-fixes-with-timing.cmd` enables timing for that process only.
The previous executable remains intact. Gameplay performance and the 60 FPS target are
unverified until a same-scene user retest with warmed shaders.

Validation: `build-fe-shaders/throughput-build.log` reports BUILD READY using Clang 22.1.8
Release with 12 jobs. All 390 cases passed (17,685,615 assertions). A second focused run
covering snapshots, mapping caches, empty dispatches, drain metrics and existing command
synchronization passed 22 cases (8,432 assertions). The prior CPU/GPU executable's hash
is unchanged. No emulator or game was launched. New executable SHA-256:
`35431A5FAAEC7E801C9B5202A319DD363B2591D2F113CE5A53FEC8C404FB7F76`.

### CPU/GPU audit integration

After the final-copy clipping build still ran at approximately 15 FPS, three independent
read-only audits covered command processing, Vulkan compute/shaders, and buffer/texture
visibility. The user then requested implementation of validated issues. The combined patch:

- Omits unchanged native inline-QMD data words (`0xc8..0x106`) on the compute front, while
  preserving the `0x107` upload/launch trigger, diagnostic writes, bulk writes and all other
  side effects. Existing timing output reports considered/skipped QMD writes.
- Resolves native storage bindings with `MaxContinuousRange` instead of materializing every
  mapping segment and discarding all but the first. Tests compare actual memory-manager
  results on holes, discontinuities, small/big pages, unaligned requests and remaps.
- Replaces a compute binding with the null binding when its source constant bank is disabled,
  preserving the descriptor slot instead of accidentally reusing the prior dispatch's SSBO.
  This is a correctness fix; the triggering warning was not observed in the gameplay log.
- Keeps whole-texture readback intact but restricts reusable CPU-read coverage to the original
  observed pages. Only those pages become preflushable buffer pages. An unseen neighbor must
  still notify the buffer cache before its read trap is removed. Tests cover same/other-core
  reads, later GPU rewrites, host-cache certificates and completion generations.
- Moves software-HMMA half rounding into a uniform branch at K=8 boundaries. FMA order,
  rounding implementation and the K=16 midpoint remain unchanged; subgroup shuffles stay
  outside the branch. Vulkan shader-cache version is 39; OpenGL is unchanged.

Command-fetch drains were not removed: the first queued GPU write may establish command-page
dirtiness only after that drain. Existing command ordering tests remain required. Draw-local
texture-table snapshot sharing and larger mapping caches remain separate work. None of the
CPU-side timing buckets is a measurement of hardware GPU execution time.

Build output is `build-fe-shaders/emulator-nx2-4285e58d/bin-cpu-gpu-fixes/eglantine.exe`.
Earlier binaries remain available. The timing launcher is
`build-fe-shaders/Run-cpu-gpu-fixes-with-timing.cmd`. No game is launched for validation;
gameplay and FPS still require the user's same-scene retest after shader rebuilding settles.

Validation: `build-fe-shaders/cpu-gpu-fixes-build.log` reports BUILD READY, all 380 cases
passed (17,710,778 assertions), Clang 22.1.8 Release with 12 jobs. The 11 focused mapping,
binding, QMD and readback-coverage cases passed (16,564 assertions). The tensor suite passed
8 cases (76,623 assertions) and exported fresh shaders to `build-fe-shaders/cpu-gpu-tensor-spv`.
All four software K8/K16 single/64-repeat shaders passed `spirv-val --target-env vulkan1.3`.
`check-software-matrix.exe` matched all 128 output words in each GPU test. The K16 negative
control with a final-round-only reference correctly failed (66/128 words matched), verifying
that the check detects missing midpoint rounding. No game was launched by the validation.
Executable SHA-256: `9BD78B5D798338D4DD75254465F63C41A48459D35EADBD89117A4F37B7E53573`.

### Selective buffer readback follow-up

The combined build still ran at approximately 5 FPS in the user's test. The preserved
`build-fe-shaders/readback-before-runtime.log` records about 9.2-9.5 GiB/s of queued buffer
downloads in the two steady-state profiling windows, with no native shader rejection or
ongoing asynchronous shader builds. These are software byte counters, including copy
alignment, not measured PCIe bandwidth. GPU-thread timings are CPU-side wall durations,
not hardware GPU timestamps.

`DownloadsAtFence` treated any preflushable page inside a GPU-written storage binding as
permission to download the entire binding. For native bare-pointer bindings, this could
expand a single CPU-visible page to an 8 MiB download. Fence queueing now unions only the
preflushable-page intersections and known CPU-read ranges. Other GPU-written pages retain
their dirty state and reactive read traps. Existing full-download policy is unchanged.
`GetFlushArea` also requires complete preflushable coverage before skipping a reactive flush
of a multi-page CPU read; one marked page must not hide stale neighboring pages.

Four regression cases exercise clipped and sparse ranges, word/manager boundaries, repeated
writes, overlapping CPU reads, retained dirty state, full-download behavior and a reference
page-selection model. This change adds no toggle, disables no required synchronization,
and retains all five performance candidates and the earlier rendering fixes. Actual FPS
improvement and gameplay correctness require a fresh user test of the rebuilt executable.

Validation: `build-fe-shaders/readback-ranges-verified-build.log` reports BUILD READY after
all 366 test cases passed (17,733,961 assertions), using Clang 22.1.8 Release with 12 jobs.
The executable is `build-fe-shaders/emulator-nx2-4285e58d/bin-combined/eglantine.exe`, SHA-256
`2AB18C78B8C89E91FA5B2A8E11A0C935724382AF5F9DF37CCEAC37A59EB9114B`.
The original `bin/eglantine.exe` remains unchanged. No emulator/game was launched during
this implementation or verification. An initial compile failure from Windows min/max
macros was corrected using macro-safe calls before the successful rebuild.

### Final fence-copy clipping follow-up

The user confirmed approximately 15 FPS with the selective-readback build. Its timing log
recorded a 69,020 us average over the last 600 presents. Further inspection found that
`CommitAsyncFlushesHigh` let page-granular dirty tracking widen the selected byte intervals
again before intersecting byte-level GPU dirtiness. Two disjoint selections on one dirty
page could therefore each copy the same extra bytes.

The final fence-copy enumerator now intersects all three constraints: the selected interval
(already clipped to the host buffer), page-level tracking, and byte-level GPU dirtiness.
Only staging offsets retain 64-byte padding; the source copy size is not widened. Pending
download accounting, completion, read traps and synchronization remain unchanged.
Three additional regression cases exercise queue selection through final-copy enumeration,
disjoint selections on a page, buffer and manager boundaries, sparse dirtiness, repeated
enumeration and byte-exact full/selective reference results with no duplicate copies.

This follow-up is built into `build-fe-shaders/emulator-nx2-4285e58d/bin-readback-clipped`,
leaving the user's running `bin-combined` executable intact. It retains all earlier fixes
and performance candidates. Texture-table reuse and finer compute profiling are separate
future work; no FPS benefit for this follow-up has been measured yet.

Validation: `build-fe-shaders/final-readback-build.log` reports BUILD READY after all
369 regression cases passed (17,713,560 assertions), with Clang 22.1.8 Release and 12 jobs.
Executable SHA-256: `F0E619DA29986D15D2DD3D20C7747F56262A68ED2BFA68FDB0EA6B7EC510F397`.
`build-fe-shaders/Run-readback-clipped-with-timing.cmd` launches this build with the existing
process-local profiler for a comparable user retest; saved settings are unchanged.

### White gameplay frame follow-up

The user's 2026-11-02 gameplay session reached a white frame at approximately 4 FPS.
The preserved `build-fe-shaders/white-screen-runtime.log` rejects native compute program
`0x19a1c97e00` at `0x360`: `16027f60 70000402 0c9e0710 00014200`.
This matches static corpus program 15901, a uniform-handle cube sample. Auto/LZ cube
sampling now gathers XYZ from Ra/Ra+1/Rb before any aliased destination is written;
the existing handle-pair proof and SSA fallback remain required. Cube explicit LOD/bias,
array modes and misaligned coordinate pairs remain rejected rather than guessed.

Walking the rest of that program exposed uniform-source FSET (`0xc0a`, first at `0x390`).
It now uses the existing float comparison lowering with a validated uniform operand,
preserving modifiers, predicate combination, unordered comparisons and FP 1/0 results.
The complete program passes the offline native-to-Maxwell lowering check after both changes.

The same log rejects material programs corresponding to corpus 16088 and 16092. Their
handle extraction precedes ordinary immediate FADDs and bound texture samples, neither
of which writes uniform registers. Those two validated instruction families may now be
crossed by the existing bounded scan; uniform writes and control-flow checks are unchanged.
All 24 previously remaining local texture-provenance rejections disappear in the corpus.
Eleven other corpus programs still reject (ten exit-setup guards and one shared ATOMS form).
This is lowering coverage only, not complete pipeline compilation or gameplay validation.

Regression cases exercise cube masks, predicates, coordinate/output aliasing, direct and
SSA-verified descriptor paths, rejected cube modes, interleaved material samples, uniform
clobbers, and captured FSET comparison results including NaN. Shader-cache versions are now
Vulkan 38 and OpenGL 31. No user settings, saves or caches were manually changed.
The verified Windows build reports BUILD READY in
`build-fe-shaders/white-screen-verified-build.log`: all 362 regression cases passed
(17,721,980 assertions). An initial test incorrectly expected FP comparison constant folding;
it was corrected to inspect the comparison IR, operands, FTZ and FP 1/0 selection directly.
The executable is `build-fe-shaders/emulator-nx2-4285e58d/bin/eglantine.exe`.
The white-screen and FPS outcomes still require a fresh gameplay test of the rebuilt binary.

### Combined local performance build

The user subsequently confirmed the white-screen build renders gameplay, but reported
approximately 4 FPS (screenshot around 226 ms/frame). This confirms progress beyond the
white frame, not complete gameplay correctness or acceptable performance.

At the user's explicit request, the five performance-coordinator candidates were combined
with this worktree's rendering fixes. Exactly six production files were imported and their
SHA-256 values matched the coordinator checkout: `core/memory.cpp`, `video_core/macro_turing.h`,
`audio_core/renderer/command/resample/resample.cpp`, `core/file_sys/vfs/vfs_vector.cpp/.h`,
and `shader_recompiler/ir_opt/ssa_rewrite_pass.cpp` (all under `src/`). Their independent
tests and microbenchmarks remain in the five worker worktrees referenced by
`t3-performance-coordinator/tools/performance/coordinator/HANDOFF.txt`.

These are narrow optimizations, not an established remedy for 4 FPS: Clang already optimizes
the CPU guard similarly; the macro edit does not affect the default x86 JIT; directory lookup
and SSA mostly target loading/compilation; the medium-quality resampler has measured Clang
benefit but no demonstrated game-FPS gain. No rendering accuracy or synchronization is reduced.

The preserved `build-fe-shaders/4fps-complete-runtime.log` has no native shader rejection.
It records compilation bursts through approximately 47 seconds; over the first 600 presents
the presentation fence wait averages 0 us (36 us maximum), with 9 us average compositing.
Those measurements do not cover all possible GPU/CPU synchronization points. A short live
Windows counter sample reported approximately 30% activity on the game's 3D engine, but
the game exited before the longer per-thread CPU sample. The isolated performance build
was concurrently compiling at 10 jobs, so the 4-FPS session was not an isolated benchmark.
The root cause of the sustained slowdown is not established by these observations.

`build-fe-shaders/build-combined-performance.ps1` reuses the existing Clang Release build,
with two jobs while the coordinator owns ten. Its config-specific output directory is
`build-fe-shaders/emulator-nx2-4285e58d/bin-combined`, leaving the prior `bin/eglantine.exe`
unchanged. This overrides the runtime output path in this build's CMake cache; use the combined
helper for subsequent builds. The original build helper also now reads that cache setting
before running tests/reporting the executable, avoiding accidentally testing the old binary.
No emulator was launched, stopped or reconfigured for this integration. Retest the same
scene after background compilation finishes before attributing any FPS difference to code.
For a follow-up diagnostic run, `build-fe-shaders/Run-combined-with-timing.cmd` starts that
same executable with the existing `EDEN_NX2_GPU_PROFILE` instrumentation enabled only in
the launched process. This records exclusive CPU time on the GPU thread, downloads and
semaphore activity every ten seconds in `eden_log.txt`; it is not hardware GPU timing.
It changes no saved configuration or rendering feature. Profiling has overhead, so use
the direct executable for normal performance comparisons. The launcher was not executed
as part of this integration.

Combined build result: `build-fe-shaders/combined-performance-build.log` reports BUILD READY,
with all 362 regression cases passing (17,727,205 assertions). The original working
executable retains SHA-256 `728B39BA1C5FAFA24A38E55ED45D49BABB7B41CA07A86878F516B630AA0FF2BB`.
The performance coordinator also reported its separate build/tests finished, releasing its
ten compile slots before this build finished. No combined-build gameplay/FPS result is claimed.

### Overnight missing-operation audit

The first fresh run with compact matrix helpers passed shader loading and started presenting
frames. It then rejected compute shader `0x1150286000` at offset `0x5430`: packed-half
`HSET2` immediate comparison. Register, immediate and constant-buffer HSET2 forms now
preserve the two comparisons, source swizzles/modifiers, boolean predicate combine, BF
result encoding and FTZ. The captured 2,799-instruction compute program lowers completely.
The subsequent run reached presentation without that shader rejection; this is not evidence
that the displayed image or gameplay is correct.

The static corpus audit also found missing gradient-sample channel masks, TXQ mip-count
selection and a uniform-address floating atomic reduction. TXD now packs selected channels
across the native destination pairs; TXQ mask 8 requests mip count through the existing
Maxwell query. NVIDIA nvdisasm identifies `ff00798e 00000001 0c10e704 000fe200` as
`RED.E.ADD.F32.FTZ.RN.STRONG.GPU [UR4], R1`; the existing Maxwell F32 atomic IR supplies
the same rounding and flush-to-zero mode. Other uniform reduction types remain guarded.
Immediate FFMA is also recognized as not writing uniform texture handles, without relaxing
the existing control-flow provenance guard.

Known remaining audit gaps are not silently ignored: uniform bindless texture provenance
through complex control flow, one uniform cube-sample form, shared `ATOMS.CAST.SPIN`,
and ten programs rejected by the exit-setup guard. The offline corpus is broader than the
observed boot path, so acceptance counts do not establish gameplay coverage. Runtime logs
also contain auto-stubbed nx2 gsv/vi2/syncpt/scaler and HID services; their required behavior
is not established by shader compilation. No additional service success stubs were added.

The reference audit exposed a missing buffer-backed texture-handle path. Texture tracking
must not follow a storage load into its address and mistake that address for a handle.
The port now retains record indices, table bounds, field/query identity, and proven
branch exclusions. Vulkan synchronizes and captures each handle table before resolving
its records. Unsupported expressions remain explicit errors. This removed the observed
out-of-range handles, but the buffer-texture-only build still captured black frames.

Native global pointers also do not carry an NVN length word: a following texture ID or
dimension must not truncate a compute output buffer. The follow-up classifies mapped
pointer slots, excludes stride/predicate expressions, retains all candidates for selected
pointers, and bounds inferred storage windows to the first device-contiguous mapping.
The original cached searches remain in use for non-native shaders. Native classifications
are captured before background translation and serialized for cached launches. Vulkan
selects the closest candidate inside the bounded window; GLSL/GLASM explicitly reject
the new multi-pointer path. Legacy NVN descriptor guesses are not added to this path.

The texture/branch proof and pointer changes are adapted from Duck912/eden-fefw
`8c6f0522b6ee07caa47ab4363823d3dc00aa8ac1`, with separate-bank candidate identity,
cached native searches, detached-environment classification, and bounds safeguards.
At that stage shader-cache versions were Vulkan 37 and OpenGL 30; user caches and saves were not
manually deleted.

The native-pointer run then exposed a missing SPIR-V backend path in compute program
`0x114fbc5000`: NVIDIA disassembly confirms `LDG.E.U16` and `STG.E.U16`.
Raw signed/unsigned 8/16-bit loads now extract the appropriate little-endian field;
stores use compare-exchange to preserve neighboring invocations' bytes. Raw 32-bit
integer add/min/max/bitwise/exchange and wrapping increment/decrement atomics are also
ported. CAS loops use atomic reads. Unsupported backend instructions now identify their
IR opcode instead of reporting only "SPIR-V Instruction".

The global-memory follow-up passed 346 tests (17,620,237 assertions) in
`build-fe-shaders/global-memory-verified-build.log`. Its generated candidate-selection
module also passes `spirv-val --target-env vulkan1.2 --uniform-buffer-standard-layout`,
matching the renderer's uniform-buffer layout feature. Tests cover loaded-handle/address
separation, descriptor bounds, Phi/select identity, branch proofs, native pointer spacing,
same-offset pointers in different banks, repeated accesses, cache serialization, detached
classification, and SPIR-V branch ordering.

All 19 raw-memory variants pass that validator and execute correctly on the local NVIDIA
Vulkan device: 4,864/4,864 words match an independent CPU reference, including return
values, signed data, wrapping boundaries, contended atomic addition, neighboring-byte
stores, and guard words. Reproduction details and the harness are retained in
`build-fe-shaders/global-memory-gpu-validation.txt` and `check-global-memory.cpp`.
These constructed GPU cases are not a gameplay test. The subsequent runtime no longer
rejected that compute shader, but `global-memory-frames/frame-05161.png` was still black.

The next integration audit found that Vulkan's `RecordingEnvironment` did not forward
three new environment methods: `HasNativeInstructions`, `SurfaceBoundBuffer`, and
`IsCbufStorageBufferPointer`. Thus graphics translation through shader reuse could lose
extended register copies, select the texture bank for surface writes, and use legacy
pointer tracing. The wrapper now forwards all three, compares the fixed properties on
reuse, and records/rechecks pointer classifications (including false answers). Tests
exercise the wrapper itself, extended register translation and a bound surface write
with a different texture bank. This retains shader reuse instead of disabling it.
The full build passed 349 cases (17,587,937 assertions). This removed the observed
shader-level zero-format descriptor diagnostics, but the fresh captured frame
`reuse-native-frames/nx2-frame-01801.png` was still black.

An explicitly validation-layer-enabled diagnostic run then reported missing
`VK_KHR_shader_maximal_reconvergence` for quad control, missing Geometry capability
for fragment `PrimitiveId`, creation of an unsupported stencil-export helper,
dynamic vertex stride on pipelines with no bindings, and queries started outside
a render pass by deferred clears and ended inside the realized pass. The follow-up
enables the required dependency only with feature support, declares the capability,
guards the unused helper, omits the unnecessary dynamic stride and defers query
start until a real draw. A generated fragment test covers PrimitiveId presence and
absence. The full build passed 350 cases (17,688,843 assertions). The subsequent
validation-layer run reported no `Validation Error:` messages in
`final-validation-output.log`; this does not prove visual correctness.

The captured graphics program `b78c0b4bbc3ba43f` at GPU address `0x19a130e200`
reads matrix coefficients from native constant bank 24 (including offsets 0, 4,
8, 0x10, 0x14, 0x18, 0x20, 0x24 and 0x28). The previous lowering remapped bank
24 to bank 17, then `BlitConstantsPass` substituted guessed fullscreen values
for every bank-17 read. That substitution is removed. Native root-table methods
0x504/0x508 now populate banks 24 through 31, preserved through translation,
shader-info masks, detached environment snapshots and draw-time uniform uploads.
Repeated equal uploads still execute and advance the cursor. Texture-handle
reads also resolve native root banks instead of guest address zero. Reserved
banks remain rejected, and compute QMD accesses are bounded separately.
The root-table transport is adapted from the same reference commit. The full
12-job Clang build passed 354 cases (17,637,481 assertions), including bank
isolation, repeated uploads, invalid uploads, snapshot copies, native constant
lowering, frontend bank retention and compute descriptor bounds. Evidence:
`build-fe-shaders/root-table-build.log`. The rebuilt runtime still captured black
frames, with no validation-layer errors in the explicitly enabled diagnostic run.

### Display-query black-screen fix

RenderDoc replay of a diagnostic descriptor-set build isolated the failure further:
the game's Autosave / A Next screen was visible in an intermediate
texture, while post-processing uniforms already contained `0x7fc00000` (NaN).
The later composite propagated NaNs into the final black presentation. Fullscreen
vertex positions and the presentation image lookup were correct. This diagnostic
build is not the normal executable; the descriptor-buffer path was restored.
Accurate CPU, High GPU accuracy and synchronous memory operations separately did
not repair the black frame. All diagnostic configuration changes were restored.

The service audit then found that VI2 commands 104 and 105 returned empty success
replies, and command 106 fell through the auto stub. The reference implements typed
display-query responses. The default path now returns the SDR format, display state
and maximum buffer-resolution mode, with corresponding layouts for commands 107
and 354. It validates packet shape and the supported display name, returns explicit
errors for unsupported names, and uses the one-word TIPC result rather than HIPC's
padded result. Tests cover layouts, values, malformed packets and error results.
There is no game-specific toggle or NaN-clamping shader workaround.

The first verified build passed 356 cases (17,641,448 assertions) in
`build-fe-shaders/display-query-verified-build.log`. The final build, including
exact invalid-name error tests, passed 356 cases (17,656,742 assertions) in
`build-fe-shaders/display-query-final-build.log`. A fresh run of the normal
executable now presents the actual Autosave / A Next screen, confirmed in
`build-fe-shaders/display-query-frames/check-2.png` (present 1201), with runtime
log `display-query-runtime.log`. This verifies that boot's shader-loading hang and
black presentation are overcome on this machine; it is not a full gameplay test.
The remaining service auto stubs, corpus rejections and unmapped-read diagnostics
listed above remain audit gaps. Windows computer-use was unavailable, so no input
was sent to advance past that screen. Human testing is still required.

That display-query executable also shows the complete screen on an ordinary cached launch:
`build-fe-shaders/display-query-warm-frames/check.png`, with log
`display-query-warm-runtime.log`. Its SHA-256 is
`e22a3075113fb06e9009cb524a20d616df72da4911d6f9346f60848031e98636`.
An intervening launch with the Khronos validation layer loaded showed the
background but lost text and foreground elements. It reported zero validation
errors and four maintenance9 layer-count compatibility warnings. This visual
anomaly is unresolved; the following ordinary cached launch rendered the full
screen. The evidence does not establish that cache reuse or the validation layer
itself caused the missing elements. Do not describe this as full gameplay validation.

### Transparent-character follow-up (2026-11-02)

The user's subsequent cutscene log records rejected fragment programs at
`0x19a33a7500`, `0x19a4085c00`, `0x19a39c2c00`, `0x19a4ec8500`,
`0x19a3615500` and `0x19a3159000`, followed by skipped draws. The rejection is
`native uniform bindless texture handle provenance`, not an opacity setting.
The log is retained as `build-fe-shaders/transparent-characters-runtime.log`.

The source-order check found the sampler extraction but rejected unrelated
control flow anywhere in the program. For those cases the lowering now retains
both uniform values in a native-only marker. The frontend represents their
packing with BitFieldInsert. TexturePass accepts the original packed handle only
when SSA proves that the sampler is the logical-right-shift-by-20 of the exact
same reaching value; a mismatched or unproven pair is still an explicit error.
The simple straight-line path is unchanged. Reserved BRA bits are checked in the
branch lowering itself, including when handle validation is deferred.

The first offline lowering audit admitted 752 previously rejected corpus
programs, leaving 24 handle-provenance rejections plus the previously noted exit,
cube-sample and atomic gaps. This is lowering coverage, not complete pipeline or
gameplay validation. The verified emulator build passed 357 cases and 17,609,421
assertions (`build-fe-shaders/character-handle-verified-build.log`), including a
frontend/SSA/texture-descriptor test and rejection of a mismatched sampler.
The affected cutscene still requires a user test: the Windows computer-use native
pipe was unavailable, and no user game process was stopped or save data changed.

Before the display-query fix, the presentation trace selected the expected
3840x2160, 10-bit render target through
its physical alias; its modification tick advances. This rules out the observed
path simply failing to find any render target, but does not prove the image content
is correct. Those earlier validation and ordinary runs both captured black output.

The earlier offline 16,738-program
corpus has 788 first-rejection reports remaining: 776 provenance, 10 exit setup, one uniform
cube sample and one shared CAST.SPIN. The harness reports the first error per program,
not an exhaustive instruction count, and does not run the generated programs.
All validation here is agent-run. Human gameplay testing and review are still required.
The contribution policy requires human review/testing and human-written commit/PR text
before publication, including draft PRs; no automated publication is claimed.

### Shader-loading stall follow-up

The user's subsequent Windows run stopped progressing at 40/42 cached shaders. A sampled
busy worker was inside `nvgpucomp64.dll`, not an emulator wait loop. Its log and shader cache
were preserved in `build-fe-shaders/shader-loading-stall*`; no cache deletion was required.

On NVIDIA Vulkan, HMMA now lowers to a compact software collective with one shared,
bounded SPIR-V loop helper per K=8/K=16 shape. This is automatic, with no environment
toggle and no dependency on a cooperative-matrix extension. Other backends keep the
expanded software path. The helper retains the existing lane layout, FP32 FMA order and
FP16 rounding after each K=8 group. Integer significand rounding prevents the driver from
eliminating an intermediate pack/unpack round trip, which the live GPU comparison caught.

High-register shadowing now also describes F2I operands, SURED data/coordinate operands
and the full width of local-memory vector operands. Tests that previously required rejecting
now-supported aliases instead check the actual shadow copies and preserved operands.

Validation of this revision: the full suite passed 324 cases; four compact SPIR-V modules
passed `spirv-val --target-env vulkan1.2`. On the user's NVIDIA GPU, K=8 and K=16 with one
and 64 repeated operations matched all 512 checked output words against a CPU reference.
Those small diagnostic modules compiled in 25–43 ms; this is not a game-load benchmark.
Gameplay and completion of the game's full shader-loading sequence still need user testing.

The Windows Build41 black-screen log still rejects graphics calls and several compute
instruction families. The older run notes below describe different experimental revisions;
their trap-as-no-op and permissive CALL behavior are not enabled by this patch.

Implemented in the current source:

- `IDP.2A.LO`: two 16-bit by 8-bit products, with independent signedness and a 32-bit
  accumulator. NVIDIA `nvdisasm -b SM87` identifies the logged `0x226` instructions with
  `word2 = 0x000010ff` as **IDP.2A.LO.U16.U8**, not IDP.4A.S8.S8 as labeled in the
  historical tables below. The high-byte variant remains rejected.
- Non-uniform `IADD3.X`, including register, immediate, constant-buffer and uniform-register
  second operands, consumes both carry predicates. Extended source modifiers complement
  bits rather than arithmetically negate. Carry-output and unsupported uniform-rewritten
  forms retain their guards.
- Register-address `LDGSTS.E.BYPASS.128` becomes a synchronous 16-byte global load and
  shared store. A disabled source zero-fills shared memory without accessing global memory.
  The original execution predicate and signed offsets are preserved. This does not introduce
  a workgroup execution barrier or general support for other async-copy modes.

Regression coverage includes arithmetic boundary/random values, aliasing, predicate
preservation, suppressed memory reads, zero fill, branch scheduling-slot alignment and
production Maxwell control-flow/IR translation. Vulkan and OpenGL shader-cache versions
are bumped because previously accepted extended adds can generate different code.

### Default-path follow-up on nx2 `4285e58d06` (2026-11-02)

Adapted the relevant implementation from [Duck912/eden-fefw v1.07](https://git.eden-emu.dev/Duck912/eden-fefw/commit/8c6f0522b6ee07caa47ab4363823d3dc00aa8ac1),
including the shader work introduced in `0b1ba0db7607e8b47bb960b3ef1508b5474b33f7`.
These paths are automatic; no game-specific toggle or environment switch is required.

- HMMA.1688 and HMMA.16816 F16 use shuffle/FMA expansion, rounding the accumulators to
  half precision every eight products. Scratch registers are limited to R246-R254 so this
  fork's live uniform-register bank is preserved. No cooperative-matrix extension is needed.
  Tests compare packed results against recorded RTX 4090 outputs, including source/output
  aliasing and preservation of every non-output architectural register.
- Unsigned IMAD.WIDE writes the requested carry predicate after consuming all inputs,
  including when the output predicate aliases the instruction guard.
- Predicated NOINC calls that skip a verified backward branch work even if a captured
  library contains RET. Unrecognized call linkage still fails closed.
- Synchronous copy commit/wait operations retain memory barriers, without introducing
  a workgroup execution barrier.
- Compatible six-layer 2D array views expose cube sampling views for point-light textures
  (`a1b23ac3`). The existing typed-null fallback is retained for incompatible views.
- Multi-range buffer segments are invalidated when their backing buffer is deleted and
  re-resolved after buffer updates; binding checks every id is live (`8c6f0522`). Page-table
  removal does not erase a replacement buffer's entry, and freed LRU entries are not touched.
- Runtime traps are removed only after bounded uniform-prologue evaluation proves them
  unreachable using current dispatch constants. Proof is repeated on cached dispatches;
  missing inputs and reachable traps reject the dispatch without permanently poisoning the
  shader rejection cache. Guard-dependent shaders are not serialized to disk.

The offline corpus now lowers 15,898 of 16,738 programs and no longer reports its previous
195 CALL rejections. Remaining unsupported programs include texture-provenance cases;
offline acceptance alone does not establish that every dispatched shader works.

Validation: the focused Ampere suite passes 124 cases / 9,859,888 assertions, and the tensor
backend suite passes six cases / 54,298 assertions. Production syntax checks pass for
`shader_environment.cpp`, `shader_cache.cpp`, `vk_texture_cache.cpp` and `vk_rasterizer.cpp`.
The focused executables link the existing compiler libraries; the full nx2 rebuild and its
complete regression suite are separate validation steps.

Full Windows Clang build completed on 2026-11-02: all 319 regression cases passed
(13,883,383 assertions), including a new SlotVector occupancy regression. The buffer-cache
port requires the added `SlotVector::contains` API; it tests occupancy before dereferencing
an id. Executable: `build-fe-shaders/emulator-nx2-4285e58d/bin/eglantine.exe`.

These are source-level fixes, not a verified gameplay fix. A fresh runtime log and visible
rendering are required before claiming the black screen is resolved.

## State (2026-10-31)

Boots and runs at 60 fps, but the picture stays black: the native (Ampere) shader lowering
rejects the shaders the game dispatches, so every draw is skipped (`Draw skipped (no pipeline)`)
and the compute dispatches are dropped (`Native QMD failed: native shader dispatch required`).
The game logic itself keeps running (~56 presents/s, vblank-paced, no hangs, no crashes).

## How to run a loose dump from the CLI

`eden-cli` opens `<path>/main`, so the path must be the **exefs directory**, not the game folder:

```
SDL_VIDEODRIVER=wayland WAYLAND_DISPLAY=wayland-1 \
  ./eden-cli -g "/home/leon/Switch2/Fire Emblem Fortune's Weave/updates/131072/exefs"
```

The update's `romfs/` is found next to that `exefs/` directory. Log: `~/.local/share/eden/log/eden_log.txt`.

## Blockers, in order of first hit

`lower_to_maxwell.h` fails on the first offending instruction of a program, so later modes
surface only once the earlier ones are lowered.

Lowered already (2026-10-31, this branch): `ISETP.EX` (64-bit compare high word), `REDUX.MIN`,
`SHF` with a constant source (`0xa19`).

Lowered so far, each verified by a run: `ISETP.EX`, `REDUX.MIN`, `SHF` with a constant
source (`0xa19`), `IMNMX` with a 32-bit immediate (`0x817`), `IMAD.HI.S32` (`0x827`), `IMAD.HI`
with a constant multiplier (`0xa27`), `IADD3.X` with a constant source (`0xa10`), `LEA.HI.X`
with a constant base (`0xa11`), `ATOMS.MIN/MAX` (`0x38c`), `BAR.SYNC.DEFER_BLOCKING` (`0xb1d`),
`BPT.TRAP` as a no-op (`0x95c`), and `LDG` through `desc[URn]` (`0x981`).

**Graphics path: nothing is rejected any more and no draw is skipped.** The two structural
fixes that got there:

- Surface stores and loads keep their own descriptor buffer (`required_surface_buffer`,
  `SurfaceBoundBuffer()`), so a program can sample a texture from one constant buffer and store
  through a surface in another; the pipeline cache version went to 19 and the native override
  header to seven words for it.
- `SUST.P` takes a partial channel mask (`Bits(72,4)`) instead of requiring all four channels.
- `TXQ.B` on a bindless handle (`0x370`) becomes Maxwell `TXQ (b)` with the handle in the register.
- `CALL` accepts the predicated NOINC forward transfer when the next instruction is `BSSY`
  (`0x947`), the rule the nxnative build uses.

The screen is still black because the *compute* side never dispatches: 24 × `native shader
dispatch required` (a QMD needs a lowered program that was rejected). What is left is one family
per line, all compute:

| Message | Instruction | Needs |
|---|---|---|
| unsupported native opcode | `825 ccd43825 00000010 078e02d2` | IMAD.WIDE where bits 32-39 carry a register, not an immediate |
| native integer carry/third source | `210 05087210 00000007 07b5e800`, `c10 0402ac10 0000000a 0ffde0ff`, `c10 0c0a7c10 00000004 0ff9e0ff` | IADD3 with a third source and two carry predicates, in the uniform-rewritten form |
| native constant add carry mode | `a10 2c2c8a10 00005c00 07b5e005`, `a10 1a1aca10 00005c00 0783e011` | IADD3 with a cbuf addend, a third source and both carries |
| native constant LEA carry mode | `a11 2935ea11 00009500 010f0c27` | LEA.HI.X with a constant base and a carry predicate |
| native uniform-source effective address mode | `c11 0a0a7c11 00000004 0f8fe0ff` | LEA with a uniform base |
| native program uses lowering scratch registers | `812 00f67812 00000010 078ec0ff` | an operand names R250-R255, which the lowering keeps for itself |
| native MUFU mode | `308 000c7308 0000000c 00002400` | MUFU with a form the lowering does not model |
| unsupported native opcode | `fae 020d7fae 00000000 0e101c48` | LDGSTS.E.BYPASS.128: Maxwell has no async copy, so LDG + STS |
| unsupported native opcode | `226 47407226 00000040 000010ff` | IDP.4A.S8.S8: four signed 8-bit products summed |

Reminder: this is fail-fast, so accepting one instruction only reveals the next. The count of
failing compute dispatches stays at 24 until every program lowers; the screen stays black while
any of them does.

Done in this pass, each verified by a run: `LEA`/`LEA.HI` with a carry predicate (Pp out of
bits 81-83), `F2I.FTZ.U64.TRUNC` (`0x311`, the auto-exposure of `Lighting.bfsha`), `IMAD.X` with a
carry value and bit 73, `FMNMX` with a dropped predicate output, `HMMA` with a plain addend, and
`IADD3` third-source modifiers.

Also seen, not yet shader related:

- `Unmapped Device ReadBlock` bursts — guest reads inside ranges that never got mapped.
- `nx2:gsv` TIPC `11` unimplemented (falls back to the auto stub).
- `Load: Failed to read title for ROM` — no control NACP in a loose dump, expected.

## What the nxnative repo says about this game (for reference)

`git.eden-emu.dev/penis6/nxnative-penis6`, profile `dm/games/04007EF01F8AE000`. That project is
not an emulator — it recompiles the game's own binaries and rebuilds Switch graphics on Metal —
so its patches do not transfer, but its game profile does:

- 2.0.0 / `131072`, SDK 22.2.0, modules `main subsdk0 subsdk1 subsdk2 sdk`, NVN2.
- Shaders live in RomFS as `.bfsha.zst` / `.ptcl.zst`: 16796 of 16809 programs convert offline.
- Memory: reserve `0x1400000000`, alias `0xC00000000`, stack `0x10000000`, app pool `0x263000000`;
  the game asks for a 36 GB allocator at boot.
- Movies are AV1; there the host's decoder needed the CPU-buffer path, forced by patching the
  movie player in `main`. Not relevant to an emulator, which runs the console path.
- DLSS goes through a temporal scaler.

The same SASS semantics keep biting both projects (FSEL source mods, FSET with a uniform,
IMAD.HI's 64-bit addend, F2I.U64's high word, calls returning into their block, jump tables in
`c1`) — ours already lower all of these; the modes above are the ones neither has yet.

## Still rejected (2026-11-01, 20 compute dispatches)

| Family | Instruction words | What is missing |
|---|---|---|
| IDP.4A.S8.S8 | `226 3f367226 00000036 000010ff`, `226 42797226 00000039 000010ff`, `226 47407226 00000040 000010ff` | signed byte dot product (3 programs) |
| LEA.HI with a uniform source | `c11 00057c11 00000004 0f8fd0ff`, `c11 0a0a7c11 00000004 0f8fe0ff` | uniform base in the high-word path |
| LDGSTS.E.BYPASS.128 | `fae 020d7fae 00000000 0e101c48` | LDG.128 + STS.128 split |
| I2F variant | `312 00027312 00000002 00301000` | opcode 0x312 has no case yet |
| MUFU operation 9 | `308 000c7308 0000000c 00002400` | SM86 MUFU op 9 |
| reserved register as a source | `211 f73b7211 00000000 078f18ff`, `831 d88d7831 3a003a00 0000008d` | programs whose own operands use R246-R254 |

Getting a picture needs every one of these; the display buffer (0x91f41000, 3840x2160) is written by compute passes only.

## Still rejected (2026-11-01, 12 compute dispatches, 6 instructions)

| Family | Instruction words | What is missing |
|---|---|---|
| reserved GPR as a source | `211 f73b7211 00000000 078f18ff`, `831 d88d7831 3a003a00 0000008d` | the program's own operands live in R216-R254, where the lowering keeps uniforms and scratch: needs register remapping |
| MUFU operation 9 | `308 000c7308 0000000c 00002400` | SM86 MUFU op 9 |
| LDGSTS.E.BYPASS.128 | `fae 020d7fae 00000000 0e101c48` | LDG.128 + STS.128 split |
| I2F variant 0x312 | `312 00027312 00000002 00301000` | opcode 0x312 has no case yet |
| CALL with another link setup | `944 00007944 00002d00 03c00000` | the preceding move is not the `0x7802` form the lowering expects |

Done since the last table: IADD3 with one or two carry predicates and a third-source carry-in, LEA.HI.X with a constant base, LEA.HI with a uniform base, HFMA2.RELU with a predicate output, USHF over UR0-UR21, IDP.4A.S8.S8.

## Still rejected (2026-11-01, 10 compute dispatches, 5 shaders, 5 instructions)

| Shader | Instruction | What is missing |
|---|---|---|
| 0x19a2a1d000 | `312 00027312 00000002 00301000` | opcode 0x312 (I2F, the inverse direction of 0x311) has no case |
| 0x11504cb000 | `fae 020d7fae 00000000 0e101c48` | LDGSTS.E.BYPASS.128 needs a LDG.128 + STS.128 split |
| 0x1150686000 | `308 000c7308 0000000c 00002400` | MUFU operation 9 |
| 0x1150562000 | `831 d88d7831 3a003a00 0000008d` | operand in R216-R254: needs register remapping |
| 0x1150588000 | `211 f73b7211 00000000 078f18ff` | operand in R216-R254: needs register remapping |

Just lowered: the CALL that sits after the main EXIT (a call inside the captured library region), now handled by the same linkage path.

### Encoding notes for the last five

I verified the MUFU operation field with the turingas assembler (sm_80): `w2 = op << 10`, COS=0, SIN=1, EX2=2, LG2=3, RCP=4, RSQ=5 — matching Eglantine's `operation = Bits(74, 6)`. Fortune's Weave uses operation **9** (`w2 = 0x00002400`), which turingas does not know and which has no Maxwell equivalent; it needs an expansion (for example tanh as (e^2x - 1)/(e^2x + 1) from EX2 and RCP).

Reproduce an encoding the same way: write a two-line file with a `<params>` block and control codes (`--:-:-:-:1    MUFU.SIN R0, R1;`), assemble with `python3 -m turingas.main -i test.sass -o test.cubin -arch 80`, then read `.text.kern` from the cubin (16 bytes per instruction, low word first).

## Still rejected (2026-11-01, 10 compute dispatches)

| Shader | Instruction | What is missing |
|---|---|---|
| 0x11504cb000 | `fae 020d7fae 00000000 0e101c48` | LDGSTS.E.BYPASS.128 needs a LDG.128 + STS.128 split |
| 0x1150686000 | `308 000c7308 0000000c 00002400` | MUFU operation 9, no Maxwell equivalent: needs an expansion |
| 0x1150562000 | `831 d88d7831 3a003a00 0000008d` | operand in R216-R254: needs register remapping |
| 0x1150588000 | `211 f73b7211 00000000 078f18ff` | operand in R216-R254: needs register remapping |
| (library caller) | `20c 0600720c 00000004 03f26070` | a callee that overwrites the link register: nested calls in the captured library |

Lowered in this round: `0x312` is I2F.U64.F32 with the rounding in bits 78-79 (verified against the turingas grammar, where `0x312` is the `I2F` 64-bit form), `0x227` is the second IDP.4A.S8.S8 form, `0x225`'s operand modifier byte is not fixed (0x0780/0x078e), `0x224` with both factors in RZ is a move, and IADD3 in `0x210` may set bit 63.
