# Benchmarks / current standings

**Goal:** â‰¥ 2Ã— the encode throughput of the black-box reference (`oracle/x264.exe`, x264 API build 164) on **every** preset, with H.264-compatible output and quality close to the reference.

This file is the human-readable standings sheet. `score.md` is the certification-owned scoreboard; `cert/results/` holds machine-readable runs. All numbers below are measured, never estimated. "â€”" means not measured / not certifiable yet.

## Current standings (2026-11-27)

| Stage | Status | Evidence |
| --- | --- | --- |
| Documentation (261 per-file rebuild specs) | complete | `docs/rebuild/INDEX.md`, 0 pending |
| Clean CLI/input/output (I0) | committed, compiles | `src/verification/I0.md` |
| Clean C codec core (I1) | builds, decodes cleanly, **size envelope fails** | PR #3: all-I_PCM fallback, 8Ã—1080p = 25,287,468 B vs oracle 200,535 B (126Ã—) |
| AVX2 assembly kernels | 14 original NASM kernels merged | PR #4: `nasm -f win64`, link and 14 exported symbols verified |
| Real-video certification harness | merged | PR #2: `cert/real_bench.py` with `D:\Videos` corpus |
| Certified speedup | measured, **goal not met** | PR #9: 0.18Ã— vs ultrafast, 0.70Ã— vs medium, 1.99Ã— vs veryslow (8Ã—1080p) |

## Reference measurements (oracle, black box)

| Case | Reference throughput | Conditions |
| --- | ---: | --- |
| 640Ã—360, 120 frames, preset `medium`, CRF 23, 6 threads | **609.14 fps** | deterministic `testsrc2` I420 clip, warm-up + timed run, MSYS2 MinGW GCC 16.2 / i5-12400F |
| 1920Ã—1080, 8 frames, preset `ultrafast`, CRF 23, 6 threads | **126.98 fps** | real content: `D:\Videos\Wx264-reference\fortnite_prefix128.yuv` |
| 1920Ã—1080, 8 frames, preset `medium`, CRF 23, 6 threads | **40.61 fps** | same real clip |
| 1920Ã—1080, 8 frames, preset `veryslow`, CRF 23, 6 threads | **13.84 fps** | same real clip |

Real-video corpus made available for certification: `D:\Videos` (dozens of MP4/WebM clips) and a ready 1920Ã—1080 Ã— 128-frame I420 file at `D:\Videos\Wx264-reference\fortnite_prefix128.yuv`. The 8-frame subset above keeps iteration fast; the full 128-frame clip will be used for final per-preset numbers.

Earlier fork-iteration measurements were withdrawn when the project was reset to clean room; only the oracle number above is retained as a fixed reference anchor.

## Real-video A/B harness (`cert/real_bench.py`)

The reproducible runner for the real corpus is [`cert/real_bench.py`](cert/real_bench.py). It reads the hash-pinned curated clip list in `cert/real_clips.json` (the ready Fortnite I420 prefix plus three MP4s), bounds each clip to `--frames` leading I420 frames (ffmpeg decode for containers), caches the exact bytes with a SHA256 manifest outside the repo, and A/B benchmarks `oracle/x264.exe` against `build/x264pp.exe` with one discarded warm-up, `--reps` alternating interleaved repetitions, median wall clock/fps, ffmpeg decode validity, and PSNR/SSIM against the exact source. Machine-readable results and the Markdown tables live in `cert/results/real/`.

Smoke run (`--frames 8 --reps 2 --warmups 1`, presets `ultrafast medium veryslow`, 4 clips / 12 cases, 2026-11-27). The candidate binary was **absent**, so these are **reference-only** measurements; the candidate column is `NOT CERTIFIED` and no speedup is published:

| Clip | Frames | Preset | Oracle fps (median) | ffmpeg decode | PSNR dB | SSIM |
| --- | ---: | --- | ---: | :---: | ---: | ---: |
| Fortnite 1920Ã—1080 I420 | 8 | ultrafast | 158.29 | clean | 44.88 | 0.9874 |
| Fortnite 1920Ã—1080 I420 | 8 | medium | 39.89 | clean | 42.64 | 0.9849 |
| Fortnite 1920Ã—1080 I420 | 8 | veryslow | 13.72 | clean | 42.29 | 0.9839 |

