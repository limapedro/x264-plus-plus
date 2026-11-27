# cert/ — independent certification (read this first)

This directory belongs to the **independent certification team**. It measures
and reports; it never edits `src/`, `spec/`, `bench/`, or the reference source.
The only reference artefact used here is `oracle/x264.exe`, treated strictly as
a black-box behavioural and speed oracle. See
[`docs/process/CLEANROOM.md`](../docs/process/CLEANROOM.md) for the wall rules.

**Status: NOT CERTIFIED YET.** No clean-room candidate binary is present at
`build/x264pp.exe`, so `score.md` contains no speed claims. The harness reports
`blocked` / "not certified yet" and writes no fabricated measurements.

## Quick start

```powershell
# Prerequisites: Python 3.10+ with numpy; ffmpeg at the path below or --ffmpeg.
$env:PATH='C:\msys64\mingw64\bin;C:\msys64\usr\bin;'+$env:PATH

python cert\certify.py                       # full certification run
python cert\certify.py --cases 12 --reps 5   # more statistical samples
python cert\certify.py --force-fallback      # ignore the spec (flagged in output)

# Pipeline check with NO publication: oracle is used as both encoders.
python cert\certify.py --self-test --cases 2 --reps 2 --frames-cap 60
```

`--self-test` prints a loud banner, writes only under `cert/results/selftest/`,
never appends to the reliability log, and never touches `score.md`. Its
numbers are **not** measurements of the candidate.

### Real-video A/B benchmark (`cert/real_bench.py`)

A separate, deterministic harness benchmarks the oracle against the candidate
on the **real video corpus** the user provided (`D:\Videos`), not just
generated clips. It reads the hash-pinned clip list in `cert/real_clips.json`
(Fortnite 1920x1080 I420 + several MP4s), decodes each container to a bounded
number of leading I420 frames with ffmpeg, caches the exact bytes with a
SHA256 manifest **outside** the repo, and runs A/B (warm-up + alternating
interleaved reps, median wall clock/fps) across `--presets` (default
`ultrafast medium veryslow`). Every bitstream is ffmpeg-decoded and PSNR/SSIM
is measured against the exact source I420.

```powershell
# quick smoke run on real clips (small frame count)
python cert\real_bench.py --frames 8 --reps 2 --warmups 1

# full run: 128-frame Fortnite clip, 5 reps
python cert\real_bench.py --frames 128 --reps 5

# one clip / one preset, list the curated corpus
python cert\real_bench.py --clips fortnite-raw-1080p --presets medium
python cert\real_bench.py --discover
```

Outputs: `cert/results/real/latest.json` (machine-readable), `latest.md`
(Markdown tables) and immutable `runs/<run_id>.json`. Exit codes: 0 completed,
1 partial, 2 blocked. A **blocked** run still writes the reference-only table.

**Candidate gating:** a missing or invalid `build/x264pp.exe` is reported as
`NOT CERTIFIED`; candidate fps/speedup cells stay `n/a` / `—`. A present
candidate must decode cleanly on **every** case before any speedup is
published. Using the oracle as a stand-in candidate (pipeline check) is only
for validating the harness and must never be presented as a candidate result.

The four already-established reference anchors (640x360 medium 609.14 fps;
1080p ultrafast/medium/veryslow 126.98/40.61/13.84 fps) are documented in
`benchmarks.md` / `README.md` and reproduced as a labelled historical table
in the generated report; they are never overwritten by a fresh smoke run.

The default `--cases 9` is a smoke run, not a statistical certification batch:
spec §4 suggests ≥100 cases initially and ≥300 for a final claim. Runs below
100 cases emit a warning and mark the score-table notes "SMOKE sample".

## Files

