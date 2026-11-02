# Mario Tennis Fever startup compatibility port

## Observed failure

The user's latest-upstream first-port candidate (`75bdb925df`, executable SHA-256
`48684D15E86D168933CCB9FAC236A6EEBB211A6B6EC68B861C66227E384B0253`) booted title
`0400D4201C6EA000` from the MarioTennisFever_v1.1.1 update directory, but remained on loading.
The log loaded `main` at `0x80d09000`, then read address zero at PC `0x80d09de0`
(`main+0xde0`) at elapsed time 330.197 seconds. Reads/writes of addresses `0xc`, `0x2`
and other small invalid addresses followed. This is the startup signature described in
<https://github.com/Djihads80/Eglantine/pull/292>, not evidence of a slow shader compiler.

The older local fork's commit `3140648a66` contained a worker-start/IPC grace workaround.
It had deliberately not been copied into the first upstream candidate because it changes
scheduling. The matching failure now provides a concrete reason to adapt it for this title.

## Change

This candidate retains upstream GPU synchronization and all first-batch correctness ports.
Only Mario Tennis Fever records newly started workers that have lower priority than the
caller and are assigned to the same core. After a successful synchronous IPC request, the
caller gives these workers guest sleep slices until they reach a non-mutex wait, terminate,
or exhaust a shared 20 ms host-time budget. A mutex wait can still be SDK startup work,
so it does not by itself prove the worker consumed the stack-backed arguments.

The sleep slice is 50,000 ns in the existing kernel timer units. The budget is checked between
sleeps; host scheduling can overshoot it, so this is not a hard real-time bound. This is a
bounded compatibility workaround, not a proof of complete hardware IPC timing emulation.

- State, priority, core and mutex observations use the scheduler lock.
- Sleeping and releasing references happen outside that lock.
- Pending workers have owning kernel references. References are released after the grace
  period, on interruption, and when the starting thread finalizes without reaching IPC.
- Failed IPC requests retain the cohort for a later successful request or finalization.
- Termination stops the wait; subsequent IPC calls have no grace period unless another
  qualifying worker was started.
- No environment variable or user toggle is required. Other titles never record workers.
- No GPU fence, command-fetch, readback or shader-cache changes were made in this round.

The log line `MTF startup worker grace` reports workers, sleep slices and budget exhaustion.
If the budget expires and the same `main+0xde0` fault remains, this workaround is not
sufficient; do not treat disappearance of a loading animation alone as confirmation.

## Validation status

Clang 22.1.8 Release compilation completed with 12 jobs. The executable is
`build-fe-shaders/upstream-refresh-build/bin-tennis/eglantine.exe`, SHA-256
`81A9C964911866679F45B4DE76AB102BAC10882F172210355EF1731BDC8FAF7A`.
The prior `bin-ported` executable's hash remains unchanged.

- Startup policy: 3 cases / 17 assertions passed.
- Full regression: 120 cases, 119 passed and the known upstream NUP archive-hash case failed;
  3,909,045 of 3,909,046 assertions passed. No new failure appeared.
- Explicit rerun excluding only `NUP output is reproducible`: 119 cases / 3,920,566
  assertions passed. This is not a green full-suite claim.
- `git diff --check` passed.

The policy tests check title/core/priority selection, the mutex/termination decision and
the shared budget. They exercise the production policy but do not emulate real guest
scheduling or prove successful gameplay. The build driver retains a failure exit code for
the NUP test even though compilation and linking succeeded.

Logs are `build-fe-shaders/upstream-refresh-tennis-build.log`,
`build-fe-shaders/upstream-refresh-tennis-focused.log`, and
`build-fe-shaders/upstream-refresh-tennis-excluding-known-nup.log`.

Computer-use initialization was retried in this turn. It still fails with `Computer Use
native pipe is unavailable: failed to connect native pipe: The system cannot find the file
specified. (os error 2)`. No game has been launched or stopped, no settings changed, and no
save or shader cache deleted during this work. Repeated user boots are required to validate
the runtime effect.
