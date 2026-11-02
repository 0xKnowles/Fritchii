# Mario Kart World loading investigation

## Scope and evidence

The reported failing run used `bin-buffer-reuse-fixes/eglantine.exe` and title
`0400C3F00006E000`. The log contains rejected native graphics programs at stage 1,
including opcodes `0x923` and `0x927`, and unmapped device reads of 69,632 bytes.
The log later reports that the emulation thread failed to stop. These observations
do not, by themselves, establish which operation caused the loading stall.

## Synchronization fixes in this checkout

- Run one pending-channel scheduling pass before returning to the GPU command
  queue. Previously a ready acquire was treated as progress even when dispatch
  could not execute anything; repeated retries could starve other commands.
- Do not attempt to dispatch a disabled or empty DMA queue.
- Refresh the GPU thread's short-poll window after dispatching a resumed channel,
  avoiding an extra millisecond sleep between links of a channel dependency chain.
- Retain a successfully observed acquire condition between the scheduler's
  readiness check and dispatch. Re-reading an overwritten semaphore must not
  undo an acquire that already succeeded.
- Route 32-bit Ampere `SEM_EXECUTE` acquires through the existing suspended-channel
  mechanism, rather than logging an unmet condition and continuing execution.
  Keep legacy and Ampere opcode encodings distinct. Equality, unsigned GE,
  circular GE, AND and NOR conditions share their comparison implementation
  with the GPU-release latch.
- Preserve intermediate matching GPU releases even if a later release overwrites
  the semaphore before the scheduler polls it.

