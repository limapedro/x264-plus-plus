# I1 — implementation-side questions and recorded deviations

This is the clean implementation team's question log for increment I1. Items
marked **DEVIATION** are deliberate, documented departures from the full
behavioural specification; items marked **QUESTION** need an answer from the
specification team before the next increment. Nothing here is an excuse to
read the reference source or run the oracle.

## DEVIATION 1 — deblocking filter disabled

`docs/process/CLEANROOM.md` names deblocking in the I1 exit criterion, but the
I1 build instructions explicitly say "Disable deblocking via slice header in
I1 (documented deferral)". The encoder writes
`disable_deblocking_filter_idc = 1` and does not model the filter when forming
predictions. **QUESTION:** should certification treat filtered/unfiltered
reconstruction as equivalent at I1, or must the filter be implemented before
I1 is signed off?

## DEVIATION 2 — `I_16x16` macroblocks only

The emitted macroblock type is always `I_16x16` (with the four intra predictors
selected by SAD). `I_4x4`, `I_8x8`, `I_PCM`, the `me(v)` CBP mapping and
`transform_size_8x8_flag` are implemented only as library support in
`predict.c`/`cavlc.c`, not selected. The stream is a valid H.264 I-slice.
**QUESTION:** is macrotype diversity part of the I1 decodability contract, or
may it wait for the RDO increment?

## DEVIATION 3 — CAVLC table values

`cavlc.c` contains fixed VLC tables for `coeff_token` (nC groups 0–1, 2–3, 4,
and 2x2 chroma DC), `total_zeros` and `run_before`, transcribed from the
published H.264 clause 9.2 tables. The rebuild specs intentionally do not
reproduce the code-bit values, and the clean-room rules forbid consulting an
implementation. **QUESTION:** can the specification team confirm the exact
bit patterns (or supply an independently derived generator) before
certification? This is the highest-risk item in I1 because a single wrong
code can make a slice undecodable.

## DEVIATION 4 — quantisation scaling

`quant.c` uses the standard levelScale vector and `qp/6−4` dequantisation
shift. The forward-quantisation factor is 4 for ordinary 4x4 AC blocks and 2
for the Hadamard DC blocks (luma 16x16 and chroma 2x2). These factors were
derived from the decoder's IDCT/Hadamard normalisation, not observed. They
affect reconstruction quality, not syntax validity. **QUESTION:** confirm the
forward scaling convention used by the target decoder model.

## DEVIATION 5 — prediction approximations

The 16x16, chroma and all nine 4x4/8x8 prediction equations are implemented,
but the encoder only selects 16x16 prediction modes and chroma DC. The
directional 4x4/8x8 equations are the published projection rules; some edge
cases use clamped indices rather than the standard's exact neighbour tables.
Since the decoder computes its own prediction from the signalled mode, this
cannot change syntax validity. **QUESTION:** does the I1 quality contract
require the exact prediction outputs at this stage?

## DEVIATION 6 — rate control approximation

`--crf C` maps to QP≈round(C) and `--qp Q` maps to Q directly; `--qp 0` also
sets the qpprime-y-zero bypass flag in the SPS. There is no lookahead, AQ,
MB-tree, VBV, or content-adaptive QP. **QUESTION:** what is the intended CRF→QP
mapping for the clean build at I1?

## DEVIATION 7 — headers and auxiliary NALs

SPS and PPS are emitted once, in the first access unit, and no SEI/AUD/filler/
end-of-stream NALs are produced. The oracle emits a version SEI (type 5
unregistered payload), but the syntax spec explicitly says its exact bytes are
encoder-chosen and not required to match. **QUESTION:** is a version SEI
required by the I1 black box, or may it wait for the full rate-control
increment?

## DEVIATION 8 — high profile not selected

`params.c` can derive a High-profile SPS (profile_idc 100 with chroma format,
bit depths and no scaling matrix) and `encode.c` writes the High PPS tail, but
the default is Constrained Baseline (profile_idc 66, constraint flags 0 and 1)
with CAVLC. `transform_8x8_mode_flag` is always zero. **QUESTION:** does
"Baseline/High" in the I1 goal require both profiles to be selectable by a
CLI option, or is a conforming Baseline stream sufficient?

## DEVIATION 9 — frame numbering

Every picture is an IDR with `frame_num = 0`, `idr_pic_id = 0` and
`pic_order_cnt_lsb = 0` (POC type 0). **QUESTION:** confirm that repeating
frame_num 0 for consecutive IDR pictures is accepted by the conformance model
used by certification.

## DEVIATION 10 — output capacity and I_PCM fallback

The CLI supplies a buffer of `width*height*6 + 4096` bytes. `append_nal`
returns an error rather than overflowing if a NAL cannot fit. I_PCM is not
used as a fallback. **QUESTION:** should an I_PCM fallback be specified for
pathological high-entropy inputs where CAVLC expansion exceeds the buffer?

## Non-deviations worth recording

* All NALs use Annex B 4-byte start codes, with emulation-prevention bytes
  inserted according to the destination zero-run rule.
* Parameter sets and slice headers follow the normative field order from
  `spec/h264-syntax.md` §3–§5.
* Only `src/` files owned by I1 were written; `spec/`, `docs/`, `cert/`,
  `score.md` and the I0-owned CLI/Makefile were not modified. `api_stub.c` was
  deleted after `api.c` was added, as instructed.
