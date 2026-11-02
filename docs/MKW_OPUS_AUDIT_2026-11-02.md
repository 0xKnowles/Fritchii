# Mario Kart World: audit against official c0cb3325 (third pass)

Reference: official `Djihads80/Eglantine` **c0cb3325e2 (nx2-experimental)**, the exact
source of the user's binary, extracted read-only from the local reference clone to
`build-fe-shaders/c0cb-src` with `git archive`. The fork tree is HEAD `5bef7354b4` (main
committed the previously dirty edits, including the async-draw experiment) plus this pass's
working-tree changes. No commit, push or PR was made.

**No gameplay was run in this pass.** Both computer-use transports were unavailable, so no
same-scene measurement exists. Every FPS effect below is **unmeasured**. The evidence is
code comparison plus the preserved logs (`mkw-risky-20261102-161538/baseline.log`,
`mkw-risky-dark-stutter-20261102-162922.log`, `mkw-risky-freeze-20261102-161858.log`).

## Verified defects (fixed)

### 1. The official build and this fork destroyed each other's pipeline cache

Both builds wrote `cache/shader/<title>/vulkan.bin` and `vulkan_pipelines.bin`. Official
c0cb3325 uses `CACHE_VERSION = 18`; the fork uses `39`. Each build deletes a file with
another version at boot. Evidence:

- `baseline.log` (first fork run after the official process ran at 15:56):
  `Deleting old pipeline cache`, then `Total Pipeline Count: 0`.
- Later fork runs loaded 1016 and 1118 pipelines. The current file header is version 39.
- That cold run built ~412 runtime pipelines on the GPU thread at ~2–4 ms each.

