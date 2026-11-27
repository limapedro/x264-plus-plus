# x264++ benchmark harness

Reproducible speed + bit-identity benchmark comparing the pristine upstream
x264 CLI against the optimized x264++ CLI. Pure Python (standard library
only; numpy is optional and only speeds up clip generation). No ffmpeg and no
external media files are required.

## Files

| File | Purpose |
|---|---|
| `generate_clip.py` | Deterministic I420/YUV420p clip generator (integer hash + drifting sine fields, moving blob/patch, grain). Writes a sidecar `*.manifest.json` with parameters + SHA256. |
| `bench.py` | Main runner: generates/reuses the clip, times both encoders at every preset, hashes outputs, writes JSON + Markdown. |
| `assets/` | Generated clip + manifest (created at runtime; `.yuv` is not checked in). |
| `temp/` | Per-run `.264` outputs (deleted unless `--keep-outputs`). |
| `results/bench_result.json` | Machine-readable result. |
| `results/score_table.md` | Markdown table suitable for copying into `score.md`. |

## Requirements

* Python 3.8+ (`python --version`). numpy 2.x is used automatically when
  installed; otherwise a slower pure-Python generator is used. The backend,
  numpy version and clip SHA256 are recorded in the manifest, so a change of
  backend triggers regeneration instead of silently changing the input.
* Two built x264 CLIs. Defaults:
  * upstream: `build/upstream/x264.exe`
  * optimized: `build/opt/x264.exe`
* If a MinGW-built `x264.exe` exits with `0xC0000135` (DLL not found) when
  launched from plain PowerShell, run inside an MSYS2 MinGW64 shell or add
  `C:\msys64\mingw64\bin` to `PATH`. The harness prepends that directory to
  child-process `PATH` automatically when it exists, and records it in the
  JSON (`settings.mingw_path_prepended`).

## Quick start

```powershell
# 1. sanity-check the generator (no encoders needed)
python bench\generate_clip.py --frames 24 --print-manifest

# 2. full benchmark (defaults: 640x360, 240 frames, CRF 23, 6 threads, 5 reps)
python bench\bench.py

# 3. quick smoke test with fewer frames / repetitions
python bench\bench.py --frames 24 --reps 2 --presets ultrafast medium

# 4. custom run (e.g. all logical cores, 7 reps, different paths)
python bench\bench.py --threads 12 --reps 7 `
  --upstream ..\build\upstream\x264.exe --opt ..\build\opt\x264.exe
```

If an executable is missing, the harness still generates/verifies the clip and
writes a `blocked` JSON + Markdown explaining what is missing (exit code 2),
so it can be wired into CI before the builds exist.

## What is measured

* **Presets**: `ultrafast superfast veryfast faster fast medium slow slower
  veryslow`. `placebo` is intentionally excluded (too slow); it can be added
  explicitly with `--presets ... placebo`.
* **Common options for both encoders** (only the output filename differs):

  ```
  --demuxer raw --input-csp i420 --input-res WxH --fps 30
  --preset <p> --crf 23 --threads 6
  --keyint 250 --min-keyint 25 --scenecut 0
  --quiet -o <out.264> <clip.yuv>
  ```

  Rationale: fixed CRF removes rate-control adaptation differences;
  `scenecut 0` disables adaptive I-frame decisions so GOP structure is
  deterministic; fixed keyint/min-keyint, threads and fps keep the workload
  identical; PSNR/SSIM computation is left **off** (this x264 source exposes
  opt-in `--psnr`/`--ssim`, so the harness simply does not pass them) because
  it adds work and extra output. `--quiet` reduces console-printing overhead.
  Extra flags can be appended identically to both command lines with
  repeatable `--extra-arg` (e.g. `--extra-arg --no-deblock`).
* **Timing**: one warm-up encode per encoder per preset (output discarded),
  then `--reps` timed repetitions, **A/B interleaved** per repetition to
  cancel thermal/cache drift. Median wall-clock milliseconds and frames/sec
  are reported; raw per-rep times, min/mean/max are kept in JSON.
  Wall-clock includes process startup and writing the `.264` to disk; both
  encoders are treated identically.
* **Speedup** = `median_upstream_ms / median_opt_ms`, so values **> 1 mean the
  optimized build is faster**.

## Bit-identity checks

Two SHA256 values are stored per encoder per preset:

* `hash_full` — SHA256 of the complete `.264` file (the requested byte-identity
  check).
* `hash_video_only` — SHA256 after stripping SEI NAL units (type 6) and
  canonicalising Annex-B start codes. x264 embeds an "encoding settings"
  string containing its build version in an SEI; if the two builds report
  different versions, the full files differ even when the coded video is
  bit-for-bit identical. This derived hash isolates that case.

Interpretation:

* both identical → outputs match exactly.
* full differs, video-only identical → difference is metadata/version string
  only; coded video matches.
* video-only differs → the bitstream itself changed (or x264 emitted a
  different NAL layout). Investigate `size_delta_bytes` and encoder settings.
* The harness also verifies that all repetitions of a given encoder produce
  identical hashes (`reps_identical_full` / `reps_identical_video_only` in
  JSON); a `false` means the encoder is not deterministic under these flags.

Size delta (`opt_size - upstream_size`) and percentage are reported as well.

## Outputs

`results/score_table.md` is a self-contained report with the configuration,
per-preset table, summary (median and geometric-mean speedup, identity flags)
and interpretation notes. Copy the table/summary into `score.md` as needed.
`results/bench_result.json` contains everything, including versions, CPU
info, clip manifest, per-rep timings and hashes.

## Useful options

```
--width/--height/--frames/--fps/--seed   clip parameters (default 640x360/240/30/1337)
--clip PATH                              explicit clip path (reuse an existing clip)
--crf FLOAT                              default 23
--threads INT                            default 6 (physical cores on the i5-12400F)
--keyint / --min-keyint / --scenecut     GOP settings (250/25/0)
--reps INT                               default 5
--presets P [P ...]                      override preset list
--timeout SECONDS                        per-encode timeout (default 900)
--keep-outputs                           retain .264 files in temp/ for inspection
--no-generate                            fail rather than regenerate a stale clip
--generate-only                          only generate/verify the clip and exit
```

## Reproducibility notes

* The clip is generated from explicit integers (no `random` state, no clock),
  so identical arguments produce identical bytes on the same platform/libm.
  Its SHA256 is verified on every run; a mismatch triggers regeneration.
* Because both encoders read the same file and receive the same CLI except for
  `-o`, any measured difference is attributable to the binary/build itself.
* For the most stable numbers, close other CPU-heavy work, keep the machine
  at a fixed power profile, and use more repetitions (`--reps 7` or higher).
