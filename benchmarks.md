# Benchmarks / current standings

**Goal:** ≥ 2× the encode throughput of the black-box reference (`oracle/x264.exe`, x264 API build 164) on **every** preset, with H.264-compatible output and quality close to the reference.

This file is the human-readable standings sheet. `score.md` is the certification-owned scoreboard; `cert/results/` holds machine-readable runs. All numbers below are measured, never estimated. "—" means not measured / not certifiable yet.

## Current standings (2026-11-27)

| Stage | Status | Evidence |
| --- | --- | --- |
| Documentation (261 per-file rebuild specs) | complete | `docs/rebuild/INDEX.md`, 0 pending |
| Clean CLI/input/output (I0) | committed, compiles | `src/verification/I0.md` |
| Clean C codec core (I1) | builds, **not valid yet** | ffmpeg decodes SPS/PPS but reports `corrupted macroblock 0 0 (total_coeff=-1)` |
| Assembly kernels | not started (deliberately paused until C core is valid) | `src/x86/` contains only `ABI.md` + `kernels.h` |
| Certified speedup | **none** | `score.md`: "not certified yet" |

## Reference measurements (oracle, black box)

| Case | Reference throughput | Conditions |
| --- | ---: | --- |
| 640×360, 120 frames, preset `medium`, CRF 23, 6 threads | **609.14 fps** | deterministic `testsrc2` I420 clip, warm-up + timed run, MSYS2 MinGW GCC 16.2 / i5-12400F |
| 1920×1080, 8 frames, preset `ultrafast`, CRF 23, 6 threads | **126.98 fps** | real content: `D:\Videos\Wx264-reference\fortnite_prefix128.yuv` |
| 1920×1080, 8 frames, preset `medium`, CRF 23, 6 threads | **40.61 fps** | same real clip |
| 1920×1080, 8 frames, preset `veryslow`, CRF 23, 6 threads | **13.84 fps** | same real clip |

Real-video corpus made available for certification: `D:\Videos` (dozens of MP4/WebM clips) and a ready 1920×1080 × 128-frame I420 file at `D:\Videos\Wx264-reference\fortnite_prefix128.yuv`. The 8-frame subset above keeps iteration fast; the full 128-frame clip will be used for final per-preset numbers.

Earlier fork-iteration measurements were withdrawn when the project was reset to clean room; only the oracle number above is retained as a fixed reference anchor.

## Candidate measurements

| Preset | Reference (fps) | x264++ (fps) | Speedup | ffmpeg decode | Notes |
| --- | ---: | ---: | ---: | --- | --- |
| ultrafast | — | — | — | not certified | — |
| superfast | — | — | — | not certified | — |
| veryfast | — | — | — | not certified | — |
| faster | — | — | — | not certified | — |
| fast | — | — | — | not certified | — |
| medium | 609.14 | — | — | **fails** (CAVLC `total_coeff=-1`) | 3-frame 64×64 smoke encode produced 15,018 bytes; SPS/PPS parsed correctly |
| slow | — | — | — | not certified | — |
| slower | — | — | — | not certified | — |
| veryslow | — | — | — | not certified | — |

**Honest summary:** x264++ is currently slower-or-unknown and not bitstream-valid. The 2× target is not met. The immediate blocker is CAVLC table correctness in `src/core/cavlc.c`; a fix is in progress against the normative ITU-T tables.

## Reproduction

```powershell
# build candidate
& 'C:\msys64\usr\bin\bash.exe' -lc 'export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH; cd /e/__PROJECTS__/x264++ && make -f src/Makefile'

# encode a tiny deterministic clip and verify with ffmpeg
$ff='C:\ffmpeg\ffmpeg-master-latest-win64-gpl\bin'
$env:PATH='C:\msys64\mingw64\bin;'+$ff+';'+$env:PATH
& ffmpeg -y -f lavfi -i 'testsrc2=size=64x64:rate=30' -frames:v 3 -pix_fmt yuv420p -f rawvideo scratch/tiny.yuv
& build\x264pp.exe --input-res 64x64 --input-csp i420 --fps 30 --preset medium -o scratch/cand.264 scratch/tiny.yuv
& ffmpeg -v error -i scratch/cand.264 -f null -

# full certification (usage-model sampling, timing, quality bands)
python cert/certify.py
```
