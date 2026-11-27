# `common/loongarch/pixel.h` — LoongArch pixel-metric declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\pixel.h`. Role: declares LSX/LASX distortion metrics and the combined intra-prediction metric helpers implemented in `loongarch/pixel-a.S`, `sad-a.S`, and `loongarch/pixel-c.c`.

## 1. Purpose

Exposes all LoongArch pixel-distortion primitives to the generic dispatcher, in both 128-bit LSX and 256-bit LASX flavours. The set is broader than the MIPS port: it includes 32-byte-wide SAD x4 for large blocks, LASX SATD/SSD/SA8D variants, and a separate `hadamard_ac_8x8_*` core symbol that the C wrappers in `pixel-c.c` call.

## 2. Public interface

**SATD** — `x264_pixel_satd_WxH_lsx` for eight sizes (4x4, 4x8, 4x16, 8x4, 8x8, 8x16, 16x8, 16x16), but the 4x4 implementation is labelled `_lsx` only; LASX variants exist for 4x8, 4x16, 8x4, 8x8, 8x16, 16x8, 16x16 (not 4x4). Signature `int32_t (uint8_t*, intptr_t, uint8_t*, intptr_t)`.

**SAD single** — `x264_pixel_sad_WxH_lsx` for eight sizes; `x264_pixel_sad_8x4_lasx` only for LASX. Same signature as SATD.

**SAD x4** — `x264_pixel_sad_x4_WxH_lsx` for 16x16, 16x8, 8x16, 8x8, 8x4, 4x8, 4x4; `x264_pixel_sad_x4_WxH_lasx` for 16x16, 16x8, 8x8, 8x4.

**SAD x3** — `x264_pixel_sad_x3_WxH_lsx` for 16x16, 16x8, 8x16, 8x8, 8x4, 4x4, 4x8; `x264_pixel_sad_x3_16x16_lasx` and `16x8_lasx`.

**Hadamard/AC** — core `x264_hadamard_ac_8x8_lsx/_lasx` and public `x264_pixel_hadamard_ac_WxH_lsx/_lasx` for 8x8, 8x16, 16x8, 16x16 (some sizes LSX-only, see declared set). Public signature `uint64_t (uint8_t*, intptr_t)`.

**SSD** — `x264_pixel_ssd_WxH_lsx` for eight sizes; `x264_pixel_ssd_16x16/16x8/8x16/8x8_lasx`.

**SA8D** — `x264_pixel_sa8d_8x8_lsx`/`16x16_lsx` and `_lasx` counterparts.

**Variance** — `x264_pixel_var_16x16/8x16/8x8_lsx`, `x264_pixel_var2_8x16/8x8_lsx` and `_lasx` variants. The `var2` signature here is `int32_t (uint8_t*, uint8_t*, int32_t ssd[2])` — note it lacks explicit strides (unlike MIPS), using the source pointers directly.

**Intra combined** — `x264_intra_sad_x3_*_lsx`, `x264_intra_satd_x3_*_lsx/_lasx`, `x264_intra_sa8d_x3_8x8_lsx/_lasx` with the same argument patterns as the MIPS header (`p_edge[36]` variant for 8x8).

All names are mangled by `x264_template(...)`; there is an `#if !HIGH_BIT_DEPTH` guard around the file body in the implementation, not around individual declarations here.

## 3. State and data structures

No state. Include guard `X264_LOONGARCH_PIXEL_H`. The `p_edge[36]` array is the standard x264 intra neighbour buffer; LASX intra helpers use the same offsets as the LSX versions because they call the LSX predictor for some modes.

## 4. Algorithm / behaviour

Declarations only. Metric definitions match the generic x264 C versions:

- SAD: `Σ|src−ref|`, 32-bit result.
- SSD: `Σ(src−ref)²`.
- SATD: `Σ|Hadamard(residual)| >> 1`.
- SA8D: 8x8 Hadamard energy with `(sum+2)>>2` scaling.
- Hadamard-AC: packed low word AC (÷2), high word DC (÷4), produced by the C wrappers from the 8x8 core's `uint64_t` result via `((sum >> 34) << 32) + ((uint32_t)sum >> 1)`.
- Variance: packed `sum | (sumsq << 32)`; `var2` adds the `E[x]²` correction with a `log2(n)+1` shift.

The assembly implementations use LSX `vr*` / LASX `xr*` registers, `xvdp2.h.bu.b` dot-product helpers from `loongson_asm.S`, and Hadamard butterfly macros (`HADAMARD4_V`, `SUMSUB_BADC`, `HADAMARD_1/2`) defined in `pixel-a.S`.

## 5. Dependencies

- **Included by:** `common/loongarch/pixel-a.S`, `sad-a.S`, `pixel-c.c`, and `common/pixel.c`.
- **Requires:** `x264_template`, `int32_t`, `uint64_t`, `intptr_t`, `uint8_t`, `pixel`, `ALIGNED_ARRAY_16`.
- **Cross-file:** `loongarch/predict.h` must be visible where the intra combined helpers are implemented; `pixel-c.c` includes it.

## 6. Constants and tables

None in the header. Implementation constants are documented in `pixel-a.S.md` and `sad-a.S.md`.

## 7. Performance notes

No runtime cost. For rebuild, preserve the mixed LSX/LASX declaration coverage — a missing LASX declaration would prevent the dispatcher from installing the wider variant for that block size.

## 8. Verification checklist

1. Include header alone and compile under both bit-depth configurations.
2. Verify each declared symbol exists in one of `pixel-a.S`, `sad-a.S`, or `pixel-c.c`.
3. Check the `var2` signature difference from MIPS — a wrong stride assumption is a likely source of runtime corruption.
4. Confirm the set of LASX SAD/SATD/SSD sizes matches what the dispatcher installs.

## 9. Open questions

- UNKNOWN: why 4x4 SATD/SA8D have no LASX declarations — likely because one 4x4 block fits a 128-bit register and widening brings no benefit; confirm.
- UNKNOWN: exact `ssd[2]` layout in `var2` (whether element 1 is used) — validate against the generic `x264_pixel_var2_t` typedef.
