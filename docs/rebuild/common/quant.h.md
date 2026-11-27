# `common/quant.h` — quantization dispatch interface

**Role:** declares `x264_quant_function_t`, the complete vtable for quantization,
dequantization and coefficient analysis, plus the trellis parameter macro.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/quant.h`.
- Role in one sentence: fix the signatures and contracts of every quantizer, dequantizer,
  coefficient scanner and trellis optimizer so they can be dispatched by CPU feature.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

Quantization is invoked many times per macroblock and has many variants (4x4, 8x8, DC-only,
4:2:2 chroma, trellis, psy-trellis). A single vtable keeps the call sites uniform and lets
`x264_quant_init` select the fastest available implementation. The header also centralizes
the long trellis argument list in a macro so all trellis variants share one ABI.

---

## 3. Public interface

Type `x264_quant_function_t` contains these pointer groups:

- Quantizers (return nonzero flag / bitmask):
  - `quant_8x8(dctcoef dct[64], udctcoef mf[64], udctcoef bias[64])`
  - `quant_4x4(dctcoef dct[16], udctcoef mf[16], udctcoef bias[16])`
  - `quant_4x4x4(dctcoef dct[4][16], udctcoef mf[16], udctcoef bias[16])`
  - `quant_4x4_dc(dctcoef dct[16], int mf, int bias)`
  - `quant_2x2_dc(dctcoef dct[4], int mf, int bias)`
- Dequantizers (in-place, `dequant_mf[6][N]` for the six QP residues):
  - `dequant_8x8`, `dequant_4x4`, `dequant_4x4_dc`
- Combined 4:2:2 DC inverse:
  - `idct_dequant_2x4_dc(dct[8], dct4x4[8][16], dequant_mf, i_qp)`
  - `idct_dequant_2x4_dconly(dct[8], dequant_mf, i_qp)`
- Chroma DC sparsity: `optimize_chroma_2x2_dc(dct[4], dequant_mf)`,
  `optimize_chroma_2x4_dc(dct[8], dequant_mf)`.
- `denoise_dct(dct * , uint32_t *sum, udctcoef *offset, int size)`.
- Decimation: `decimate_score15`, `decimate_score16`, `decimate_score64`, each
  `int (*)(dctcoef *dct)`.
- Coefficient statistics:
  - `coeff_last[13]`, `coeff_last4`, `coeff_last8`:
    `int (*)(dctcoef *dct)`;
  - `coeff_level_run[13]`, `coeff_level_run4`, `coeff_level_run8`:
    `int (*)(dctcoef *dct, x264_run_level_t *runlevel)`.
- Trellis family: macro `TRELLIS_PARAMS` expands to a fixed argument list containing
  `unquant_mf`, `zigzag`, `lambda2`, `last_nnz`, `coefs`, `quant_coefs`, `dct`,
  `cabac_state_sig`, `cabac_state_last`, and packed `level_state0`/`level_state1`.
  Members: `trellis_cabac_4x4(..., int b_ac)`, `trellis_cabac_8x8(..., int b_interlaced)`,
  `trellis_cabac_4x4_psy(..., int b_ac, dctcoef *fenc_dct, int psy_trellis)`,
  `trellis_cabac_8x8_psy(...)`, `trellis_cabac_dc(..., int num_coefs)`,
  `trellis_cabac_chroma_422_dc(...)`.

Exported function:

- `void x264_quant_init(x264_t *h, uint32_t cpu, x264_quant_function_t *pf)`.

`six` in `dequant_mf[6][N]` corresponds to `qp % 6`; the surrounding QP quotient is
passed separately as `i_qp`.

---

## 4. State and data structures

- `x264_run_level_t` (declared in `bitstream.h`) carries `last`, `mask` and `level[18]`;
  the level array is 16-byte aligned for SIMD writers.
- The `level_state0`/`level_state1` trellis parameters are packed CABAC states copied from
  the encoder's context array; they are values, not pointers, and are passed by value to
  keep the trellis inner loop register-based.
- `udctcoef` is the unsigned coefficient type used for multipliers/biases; `dctcoef` is
  signed. In high-bit-depth builds both are widened.
- The vtable is stateless; all mutable data is in caller-supplied coefficient arrays.

---

## 5. Algorithm / behaviour

The header encodes these contracts:

1. **In-place semantics:** all quantizers/dequantizers overwrite their `dct` argument with
   the resulting levels; return values are flags/counts, not new arrays.
2. **Multiplier semantics:** `mf` and `bias` are arrays indexed per coefficient for AC
   blocks and scalars for DC-only blocks. The bias carries the deadzone/rounding policy.
3. **Mask semantics:** `quant_4x4x4` returns a 4-bit bitmap, bit `j` set iff the j-th 4x4
   block in the array contained a nonzero coefficient.
4. **Statistics semantics:** `coeff_level_run*` fills `x264_run_level_t` with the last index,
   a bitmask of nonzero positions and the sequence of levels, returning the count.
5. **Trellis semantics:** the `_psy` variants take the original (unquantized) transform of
   the source block for the psycho-visual bias; the DC variants operate on the compact DC
   arrays. All trellis functions return a nonzero flag and write chosen levels to `dct`.
6. **Dispatch:** `x264_quant_init` installs all scalar entries first, then upgrades them in
   increasing CPU capability order; flat quantization matrices select specialized fast
   dequantizers.

---

## 6. Dependencies

- Types from `common.h`, `x264_run_level_t` from `bitstream.h`, `x264_t` from `common.h`.
- Implemented in `quant.c`; consumed by `encoder/macroblock.c`, `encoder/rdo.c`,
  `encoder/cabac.c`, `encoder/cavlc.c` and SIMD quant headers.
- The trellis ABI depends on `x264_cabac_*` tables held by the encoder context.

---

## 7. Constants and tables

- `TRELLIS_PARAMS` is the only macro; its parameter list is fixed and shared by all trellis
  variants.
- Array sizes encode block shapes: 4, 8, 15, 16, 64 coefficients.
- `13` indexed coefficient-stat entries correspond to the CABAC block categories
  `DCT_LUMA_DC..DCT_CHROMAV_8x8` (see `macroblock.h`).

---

## 8. Performance notes

- Keep the vtable in a hot, read-only cache line; it is consulted for every coefficient block.
- ABI: passing `mf`/`bias` as contiguous arrays lets SIMD load them with a single vector load
  per block.
- The packed trellis state parameters exist specifically to avoid memory traffic in the
  inner loop — an independent implementation should preserve the by-value convention.
- See `quant.c.md` section 8 for the full SIMD and algorithmic optimization discussion.

---

## 9. References

- ITU-T H.264 clauses 8.5/8.6 and 9.3.
- JVT-B118 decimation scoring.
- x264 `quant.c`/`quant.h` and SIMD quant kernels, public source tree.

---

## 10. Verification checklist

1. Compile all struct members with correct signatures; assert non-null after
   `x264_quant_init(0, &pf)` with a dummy encoder context.
2. Check that `coeff_last[13]` and `coeff_level_run[13]` alias the correct core variants for
   DC, AC, 4x4 and 8x8 categories.
3. Expand `TRELLIS_PARAMS` in a dummy declaration and confirm all seven trellis pointers
   have identical leading signatures.
4. Verify DC-only entries accept scalar `mf`/`bias` while AC entries accept arrays.
5. Compare native-CPU and `cpu=0` vtables functionally with the checks in `quant.c.md`.

---

## 11. Open questions

- `UNKNOWN`: the mapping from the 13 CABAC categories to the three core coefficient-stat
  routines is fixed in `quant.c`, not in this header.
- `UNKNOWN`: whether additional trellis variants appear in future revisions is outside the
  documented interface.
