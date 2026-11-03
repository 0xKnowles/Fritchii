# AGENTS

See `docs/Coding.md` for technical coding guidance.

Keep comments to a reasonable level. Do not overexplain or add/remove comments from unrelated parts of the codebase.

## Game bug reports

Users report game bugs with Lichii logs (`eden_log.txt`, `eden_log.txt.old`). Each `Booting game: <program id>` line starts a session; the build is on the first line (`Lichii Version: ... | nx2-<sha>`). Known program ids:

| Program id | Game |
|---|---|
| 040074A01BF12000 | Donkey Kong Bananza |
| 040077201BE50000 | Kirby Air Riders |
| 04007C6028AA8000 | Yoshi and the Mysterious Book (demo) |
| 0400D4201C6EA000 | Mario Tennis Fever |
| 0400E88025B1A000 | Star Fox |

Add ids here when a report identifies a new game.

## Before writing a fix

Lichii is a fork of [Eglantine](https://github.com/Djihads80/Eglantine). Do not guess, and do not rewrite a fix upstream already has.

1. `git remote add upstream https://github.com/Djihads80/Eglantine.git && git fetch upstream`.
2. Check `upstream/nx2`, open and closed PRs, unmerged branches, and issues for the game and symptom (`gh pr list -R Djihads80/Eglantine --state all`, `gh issue list -R Djihads80/Eglantine --state all`, `gh pr diff <n>`). Read the diffs, not only the titles.
3. If nx2 is behind `upstream/nx2`, merge upstream first. Port an existing upstream fix before writing a new one, and cite its PR or commit.
4. Write a new fix only when nothing upstream addresses the root cause the log shows.

## How work is split

- One subagent per game, all running in parallel.
- Small research (upstream PR and issue surveys, log reading): a cheap model, DeepSeek V4.1 Flash at max reasoning when its endpoint is available.
- Hard fixes (emulation, shaders, GPU): GPT-6 Astra at high reasoning.
- The main thread (Opus) orchestrates and reviews.

## Builds

- Cloud machines build with clang and Ninja for compile and unit-test checks.
- The build the owner tests runs on the owner's Windows PC (DESKTOP-4UD89UT, Ryzen 9 5900X), built with clang using 12 jobs (`cmake --build <dir> --parallel 12`).
- Every fix gets its own debug build on that PC in `C:\Users\Zortos\.capy\work\lichii-debug\<build>\` and a desktop shortcut `Lichii Debug - <Game> - <fix>` that launches it with the owner's existing user data and keeps the log next to the build, so the owner can run it and send the log back.