| File | Purpose |
| --- | --- |
| `certify.py` | Main runner: sampling, timing, verification, results, score-table update. |
| `real_bench.py` | Real-video A/B runner (Fortnite + MP4 corpus), warm-up/interleaved timing, decode + PSNR/SSIM, reference-only fallback. |
| `real_clips.json` | Hash-pinned curated clip list for `real_bench.py` (read-only input). |
| `results/real/` | Machine-readable + Markdown output of `real_bench.py`. |
| `usage_model.py` | Reads `spec/usage-model.md`; built-in fallback profile; seeded sampling. |
| `clipgen.py` | Deterministic I420 clip generator keyed by case parameters + SHA256 manifest. |
| `verify.py` | ffmpeg decode check, PSNR/SSIM measurement, quality-envelope parsing. |
| (temp) `x264pp-cert-assets/` | Cached verified clips and manifests, **outside the repo** as the usage model requires (override with `--assets-dir`). |
| `temp/` | Per-run `.264` files (deleted unless `--keep-outputs`). |
| `results/latest.json`, `results/latest.md` | Latest machine-readable / human-readable report. |
| `results/runs/<run_id>.json` | Immutable archive of each run. |
| `results/test-log.jsonl` | Append-only per-case outcomes used for reliability estimates. |
| `results/selftest/` | Isolated self-test output; never a certification result. |

## Statistical method

### 1. Usage-model sampling

* The usage model is `spec/usage-model.md`, owned by the specification team and
  **read-only** here. The loader natively parses the spec's **factored
  first-order Markov chain** (§1–2): the six dimensions `R/N/P/C/T/K` with
  stationary weights `w_d`, stickiness `λ_d`, explicit transition matrices,
  and the ffmpeg lavfi content sources in §1.1. Generic Markdown distribution
  tables, `key: value` lists and JSON/YAML fences are also accepted. Unparsed
  material is reported as a warning; nothing is guessed from reference source.
* The first case draws all six dimensions independently from `w_d`, so the
  chain is stationary from `t=0`. Each later case draws each dimension from
  that dimension's transition row (or from the `λ_d` stickiness formula when
  no explicit matrix is given). This is exactly the algorithm in spec §4.
* If the spec's `w_d` row omits a value that appears in its own transition
  matrix, the harness adds that value with the remaining stationary mass and
  emits a warning. This currently happens for preset `slow` in the spec; the
  warning is recorded in every result (`usage_model.warnings`).
* Sampling uses `random.Random(seed)` with a recorded seed
  (`--seed`, default `20261127`), so a run can be reproduced exactly. Case
  seeds are derived as `seed + 104729 * case_index`; clips are deterministic
  functions of those parameters, with a SHA256 manifest verified on reuse.
* Every case records which attributes came from the spec and which had to be
  filled from built-in defaults (`attribute_sources` in the JSON). The spec
  fixes fps to 30 and leaves GOP settings at their defaults (§4).
* Input clips are generated with the spec's exact ffmpeg lavfi source strings
  (`testsrc2`, `mandelbrot`, noise and static patterns), substituted with the
  sampled `WxH`/fps and written to a temp directory outside the repo. Clips are
  cached with a SHA256 manifest and verified before reuse. The numeric
  `clipgen.py` generator remains the fallback when no spec is available.
* `--presets` restricts sampled presets (renormalising the transition rows);
  this changes the sampled population and is recorded in `settings`.
* `--frames-cap` caps frame counts for smoke tests (recorded in `settings` and
  changes the sampled population, so it is not used for a publication run).

### 2. What happens if the spec is missing/incomplete

If `spec/usage-model.md` is absent, unreadable or contains no usable
resolution/preset/frame distributions, the harness uses the following
**built-in fallback profile** and sets `usage_model.fallback_used: true`,
`status: "built-in fallback (...)"` plus an explicit warning everywhere
(stdout, JSON, Markdown, and the score-table notes). No result is presented as
spec-driven when the fallback was used.

| State | Initial weight | Resolution | Frames | Presets | CRF | Content |
| --- | ---: | --- | --- | --- | --- | --- |
| short | 0.25 | 320x180 (0.4), 640x360 (0.6) | 30 / 60 | ultrafast, veryfast, faster, medium | 23 / 25 | synthetic, flat |
| standard | 0.55 | 640x360 (0.5), 854x480 (0.3), 1280x720 (0.2) | 120 / 240 | all nine standard presets | 21 / 23 / 25 | synthetic, motion, grain, edges |
| heavy | 0.20 | 1280x720 (0.75), 1920x1080 (0.25) | 120 / 240 | medium, slow, slower, veryslow | 20 / 23 | synthetic, motion, grain |

Transition rows: short → (0.55, 0.35, 0.10), standard → (0.25, 0.55, 0.20),
heavy → (0.10, 0.35, 0.55), in the order short/standard/heavy. Common
defaults: fps 30, threads 6, keyint 250, min-keyint 25, scenecut 0.

