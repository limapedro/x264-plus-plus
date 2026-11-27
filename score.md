# x264++ scoreboard (clean-room)

**Goal:** >= 2.00x the encode throughput of the black-box reference (`oracle/x264.exe`, x264 API build 164) on every preset.

This scoreboard is owned by the independent certification team (`cert/`) and is populated only by reproducible measurements. Earlier numbers from the discarded fork iteration were withdrawn when the project was reset to a clean-room implementation.

## Current score

_No certified implementation increments yet._

| Preset | Reference (fps) | x264++ (fps) | Speedup | Valid H.264 | Size delta | Notes |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| ultrafast | — | — | — | — | — | Not certified yet — no candidate binary |
| superfast | — | — | — | — | — | Not certified yet — no candidate binary |
| veryfast | — | — | — | — | — | Not certified yet — no candidate binary |
| faster | — | — | — | — | — | Not certified yet — no candidate binary |
| fast | — | — | — | — | — | Not certified yet — no candidate binary |
| medium | — | — | — | — | — | Not certified yet — no candidate binary |
| slow | — | — | — | — | — | Not certified yet — no candidate binary |
| slower | — | — | — | — | — | Not certified yet — no candidate binary |
| veryslow | — | — | — | — | — | Not certified yet — no candidate binary |

## Methodology (summary)

- Reference: `oracle/x264.exe`, used only as a black-box behavioural and speed oracle.
- Candidate: clean-room binary built from `src/`, never from reference source.
- Inputs: deterministic clips plus random cases drawn from `spec/usage-model.md`.
- Timing: warm-up + interleaved repetitions, median wall clock; speedup = reference_median / candidate_median.
- Conformance: decoder acceptance + observable behaviour checks from `spec/`.
- Full methodology and machine-readable results: `cert/`.

## Real-video reference anchors (not a certification claim)

Measured on the user-provided corpus, published in `benchmarks.md` / `README.md`:

| Case | Reference throughput |
| --- | ---: |
| 640×360, 120 f, `medium`, CRF 23, 6 threads (synthetic `testsrc2`) | 609.14 fps |
| 1920×1080, 8 f, `ultrafast`, CRF 23, 6 threads (Fortnite I420) | 126.98 fps |
| 1920×1080, 8 f, `medium`, CRF 23, 6 threads | 40.61 fps |
| 1920×1080, 8 f, `veryslow`, CRF 23, 6 threads | 13.84 fps |

The reproducible A/B runner is `cert/real_bench.py` (clips pinned in `cert/real_clips.json`); its reference-only results are under `cert/results/real/`. **The candidate remains NOT CERTIFIED until ffmpeg decodes its output cleanly on every case.** A missing or invalid `build/x264pp.exe` is reported honestly and produces only the reference-only table — no speedup is inferred or fabricated.

## Progress log

| Date | Event |
| --- | --- |
| 2026-11-27 | Repository created (initially as a fork — discarded) |
| 2026-11-27 | Reset to clean-room process; reference source purged; only black-box oracle retained |
| 2026-11-27 | Added reproducible real-video A/B harness; candidate absent → reference-only table, NOT CERTIFIED |
