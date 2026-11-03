# Bananza timing trace reference

`EDEN_DKB_FRAME_TRACE=1` enables the `DKB frame`, `DKB renderer`, `DKB host_pace`, and
`DKB gpu_frame` records. The value must be exactly `1`. The process caches it on first
use. Unset, empty, `0`, and other values disable these diagnostics. The flag does not
select a title, change a swap interval, bypass a wait, or enable a rendering workaround.

The records use the existing logger at Info level. `DKB frame` uses the Service category.
The Vulkan records use Render_Vulkan. A log filter that excludes those categories also
excludes these records. Existing diagnostics and their switches remain independent.

## Guest presents and pacing

`DKB frame` samples one completed `nx2:vi2` present at most once per second per session.
Times describe that present, not an average over the reporting interval. Requested
timing changes are counted on every present, even between samples and while speed
limiting is disabled.

| Field | Meaning |
| --- | --- |
| `present` | Session-local present counter. |
| `word12` | Raw present command word 12. |
| `requested_interval`, `requested_hz` | Raw high and low 16-bit halves of word 12. |
| `effective_interval`, `effective_hz` | Values selected by the existing pacing calculation. The interval is clamped to 1 through 4 unless overridden. Refresh rates outside 30 through 240 become 60 Hz. These values do not imply that pacing is enabled. |
| `forced_interval` | Parsed `EDEN_NX2_SWAP_INTERVAL`, with zero meaning no override. |
| `speed_limit`, `speed_percent` | Current speed-limit setting and percentage. Disabled speed limiting or zero percent skips guest vblank pacing. |
| `switches` | Number of raw word-12 changes since the preceding trace record. The first request is not a switch. |
| `last_switch` | Most recently observed old and new raw timing words. Zero to zero means none has been observed. This is not a lossless event stream. |
| `cpu_frame_wall_ms` | Wall time between consecutive present-handler entries. The first sample is `-1`. Includes guest execution, scheduling, and waits between entries. It is not CPU utilization or active CPU execution time. |
| `cpu_present_wall_ms` | Wall time from handler entry through completion of guest pacing. Excludes trace logging, the existing statistics log, and reply construction. |
| `fence_wall_ms` | Wall time in the existing current or previous guest-fence wait policy. This is a host wait, not a hardware GPU duration. |
| `composite_wall_ms` | Wall time in the service's `Composite` call. This is not the Vulkan renderer's device execution time. |
| `target_ms` | Requested guest pacing period after interval, refresh, and speed percentage. Zero when guest pacing is bypassed. |
| `sleep_requested_ms` | Duration supplied to the guest-kernel sleep, before its existing 2 ns deadline adjustment. Zero when no sleep occurs. |
| `sleep_actual_wall_ms` | Host steady-clock elapsed time around that guest-kernel sleep call, including descheduling and wake-up latency. Zero when no sleep occurs. |
| `pace_wall_ms` | Wall time for the whole `PaceToVblank` call, including calculation and existing logs. |
| `deadline_reset` | The sampled call discarded its old deadline because it was more than one selected interval late. The first paced present normally resets the uninitialized deadline too. |

A raw request of interval 2 at 60 Hz requests one frame per two refreshes, nominally
30 fps at 100% speed. It establishes the requested cadence, not why the game chose it.
A 33 ms CPU interval with a raw interval-1 request does not establish a game-requested
30 fps cap. Fence waits, CPU work, host presentation, and pacing must be examined
separately. A sampled reset or a long GPU duration alone does not identify a cause.

## Renderer CPU timing and host pacing

`DKB renderer` samples one visible Vulkan `Composite` call at most once per second per
renderer. `renderer_frame` counts visible composites, starting at 1. It is independent
of the service's present counter. Hidden-window composites do not advance it.

`cpu_frame_wall_ms` measures entry-to-entry wall time, with `-1` for the first visible
frame. `cpu_composite_wall_ms` measures that call through `TickFrame`, excluding trace
logging and the final `OnFrameDisplayed` callback. The phase fields divide that interval:

- `capture_wall_ms` covers applet capture and screenshots before frame acquisition.
- `acquire_frame_wall_ms` covers `GetRenderFrame`, including waiting for free frame resources.
- `record_submit_wall_ms` covers compositor command recording and submission. It includes
  generated-frame processing when enabled, and any waits those calls perform.
- `present_call_wall_ms` covers `Present`. With asynchronous presentation this queues work.
  Without it, this can include swapchain acquisition, pacing, and presentation.
- `frame_end_wall_ms` covers the subsequent dispatch, frame-end notification, and `TickFrame`.

These are wall-clock CPU scopes, not active CPU times. In particular, acquisition and
presentation waits are not renderer computation. Asynchronous presentation happens on
another thread and is not measured by `present_call_wall_ms`.

`host_pacing_mode` and `vsync_mode` are the numeric configured settings from
`src/common/settings_enums.h`, not measured display refresh or actual Vulkan present mode.
`DKB host_pace` reports the separate scheduler pacer, at most once per second per calling
thread, when a positive host target FPS is supplied. Auto mode supplies zero and does
not emit this record. Android API 30 and later bypass this host pacer.