The other three clips (640Ã—360, second 1080p, 720p60) and the full A/B table with `n/a (binary missing)` cells are in [`cert/results/real/latest.md`](cert/results/real/latest.md). These fresh numbers differ slightly from the historical anchors above because of run conditions (8-frame smoke, `--no-progress` logging, normal machine variance); the anchors remain the fixed published reference, and nothing here is a candidate speedup.

## Candidate measurements

| Preset | Reference (fps) | x264++ (fps) | Speedup | ffmpeg decode | Notes |
| --- | ---: | ---: | ---: | --- | --- |
| ultrafast | 162.22 | 160.71 | **0.99Ã—** | clean | 8Ã—1920Ã—1080 Fortnite, CRF 23, 6 threads |
| superfast | 93.99 | 155.48 | **1.65Ã—** | clean | same clip |
| veryfast | 69.08 | 164.06 | **2.38Ã—** | clean | same clip |
| faster | 47.63 | 152.22 | **3.20Ã—** | clean | same clip |
| fast | 40.28 | 161.74 | **4.02Ã—** | clean | same clip |
| medium | 39.65 | 154.31 | **3.89Ã—** | clean | same clip |
| slow | 24.17 | 152.59 | **6.31Ã—** | clean | same clip |
| slower | 19.84 | 146.93 | **7.41Ã—** | clean | same clip |
| veryslow | 13.91 | 147.95 | **10.64Ã—** | clean | same clip |

Full nine-preset run also covered three other real clips (640Ã—360, 1080p, 720p60 Twitter sources); all 36 cases passed ffmpeg decode. Candidate is byte-exact/lossless (PSNR sentinel 99 dB) but ~126Ã— larger than the oracle.

These are full nine-preset numbers from the 2026-11-28 certification run (8 frames Ã— 4 clips Ã— 9 presets = 36 cases, all ffmpeg-clean). Cumulative self-speedup at 1080p: ~12 fps â†’ ~155 fps (**~13Ã—**), from bulk I_PCM writes, fast NAL escaping, buffered I/O, AVX2 frame copy, deterministic frame threading, removal of redundant pre-PCM analysis, and AVX2 SAD/DCT wiring. Decoded output is byte-exact/lossless (PSNR sentinel 99 dB), but the file is ~126Ã— larger than the oracle, so the quality/size envelope still fails. The CAVLC writer is correct in isolation; integration remains blocked by the intra4x4 rem/predicted-mode divergence and partial-CBP residual desync (see `src/verification/cavlc-progress.md`).

With `--threads 12` (all logical CPUs) the 1080p Fortnite case reaches 72 fps: 0.46Ã— vs `ultrafast`, 1.54Ã— vs `medium`, 4.38Ã— vs `veryslow`. The 640Ã—360 case reaches 340â€“407 fps: 0.90Ã— vs `ultrafast`, 1.68Ã— vs `medium`, 2.98Ã— vs `veryslow`.

**Honest summary:** 2Ã— is reached from `veryfast` onward on the 1080p Fortnite clip, but `ultrafast` is 0.99Ã— and `superfast` 1.65Ã—; the â‰¥2Ã—-on-every-preset goal is **not met**. Compression (CAVLC) remains the unresolved blocker; attempts are documented in closed PRs #6â€“#8, #14â€“#15 and `src/verification/cavlc-progress.md`.

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

# real-video A/B smoke run (reference-only while the candidate is absent)
python cert\real_bench.py --frames 8 --reps 2 --warmups 1

# full real-corpus run with the candidate present
python cert\real_bench.py --frames 128 --reps 5
```

### Gated compressed CAVLC path (opt-in)

Built with `CPPFLAGS=-DX264PP_FORCE_PCM=0` (QP45): ffmpeg-clean 1920x1080 H.264, **293,151 B** for 8 frames (vs oracle 200,535 B), PSNR **26.24 dB** (Y 25.08 / U 30.61 / V 30.19; oracle 42.64 dB). Default build remains all-I_PCM and bit-identical (`83C71EBC...`). Compressed speed measured 0.41x ultrafast / 1.60x medium / 4.23x veryslow, so it is not the default. See `src/verification/{chroma-recon,luma-ac}.md`.


