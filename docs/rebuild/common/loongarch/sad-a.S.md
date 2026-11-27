# `common/loongarch/sad-a.S` — LoongArch LSX/LASX sum-of-absolute-differences assembly

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\loongarch\sad-a.S`. Role: assembly implementations of single-reference SAD and the x3/x4 multi-reference SAD variants used by motion search, in LSX and LASX flavours. Compiled only for 8-bit builds.

## 1. Header

The title line above gives the source path and the file's role in one sentence; the adjacent bold marker identifies this document as a **dirty-side behavioural specification** written from the reference source and public literature. No reference source or near-verbatim pseudocode is reproduced.

## 2. Purpose

SAD is the innermost loop of x264's motion-estimation search: for each candidate motion vector, one or several block comparisons are performed. This file implements both the simple one-reference form and the batched forms that compare one source block against three or four references in a single pass, reusing loaded source data. LASX's 256-bit registers allow a full 16x16 row pair per instruction and halve the loop count for large blocks.

## 3. Public interface

Exports (prefixed by `ASM_REF`), declared in `loongarch/pixel.h.md`:

**Single SAD** — `pixel_sad_16x16_lsx`, `16x8_lsx`, `8x16_lsx`, `8x8_lsx`, `8x4_lsx`, `4x16_lsx`, `4x8_lsx`, `4x4_lsx`, and `pixel_sad_8x4_lasx` (only LASX single variant). All have signature `int32_t (uint8_t *src, intptr_t src_stride, uint8_t *ref, intptr_t ref_stride)`.

**x4 SAD** — LASX: `pixel_sad_x4_16x16_lasx`, `16x8_lasx`, `8x8_lasx`, `8x4_lasx`; LSX: `pixel_sad_x4_16x16_lsx`, `16x8_lsx`, `8x16_lsx`, `8x8_lsx`, `8x4_lsx`, `4x8_lsx`, `4x4_lsx`. Signature: `void (uint8_t *src, uint8_t *ref0, uint8_t *ref1, uint8_t *ref2, uint8_t *ref3, intptr_t ref_stride, int32_t sad[4])`. Source stride is implicitly `FENC_STRIDE`.

**x3 SAD** — LASX: `pixel_sad_x3_16x16_lasx`, `16x8_lasx`; LSX: the seven sizes listed in `pixel.h.md`. Same signature with three references and `sad[3]`.

## 4. State and data structures

No globals. Working accumulators are `v8u16` (LSX) or `xv8u16` (LASX) halfword vectors, one per reference in the x3/x4 kernels. Source data is loaded once per row group and reused against all references. Row loop counts are `height/2` for 16-wide LASX, `height/4` for 8-wide LSX, and `height/4` for 4-wide kernels using word loads plus vector inserts.

## 5. Algorithm / behaviour

### Single SAD

For 16-wide blocks, one or two 16-byte vectors per source row are loaded; for 8-wide, four rows are loaded as 8-byte vectors and packed down with `pickev.d`; for 4-wide, four 32-bit words are loaded via GPRs and inserted into a vector. Bytewise absolute differences are computed with `asub_u_b`/`xvasub_u_b`, then `hadd_u_h` pairwise-sums them into halfword lanes. A `v8u16` accumulator sums across rows; the final reduction uses GPR extracts and adds. LASX processes 32 bytes per vector, so the 16x16 LASX kernel handles two rows per iteration.

### x3 / x4 SAD

The source vector(s) for a row group are loaded once. For each of the three/four references, the corresponding row vectors are loaded with `ref_stride`, absolute differences are computed against the *same* source vectors, and separate halfword accumulators are updated. This reuses source loads and the address arithmetic. For widths below the vector width, the same packing/insertion technique as single SAD is used. At the end, each accumulator is horizontally reduced into the corresponding `sad[i]` output.

### Width/height dispatch

- 4-wide kernels load four GPR words per row group and build one 16-byte vector; four rows per iteration.
- 8-wide kernels load four 8-byte vectors per side and pack them into two 16-byte vectors per group.
- 16-wide kernels load one or two full vectors per row; LASX uses `xvld` with 32-byte offsets and strides `stride`, `2*stride`, `3*stride` precomputed in GPRs (`slli.d`, `add.d` at function entry).
- Tail rows when height is not a multiple of the unroll factor are handled by scalar loops at the end of each function (or by the C caller with a scalar fallback).

## 6. Dependencies

- **Calls:** `loongson_asm.S` register aliases and any dot-product macros used for SAD accumulation; `loongson_util.S` function macros and `FENC_STRIDE`.
- **Called by:** `x264_pixel_init()` in `common/pixel.c` through `loongarch/pixel.h`; motion search calls the x3/x4 variants via `x264_pixel_functions_t`.
- **Shared config:** `HIGH_BIT_DEPTH == 0`, `BIT_DEPTH`, `X264_CPU_LSX`/`LASX`.

## 7. Constants and tables

- No constant tables; all masks and strides are immediates or GPR-computed.
- Vector widths: 16 bytes (LSX), 32 bytes (LASX).
- Accumulator element width: 16 bits (unsigned).
- Source stride constant `FENC_STRIDE = 16` for x3/x4 helpers.

## 8. Performance notes

- **Hotness:** highest of all LoongArch kernels for inter frames; motion search may call x4 SAD thousands of times per frame.
- **SIMD status:** fully LSX/LASX; the x3/x4 batched forms avoid redundant source loads and are critical for search throughput.
- **Optimization tips:** (a) prefer the x4 variant over four single SAD calls — it saves source loads and address setup; (b) LASX variants process twice as many bytes per instruction — install them on LASX CPUs; (c) keep reference strides in GPRs computed once per call (the code already does `slli.d`/`add.d` at entry); (d) align source/reference pointers to 32 bytes for LASX; (e) for 4-wide blocks, the GPR-load-plus-insert path is cheaper than strided vector loads — keep it.
## 9. References

x264 `common/pixel.c`; motion-estimation literature; LoongArch LSX/LASX ISA manuals.

## 10. Verification checklist

1. For all eight single-SAD sizes, compare with scalar SAD on random and max (255) inputs.
2. x3/x4: verify each output slot against an independent scalar SAD call; confirm source stride is `FENC_STRIDE`.
3. Verify LSX and LASX variants of the same operation agree bit-for-bit.
4. Test odd reference strides and heights not divisible by the unroll factor to exercise tail handling.
5. Check that no accumulator overflow occurs for height 16 (max SAD = 16·16·255 = 65280, fits in a 16-bit halfword; larger accumulators must be reduced periodically).
6. ASan-style page-boundary test to catch over-reads from wide vector loads.
7. Full-encode bitstream equivalence between LoongArch SAD and C SAD builds.

## 11. Open questions

- UNKNOWN: exact tail handling for block heights not divisible by 4/2 in each kernel — inspect the scalar epilogues before relying on them for arbitrary heights.
- UNKNOWN: why only `8x4` has a LASX single-SAD variant — likely a deliberate minimal set; confirm from the dispatcher.
- UNKNOWN: whether the x3/x4 LASX variants for 16x16 use `FENC_STRIDE` or an explicit source stride parameter — the generic signature has no source stride, so confirm from the calling convention in `common/pixel.c`.