So any A/B in which the official build ran in between started cold, and the official build
also lost its cache whenever the fork ran in between. This confounds the 30 vs 15 FPS
comparison (it does not explain all of it). Fix: `video_core/pipeline_cache_files.h`. Files
are now `vulkan.fork-v39.bin` / `vulkan_pipelines.fork-v39.bin`. A legacy file of exactly
this version is renamed once, so the existing 46 MB warm cache survives. Files of any
other version (the official build's) are never touched. The Qt remove-cache actions also
remove the suffixed files. Four tests cover naming, adoption, foreign versions/magic,
truncation and never overwriting.

### 2. Async-draw experiment: indirect draws used stale counts

Main's opt-in `EDEN_NX2_EXPERIMENTAL_ASYNC_DRAWS` passed `draw_state.vertex/index_buffer.count`
for every draw. For `DrawIndirect` those are the previous direct draw's counts, so an
indirect (often GPU-culled) draw could be judged ">6 vertices" from a meaningless number.
`PrepareDraw` now takes the count explicitly; indirect draws pass 0 (never omitted).

The rest of main's hunks were reviewed and kept: side-effect predicate, null return only for
native async with the env opt-in, `submitted = true` only for deliberate omission (genuine
shader failures unchanged), the other `CurrentGraphicsPipeline()` callers default to 0, the
compile-time texture-buffer early return (unbind still runs; `texture_buffer_it` is unused
afterwards), the small_vector sizes, and scheduler-thread-only wait logging. Residual: DKB's
indirect-reset program emulation runs after the pipeline lookup. If that program's pipeline
were eligible (color+depth, >6 points) and unbuilt, the CPU reset would be skipped. It is
unlikely (point pass, no color0 expected) and the experiment is opt-in only.

## Root-cause audit: what c0cb3325 actually does differently

### Guest CPU reads of GPU-written memory (largest measured cost)

In `baseline.log` at 185 s, one guest core spent **5.95 s of 10 s in `ordered_read`**
(10,314 reads, max 291 ms). Other cores spent 0.26–1.3 s. Our `GPU::OnCPURead` always uses
`gpu_thread.ReadMemory`: wait for the front to reach the read, then a full split drain,
`GetFlushArea`, `FlushRegion`, buffer download and a Vulkan `Finish`. In the same windows,
`buffer_finish` cost about 214 ms/s on the GPU thread (5,697 Finishes per 10 s for ~100 KB).

c0cb3325 `OnCPURead` returns **immediately** when `GetFlushArea` reports a preemptive
(already eagerly downloaded) area. It orders only first reads of non-preflushable GPU data.
That is the existing `EDEN_NX2_UNSAFE_PREEMPTIVE_READS=1` behaviour. The user's run of it
reached ~20 FPS but froze, stuttered and rendered dark, so it stays off. **Hypothesis (not
verified):** official correctness relies on fence downloads being written back before the
guest observes the matching syncpoint/semaphore. In this fork's split front/back design,
some releases may become guest-visible before back-thread downloads land, so stale
exposure/visibility data gives the darkness and a poll on GPU-written data never sees its
value (the freeze). Proving this needs a run that logs release vs. writeback order. It is
the most valuable next investigation, not something to switch on blindly.

### Command-fetch drains (second largest; opt-in experiment added)

Every fetch drains queued possible writers before testing GPU-written page history. In
every logged window: **unsynchronized candidates** (no SYNC_WAIT and no known GPU-written
page) waited **200–460 ms/s**, and "first GPU writes seen during their sync" was **0 for
the entire session**. c0cb3325 drains/flushes only when a page is already known GPU-written
(`IsGpuWrittenCommands` before the drain). It executes submit-time snapshots otherwise.

New opt-in `EDEN_NX2_SYNC_WAIT_FETCH_ORDER=1` (default **off**) skips the drain only for
those candidates. SYNC_WAIT entries and known pages keep the full drain → test → flush
order. This is still **risky**: a queued *first* write to a command page would be missed,
which is exactly c0cb's known gap (and the original MKW missing-trees bug class). The NVIDIA
PBDMA contract (see `command_fetch_sync_audit.h`) says such a guest would race on hardware
too. The log line now reports `SYNC_WAIT-contract skips /s`. Two tests cover skip
eligibility and that default-off ordering is unchanged. FPS effect: unmeasured.

### Shader pipelines and disk cache

c0cb3325 has **no** asynchronous graphics translation (`Detach` does not exist there); it
also translates on the GPU thread. Our translation worker fails to detach every MKW
pipeline (`texture snapshot unavailable`: pool limit ≥ 32768 or not fully mapped), and even
when detached the GPU thread blocks on `future.get()`. It is not an upstream advantage.
The real upstream-relative loss was defect 1 above. One c0cb memory fix was ported: compute
pipelines release their `VkShaderModule` after pipeline creation (Vulkan permits it; the
module is used only in the constructor).

### Texture/VRAM residency

Logs show VMA heap 0 usage of **9757 MiB against a budget of 8836 MiB** (378 s) and
**9045 against 8814** (415 s), with 4330/3260 images deleted per 600 frames (2781/1955
costly) and 3967/10757 MiB uploaded. Over-budget usage on Windows invites WDDM paging. That
is consistent with, but does not prove, the 3.7–3.9 s worst frames (scene changes overlap).
Both builds already recompress ASTC to BC3 under a 12 GiB budget (`UseBc3ForSwitch2OnSmallCards`
fired at 4.59 s) and both count allocation bytes as usage. Differences in c0cb3325:

1. Critical memory capped at **5 GiB** on every GPU (`MaxCriticalMemory`), expected = critical −
   768 MiB. Ours: budget − 1 GiB (≈7.8 GiB here). **Not ported:** that is a much lower
   working set, and a too-low cap is precisely what made earlier builds evict and re-decode
   costly ASTC. It needs a same-scene test (`EDEN_NX2_TC_CRITICAL_MIB=5120`,
   `EDEN_NX2_TC_EXPECTED_MIB=4352` reproduce it without rebuilding).
2. Allocator **block slack reserved in the margin, with hysteresis** (rises at once, falls ≤16
   MiB per 30-frame re-read, capped at 1.5 GiB). **Not ported:** our earlier slack subtraction
   self-chased. Hysteresis damps but does not remove that feedback.
3. **Emptiest-block-first eviction.** **Ported** in candidate B (below), ordering only:
   `block_aware_eviction.h`. Among up to 8× the pass budget of already age-eligible images,
   those in the least-filled VMA blocks are offered first, so whole blocks can be freed back
   to the driver. Native-tiling titles on Vulkan only. Eligibility, costly/download rules
   and the per-pass budget are unchanged. Three tests cover order, stability, completeness
   and the no-data fallback. Residency/FPS effect: unmeasured.

### Open PRs re-checked (code read, not claims)

- #310 (buffer readback optimization): built on a read-trap/`AsyncBufferDownloads` base that
  neither c0cb3325 nor this fork has, and its author states it was not built or tested.
  Not ported.
- #124: 128-byte split atomics alignment (micro); TC log gating (once per 600 frames).
  Not ported; negligible.
- #292: holds HLE IPC replies up to 50 µs for a Mario Tennis Fever race. It adds latency and
  is not an MKW perf fix. Not ported.
- #313 (MKW multiplayer/LDN), #301/#304/#306 (other titles' shader forms), #305 (cache version
  bump for upstream's own FSEL fix): not MKW performance.
- Previously integrated #256/#277/#276/#237/#257-part and excluded #157/#28/#250/#288 are
  unchanged (see `MKW_UPSTREAM_AUDIT_2026-11-02.md`).

## Candidates

Both builds use HEAD 5bef7354b4 plus this pass's changes. All experiments are opt-in, per child process.

- **A** `build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-opus-audit/eglantine.exe`
  (61,925,376 bytes), SHA-256
  `5FB1445396F17ADDE9436C252B2EC2EE57EE8E10FE3F27A3CC41FA8DA1CF96FA`. Contains the cache
  coexistence fix, the indirect-count fix, the compute module release and the opt-in fetch
  contract. Clang Release: **493 cases / 19,006,932 assertions passed**, BUILD READY
  (`mkw-opus-audit-build.log`). The assertion total differs from earlier runs because
  several suites are randomized (seeded per run).
- **B** `build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-opus-residency/eglantine.exe`
  (61,936,128 bytes), SHA-256
  `EA5455F5D52B4BC286F6AB62F2766C294F10FD35F769AB6E9A60FD5EEC01C84B`: A plus block-aware
  eviction. **496 cases / 19,118,314 assertions passed**, BUILD READY
  (`mkw-opus-residency-build.log`). Five focused runs (`[pipeline_cache_files]`,
  `[block_aware_eviction]`, `[native_async_draw]`, `[sync_audit]`, `[descriptor_table]`,
  `[memory_budget]`) each passed **26 cases / 44,550 assertions**
  (`mkw-opus-focused-tests.log`). These tests do not run real Vulkan allocation or gameplay.
- The previous `bin-mkw-upstream-pr-cache` executable is unchanged (`F25DEF67…6DCD` rechecked).

Note that the first boot of A or B renames the existing version-39 `vulkan.bin` to
`vulkan.fork-v39.bin`. Older fork builds (pr-cache and earlier) would then start cold once and
recreate a legacy `vulkan.bin`; the official build's own file is no longer deleted by A/B.

The launcher `build-fe-shaders/Run-MKW-opus-audit.ps1 [-Candidate A|B]` starts a candidate with the game.
Switches: `-SyncWaitFetchOrder`, `-ExperimentalAsyncDraws`, `-UnsafePreemptiveReads`. Each
is explicitly set to 0 unless selected, the parent environment is restored, and the
launcher refuses to run beside another emulator. Back up the save before experiments and
do not save progress in them.

Suggested same-scene protocol: boot the candidate once and quit (warms *its own* cache
now that the official build no longer deletes it). Then compare the same course, camera
and settings. Run official c0cb3325, then A default, then A `-SyncWaitFetchOrder`, then B.
Compare the logged `ordered_read`, `command fetch`, `VMA heap 0` and frame windows.
