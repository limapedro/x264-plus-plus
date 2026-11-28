# x264++

> A **clean-room**, speed-first H.264/AVC encoder. Not a fork of x264.

**Repository:** `limapedro/x264-plus-plus` (GitHub disallows `+` in names).

## What this project is

An independent implementation of an H.264 encoder that is CLI- and bitstream-compatible with x264 in observable behaviour, built under a strict **Chinese wall**:

- a **specification team** probes `oracle/x264.exe` as a black box and writes behavioural specs into `spec/`;
- an **implementation team** writes original code in `src/` using only `spec/` and public standards — it never sees reference source;
- an **independent certification team** statistically tests and publishes results in `score.md`.

The process follows IBM Cleanroom Software Engineering: box-structure specification (black → state → clear), stepwise refinement with verification, incremental delivery, defect prevention over debugging, and statistical usage testing. See [`docs/process/CLEANROOM.md`](docs/process/CLEANROOM.md).

**Goal:** >= 2x the encode throughput of the reference encoder, measured per preset, with published honest numbers. Speed-first: the bitstream need not be byte-identical, but it must be valid H.264 and meet the documented quality envelope.

## Benchmarks

> **Candidate status: CERTIFIED as valid H.264, but NOT meeting the project goal.**
> All speedups below are measured by `cert/real_bench.py` with ffmpeg decode checks.
> The candidate is lossless (PSNR = inf) but uses all-I_PCM, making its files ~126×
> larger than x264.

Full standings and reproduction commands: [`benchmarks.md`](benchmarks.md). Machine-readable results: `cert/results/` and `cert/results/real/`.

### Reference (oracle x264, black box)

| Case | Reference throughput |
| --- | ---: |
| 640×360, 120 f, `medium`, CRF 23, 6 threads (synthetic `testsrc2`) | **609.14 fps** |
| 1920×1080, 8 f, `ultrafast`, CRF 23, 6 threads (real Fortnite clip) | **126.98 fps** |
| 1920×1080, 8 f, `medium`, CRF 23, 6 threads | **40.61 fps** |
| 1920×1080, 8 f, `veryslow`, CRF 23, 6 threads | **13.84 fps** |

Real-video corpus: `D:\Videos`, including a ready 1920×1080 × 128-frame I420 file at `D:\Videos\Wx264-reference\fortnite_prefix128.yuv`. The reproducible A/B runner is [`cert/real_bench.py`](cert/real_bench.py) (`python cert\real_bench.py --frames 8 --reps 2 --warmups 1`); its latest reference-only report is [`cert/results/real/latest.md`](cert/results/real/latest.md).

### x264++ (clean-room candidate) — CERTIFIED, goal not met

8×1920×1080 Fortnite, CRF 23, 6 threads (measured 2026-11-28):

| Preset | Reference | x264++ | Speedup | ffmpeg | Size vs oracle |
| --- | ---: | ---: | ---: | --- | --- |
| ultrafast | 162.22 fps | 160.71 fps | **0.99×** | clean | ~126× larger |
| superfast | 93.99 fps | 155.48 fps | **1.65×** | clean | ~126× larger |
| veryfast | 69.08 fps | 164.06 fps | **2.38×** | clean | ~126× larger |
| faster | 47.63 fps | 152.22 fps | **3.20×** | clean | ~126× larger |
| fast | 40.28 fps | 161.74 fps | **4.02×** | clean | ~126× larger |
| medium | 39.65 fps | 154.31 fps | **3.89×** | clean | ~126× larger |
| slow | 24.17 fps | 152.59 fps | **6.31×** | clean | ~126× larger |
| slower | 19.84 fps | 146.93 fps | **7.41×** | clean | ~126× larger |
| veryslow | 13.91 fps | 147.95 fps | **10.64×** | clean | ~126× larger |

640×360: 0.90–1.55× vs ultrafast, 2.3× vs medium, 5.6× vs veryslow. 720p60: 1.20× vs ultrafast, 2.3× vs medium, 4.3× vs veryslow.

**Honest summary:** the ≥2×-on-every-preset target is **not met** — `ultrafast` 0.99× and `superfast` 1.65×; 2× is reached from `veryfast` onward. Decoded frames are byte-exact/lossless, but the all-I_PCM bitstream is ~126× larger than x264, so the quality/size envelope fails. CAVLC integration is the remaining blocker: the writer is correct in isolation (20,000/20,000 vectors), and an all-prev/DC/full-CBP configuration decodes cleanly, but at 8.57 dB PSNR; see `src/verification/cavlc-progress.md`.

## Layout

```
spec/                 black-box specifications (clean team's only input)
src/                  original implementation (C + NASM SIMD)
cert/                 independent certification harness and results
oracle/               reference CLI binary, black-box probe target only
docs/process/         clean-room process definition
bench/                older benchmark harness (being folded into cert/)
score.md              public scoreboard
```

## Status

Increments I0–I5 are defined in `docs/process/CLEANROOM.md`. I0 (CLI/I/O) and I1 (I-slice CAVLC core) are implemented; I1 is **not yet bitstream-valid** (ffmpeg reports `corrupted macroblock 0 0 (total_coeff=-1)` from incorrect CAVLC tables). Assembly kernels are deliberately paused until the C core is valid. [`score.md`](score.md) is intentionally empty of claims until certification passes.

## License

Original code: GPLv2-or-later. The reference binary in `oracle/` is a GPL x264 build, included only as a test oracle and not part of the implementation. See [`COPYING`](COPYING).
