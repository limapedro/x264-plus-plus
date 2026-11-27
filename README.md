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

Increments I0–I5 are defined in `docs/process/CLEANROOM.md`. No implementation increments have been certified yet; [`score.md`](score.md) is intentionally empty of claims until certification produces measurements.

## License

Original code: GPLv2-or-later. The reference binary in `oracle/` is a GPL x264 build, included only as a test oracle and not part of the implementation. See [`COPYING`](COPYING).
