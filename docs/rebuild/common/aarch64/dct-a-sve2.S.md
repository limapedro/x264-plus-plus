# `common/aarch64/dct-a-sve2.S` — SVE2 4×4 inverse DCT with add

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/dct-a-sve2.S`
- Role: SVE2 implementation of the fused "inverse 4×4 DCT + add to reconstruction" operation.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The reconstruction path applies a 4×4 inverse transform to quantized coefficients and adds the result to the predictor plane with rounding and byte saturation. This is one of the hottest reconstruction kernels. The SVE2 version exploits SVE2's predicated rounding shift-right (`srshr`) and narrowing-with-saturation (`sqxtunb`) instructions, plus predicated byte stores, to fuse operations that NEON must do in several steps.

## 3. Public interface

One exported function (declared in `dct.h`):

`void x264_add4x4_idct_sve2( uint8_t *p_dst, int16_t dct[16] );`

- `x0` = reconstruction plane pointer (destination and source of the predictor), row stride fixed at 32 (`FDEC_STRIDE`).
- `x1` = 16 signed 16-bit transform coefficients.
- Return: nothing; four rows of four bytes in `p_dst` are updated in place.

## 4. State and data structures

- Predicates: `p0.h` with `vl8` (used for predicated byte loads of eight lanes) and `p1.h` with `vl4` (used for the four-lane rounding shifts and stores).
- Coefficient vectors: two 8-halfword registers loaded from `dct` (16 halfwords = two vectors).
- Four reconstruction row vectors (`z28`–`z31`) loaded as predicated widening byte loads from successive rows of `p_dst`.
- SVE2-only instructions used: `srshr` (predicated signed rounding shift right), `sqxtunb` (signed saturate to unsigned byte, narrow, low half), `st1b` under predicate.

No persistent state; all values are register-resident.

## 5. Algorithm / behaviour

1. Load the 16 coefficients as two 8-halfword vectors. Apply the first 1-D inverse butterfly to the pairs formed by these vectors: compute the sum and difference of the two halves (`a+b`, `a-b`) and form the "1.5×" terms by arithmetic-shifting one input right by one and combining (`x/2 + y`). Specifically, the odd outputs use `sshr(x,1) - y` and `sshr(y,1) + x`, matching the standard inverse-transform pattern.
2. Replicate the high half of each vector into its low half (`mov v.d[0], v.d[1]`) so that the eight lanes carry the four values needed for the second stage duplication.
3. Load four rows of four predictor bytes from `p_dst` (predicated `vl8` byte loads widening to halfwords).
4. Transpose the 4×4 halfword coefficient matrix using the shared `transpose4x4.h` macro (with a register order that matches the inverse butterfly's output convention).
5. Apply the second 1-D inverse butterfly at 4-lane width, producing four vectors of four transformed contributions.
6. Round each contribution with an arithmetic right shift by 6 with rounding (`srshr`, rounding half away from zero for signed values). This is the H.264 inverse-transform scaling (`1/64` with rounding).
7. Add the four predictor rows (widened bytes) to the four shifted contributions.
8. Saturate each 16-bit sum into an unsigned byte (`sqxtunb`: signed value clamped to 0..255, narrow to byte).
9. Store four bytes per row back to `p_dst` using predicated byte stores, advancing the destination pointer by stride 32 each row. The store order is row0, row1, row3, row2 — note the deliberate swap of the last two rows, which matches the register permutation produced by the earlier transpose/butterfly. A clean-room port must reproduce this output ordering exactly.

Rounding and saturation: exactly as specified by H.264 — round before adding the predictor (`srshr #6`), then clamp the sum to [0,255]. There is no additional rounding in the add.

### Inverse-transform equations

For each 1-D inverse pass with inputs `(a, b, c, d)`:

```
e = a + c ;  f = a − c
g = (b >> 1) − d ;  h = (d >> 1) + b
o0 = e + h ;  o1 = f + g ;  o2 = f − g ;  o3 = e − h
```

