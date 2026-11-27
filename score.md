# x264++ scoreboard (clean-room)

**Goal:** >= 2.00x the encode throughput of the black-box reference (`oracle/x264.exe`, x264 API build 164) on every preset.

This scoreboard is owned by the independent certification team (`cert/`) and is populated only by reproducible measurements. Earlier numbers from the discarded fork iteration were withdrawn when the project was reset to a clean-room implementation.

## Current score

_No certified implementation increments yet._

| Preset | Reference (fps) | x264++ (fps) | Speedup | Valid H.264 | Size delta | Notes |
| --- | ---: | ---: | ---: | --- | ---: | --- |
| ultrafast | — | — | — | — | — | — |
| superfast | — | — | — | — | — | — |
| veryfast | — | — | — | — | — | — |
| faster | — | — | — | — | — | — |
| fast | — | — | — | — | — | — |
| medium | — | — | — | — | — | — |
| slow | — | — | — | — | — | — |
| slower | — | — | — | — | — | — |
| veryslow | — | — | — | — | — | — |

## Methodology (summary)

- Reference: `oracle/x264.exe`, used only as a black-box behavioural and speed oracle.
- Candidate: clean-room binary built from `src/`, never from reference source.
- Inputs: deterministic clips plus random cases drawn from `spec/usage-model.md`.
- Timing: warm-up + interleaved repetitions, median wall clock; speedup = reference_median / candidate_median.
- Conformance: decoder acceptance + observable behaviour checks from `spec/`.
- Full methodology and machine-readable results: `cert/`.

## Progress log

| Date | Event |
| --- | --- |
| 2026-11-27 | Repository created (initially as a fork — discarded) |
| 2026-11-27 | Reset to clean-room process; reference source purged; only black-box oracle retained |
