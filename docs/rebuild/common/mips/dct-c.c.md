# `common/mips/dct-c.c` — MSA transform and zigzag

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\dct-c.c`. Role: supplies MSA (MIPS SIMD Architecture) implementations of the forward/inverse 4x4 and 8x8 H.264 integer transforms, the 4x4 DC Hadamard transform, block-add variants, and the 4x4 frame zig-zag scan. The whole file is compiled only when `HIGH_BIT_DEPTH == 0` (8-bit pixels, 16-bit transform coefficients).

## 1. Purpose

The transform stage is one of the hottest non-search parts of the encoder. This file provides 128-bit SIMD versions of every primitive the generic `common/dct.c` exposes, so that a MIPS machine with MSA can reach comparable throughput to x86-64 without changing callers. It relies on the local `macros.h` wrapper layer for loads/stores/interleaves; it adds only the algorithm-specific butterflies. Every routine is a drop-in replacement with bit-identical results to the scalar implementation for the supported block sizes.

## 2. Public interface

All symbols are declared in `mips/dct.h` and use the `x264_template` name-mangling macro (so the real exported name depends on bit depth, e.g. `x264_8_add4x4_idct_msa`). Calling convention is the standard C ABI; pointers are ordinary host pointers.

- `void x264_dct4x4dc_msa(int16_t d[16])` — in-place 4x4 Hadamard transform of sixteen DC coefficients. May overwrite all 16 entries.
- `void x264_idct4x4dc_msa(int16_t d[16])` — in-place inverse 4x4 Hadamard transform (the same matrix, applied again; it is its own inverse up to a factor of 4 absorbed by the caller).
- `void x264_add4x4_idct_msa(uint8_t *dst, int16_t dct[16])` — inverse 4x4 transform, add to an 8-bit destination block of stride `FDEC_STRIDE`, clip to `[0,255]`, and zero the 16 source coefficients.
- `void x264_add8x8_idct_msa(uint8_t *dst, int16_t dct[4][16])` — four 4x4 inverse transforms placed at the four quadrant positions of an 8x8 block.
- `void x264_add16x16_idct_msa(uint8_t *dst, int16_t dct[16][16])` — sixteen 4x4 inverse transforms covering a 16x16 block; the source is four `sub8x8`-shaped groups.
- `void x264_add8x8_idct8_msa(uint8_t *dst, int16_t dct[64])` — one 8x8 inverse transform added to an 8x8 destination.
- `void x264_add16x16_idct8_msa(uint8_t *dst, int16_t dct[4][64])` — four 8x8 inverse transforms tiled over a 16x16 block.
- `void x264_add8x8_idct_dc_msa(uint8_t *dst, int16_t dct[4])` — DC-only inverse: add `(dct[k]+32)>>6` uniformly to each 4x4 sub-block, clip, and zero the source.
- `void x264_add16x16_idct_dc_msa(uint8_t *dst, int16_t dct[16])` — the same for sixteen 4x4 sub-blocks in raster order.
- `void x264_sub4x4_dct_msa(int16_t dst[16], uint8_t *src, uint8_t *ref)` — residual 4x4 forward transform; source stride `FENC_STRIDE`, reference stride `FDEC_STRIDE`.
- `void x264_sub8x8_dct_msa(int16_t dst[4][16], uint8_t *src, uint8_t *ref)` — four 4x4 forward transforms of an 8x8 residual.
- `void x264_sub16x16_dct_msa(int16_t dst[16][16], uint8_t *src, uint8_t *ref)` — sixteen 4x4 forward transforms of a 16x16 residual.
- `void x264_sub8x8_dct_dc_msa(int16_t dct[4], uint8_t *p1, uint8_t *p2)` — sum of signed differences over each 4x4 quadrant, followed by a 4-point (2-level) Hadamard; output 4 DC coefficients.
- `void x264_sub8x16_dct_dc_msa(int16_t dct[8], uint8_t *p1, uint8_t *p2)` — the 8-value analogue over a 8x16 region; two cascade levels of 8-point Hadamard.
- `void x264_zigzag_scan_4x4_frame_msa(int16_t level[16], int16_t dct[16])` — frame-order zig-zag permutation.

Contract: forward routines write coefficients in natural (raster) order with no scaling; inverse routines add an already-dequantized coefficient block to the destination and clear the input. Clipping is always to unsigned 8-bit range.

## 3. State and data structures

All functions are pure and stateless: no globals, no persistent buffers, no thread-local state. Intermediate vectors live entirely in the 32 MSA vector registers (`v0..v31`, 128 bits each, viewed as `v16i8`, `v8i16`, `v4i32`, `v2i64`). The coefficient arrays are caller-owned:

- A 4x4 block is 16 signed 16-bit values laid out row-major; four rows of four coefficients.
- An 8x8 block using the 4x4 transform is four 4x4 blocks in raster quadrant order, each occupying 16 consecutive coefficients.
- An 8x8 block using the 8x8 transform is 64 consecutive coefficients.
- The destination frame plane is byte memory with row pitch `FDEC_STRIDE` (32 bytes in an 8-bit build); the source plane uses `FENC_STRIDE` (16 bytes).

**Convention quirk (important for rebuild):** the local load/store macros accept the *stride in units of the pointer's element type*, not in bytes. For `uint8_t*` that is bytes; for `int16_t*` a stride of 4 means four 16-bit elements (8 bytes). Internal helper functions take strides explicitly, so the public wrappers pass 4 or 8 accordingly.

## 4. Algorithm / behaviour

### Forward 4x4 residual DCT

The 4x4 integer forward transform is separable. For each of the four rows a butterfly stage is computed (pairs `a±b`, plus the "rotate" terms formed from the outer samples), then the intermediate is transposed, the same 1-D transform is applied to the new rows, and the results are packed into 16-bit output. Concretely the helper `avc_sub4x4_dct_msa` loads four 32-bit words per row from source and reference, widens bytes to signed 16-bit differences, applies the first 1-D stage to the four row vectors, transposes the 4x4 halfword matrix, applies the second 1-D stage, and stores eight output halfwords per pair via two 128-bit stores. The arithmetic is the standard x264 lossless integer DCT: no right shifts in the forward path; the magnitude of the results is at most a few hundred for byte residuals, so 16 bits suffice.

### Forward 8x8 and 16x16

These are not the 8x8 transform: `sub8x8`/`sub16x16` simply invoke the 4x4 forward transform on each 4x4 quadrant in raster order. `sub16x16` calls `sub8x8` four times with appropriate pointer offsets (`8*FENC_STRIDE`, `8*FDEC_STRIDE`). The output layout is therefore `dct[block_index][coefficient_index]` with block indices in raster quadrant order.

### 4x4 DC Hadamard

`avc_dct4x4dc_msa` treats the 16 input coefficients as a 4x4 matrix. It first applies a row-wise butterfly of the form `(a+b, a-b, c+d, c-d)` with a specific permutation of the four inputs (`src0, src2, src3, src1` ordering for the first butterfly), transposes the 4x4 word matrix, applies the same butterfly again, and packs the results back to 16-bit. The resulting matrix is `H · d · Hᵀ` with `H` the 4x4 Hadamard kernel (entries ±1, no normalization) up to the row/column ordering chosen by the butterfly's output permutation. `x264_idct4x4dc_msa` performs the same fold-back transform on the same data. Because the Hadamard matrix is symmetric and `H·H = 4I`, applying the routine twice multiplies the block by 4; the caller compensates (the inverse-DC add divides by 64, combining quantization/dequantization scaling).

**Rounding:** none in the Hadamard itself; all arithmetic is exact integer addition/subtraction in 32-bit lanes. Coefficient magnitudes after two stages are bounded so that the final 16-bit store does not wrap for valid encoder inputs.

### Inverse 4x4 plus add

`avc_idct4x4_addblk_msa` reads the 16 coefficients as four halfword vectors, applies the 1-D inverse butterfly (`(a+b)`, `(a-b)`, `(b>>1)-c`, `a+(c>>1)` style rotation, i.e. the exact transpose of the forward butterfly including the factor-of-two terms), transposes the 4x4 halfword matrix, applies the inverse butterfly again in the other dimension, then performs an arithmetic shift right by **6** with rounding (`srari`, i.e. `+32` before `>>6` for positive shifts) on each 16-bit element. The destination 4x4 bytes are widened to 16-bit, the shifted residual is added, the sum is clipped to `[0,255]`, packed back to bytes and stored with `FDEC_STRIDE`. Finally the 16 source coefficients are zeroed (two 16-byte stores of zero). The DC-only variant computes `(p_src[0]+32)>>6`, broadcasts it, adds to the four 4x4 destination rows, clips and stores, and clears the DC coefficient.

### Inverse 8x8

`avc_idct8_addblk_msa` adds 32 to `p_src[0]` first (this is the `-qbits/2` ... actually the `+32` DC rounding term mandated by the H.264 inverse 8x8 transform), loads eight halfword vectors, and applies a 2-D 8-point inverse transform built from two 4-point butterfly stages and the "even/odd with quarter shifts" extension. All_extension terms use `>>2` before summing. After the vertical pass, each element is arithmetic-shifted right by **6** with rounding, the 8x8 destination bytes are widened, summed, clipped to `[0,255]`, packed and stored in two 8x4 stores. The source coefficients are **not** cleared by the 8x8 inverse routine (matching the generic behaviour).

### Zig-zag frame scan

The MSA implementation uses two shuffle masks stored as immediate vectors:

- low half indices: `{0, 4, 1, 2, 5, 8, 12, 9}`
- high half indices: `{6, 3, 7, 10, 13, 14, 11, 15}`

A halfword `vshf` of the two input vectors with these masks produces `level[i] = dct[zigzag[i]]` for the standard H.264 4x4 frame zig-zag order. The field scan order is not implemented here.

### DC-sum helpers

`subtract_sum4x4_msa` widens the four 32-bit words of a source row and the matching prediction row into signed halfwords, computes pairwise unsigned-difference values via `hsub`, and horizontally sums them to a signed 16-bit total (max magnitude 4*4*255 ≈ 4080, fits in 16 bits). `sub8x8_dct_dc` collects four such sums (one per 4x4 quadrant of an 8x8 block) and applies a 2-level 4-point Hadamard; `sub8x16_dct_dc` collects eight sums and applies two levels of an 8-point Hadamard, writing results in the x264 DC coefficient order (indices 0,1,6,7,5,4,3,2 for the 8x16 case).

## 5. Dependencies

- **Calls:** local macros in `mips/macros.h` (`LW4`, `INSERT_W4_SB`, `ILVRL_B2_UB`, `HSUB_UB2_SH`, `BUTTERFLY_4/8`, `TRANSPOSE4x4_*`, `TRANSPOSE8x8_SH_SH`, `SRARI_H4_SH`, `SRARI_W4_SW`, `ADDBLK_ST4x4_UB`, `CLIP_SH*`, `PCKEV_*`, `PCKOD_*`, `ST8x4_UB`, `LD4x4_SH`, `LD_SH4/8`, `ST_SH2`, etc.) and MSA intrinsics from `<msa.h>`.
- **Called by:** `x264_dct_init()` in `common/dct.c`, which installs these pointers when `X264_CPU_MSA` is set and the build is 8-bit.
- **Shared config:** `HIGH_BIT_DEPTH` gates the whole file; `FDEC_STRIDE`/`FENC_STRIDE` from `common/common.h` fix destination/source pitches.
- **Cross-file:** `pixel.h`/`predict.h` are not used here; the inverse-DC and forward-DC routines must agree with `quant-c.c` scaling conventions.

## 6. Constants and tables

- Shuffle masks `{0,4,1,2,5,8,12,9}` and `{6,3,7,10,13,14,11,15}` — the 4x4 frame zig-zag order expressed as byte/halfword indices.
- Rounding constants: `+32` before `>>6` for 4x4 inverse and DC-only inverse; `+32` added to the 8x8 DC coefficient before the 8x8 inverse transform; `+1` added in the 4x4 DC Hadamard path through `SRARI_W4_SW(...,1)`.
- Clip bounds `[0,255]` for reconstruction; `CLIP_SH_0_255` uses `maxi_s_h(0)` then `min_s_h(255)`.
- Zero vector used both for clearing coefficients and for sign handling in pack/store helpers.

**Why:** the `>>6` factors are the standard H.264 inverse-transform normalization (scaling by 1/64). The `+32`/`+1` terms implement round-to-nearest rather than truncation. The zig-zag table is defined by the H.264 spec (Table 8-3 style scanning order for 4x4 blocks).

## 7. Performance notes

- **Hotness:** very high. Every macroblock invokes forward DCT for residual coding and inverse DCT for reconstruction, even in `veryfast` presets. DC-only paths dominate in flat/skip-heavy content.
- **SIMD status:** fully MSA-128. The 4x4 transform processes one row/column per vector; the 8x8 transform uses word-lane unpacking to avoid 16-bit overflow in the rotation terms.
- **Branch behaviour:** no data-dependent branches; all masking is via compare-and-select vectors. The only loops are the fixed-count 4x4/8x8/16x16 unrolls generated by helper composition.
- **Optimization tips:** (a) keep the transpose macros register-only to avoid store-forwarding stalls; (b) for DC-only blocks prefer the dedicated `add*_idct_dc` routines — they avoid the full butterfly chain; (c) when all coefficients are zero, callers can skip the add entirely (x264's skip/MB-level logic already does); (d) fuse `sub4x4_dct` and `quant_4x4` in a caller-side loop to keep residuals in registers if rewriting the pipeline; (e) on MSARev6 prefer aligned `ld/sh`/`lw/sw` over the unaligned fallbacks in `macros.h` (the header already selects these via `__mips_isa_rev >= 6`).
- **References:** MSA intrinsics specification (Imagination/MIPS "MSA Programmer's Guide"); x264's `common/dct.c` for the reference transform; ITU-T H.264 §8.5.1 (4x4 integer transform) and §8.5.3 (8x8 transform).

## 8. Verification checklist

1. For random 8-bit `src`/`ref` blocks, `sub4x4_dct_msa` output equals scalar `x264_dct4x4` output bit-for-bit.
2. `add4x4_idct_msa` on a known coefficient set, added to a known byte block, equals the C reference; destination bytes are in `[0,255]`; the source array is zero on return.
3. `dct4x4dc_msa` followed by `idct4x4dc_msa` reproduces `4 × original` (within 16-bit range) — confirms the `H·H=4I` property and output ordering.
4. `zigzag_scan_4x4_frame_msa` maps input `i` to output position per the standard frame zig-zag table; test with `dct[i]=i`.
5. `sub8x8_dct_dc_msa` on a constant-difference 8x8 block yields only a DC term in the first quadrant output (others zero after the Hadamard) with the expected magnitude.
6. 16x16 wrappers produce the same coefficient array as sixteen independent 4x4 calls with correct quadrant pointer arithmetic.
7. Run under UBSan/ASan with unaligned-but-valid pointers to confirm no out-of-bounds access from strided vector loads.
8. Compare MSA and C encoder bitstreams over a short clip: output must be bit-identical (same QP, same options).

## 9. Open questions

- UNKNOWN: whether `x264_add8x8_idct8_msa` intentionally leaves coefficients un-zeroed (observed in source) or whether callers zero them; verify against the generic 8x8 inverse implementation before assuming.
- UNKNOWN: exact output ordering of the two-level 8x16 DC Hadamard (`sub8x16_dct_dc_msa`) relative to the generic `x264_sub8x16_dct_dc`; the butterfly output permutation is non-obvious from the macro expansion and should be validated numerically against the C reference.
- UNKNOWN: behaviour for `i_qp` / coefficient values outside the normal encoder range; 16-bit wraparound in intermediate butterfly stages is not tested here.
- UNKNOWN: alignment assumptions of `LD4x4_SH`/`LD_SH8` — the macros use plain dereference casts, which on some MSA implementations require natural alignment; the public API does not document this.