The operation encodings and payload-width flag were checked against NVIDIA's
[AMPERE_CHANNEL_GPFIFO class header](https://raw.githubusercontent.com/NVIDIA/open-gpu-doc/master/classes/host/clc56f.h).
This patch does not add 64-bit payload, reduction or timestamp-release support;
those commands retain the previous path. It does not claim that these scheduling
defects are the proven cause of this particular freeze.

## Shader work still required

The observed instruction words include:

```
ISBERD: ff047923 00000000 00007800 000e2800
ISBEWR: 05000927 00001004 0000b800 0001e200
```

The existing native lowering handles a limited geometry-stage ISBERD case, not
these attribute-space forms. The stage restriction and missing ISBEWR support
already exist at base commit `4285e58d06`; they were not introduced by the latest
uniform-upload reuse optimization.

These instructions address packed inter-stage memory. Mapping their offsets
directly to ordinary vertex attributes, or treating writes as no-ops, is not a
valid general implementation. Mesa documents the
[ISBE layout](https://docs.mesa3d.org/drivers/nvk/isbe_layout.html) and
[task/mesh stage bindings](https://docs.mesa3d.org/drivers/nvk/mesh_shading_notes.html).
Task and mesh programs can occupy the vertex slot. Follow-up captures from the
09:50 run on November 2 include the complete headers and mesh controls: all ten
captured rejections have mesh control `0x1`. These are actual mesh/task programs,
not ordinary vertex attributes. Their missing support remains unfixed.

No shader guards were removed, no shader-cache version changed, and no settings
or game files were modified. The unmapped-read messages remain unexplained;
they are not being suppressed or presented as fixed.

## Follow-up memory fixes and evidence capture

Scalar reads/writes now translate each 4 KiB backing page independently in both
the GPU memory manager and the device memory manager. Previously a scalar at the
end of a page used one host pointer for the whole value, which is wrong when the
next guest page has different physical backing or is unmapped. Address overflow
does not wrap back to page zero. The const typed GPU pointer overload also no
longer recursively instantiates itself with successively nested pointer types.

Block transfers now use the device-memory walker for each mapped GPU-page range.
They no longer assume that a GPU page's cached continuity flag is sufficient to
copy directly through all of its current physical backing. The device walker
retains its small-read fast path and coalesces physically continuous ranges.
These are independently reproducible code defects, not proof that they caused
the specific 69,632-byte reads in the Mario Kart log.

On an ISBE rejection, the follow-up build automatically saves the complete
captured native instruction stream, its 128-byte graphics header and the four
mesh-control registers in `log/native-shader-rejections` under the emulator's
existing data directory. Capture is limited to 16 attempts per process, and
existing files are not overwritten. Files remain local. It neither substitutes
shader instructions nor changes game settings. The subsequent user run supplied
these previously missing program-layout inputs.

The follow-up executable is in `bin-mkw-memory-fixes` within the same build tree.
Its final full regression run passed **406 cases / 17,795,645 assertions**;
the focused page-access and semaphore run passed **8 cases / 311 assertions**.
Final logs: `build-fe-shaders/mkw-memory-final-build.log` and
`build-fe-shaders/mkw-memory-final-build-errors.log`.
Executable SHA-256: `4810A8B0BB99ABA9EF76113A4A56CB2085B7EC6E1C74877A9332966EC7EA6D08`.
No emulator processes were launched or stopped during this follow-up; the user
is testing games independently.

## Validation

The separate test executable is built under
`build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-loading-fixes`.
Regression tests cover predicate decoding, unsigned/circular boundaries, mask
conditions, release latching, address isolation and replacement of an old wait.
The full build passed **402 test cases / 17,714,604 assertions**. Build output is
in `build-fe-shaders/mkw-build.log` and `mkw-build-errors.log`. The assertion count
of the full suite can vary because existing timing/concurrency tests are dynamic.
Runtime Mario Kart loading/gameplay still requires a retest; successful unit
tests and linking do not establish that gameplay is fixed.

## Native texture readback batching

The user subsequently confirmed loading worked after closing a second emulator
instance, but reported low FPS. No emulator was running when this performance
follow-up began. The completed log shows frequent texture downloads, including
one 144 MiB image for a small CPU read, plus repeated synchronous shader
translation. These observations do not quantify the steady-state FPS bottleneck.
In particular, the cache already excludes clean/CPU-modified images: removing
necessary readbacks is not a safe performance fix.

The new native default path batches texture copies before one completion wait,
retaining modification-order writeback and the existing dirty-image selection.
Aggregation is capped at 64 MiB; a larger image gets its own batch. This promotes
the previous opt-in batching path, removes its environment toggle, and uses
tested shared planning/execution helpers. It does not reduce the number of
bytes downloaded and cannot help a readback containing only one image. Sampled
runtime performance is still needed to measure any FPS improvement. Automatic
ten-second summaries record actual images, waits, waits saved, bytes and time.

`bin-mkw-readback-batches/eglantine.exe` built successfully. The full suite passed
409 cases / 17,717,167 assertions; focused batch tests passed 3 cases / 54
assertions. They cover alignment, the aggregation limit, oversized images,
copy/wait/writeback ordering, and empty batches, but are not a Vulkan runtime
benchmark. The previous executable remains untouched. No game/settings changes,
emulator launches or process termination were performed.

## Live low-FPS investigation: memory-budget units

On November 2 the user reported 16 FPS with a single instance of the readback
build (PID 14820). Read-only thread snapshots and the live log were inspected;
the process was neither stopped nor restarted. In this scene the readback
summaries show `waits_saved=0`, with roughly 0.17–0.25 seconds in readback per
ten-second reporting window. Batching is not the principal steady-state fix.

The same log reports VMA heap-0 usage 9180 MiB against a budget of 8994 MiB,
while the texture cache's usage estimate is only 7803 MiB. Code inspection
explains the units mismatch: `AllocatedUsage` subtracts unused VMA block space
from driver usage, but `GetDeviceMemoryBudget` previously kept the full driver
budget. Unused portions of allocated blocks still count against the driver
budget ([VMA statistics](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/struct_vma_statistics.html)).

The follow-up patch translates the main-heap budget to the same allocation-based
units, after the raw heap limit and before adding usage from other heaps. It
retains saturating arithmetic and distinguishes an unavailable budget from a
known exhausted budget. Tests check preservation of headroom/over-budget excess,
additional-heap cancellation, raw limits, and boundary values. This is a verified
accounting defect; actual paging and the share of frame time it causes have not
been measured. GPU utilization snapshots alone do not establish a CPU bottleneck.

The separate `bin-mkw-memory-budget/eglantine.exe` build passed 411 test cases /
17,797,989 assertions. Logs are `mkw-memory-budget-build.log` and
`mkw-memory-budget-build-errors.log` under `build-fe-shaders`. The running
readback-batches executable was left untouched. This candidate's FPS still
requires the same-scene user test; successful regression tests do not measure it.

## Parallel deep-dive follow-up

The next live run was verified as PID 32532 using `bin-mkw-memory-budget`.
Twelve brief thread-context snapshots were saved locally as
`build-fe-shaders/mkw-deep-sample-*.txt`. GPU-front samples included command
processing, split-thread drains, and Windows graphics synchronization waits;
several deeper unwound frames were invalid and are not used as evidence.
The log snapshot is `build-fe-shaders/mkw-deep-live.log`. A startup streaming
window contained thousands of costly texture evictions, but later steady-state
windows had substantially less churn while performance remained low. Neither
readback batching nor cache cleanup alone is established as the main FPS limit.

Three scoped agents audited CPU event waits, GPU command processing, and texture
memory cleanup. Integrated changes:

- Windows event waits retry unrelated hardware-monitor wakes until the signal or
  actual deadline, clamp the timer before narrowing to 32 bits, consume signals
  atomically, and return the correct timeout result. Tests exercise auto-reset,
  unrelated cache-line writes, nonpositive intervals and signalling.
- Cache cleanup credits owned allocations already queued for delayed destruction.
  Previously, the next frame's driver usage counted those resources again and
  could trigger more evictions for the same pending frees. RAII credits end after
  destruction; move/teardown/staggered-ring lifecycle tests check this ordering.
  Immediate destruction and images retained for pending downloads receive no
  delayed credit. This does not discard in-use GPU resources early.
- Automatic MME DMA-read attribution samples one read in 64 and counts all reads
  and flushes. Ten-second summaries distinguish drain, dirty check, flush and
  scalar-read times. These are explicitly sampled totals, not full-window times.
  No barriers/drains were removed, and spin counts were not tuned speculatively.

HostTiming's reported CPU time alone is not proof of useful CPU load: a
hardware-monitor wait can still count as a running thread. No game process was
launched, stopped, restarted or patched in memory during this audit.

Final build: `bin-mkw-deep-perf/eglantine.exe`, SHA-256
`4EB71BCCE602C0A4A69A7E43642148B1A8D2A9C0FE805C13FC55383004D7E2F6`.
Full regression suite passed **418 cases / 17,662,941 assertions**. Focused event,
retirement, split-metrics and memory-budget tests passed **12 cases / 564
assertions**; the event cases additionally passed three repeated runs. Final
review added exception-safe credit transfer and a throwing-move regression.
Build logs: `mkw-deep-perf-final-build.log` and its `-errors.log` companion under
`build-fe-shaders`. Actual in-game FPS for this executable remains unverified.

## Shader compilation and sustained 15 FPS follow-up

The user tested `bin-mkw-deep-perf` and reported approximately 15 FPS. The live
process was verified as PID 13424, with one emulator instance. Its preserved log
is `build-fe-shaders/mkw-shader-perf-baseline.log`.

At 231–339 seconds, the present reports remained near 59–61 ms/frame with almost
no present-fence or vblank wait. No new pipeline translations were reported in
that steady-state interval. Texture readbacks consumed roughly 0.2 seconds per
10-second reporting window; sampled MME reads reported zero flushes and small
drain totals. Texture creation/upload traffic also settled substantially. These
observations do not establish the main frame-time bottleneck, but they do rule
out ongoing shader translation as the explanation for that entire slow period.
Thread snapshots showed both guest CPU waits and GPU command/drain waits; their
optimized/live-stack unwinding was unreliable, so no speculative synchronization
removal or thread-priority change was made from those samples.

Shader-loading changes:

- Coalesce concurrent translation-cache misses by translation key. The waiting
  worker revalidates all recorded environment dependencies before reuse. Different
  keys compile independently; the per-key guard is released before Vulkan driver
  pipeline compilation and on exceptions. Native draw correctness is unchanged.
- Reuse scratch shader arenas per cache-loading worker instead of constructing
  an 8192-instruction arena for each pipeline. The loader owns these arenas and
  frees them after loading, not after the whole emulation session.
- Report existing reuse hits, coalesced misses and translation attempts after
  cache loading. Correct the misleading "texture pool not contiguous" fallback
  description: snapshot eligibility can fail for other reasons too.

The eight-worker cold-miss regression requires exactly one translation build.
Additional tests cover independent keys, map growth while a guard is held,
exception/retry unlocking and rejection of incompatible environment variants.
The optimization's actual loading-time gain has not yet been measured in-game.

Automatic native-title diagnostics now also report per-thread sampled readback
costs and split-drain reasons. These are attribution only, not optimizations;
sample totals are not full-window totals and overlapping scopes are not additive.
They do not enable the full GPU profiler or its shared page-tracking state.
No emulator was launched, stopped, restarted or patched in memory.

Validated candidate: `bin-mkw-shader-perf/eglantine.exe`. Final full regression
suite passed **421 cases / 17,726,308 assertions**. Focused shader-reuse tests
passed twice (**6 cases / 260 assertions** each). Build and test output is in
`build-fe-shaders/mkw-shader-perf-verified-build.log` and its `-errors.log`
companion. The previous executable was preserved. Actual startup-time and FPS
improvements remain unmeasured; the steady-state FPS issue is not resolved yet.

## Command-fetch synchronization follow-up

The user next reported 11 FPS using `bin-mkw-shader-perf` (PID 2368), then closed
the game. The completed log is preserved as
`build-fe-shaders/mkw-command-frontier-baseline.log`. There was one emulator and
no concurrent build. The new cache counters confirmed 102 coalesced translation
misses during startup. Gameplay command-fetch drains, however, spent roughly
350–370 ms per second waiting on the split backend, with tens of thousands of
pending fetches per second. This is a substantial measured cost, not proof that
eliminating it alone would reach 60 FPS. The final unmapped-memory warning was at
the user-triggered stop; it is not established as the cause of low gameplay FPS.

The command-write frontier previously treated every direct macro write and every
multi-method entry as a possible command-memory writer. Native compute marks all
methods executable, including the QMD fields whose CallMethod fast path only
stores registers. The new front-side classification uses that same register-only
predicate for single, macro and multi-method compute writes. For 3D, only
non-executable state writes with no potentially deferred draw are exempted;
initial/unknown state and pending instanced draws remain conservative. The
ordered FinishFrontMacro callback remains a writer before the front clears its
pending-draw flag.

No command or register update is dropped or reordered. The change only shortens
the ring position a command fetch must wait for. Dispatches, draws, uploads,
semaphores, unknown methods, deferred-draw flushes and callbacks retain their
hazards. The historical GPU-written-page check still occurs AFTER synchronization
with queued writers, including a first-ever write to a command page. Existing
backpressure, publication and Vulkan/guest synchronization remain unchanged.

Regression coverage includes all 3,125 five-action state/draw/upload/callback
sequences (checking every prefix), startup-pending draws, the first GPU command
write, and a 4,095-entry inert tail after a dispatch. Full build/tests passed
**423 cases / 17,769,203 assertions**. Build output:
`build-fe-shaders/mkw-command-frontier-build.log` and its `-errors.log` companion.
Candidate: `bin-mkw-command-frontier/eglantine.exe`. The previous builds remain
untouched. In-game FPS and visual correctness of this candidate still require
the same-scene user test; no game was launched for this build.

## Follow-up: completion publication and fence-range merging

The command-frontier run still logged roughly 83 ms per presented frame. About
600 ms/s on the front was spent in split drains; deferred buffer staging requests
totalled about 2.1 GB/s. These byte counts are requested copies, not a measured
transfer duration or newly allocated VRAM. Runtime shader compilation was quiet.
The logs do not yet establish that persistent CPU-read history is unnecessary:
removing that history or its readbacks without verifying read-trap behavior could
break correctness, so this patch preserves it.

The backend previously published completion only at a batch end or after 4096
words. A front waiting for an earlier writer could therefore wait for an unrelated
tail even after that writer completed. A single-producer request hint now asks
the backend to publish its actual completed position when it reaches the target.
The hint does not acknowledge execution: the existing release/acquire completion
counter remains the sole visibility guarantee. Ordinary batching and fallback
publication remain, and the hint resets only after joining the backend thread.

Fence downloads previously subtracted every later accumulated range set from
every earlier one. All their copies read the same current buffer contents at
commit time, so merging their union preserves the exact selected bytes while
avoiding pairwise set walks and fragmented ownership. Dirty-page and byte-range
intersection, async reference counts, copy barriers, and CPU writeback ordering
are unchanged. New tests compare coverage with the previous subtraction algorithm.

Readback diagnostics additionally time fence-download preparation and CPU
writeback (sampled durations, not whole-window totals). This separates those costs
from staging-buffer reservation in the next user test.

The user's settings already enable multicore, fastmem, async shaders, async
presentation, disk/driver shader caches and reactive flushing. No settings were
changed. Force-max-clock was left disabled because its implementation adds a
separate background compute workload, and this run did not establish a clock
bottleneck. No game was launched or interrupted.

Validation: full Release suite passed 428 cases / 17,799,344 assertions. The five
new targeted cases passed ten consecutive runs (60,054 assertions per run).
Build log: `build-fe-shaders/mkw-sync-readback-build.log`.
Executable: `build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-sync-readback/eglantine.exe`.
SHA256: `1E97D3B9E437AD42C40AA7A832CA449CBC4B6B30D95901869B89AF72CF645F61`.
The executable's gameplay/FPS benefit is not yet measured; previous candidates
are retained for a same-scene comparison.

## Three-way audit follow-up: collector feedback and fence correctness

The user reported no sustained FPS improvement and new periodic stutter with
`bin-mkw-sync-readback`. The stopped run is preserved in
`build-fe-shaders/mkw-sync-readback-stutter-baseline.log`. Three Opus 5.5 High
audits inspected synchronization, readback and texture residency independently.
No game was launched for the audits or combined build.

At 161 seconds, the log records 9026 MiB heap usage against an 8862 MiB budget,
heavy collector churn, 1954 MiB of texture uploads in the reporting window and a
1.19-second worst frame. This correlates with the stutter but does not isolate
every stall or establish a same-scene performance comparison.

The earlier adjustment subtracting VMA free block space from both usage and
budget is inappropriate as a feedback controller: freeing an image into a still
allocated block reduces both the measured usage and the next eviction target.
The collector can chase its own holes indefinitely. The combined candidate uses
the driver budget for the target, with a bounded margin, and allocation-adjusted
usage for progress. A fragmented-heap model checks convergence. This intentionally
reverses that part of the earlier accounting patch. Driver residency pressure can
still exceed budget when blocks are fragmented; block-aware compaction remains
unimplemented, so this is not a guarantee against driver paging.

Downloaded collector victims are batched (up to 64 MiB on native-tiling titles,
oversized images singly). All copies complete before CPU writeback and eviction;
no required download is discarded. Legacy paths retain one wait per image.

The buffer audit removed duplicate CPU-read tracking (the same pages are already
marked Preflushable) and its redundant Vulkan lock. Pending copies are now cut
per fence when newer CPU/inline data invalidates their bytes. Merely deleting a
shared range count allowed an old fence to consume a newer fence's count and
write stale data. Tests cover partial splits, multiple fences and random byte
models. Move assignment now returns its reference and moved-from range sets are
explicitly emptied. Retired staging is made reusable at the next commit instead
of waiting for a frame boundary. CPU read-history expiry remains unsafe without
invalidating the per-core cached read areas; it is not enabled.

Semaphore polling now waits for queued memory writers rather than an inert
register-only tail. Command-fetch first-write protection is retained. Backend
idle time, command SYNC flags and newly observed GPU-written-page counts are
reported to distinguish serialization from backend saturation. Main integration
removed proposed speculative command-content and guest-writeback comparisons:
those added reads can race with writers and add copying overhead. No missing
content samples should be interpreted as evidence that skipping waits is safe.

HEAD changed externally to d7ce3d8c08 during the audits, capturing intermediate
edits. The combined build uses the final working tree, including the later
per-fence invalidation fix, not that commit alone. No commit or push was made by
this integration. Remaining known concerns include native storage windows marked
written at coarse granularity, frequent synchronous CPU readbacks, and subsequent
CPU writes within a per-core cached page that bypass the first-write hook.

Integration caught one more issue in the new random readback model: when a
completed interval overlapped any newly queued write, the entire interval stayed
GPU-dirty. Bytes outside the later selection could stay dirty indefinitely.
Writeback now subtracts only the completed bytes not covered by queued writes,
preserving the exact rewritten portions. The first combined suite failed this
check (444/445 passed); its failure is retained in `mkw-audit-combined-build.log`.
The corrected candidate is verified separately in `mkw-audit-combined-retest.log`.

Final validation: 445 cases / 18,872,676 assertions passed. Focused readback,
collector, sync, completion, buffer-readback, memory-budget and batching tests
passed three repeated runs: 36 cases / 1,134,690 assertions each. No gameplay
benchmark was run. Candidate:
`build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-audit-combined/eglantine.exe`.
SHA256: `B4FA75346C1E3F0A286E395FA5D864A020CCF48498543A6F3BC367FC3BD3DBC4`.

### Suspended-channel command wakeup (2026-11-02)

The pending-channel loop previously used an unconditional 1 ms sleep after
its initial 500 us yield period. A newly queued submission or ordered CPU read
could not interrupt that sleep, even though the queue already notifies its
consumer. The loop now uses a stoppable, timed queue pop with the same 1 ms
timeout. Commands wake it early; CPU semaphore stores that enqueue no command
still receive periodic polling. FIFO order, semaphore checks, command-fetch
writer drains and buffer-download synchronization are unchanged. No toggle
or user setting is required.

The queue uses its existing condition-variable mutex and acquire/release
predicate, including a recheck for publication between TryPop and the timed
wait. Cancellation does not consume a queued item. Six new tests cover
prequeued FIFO work, timeout, producer wakeup, cancellation, capacity release
and move-only values. They passed 30 repeated runs (36 assertions each).
The full Release build passed 451 cases / 18,917,005 assertions, recorded in
`build-fe-shaders/mkw-queue-wakeup-build-retry.log`. An earlier wrapper attempt
stopped before compilation because vswhere was missing from PATH; the retry
added the installed Visual Studio Installer directory to that process's PATH.

Candidate: `build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-queue-wakeup/eglantine.exe`.
SHA256: `E403E31E17DCBD5B0582E0150C5664DF27C6FFD9C545BA1D5A55B8AD26B8FB16`.
The prior executable remains intact. Gameplay/FPS effect is unmeasured. This
removes an avoidable scheduling delay, not a proven explanation of all the
low FPS; a same-scene run is still needed. No game was launched for validation.

### Split publication, ownership and readback integration (2026-11-02)

Possible command-memory writers now publish a release-only ring-position hint
when committed below the normal publication threshold. A running back thread
can begin them before the next command fetch drains them. Hints do not advance
the full-publication watermark or replace its wake handshake; an observed
sleeping back gets a full publication immediately. Fetches still drain before
checking GPU-written history, including first writes and non-SYNC entries.
The new publication counters report writer hints, waiting drains, unpublished
target words and sleeping-back observations. Reduced wait time/FPS is unmeasured.

Split shutdown now returns macro ownership to every registered live engine after
joining the back thread. Partial calls and compute macro selection return to the
unsplit path; restart creates fresh register mirrors. Maxwell takeover consumes
held writes on the owning back thread before copying registers, because those
writes can trigger a deferred draw. This addresses a source-level lifecycle bug,
not a proven explanation for steady low FPS before switching modes.

Critical readback and front-thread timings are now collected on every call,
with peak durations and long-wait counts. Front queue waits, pending-channel
polls/yields/timed waits and dispatch are separate events. Durations are inclusive:
dispatch and poll can include Finish, and buffer Finish includes device Finish;
do not sum nested totals. Other hot cache operations remain sampled. This uses
thread-local counters and adds no speculative guest-memory reads.

The integrated split source, Maxwell takeover, GPU thread and new test sources
pass Clang syntax checks. The publication tests also compiled and ran against
the project's real Catch2 libraries: five runs, each 3 cases / 54 assertions,
including an exhaustive 390,625-sequence model and a threaded ring model. These
models do not instantiate the real GpuSplit or verify gameplay. The subsequent
combined project build and engine-handoff tests passed as recorded below.

The readback integration now reuses pending Vulkan fence staging only when every
requested dirty byte is covered by its newest copy, no queued GPU-write selection
overlaps it, and the copy's download-selection epoch is current. It waits for the
copy submission's timeline tick, not an earlier writer tick. Partial coverage,
unknown ticks, rule changes and other uncertainty retain the existing fresh-copy
plus Finish path. OpenGL and immediate-download behavior are unchanged.
`pending_fence_readback` reports exact hit counts, bytes and inclusive wait/copy
time; no benefit is assumed until MKW actually hits this path.

Parent review also fixed the older rule-change retirement defect exposed by the
random model. An old-rule fence still releases its snapshot in order, but cannot
clear GPU-dirty bytes: a later write may have been omitted by the new selection
rule. Its pending-copy count is retired normally. A later current-rule copy or
fresh synchronous download can clear the dirty state. The random model now
requires final guest/GPU equality for all 300 seeds, including those with rule
changes, rather than excluding those seeds. A directed regression covers old-
and current-rule release behavior.

Combined Clang Release validation: **471 cases / 18,956,769 assertions passed**
(`build-fe-shaders/mkw-sync-integrated-build.log`, BUILD READY). Five focused
runs each passed **57 cases / 1,300,234 assertions**, covering handoff, publication,
pending readbacks, timing, queue waits, command frontiers, NX2 sync, buffer
readbacks and fence merging (`mkw-sync-integrated-focused-tests.log`). Both
Vulkan and OpenGL buffer-cache production instantiations compiled.

Candidate:
`build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-sync-integrated/eglantine.exe`
(61,904,384 bytes).
SHA256: `496F5CD9F25E81FAB540228FC4B099E3192D2CA659352B2B7A675BE43FE5188A`.
The previous queue-wakeup executable remains unchanged (its recorded hash was
rechecked). No game was launched, no settings changed, and no gameplay/FPS gain
is claimed. Test the same steady race with this exact binary, without changing
accuracy mid-measurement; the new exact wait and reuse counters distinguish
remaining queue/semaphore stalls from synchronous copy waits.

### Selected correctness fixes 1–4 (2026-11-02)

The user selected these four fixes; additional upstream findings are not included.

1. `Memory::HandleRasterizerWrite` no longer caches permission to collect all
   later writes to the same page. Each nonempty device-address write invokes
   `OnCPUWrite` before collection, cutting that write's bytes from pending fence
   snapshots. The model reproduces the old bypass overwriting a second write on
   the same page, and checks the production helper preserves it. This adds cache
   protection checks/locking, not a full GPU wait; runtime cost is unmeasured.
   It does not establish race freedom for every guest/host memory interaction.
2. Pushbuffer diagnostics no longer reread live command or CPU memory before
   synchronization or while reporting an error. They retain fetched hashes and
   entry metadata. The submit-time check incorrectly used GPU virtual addresses
   as CPU addresses and was removed. The old `EDEN_NX2_PB_DUMP` live-reread format
   is disabled with a warning; existing synchronized fetch capture is the safe
   replacement. Source guard confirmed no `ReadBlockUnsafe`, `ApplicationMemory`
   or `Read<u32>` diagnostic calls remain in dma_pusher.cpp. Normal fetch ordering
   and GPU-written command readbacks are unchanged.
3. SPSC queue slots now contain a live object only while occupied. Pop destroys
   the moved-from object before publishing the free slot; queue teardown destroys
   remaining entries. No extra per-slot flag is needed. Tests use non-default-
   constructible objects whose moves retain source resources, repeated wraps,
   queue teardown and a threaded 10,000-item FIFO sequence.
4. Uniform-buffer hit/shot history shifts use backward copying for rightward
   overlap. Tests cover distinct values, successive shifts, expiry and length one.

Clang Release: **476 cases / 18,987,920 assertions passed**, BUILD READY in
`build-fe-shaders/mkw-correctness-four-build.log`. Ten focused runs each passed
**34 cases / 1,202,740 assertions** in `mkw-correctness-four-focused-tests.log`.
Candidate: `build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-correctness-four/eglantine.exe`
(61,897,728 bytes).
SHA256: `1072517A23A4ED7868A49898E82012D96F89A067897D4E7A1459C494F81AD79E`.
The preceding sync-integrated executable hash is unchanged. No game/settings
changes or gameplay benchmarks were performed. Tests do not certify that every
remaining defect is medium severity or lower.

### Authorized upstream guards and pacing (2026-11-02)

After fixes 1–4, the user authorized direct upstream fixes. Integrated the
FindBuffer starting-page bounds guard (a407703), failed uniform-translation null
binding and empty-stage fast paths (46b2577), title swap interval decoding
(f8d3860), and guest-kernel present sleep (e7973da). No forced-swap toggle was
introduced. Existing root-table, multi-range, command-memory readback and first-
writer protection behavior remains. Details and excluded candidates are in
`MKW_UPSTREAM_AUDIT_2026-11-02.md`.

Final combined Clang Release build: **479 cases / 19,045,652 assertions passed**,
BUILD READY in `build-fe-shaders/mkw-upstream-guards-build.log`. Five focused
runs each passed **37 cases / 1,202,767 assertions**, including both new address
guards, native present timing and the selected fixes, in
`mkw-upstream-guards-focused-tests.log`. The kernel-sleep integration compiles;
unit tests cover timing arithmetic, not live guest scheduling or rendering.

Latest candidate:
`build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-upstream-guards/eglantine.exe`
(61,900,288 bytes).
SHA256: `9F385FE6FB5A612BD243C1D8E63F4F21292B77F582EDE4706F3A6EA0ED7A6D95`.
Prior builds remain intact; sync-integrated hash rechecked unchanged. No game
was launched or settings changed. FPS improvement and gameplay correctness
remain unverified. Upstream removal of command readbacks was deliberately not
ported because its original fix addressed missing MKW course geometry.

### Exact upstream experimental build and PR cache fixes (2026-11-02)

Validated the user's official build as c0cb3325e2 **nx2-experimental**, not the
nx2 revision in the previous comparison. Integrated descriptor geometric growth
and flat binding lookup (#277/#256), lazy descriptor invalidation from that exact
experimental source, gathered-buffer owner retirement (#276), batched shader
invalidation (#237, without bypassing invalidations), and empty storage/texture
binding fast guards from #257. Existing correctness changes remain. See
`MKW_UPSTREAM_AUDIT_2026-11-02.md` for immutable PR heads and excluded patches.

Clang Release build exited 0: **483 cases / 19,048,485 assertions passed**.
Five focused runs each passed **33 cases / 285,333 assertions**. Four new actual
descriptor-bookkeeping tests cover growth, remaps, lazy invalidations and a
50,000-operation eager-reference comparison. Vulkan/OpenGL production code
compiled; no GPU lifetime integration test or live FPS test was performed.

Candidate: `build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-upstream-pr-cache/eglantine.exe`
(61,918,720 bytes).
SHA256: `F25DEF67E1ADE586C24A7C25F0E793BD64A327E11030684A4AE00EC5FFEF6DCD`.
Logs: `mkw-upstream-pr-cache-build.log`, `mkw-upstream-pr-cache-focused-tests.log`.
Previous upstream-guards executable was preserved and its hash verified.

User authorized live testing and unstable experiments. Both computer-use transports
were unavailable, including after retries. No game/settings intervention occurred;
the official process exited on its own during compilation. The independent
`Run-MKW-upstream-pr-cache.ps1 -UnsafePreemptiveReads` launcher selects an existing
unsafe CPU-read diagnostic only for its child. It refuses concurrent emulator
instances and restores the parent environment. Do not save progress in that
experiment. It is not enabled in the normal executable and has not been run here.
The reported upstream 30+ FPS and the FPS effect of this candidate remain unverified.
