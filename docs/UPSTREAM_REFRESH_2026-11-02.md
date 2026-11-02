# Latest-upstream rebuild and selective ports

First build-and-test checkpoint complete. This is not a claim that the complete fork has been migrated or that FPS is fixed. The remaining ports are listed below; same-scene gameplay comparison is still needed before adding them.

## Pinned sources

- Official `Djihads80/Eglantine`, `nx2`: `75bdb925dfb77db2370cb027eda81352c9622bbb`.
- This includes `c0cb3325e22756edb65c87d33ef298349ade0ad3` through merge #291.
- Existing fork reference: `5bef7354b464a38527864756814cc6e0f0fc6567`, plus the uncommitted Opus audit edits.
- Isolated checkout: `build-fe-shaders/upstream-refresh-20261102`.
- Separate build directory: `build-fe-shaders/upstream-refresh-build`.
- Reproducible local driver: `build-fe-shaders/Build-Upstream-Refresh.ps1`.

No commits, pushes, PRs, game launches, configuration changes or save changes are part of this migration so far. The previous checkout and candidate binaries remain available.

## Baseline

Clang 22 Release, 12 jobs, existing static Qt/dependency locations. The local Sirit source requires `SIRIT_USE_SYSTEM_SPIRV_HEADERS=ON`; Windows PowerShell must not treat native-tool warnings on stderr as a failed compilation.

Upstream has Windows min/max macro collisions in `vertex_repack.h` and five frame-generation translation units. The baseline includes only parenthesized standard-library min/max calls to resolve these compilation errors, with no algorithm changes. The macOS icon case collision on this case-insensitive filesystem is a checkout artifact, not a Windows build input.

## Port decisions

### Retain upstream instead of replacing it

- CPU-read preflush fast path and command-fetch synchronization.
- Texture residency policies, newer mesh/geometry support and extended uniform-register handling.
- VI2 commands 104-106 already have typed display replies; root tables are already implemented. Do not replace the whole service or renderer with the older fork files.
- Newer font service layout, temporal upscaling and vertex repacking.

### First reviewed correctness batch (compiled and tested)

- SPSC queued-object lifetime and destruction; tested timed-wait API, without changing GPU-thread scheduling.
- Range-set move assignment return value, source clearing and self-assignment.
- Device scalar accesses across fragmented physical pages; unmapped-page walking; process-owner cache callback tracking.
- AVX-512 16-bit vector CLZ lane packing.
- Overlapping uniform-cache history shifts.
- X8_D24/color alias conversion, depth descriptor formats and separate array-layer copies.
- Packed-half HSET2 shader comparisons, adapted into the newer upstream lowerer without replacing its register allocation or other instructions.
- Independent Vulkan/OpenGL version-40 shader-cache filenames, including the newer used-pipeline sidecar. Official version-18 and old-fork version-39 files are not adopted, overwritten or deleted by this candidate. The first run starts with a cold cache.

The test harness also carries the existing fixes for resetting shared timing-test flags, binding the fake UDP server to loopback, and making the memory-tracker fake actually apply batched callbacks. These do not change production timing or network behavior.

### Not carried over wholesale

- Always-ordered guest CPU reads, additional per-fetch writer drains and their profiling overhead.
- Unsafe preemptive-read, async-draw omission and SYNC_WAIT-only fetch experiments.
- Old allocator-budget arithmetic, collector thresholds and sparse-resource redesigns.
- IPC startup-delay heuristics: need separate game-specific verification against the newer upstream.

### Remaining game-fix review

Follow-up: the observed Mario Tennis `main+0xde0` startup fault now has a separately built,
title-scoped compatibility candidate in `bin-tennis`. See
[Mario Tennis startup port](MARIO_TENNIS_UPSTREAM_STARTUP_2026-11-02.md). Its policy tests
pass; repeated gameplay boots remain unverified. The first `bin-ported` candidate is preserved.

The Fire Emblem shader work is not a drop-in patch: upstream's newer lowering has different uniform-register allocation and newer instructions. Compact software HMMA, guarded trap handling, bindless provenance and other fork-only opcode extensions need individual ports and regression tests; copying the old lowerer would remove newer upstream functionality. Fence snapshot retirement and split-mode ownership fixes likewise require adaptation to upstream's ownership model. These are not marked migrated merely because the upstream build succeeds.

## Validation

Both executables compiled with Clang 22.1.8, Release, 12 jobs. They link static Qt and import only Windows/MSVC runtime DLLs according to `llvm-readobj --coff-imports`.

### Preserved upstream baseline

- Executable: `build-fe-shaders/upstream-refresh-build/bin-baseline/eglantine.exe`.
- SHA-256: `76569FCE271E9CAD4E7A1359A230A07E90283084F7BD6558F3956914F5EF7865`.
- Only the Windows min/max syntax fixes described above are included.
- The initial full test run crashed with `0xC0000005`. Running `DeviceMemoryManager: UpdatePagesCachedBatch basic` alone reproduced that crash. The ported production null-owner guards fix this test's crash.
- An independent `[nup]` run passed five cases and failed the reproducibility-hash case. The baseline also showed the timing-test shared-state and fake-UDP-server issues before the test-harness ports.

### Ported candidate

- Executable: `build-fe-shaders/upstream-refresh-build/bin-ported/eglantine.exe`.
- SHA-256: `48684D15E86D168933CCB9FAC236A6EEBB211A6B6EC68B861C66227E384B0253`.
- Full regression run completed: 117 cases, 116 passed and one failed; 3,883,208 of 3,883,209 assertions passed. The sole failure is the same NUP reproducibility hash mismatch as the baseline.
- Explicit second run excluding only `NUP output is reproducible`: 116 cases / 3,905,055 assertions passed. This exclusion is recorded, not treated as a green full suite.
- Focused ports plus existing Ampere tests and the formerly crashing device-memory test: 33 cases / 25,510 assertions passed.
- Assertion counts can vary because existing suites are randomized.
- AVX-512 CLZ code compiled, but this Ryzen 5900X cannot execute that instruction path. No AVX-512 runtime-validation claim.

The NUP case expects `df8446903ab2c39a51af0304935eb4aa5d8ead105440976ceae7ab97133f6143` and both candidates produce `718231e0974ca04c7be7bd6ed4334015b777c6e5f1ed51de23f899f81ee004ab`. Its root cause is not established. The expected hash and production archive writer were deliberately left unchanged; do not infer archive reproducibility from this build. The build driver correctly returns failure for the full-suite failure even though both executables were linked successfully.

Logs: `build-fe-shaders/upstream-refresh-baseline-build-resumed2.log`, `upstream-refresh-baseline-tests-diagnostic.log`, `upstream-refresh-ported-build.log`, `upstream-refresh-ported-excluding-known-nup.log`, and `upstream-refresh-ported-focused.log`.

No runtime FPS comparison has been measured. Both computer-use connections fail with `Computer Use native pipe is unavailable`; no game was launched. Test the ported executable directly on the same course and settings, let its independent cache warm, then compare a repeat run against the preserved baseline. Do not run both emulator instances at once. The next port group remains pending this comparison; this candidate does not yet contain all Fire Emblem or old-fork synchronization fixes.