`target_fps` is the host target, not the guest request. `enabled` is the speed-limit
setting. `may_flush` distinguishes the recording-thread path from the asynchronous
presentation path. `cpu_wait_wall_ms` covers the pre-pacing submission and GPU-tick wait.
`pace_requested_ms` is the remaining time to the host pacing deadline.
`sleep_requested_ms` is the host `sleep_for` portion, excluding the existing spin tail.
`sleep_actual_wall_ms` measures that sleep. `pace_actual_wall_ms` measures all pacing,
including the yield loop and scheduling overhead. Disabled pacing has zero requested
sleep and only the small elapsed cost of checking the setting.

## GPU timestamps grouped by renderer frame

`DKB gpu_frame` reports a sampled frame's GPU timeline span and sum of GPU submission
intervals, not CPU elapsed time. It uses Vulkan timestamp queries on the scheduler's graphics queue.
Each interval begins at TOP_OF_PIPE in the submission's upload command buffer and ends
at BOTTOM_OF_PIPE in its main command buffer. The sum includes uploads, guest commands,
and compositor work submitted through this scheduler between two renderer boundaries.

A renderer boundary is the final flush for a visible `Composite`, before its `Present`
call. Frame 1 is not sampled because its preceding boundary is unknown. Each later sample
belongs to the `renderer_frame` named in the record. Frame generation's intermediate
flushes belong to the same renderer frame. Separate present-manager command buffers,
presentation, and scanout are outside these GPU intervals.

| Field | Meaning |
| --- | --- |
| `renderer_frame` | Frame whose scheduler submissions were sampled. CPU and service sampling are independent, so a CPU record for this exact frame is not guaranteed. |
| `last_submission_tick` | Scheduler completion tick of the last measured submission. |
| `gpu_frame_span_ms` | GPU timestamp difference from the first submission's begin to the final submission's end. `-1` means the submission cap truncated the sample, so a full-frame span is unavailable. |
| `gpu_submission_sum_ms` | Sum of the available timestamp differences, multiplied by the device's nanoseconds-per-tick value. |
| `measured_submissions` | Number of submission intervals in the sum. |
| `dropped_submissions` | Submissions omitted because this frame exceeded the fixed 512-pair capacity. A nonzero value means the frame's sum is incomplete. |
| `valid_bits`, `period_ns` | Queue-family timestamp width and physical-device timestamp period used for conversion. |

The span includes gaps between submissions, queue dependencies, and stalls within that
GPU timeline interval. It does not include presentation or scanout. The sum omits gaps
between submissions, but its intervals can overlap because submissions can execute in
a pipeline. Those intervals can also include dependencies or stalls. Neither measurement
is GPU utilization. Do not subtract either value from CPU frame wall time to derive CPU
work. Timestamp differences account for the queue's valid-bit wrap, but
intervals longer than a full hardware-counter wrap cannot be disambiguated.

Only one sampled frame can be outstanding. The next sample starts at a renderer boundary
at least one second after the preceding result is reported. A slow or stalled queue skips
later frames rather than waiting for diagnostic results. The worker checks the existing
completion tick, then reads results with 64-bit values and availability bits, without
`VK_QUERY_RESULT_WAIT_BIT`. Unavailable results stay pending. Queries are reset only
after their prior results have been read and their submission has completed. The worker
is drained and joined at teardown, and the last query-using submission completes before
the query pool is destroyed. No extra runtime flush or GPU wait is added for timestamps.

Unsupported timestamps or query-pool allocation failure produce one unavailable message.
A query-read error disables further GPU timing and logs the error once. Missing GPU
records are not zero-duration frames. Startup, shutdown, a hidden window, an incomplete
frame, unavailable results, or an unsupported device can leave no completed sample.

## Existing controls

- `EDEN_NX2_SWAP_INTERVAL=<n>` forces a nonzero guest interval. It is not a trace switch
  and can change emulation speed. Zero leaves the guest interval selection in effect.
- `EDEN_NX2_FRAMES_AHEAD` defaults to enabled. A value starting with `0` restores waiting
  for the current frame. The default waits for the previous frame and also waits for
  the current one when the previous fence is behind. Trace logging does not change it.
- `EDEN_NX2_GPU_PROFILE`, enabled by presence even with value `0`, reports host GPU-thread
  zones and scheduler counters every 10 seconds. Its milliseconds are CPU-thread wall
  durations, not hardware GPU timestamps.
- The existing swap-word change logs and 600-present summaries remain unchanged.
  Their rate is separate from the opt-in trace's one-second limits.

No game run is part of the instrumentation's automated checks. The helper tests cover
rate limiting and valid-bit timestamp subtraction, including wrap. Device-specific
runtime validation and a user's trace are still required before attributing Bananza's
30 fps behavior to the game, pacing, CPU work, or GPU work.

The focused Catch2 filter is `[dkb_frame_trace]`. The flag test also runs in separate
processes with the variable unset and with values empty, `0`, `1`, `10`, and `true`,
because each process caches the flag on first use.
