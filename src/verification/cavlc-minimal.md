# CAVLC minimal-reproduction verification (feat/cavlc-minimal)

Status: **partial**. One normative inverse rule was wrong in `cavlc.c` and is
fixed and independently verified on 1000 random coefficient vectors. The
follow-up integration of the corrected writer into `encode_mb` did **not**
reach a decodable full clip and was reverted; the tree keeps the known-good
I_PCM path. Acceptance is therefore **not met** (see measurements below).

## 1. Standalone harness

`cavlc_harness.c` is a scratch program (built outside the repository) that:

* calls `x264pp_cavlc_residual()` on a handmade coefficient vector;
* writes a complete Annex B H.264 stream with one 16x16 IDR picture and a
  single macroblock — either `I_16x16` with only the 16-coefficient DC block
  (`i16dc`), or `I_NxN` with the test vector in 4x4 block 0 and the remaining
  15 blocks zero (`i4`);
* can also print the exact emitted bit string (`--bits`).

`cavlc_roundtrip.py` decodes that bit string with an independent inverse of
ITU-T H.264 clause 9.2 (using the published VLC tables in `cert/bitstream_diag.py`,
read only) and reconstructs the coefficient vector per 9.2.4. It then writes
all vectors as frames of one Annex B stream and checks it with
`ffmpeg -v error -i ... -f null -`.

Reproduce (MSYS2 MinGW-w64 gcc and ffmpeg on PATH):

```
gcc -O2 -std=c11 -Wall -Wextra -Isrc -c src/core/bs.c    -o <scratch>/bs.o
gcc -O2 -std=c11 -Wall -Wextra -Isrc -c src/core/cavlc.c -o <scratch>/cavlc.o
gcc -O2 -std=c11 -Wall -Wextra -Isrc -c src/verification/cavlc_harness.c -o <scratch>/h.o
gcc <scratch>/bs.o <scratch>/cavlc.o <scratch>/h.o -o <scratch>/harness.exe
set X264PP_HARNESS=<scratch>/harness.exe
python src/verification/cavlc_roundtrip.py 1000
```

## 2. First failing vector and the one rule that was wrong

Before the fix, the first case that failed the independent inverse was

* vector (n = 16, nC = 0): `0 0 0 0 0 0 0 0 0 0 0 0 1 0 1 1`
* emitted bit string (22 bits): `0000101001000000111110`
* inverse result: `0 0 0 0 0 0 0 0 0 0 0 0 2 0 1 1`

The coefficient at scan index 12 decoded as **+2** instead of **+1**.
`coeff_token` was `(TotalCoeff=3, TrailingOnes=1)` instead of `(3, 3)`, because
the writer counted trailing ones over *raw scan positions* and stopped at the
zero gap between two `+1` levels. Clause 9.2.1 defines TrailingOnes over the
sequence of **non-zero** levels; a zero between them is described by
`total_zeros`/`run_before` (9.2.3) and must not end the run. The stray `+1` was
then coded as a regular level with `levelCode = 0`, and the decoder's
`levelCode += 2` rule for the first non-T1 level (`TrailingOnes < 3`) turned it
into `+2`.

**Fix (one rule):** scan backwards over non-zero levels only, count up to
three `±1` trailing ones, remember `non_t1_start` (the highest scan index that
is not a T1), emit sign bits for all T1 positions (skipping gaps), and emit
regular levels only from `non_t1_start` downwards.

`x264pp_cavlc_cbp_code()` was also corrected to the inverse of H.264 Table 9-4
(it previously used a non-normative ad-hoc mapping); no other table or rule was
changed.

## 3. Standalone results (final tree)

| Check | Result |
| --- | --- |
| 11 hand-picked vectors (all-zero, 1 coefficient, 2 coefficients, T1s with and without gaps, level > 1, negative levels, 16 coefficients, escape levels) — inverse round-trip | 0 mismatches |
| Same 11 vectors in a complete H.264 stream — `ffmpeg -v error` | 0 errors |
| 1000 random coefficient vectors (0..16 nonzeros, magnitudes 1..40) — inverse round-trip | **0 mismatches / 1000** |
| Same 1000 vectors as 1000 frames of one Annex B stream — `ffmpeg -v error` | **0 errors / 1000 frames** |

## 4. Integration attempt (reverted)

`encode_mb` was changed to emit `I_NxN` macroblocks with per-4x4 intra
prediction, Table 9-4 CBP and CAVLC luma/chroma residuals. Flat 64x64 input
decoded cleanly, but textured 64x64 input still produced ffmpeg errors
(`top block unavailable for requested intra mode -1`, `negative number of zero
coeffs`, `Invalid level prefix`). A debug dump of all 256 residual blocks in
one frame showed that **every emitted luma bit string round-trips with the nC
the encoder used**, so the remaining divergence is in neighbour-context /
predicted-mode modelling, not in the residual writer. The cert diagnostic also
uses the previous macroblock (rather than the macroblock above) for top
neighbours and omits `intra_chroma_pred_mode` for `I_16x16`, so it cannot be
used as an independent oracle for these paths.

Per the three-iteration limit, `src/core/encode.c` was reverted to its original
(I_PCM) version. No integration claim is made.

## 5. Full-clip measurements (final tree, I_PCM path)

| Check | Result |
| --- | --- |
| `ffmpeg -v error -i out.264 -f null -`, 64x64 `testsrc2`, 8 frames | 0 stderr lines |
| `ffmpeg -v error -i out.264 -f null -`, 1920x1080 `fortnite_prefix128.yuv`, 8 frames | 0 stderr lines |
| File size, 8x1080p | **25,287,468 bytes** (limit: 400 KB — **not met**) |
| `ffprobe` geometry | **1920x1072**, yuv420p (limit: 1920x1080 — **not met**; pre-existing crop-unit error in `params.c`, outside owned files) |
| Luma PSNR vs raw source, first 8 frames (decoded height) | effectively infinite (I_PCM is lossless); oracle 42.64 dB |
| `cert/bitstream_diag.py` on the 1080p clip | 0 errors (4 MBs parsed) |

## 6. Conclusion

The standalone CAVLC writer now passes the required 1000-vector encode/decode
check with zero ffmpeg errors, and the exact first failing vector and emitted
bit string are recorded above. The end-to-end acceptance (small file, 1080p
geometry, integrated CAVLC residuals) is **not met**; this change is limited to
the verified inverse-rule fix plus reproducible verification tooling.