The first pass is performed at 8-halfword width (two 4×4 blocks' worth of data) using `sshr #1` for the halving; the high 64 bits are copied into the low half (`mov v.d[0], v.d[1]`) so the transposed 4-lane stage sees the right values. The second pass runs at the 4-lane predicate width after `transpose4x4.h`.

After both passes:

```
contribution = srshr( transformed, 6 )     // round-to-nearest, arithmetic
sample = Clip1( predictor + contribution ) // sqxtunb narrows and saturates to 0..255
```

`srshr` adds 32 before the right shift, so a value of exactly `+0.5` rounds away from zero; the H.264 inverse transform requires this rounding direction.

### Predicate lane accounting

- `p0.h, vl8`: predicated widening byte loads of the four predictor rows read four bytes each into eight halfword lanes; the upper four lanes are inactive/zero.
- `p1.h, vl4`: the rounding shifts and byte stores operate on exactly four lanes per row. `st1b {z.h}, p1` stores the low byte of each halfword lane under the predicate.
- The store order (rows 0, 1, 3, 2) reflects the register permutation after the transpose. A clean-room port that computes the transform in a different register order must still write the rows in the standard raster order; verify by output comparison, not by mimicking the store sequence literally.

### Dispatch conditions

Selected only when `HAVE_SVE2` and `X264_CPU_SVE2` are set; overrides `add4x4_idct`. CPUs with SVE but not SVE2 keep the NEON inverse transform even though the forward SVE kernel may be active — the two feature levels are independent.

## 6. Dependencies

- Includes `asm.S` and `dct-a-common.S`.
- Requires `.arch armv8-a+sve+sve2`.
- Uses `SUMSUB_AB`, `transpose4x4.h`, and the shared inverse-butterfly idiom (documented in `dct-a.S.md`).
- Dispatched from `common/dct.c` when `HAVE_SVE2` and `X264_CPU_SVE2` are set, overriding the NEON `add4x4_idct`.

## 7. Constants and tables

- `6`: inverse-transform rounding shift.
- `1`: shift used in the 1.5× term construction.
- `32` (`FDEC_STRIDE`): reconstruction-plane row stride.
- Predicate widths `vl8`/`vl4` and the implicit H.264 transform coefficients.

## 8. Performance notes

Hot for every reconstructed 4×4 block. Compared with the NEON implementation, SVE2 saves instructions by fusing shift+round+add+narrow+store into predicated operations, and avoids separate scalar-lane loads of predictor rows. The fixed four-lane width means SVE's variable-length advantage is not exploited; the win is instruction fusion, not data parallelism.

Optimization tip for a clean-room port: on AVX2, compute four 16-bit rows in one 128-bit lane set, use `_mm_srai_epi16` plus an add of 32 for rounding, `_mm_adds_epu16`-style widening, and `_mm_packus_epi16` for saturation; emulate the swapped row-store order explicitly. SVE note: if multiple blocks are available, `vl8`/`vl16` predicates can process two or four blocks at once, but the reference deliberately does not. Public references: ARM SVE2 `srshr`/`sqxtunb` documentation; H.264 inverse transform spec.

## 9. References

- ITU-T H.264 / ISO/IEC 14496-10, Clause 8.5 (4×4 inverse integer transform and sample clipping).
- ARM SVE2 Architecture Reference Manual (`srshr`, `sqxtunb`, `st1b`).
- `docs/rebuild/common/aarch64/dct-a-common.S.md`, `dct-a.S.md`, `dct.h.md`.

## 10. Verification checklist

- Compare output against the NEON `add4x4_idct` for randomized coefficients and predictor patterns; outputs must be byte-identical.
- Verify the swapped store order (rows 2 and 3) by using a predictor row pattern with four distinct row values.
- Verify saturation: coefficients chosen to drive sums below 0 and above 255 must clamp, not wrap.
- Verify rounding: values exactly at the half-increment boundary must round in the direction mandated by H.264 (`srshr`).
- Predicated stores must touch exactly 16 bytes total (guard-page test).

## 11. Open questions

- UNKNOWN: whether the row-store order (0,1,3,2) is a deliberate permutation required by the preceding register transposes or an aliasing of the reference's register names; verify against the bitstream equivalence test rather than assuming.
- UNKNOWN: behaviour for coefficient values outside the range produced by a conforming quantizer (the macro's `sshr`/`add` chain may overflow 16 bits).
