# Mario Kart World: upstream audit and integration

## Reference and limits

Official source: [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine),
branch `nx2`, commit `a407703f6d29a5ab73328b4ededb12cf29b2758d` (build129).
The fork README identifies this upstream. A depth-60 filtered reference clone is
at `build-fe-shaders/official-eglantine-audit-20261102`. No upstream code was
executed, no submodules installed, and no game launched. A Windows filename case
collision concerns the two differently capitalized macOS icon files, not audited
source. Local analysis includes dirty working-tree fixes, not just HEAD.

There is no matched benchmark proving official Eglantine is faster on this PC.
Upstream reports measurements on different hardware and workloads. The findings
below identify code differences and defects, not a measured FPS attribution.

## Why official may spend less time synchronizing

Official [511d19f](https://github.com/Djihads80/Eglantine/commit/511d19f5de08102428a2841b72df7cfe0b6f6c5a)
reverted GPU-written command-list readback. Its fast fetch parses guest command
memory without our `FlushGpuWrittenCommands` drain/history/dirty-download path.
This is a concrete difference in the path associated with our measured waits.

It is also a correctness tradeoff: the original
[d151e53](https://github.com/Djihads80/Eglantine/commit/d151e53e6148faeca1b5754e8b16f20da72c8252)
described stale MKW GPU-generated command records omitting draws and course trees.
The revert does not explain how that problem was otherwise solved. This integration
does **not** remove our ordering or dirty-command downloads. A faster frame missing
draws would not establish a successful optimization.

Official also skips ordered CPU reads for preemptive download areas where our
default path retains ordering. Our unsafe alternative remains disabled. Both
differences predate the latest four selected fixes; baseline `4285e58d06` already
contains fork-specific correctness work, not pristine official source.

## Applied after explicit upstream authorization

- [a407703](https://github.com/Djihads80/Eglantine/commit/a407703f6d29a5ab73328b4ededb12cf29b2758d):
  reject an out-of-table starting device page before `FindBuffer` indexes the
  page table. Upstream associates the missing check with an MKW invalid uniform
  address crash. This guards the crash site, not the origin of a bad address or
  every other buffer-range operation.
- [46b2577](https://github.com/Djihads80/Eglantine/commit/46b2577bf78870ed85cbb790a9dc3c82cc77f4a7):
  failed graphics-uniform address translation produces a null binding instead
  of dereferencing an empty optional. Empty uniform/storage/texture stages exit
  their update functions immediately. Local root-table and multi-range behavior
  is preserved. The two correctness guards have directly used, testable helpers.
- [f8d3860](https://github.com/Djihads80/Eglantine/commit/f8d38607b6cf6e35ee3daad7e34e590e19421c79):
  presentation honors word 12's swap interval and refresh rate, retaining upstream
  validation/clamping. No new forced-interval environment toggle was introduced.
  Tests cover native single/double intervals, refresh rates, invalid metadata and
  speed-limit scaling.
- [e7973da](https://github.com/Djihads80/Eglantine/commit/e7973dae7e2ea44665ff4ec83d7214a5d2daa366):
  present pacing sleeps the guest service thread through the guest kernel, allowing
  other threads assigned to that core to run. It no longer sleeps the entire host
  thread during the pacing interval. Kernel timer units were checked locally.
  This addresses loader starvation and pacing, not slow MKW windows with no pacing
  wait. Live service scheduling still needs game testing.

## Not blindly imported

The native-title ASTC path already uses Flush rather than Finish, so this is not
an omitted upstream MKW optimization. The Vulkan scheduler does not contain a
fundamentally faster submission algorithm upstream; caller frequency matters.
Linux hole-punching and AMD-specific changes do not directly target this Windows/
Nvidia system. Long-unused-costly-texture eviction and broader allocation changes
need separate residency/lifetime validation before incorporation. They have not
been silently mixed into this build.

Both official and this fork use `Shader::Ampere::LowerToMaxwell`. The supplied
Ryujinx screenshot describes a different direct-SM70+-to-IR architecture, not a
feature official Eglantine has that this fork lacks. Replacing our shader frontend
would be separate work and would not automatically eliminate synchronization.

## Exact experimental-build comparison and open PR integration (second pass)

The user's running official binary was validated as PID 38976, started at
15:56:22 local time, from
`D:\Downloads\eglantine-windows-x86_64-c0cb3325e22756edb65c87d33ef298349ade0ad3\eglantine.exe`.
Its title identifies **c0cb3325e2-nx2-experimental / MSVC 19.44**. Its SHA-256 is
`8352A9E6932D70814B8936469CA04841E9B98DCC2FB92898727BF1524575E9A0`.
This is not the nx2 a407703 revision in the first-pass comparison: the experimental
merge differs in 155 files. The user reports 30+ FPS; this has not been measured
independently. The AppData eden log still identifies our previous d7ce3d8c08 run,
so it is not used as evidence of the official process's performance.

Inspected open-PR patches through GitHub's API on 2026-11-02. Integrated:

- [#256](https://github.com/Djihads80/Eglantine/pull/256), head
  `581fdee0ad593f757d988a18b1abebce474211ce`: per-table flat binding IDs instead
  of per-channel hash lookups; retained our image generation/feedback handling.
- [#277](https://github.com/Djihads80/Eglantine/pull/277), head
  `cac42ff5158adf9172ba4fb04517a16331672884`: geometric descriptor allocation
  rather than a 524,289-entry minimum. Combined with c0cb3325's lazy invalidation:
  repeated image evictions set a flag, cleared once on the next descriptor read.
  Guest descriptor contents are still read on every lookup. Existing capacity is
  retained on shrink; growing/remapping invalidates all read marks.
- [#276](https://github.com/Djihads80/Eglantine/pull/276), head
  `30583fc85cd3f53caf7f861d80b3d82219f51948`: gathered buffers, not just sparse
  bindings, retain source-owner IDs. Source removal retires both forms at a GPU
  tick; completed retired entries are reclaimed each frame without an extra wait.
- [#237](https://github.com/Djihads80/Eglantine/pull/237), head
  `9dd46737e8330f1103f58054f5fca3aabfd3b672`: batch shader invalidation under one
  mutex, with one pending-removal pass. Did NOT import the optional skip-invalidation
  setting; every supplied range is still invalidated.
- Buffer-binding portion of [#257](https://github.com/Djihads80/Eglantine/pull/257),
  head `aaf28a7578aedbe2ed622a839857c75fac2be5c8`: avoid constructing storage-binding
  sync bookkeeping when no storage/texture buffers are enabled. Uniform binding and
  post-copy barriers remain unconditional as before. Pipeline-key comparison is unchanged.

Four new `[descriptor_table]` cases exercise the actual production bookkeeping:
geometric allocation, repeated lazy invalidation, changed guest descriptors,
per-table binding isolation, remap/shrink/regrowth, and 50,000 randomized operations
against an eager-invalidation reference. They do not test real Vulkan lifetime or FPS.

Not imported blindly:

- [#157](https://github.com/Djihads80/Eglantine/pull/157) caches raw guest-memory
  pointers without an accompanying remap-invalidation hook in its patch.
- [#28](https://github.com/Djihads80/Eglantine/pull/28) snapshots command words at
  submit; c0cb3325 then skips the flushing path for snapshots on pages not already
  recorded as GPU-written. Our fetch-before-first-write ordering proof cannot be
  removed just because another build renders faster. Snapshot coherency and queued
  host-side writers require additional synchronization design.
- #288's NVIDIA vertex-repack bypass does not apply: our tree has no such repacker.
- #250's lower collector thresholds and the experimental allocator-slack budget
  subtraction are not folded in: earlier local evidence showed eviction feedback
  and thrashing. These need isolated residency tests, not a blanket cherry-pick.

Both computer-use transports failed with `native pipe is unavailable` (including
retries after the user reported recovery). No live game input, settings changes,
termination, or launch occurred. Build runs at two jobs while the official process
remains open; performance observations during compilation are not valid A/B results.
This is a bounded PR-fix candidate, not proof that the main FPS gap is resolved or
that all open PRs have been audited/integrated. Validation is recorded below when complete.

The user also explicitly requested unstable experiments. The separate launcher
`build-fe-shaders/Run-MKW-upstream-pr-cache.ps1 -UnsafePreemptiveReads` selects the
already-existing `EDEN_NX2_UNSAFE_PREEMPTIVE_READS=1` CPU-read bypass for one child
process. Without the argument it explicitly selects ordered reads. It refuses to
launch while another emulator runs, restores the parent environment and does not
change saved settings. The unsafe run may observe stale GPU results: it is only an
A/B diagnostic, not a correctness fix, and should not be used to save progress.
It does not disable command-fetch drains. Neither variant was launched here.

Validation completed: Clang Release build exited 0, `BUILD READY`;
**483 cases / 19,048,485 assertions passed**. Five focused runs each passed
**33 cases / 285,333 assertions**, covering descriptors, readback/fence handling,
sync audit, publication ordering and present timing. Both Vulkan and OpenGL
production paths compiled. These tests do not execute Vulkan buffer allocations
or establish gameplay correctness. Launcher syntax was parsed, not executed.

Executable: `build-fe-shaders/emulator-nx2-4285e58d/bin-mkw-upstream-pr-cache/eglantine.exe`
(61,918,720 bytes); SHA-256
`F25DEF67E1ADE586C24A7C25F0E793BD64A327E11030684A4AE00EC5FFEF6DCD`.
Logs: `mkw-upstream-pr-cache-build.log` and
`mkw-upstream-pr-cache-focused-tests.log` under `build-fe-shaders`.
Previous `bin-mkw-upstream-guards/eglantine.exe` hash rechecked unchanged.
The official process exited during compilation without an action by this agent.

## Native frame-rate research

Nintendo's programming director describes a **60 FPS target** for MKW in its
[developer interview](https://www.nintendo.com/au/news-and-articles/ask-the-developer-vol-18-mario-kart-world-chapter-1/).
The system's 120 FPS capability applies only to compatible software, per the
[Switch 2 FAQ](https://www.nintendo.com/au/hardware/nintendo-switch-2/frequently-asked-questions/).
The [1.8.0 update history](https://www.nintendo.com/en-gb/Support/Nintendo-Switch-2/Game-Updates/How-to-Update-Mario-Kart-World-2842542.html)
does not announce a 120 FPS mode. That is not proof against every hidden mode,
but native 120 FPS is not an evidence-backed target here.

Read-only local inspection found the supplied update at
`D:\Downloads\mkw 1.8.0 update\updates\917504`, already containing `exefs` and
`romfs`. Rendering-related filenames are not proof of algorithms, frame caps or
specific optimizations. No package was extracted or decrypted, and no plaintext
FPS-unlock configuration was found in the bounded metadata inspection.

## Validation and next measurement

Fixes 1–4 passed 476 test cases before upstream integration; the final combined
build result and executable hash are recorded in `MARIO_KART_WORLD_LOADING.md`.
No FPS improvement, visual correctness, or absence of all high-severity issues
is certified by unit tests.

Compare the same course, save, camera, accuracy, resolution, texture settings and
warm-cache state. Inspect rendering completeness alongside exact command-drain,
readback, pending-fence reuse, front-idle and presentation timings. Do not infer
speed gains by combining sampled estimates or nested inclusive timing totals.

## Third pass

See `MKW_OPUS_AUDIT_2026-11-02.md`: shared pipeline-cache deletion between official and fork
builds (fixed), ordered CPU reads and command-fetch drains as the main measured costs, and
candidates A/B with opt-in experiments.