### 3. Timing and speed

* For each case: one discarded warm-up per encoder, then `--reps` timed
  repetitions. Encoders are **A/B interleaved** and the order is flipped on
  alternate repetitions to cancel thermal/cache drift.
* Both encoders receive byte-identical argv except the `-o` path (x264-style
  raw I420 CLI). Wall clock includes process startup and file write; treating
  both binaries identically makes the ratio meaningful.
* Per case: `speedup = median(oracle times) / median(candidate times)`; fps =
  `frames / median wall-clock`. Per preset: medians of the per-case median
  times are divided, i.e. `speedup_preset = median_cases(oracle_median_ms) /
  median_cases(candidate_median_ms)`. Raw times, min/mean/max, sizes and SHA256
  hashes are retained in the JSON for audit.
* A case contributes a published speed number only if the candidate encode
  succeeds, ffmpeg decodes **exactly** the expected frame count with no error,
  and any numeric quality-envelope threshold is met. Failures are recorded,
  never hidden; a preset with no passing case shows "—".
* With small `--reps` the median is a point estimate with a wide interval; the
  JSON keeps all replicates so a reviewer can see the spread. Treat published
  ratios as measurements of this machine/build/settings, not as tight
  confidence bounds.

### 4. Conformance and quality

* `ffmpeg -progress ... -f null -` decodes every candidate frame; exit code,
  stderr and decoded frame count must all be clean. A second ffmpeg stream
  probe checks `codec_name=h264`, `pix_fmt=yuv420p` and the decoded width ×
  height. The oracle output is decoded as a control. These are the hard
  validity requirements of envelope §4.1.
* When `spec/quality-envelope.md` defines a contract, PSNR and SSIM are measured
  with ffmpeg filters against the original raw clip **for both the candidate
  and the oracle on the same case**. The spec's bands are relative, not
  absolute: PSNR delta ∈ [−0.75, +1.50] dB, SSIM delta ∈ [−0.010, +0.020],
  size ratio ∈ [0.70, 1.40]. Absolute thresholds (`PSNR >= X dB`) are also
  understood if a future spec uses them. If no numeric contract exists, no
  quality claim is made.
* Certification follows envelope §4.3: **100% validity** and **≥95% band
  compliance** over the batch. Every violation is logged with case state and
  measured deltas, and the realized marginal frequency of every sampled
  dimension is published in `summary.realized_marginals`.
* Output files are hashed (SHA256); the hash is a diagnostic for determinism,
  **not** a bit-identity requirement — the project goal is valid H.264 inside
  a quality envelope, not byte-identical bitstreams.

### 5. Confidence and MTTF intent

`results/test-log.jsonl` receives one record per certification case (self-test
runs are excluded): preset, pass/fail, failure categories, and exposure time =
sum of timed encode wall-clock seconds for both encoders. `reliability_from_log()`
fits an exponential "failures per executed case" model and reports:

* point estimate `MTTF = total exposure / failures`;
* 95% chi-square interval using the Wilson–Hilferty approximation
  (`2T/χ²(0.975, 2n+2)` … `2T/χ²(0.025, 2n)`);
* with zero failures, a **one-sided 95% lower bound** `T / -ln(0.05)` and no
  point estimate.

These are *intent* and audit aids, **not reliability claims**: with a handful
of samples the interval is enormous, and samples are correlated by machine
state. A defensible MTTF statement needs a long, representative sequence of
certification runs under the specified usage model. No MTTF number is written
to `score.md` by this harness.

## score.md handling

Only the first Markdown table under `## Current score` is replaced; every
other line (goal, methodology, progress log) is preserved byte-for-byte
including line-ending style. When the candidate is missing, all numeric cells
stay "—" and the Notes column says "Not certified yet — no candidate binary".
Use `--no-score-update` to generate results without touching the scoreboard.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | completed (including a self-test); `status` in JSON is authoritative |
| 1 | partial: some cases failed, so nothing is certified |
| 2 | blocked: missing candidate/oracle/ffmpeg/score file — "not certified yet" |

## Defect reporting

Failures are returned to the implementation team as specification/verification
defects via `src/verification/questions.md` (written by that team, not here).
Certification records the failing case parameters, command line shape, ffmpeg
diagnostic and hashes so the defect is reproducible.
