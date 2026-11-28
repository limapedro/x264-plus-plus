# Luma AC reconstruction — derivation and self-check (2026-11-28, follow-up to PR #22)

## Normative scaling (ITU-T H.264 01/2012)

`LevelScale4x4(m,i,j) = weightScale4x4(i,j) * normAdjust4x4(m,i,j)`
(§8.5.9, Eq. 8-313/8-315). With the default `ScalingList4x4` the weight is
16 and `normAdjust` is selected by coefficient parity from

```
v = { {10,16,13}, {11,18,14}, {13,20,16},
      {14,23,18}, {16,25,20}, {18,29,23} }        (Eq. 8-315)
```

`v[m][0]` for even/even coefficient positions, `v[m][1]` for odd/odd,
`v[m][2]` otherwise. Decoder dequantisation of a 4x4 AC level (§8.5.12.1,
Eq. 8-336/8-337) is therefore

```
d = level * 16*v[m][k] << (QP/6 - 4)                    (QP >= 24)
d = (level * 16*v[m][k] + 2^(3-QP/6)) >> (4-QP/6)       (QP < 24)
```

The forward 4x4 transform (§8.5.1) and inverse (§8.5.12.2) satisfy
`IDCT(d) ≈ residual` when `d = 4*F*v[m][0]/v[m][k]`, where `F` is the
encoder's forward DCT coefficient. This was verified with an isolated C test
(`scratch/dct_scale.c` in the worktree) over all 16 coefficient positions.
Hence the level written to the bitstream is

```
level = round( 4*F*v[m][0] / (v[m][k]^2 * 2^(QP/6)) )
```

and the encoder's reconstruction is `ac_dequant_one(QP, idx, level)`, i.e.
exactly the decoder's `d`. This replaces the empirical `ac_scale48` table and
`X264PP_QUANT_GAIN` multiplier in `src/core/encode.c`.

## Other luma defects found by the controlled probe

1. **`dc_sign` row 13.** The Intra16x16 DC Hadamard sign table (Eq. 8-320,
   figure 8-6) had four mirrored signs in the row for raster block 13. A
   checkerboard luma probe reconstructed `169/88/184/72` instead of
   `64/192/64/192`; after the fix it reconstructs `192/64/192/64` exactly.
2. **Plane prediction off-by-one (§8.3.3/8.3.5).** The shared predictors use
   `p[6-i]`/`p[2-i]` instead of `p[7-i]`/`p[3-i]` in their plane modes, and
   read `p[-1]`. When plane mode was selected the encoder's `fdec` diverged
   from the decoder by a growing offset (luma mean error 32 on the 1080p clip
   even with all residual levels forced to zero). `encode.c` now uses local
   normative `pred16_norm` / `pred8c_norm` helpers for both mode scoring and
   reconstruction; the library functions are not modified.

## Self-check

`luma_ac_check.py` encodes four 16x16 frames with known Y patterns
(`gradx`, `grady`, 4x4-block `check`, single-pixel `impulse`), decodes with
ffmpeg and compares every Y sample against the source.

```
make -f src/Makefile BUILD_DIR=build-gate0 \
    CPPFLAGS="-DX264PP_FORCE_PCM=0 -DX264PP_SLICE_QP_DELTA=0"
python src/verification/luma_ac_check.py --encoder build-gate0/x264pp.exe
```

Result after the fix:

```
gradx    max error  1  mean error 1.000
grady    max error  1  mean error 1.000
check    max error  0  mean error 0.000
impulse  max error 10  mean error 0.176
worst luma error: 10 (tolerance 12) -> PASS
```

Before the fixes the same probe gave max errors of 24 (`check`) and 7
(`gradx`), and encoder `fdec` disagreed with the decoder by 4.8–32 samples
per pixel on real 1080p content.

## End-to-end result (8x1920x1080 Fortnite I420, zero ffmpeg errors)

| setting | size | Y | U | V | average |
| --- | --- | --- | --- | --- | --- |
| QP23, before | 2,009,600 B | 16.86 | 31.27 | 31.38 | 18.54 dB |
| QP23, after  | 1,939,699 B | 30.26 | 35.38 | 37.04 | 31.49 dB |
| QP45, before | 302,691 B | 14.92 | 27.29 | 29.59 | 16.58 dB |
| QP45, after  | 293,151 B | 25.08 | 30.61 | 30.19 | 26.24 dB |

The **proposed quality setting is QP45** (`--crf 23` with the default
`-DX264PP_FORCE_PCM=0` build): 293,151 B (<= 400,000) and 26.24 dB average
(>= 25 dB, oracle 42.64 dB). The default I_PCM build is unchanged and still
hashes to `83C71EBCC7A01E991F052CDDFC8BFE8E7B4BA27DC7A498A103DC7EE8C00F0DB1`.
