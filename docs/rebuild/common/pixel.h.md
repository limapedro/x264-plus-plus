# `common/pixel.h` — pixel metric dispatch and block-size vocabulary

**Role:** defines the block-size enum/size tables, comparison callback types, the
`x264_pixel_function_t` vtable and the exported metric entry points.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/pixel.h`.
- Role in one sentence: fix the block-size vocabulary and the complete calling contract for
  all distortion metrics and batched intra comparisons.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

The encoder measures distortion for many block shapes (16x16 down to 2x2 chroma) and through
many metrics. A dense enum plus a vtable keeps every call site uniform and lets `pixel.c`
install the fastest implementation per CPU. The header also documents two crucial alignment
contracts that determine which SIMD variant may be used.

---

## 3. Public interface

Callback types:

- `x264_pixel_cmp_t` — `int (*)(pixel*, intptr_t, pixel*, intptr_t)`.
- `x264_pixel_cmp_x3_t` — `void (*)(pixel*, pixel*, pixel*, pixel*, intptr_t, int[3])`.
- `x264_pixel_cmp_x4_t` — `void (*)(pixel*, pixel*, pixel*, pixel*, pixel*, intptr_t, int[4])`.

Enum `PIXEL_*` (values 0..11):
`16x16=0, 16x8=1, 8x16=2, 8x8=3, 8x4=4, 4x8=5, 4x4=6`, then subsampled-chroma-only
sizes `4x16=7` (4:2:2), `4x2=8`, `2x8=9` (4:2:2), `2x4=10`, `2x2=11`.

Tables:

- `x264_pixel_size[12]` — `{w,h}` for each enum value: (16,16),(16,8),(8,16),(8,8),(8,4),
  (4,8),(4,4),(4,16),(4,2),(2,8),(2,4),(2,2).
- `x264_size2pixel[5][5]` — maps `(height-index, width-index)`-style dimensions to a
  `PIXEL_*` value; e.g. 4x4→`PIXEL_4x4`, 8x4→`PIXEL_8x4`, 16x8→`PIXEL_16x8`.
- `x264_luma2chroma_pixel[4][7]` — maps luma block sizes to the corresponding chroma size for
  each chroma format (rows: monochrome/4:2:0/4:2:2/4:4:4). E.g. for 4:2:0, 16x16→8x8,
  16x8→8x4, 8x8→4x4; for 4:2:2, 16x16→8x16; for 4:4:4 the sizes are unchanged.

`x264_pixel_function_t` members (all function pointers):

- Metrics: `sad[8]`, `ssd[8]`, `satd[8]`, `ssim[7]`, `sa8d[4]`, indexed by `PIXEL_*`.
- Selection aliases: `mbcmp[8]`, `mbcmp_unaligned[8]`, `fpelcmp[8]`, `sad_aligned[8]`.
- Batched: `fpelcmp_x3[7]`, `fpelcmp_x4[7]`, `sad_x3[7]`, `sad_x4[7]`, `satd_x3[7]`,
  `satd_x4[7]` (arrays of 7 cover the luma sizes through 4x4).
- Special: `vsad`, `asd8`, `sa8d_satd[1]`, `var[4]`, `var2[4]`, `hadamard_ac[4]`,
  `ssd_nv12_core`, `ssim_4x4x2_core`, `ssim_end4`.
- Successive elimination: `ads[7]`, signature
  `(int enc_dc[4], uint16_t *sums, int delta, uint16_t *cost_mvx, int16_t *mvs, int width,
  int thresh)`.
- Intra comparisons: `intra_mbcmp_x3_*`, `intra_satd_x3_*`, `intra_sad_x3_*` for 16x16, 4x4,
  chroma 8x8, chroma 8x16 and 8x8 luma (the latter taking `edge[36]`); and the
  `intra_*_x9_4x4`/`x9_8x8` family which may be NULL.

Export functions:

- `void x264_pixel_init(uint32_t cpu, x264_pixel_function_t *pixf)`
- `void x264_pixel_ssd_nv12(pf, pix1, i_pix1, pix2, i_pix2, i_width, i_height, *ssd_u, *ssd_v)`
- `uint64_t x264_pixel_ssd_wxh(pf, pix1, i_pix1, pix2, i_pix2, i_width, i_height)`
- `float x264_pixel_ssim_wxh(pf, pix1, stride1, pix2, stride2, width, height, void *buf, int *cnt)`
- `int x264_field_vsad(x264_t *h, int mb_x, int mb_y)`

All symbols use the `x264_template` naming macro.

---

## 4. State and data structures

- The vtable is zero-initialized by `x264_pixel_init` and then fully populated; some entries
  (`intra_*_x9_*`) may legitimately remain NULL when no SIMD implementation exists.
- `mbcmp`/`fpelcmp` are *aliases*: they point to either the SAD or SATD implementation
  according to configuration, so callers need not know which metric is selected.
- `sad_aligned` is a variant that assumes both the first argument and its stride are aligned;
  `mbcmp_unaligned` is the unaligned version used for subpel refinement.
- `var`/`var2`/`hadamard_ac` arrays are indexed by block size (up to four entries for the
  supported sizes).
- Lifetime: the table lives in `x264_t` and is filled once at startup.

---

## 5. Algorithm / behaviour

The header encodes three essential contracts:

1. **Alignment:** "SSD assumes all args aligned; other cmp functions assume first arg
   aligned." This dictates which pointer pairs a caller may pass and which SIMD variant the
   init function should install.
2. **Indexing:** arrays sized 8 cover all luma block shapes through 4x4; arrays sized 7 cover
   through 4x8 (excluding some tiny chroma shapes); `ssim[7]` similarly. The `ads[7]` array
   has only three distinct implementations aliased across sizes.
3. **Intra semantics:** the `intra_*_x3_*` helpers build a prediction and immediately measure
   it; the 8x8 luma ones consume a prepared `edge[36]`. The x9 helpers may be NULL, in which
   case callers fall back to "predict then SATD/SAD" (per the comment in `pixel.c`).

---

## 6. Dependencies

- Types `pixel`, `uint64_t`, `intptr_t`, `x264_t` from `common.h`/`base.h`.
- Implemented in `pixel.c`; consumed by `encoder/me.c`, `encoder/analyse.c`, `encoder/rdo.c`,
  `encoder/macroblock.c`, `common/frame.c`.
- The size tables are shared with the MC vtable indexing (`mc.h` uses the same `PIXEL_*` enum).

---

## 7. Constants and tables

- Enum values 0..11 and the three static tables described in section 3.
- Array sizes 8/7/4/1 reflect the number of supported block shapes per metric family.

---

## 8. Performance notes

- Keeping `mbcmp`, `fpelcmp` and their batched variants in the same table as the raw metrics
  means configuration changes are just pointer swaps — no branch in the search loop.
- The size tables are `static const` and fit in a few cache lines.
- Callers should use `sad_aligned` when both pointers are guaranteed aligned; otherwise use
  `sad` and let the implementation handle misalignment.
- See `pixel.c.md` section 8 for SIMD guidance and literature.

---

## 9. References

- x264 public source: `common/pixel.h`, `common/pixel.c`, `common/x86/pixel-a.asm`.
- SSIM reference: Wang et al., IEEE TIP 2004.

---

## 10. Verification checklist

1. Assert enum values and `x264_pixel_size` entries match the documented list.
2. Verify `x264_size2pixel` for representative dimensions and `x264_luma2chroma_pixel` for all
   four chroma formats.
3. After `x264_pixel_init(0, &pixf)`, assert every non-optional entry is non-null; document
   which x9 entries are NULL on the test platform.
4. Check that `mbcmp`/`fpelcmp` point to a metric of the expected family after init for a
   given configuration.
5. Validate the alignment contracts by passing deliberately misaligned pointers to a `sad`
   entry (should work) and reading the documented behavior for `ssd` (requires alignment).
6. Compare the table produced with `cpu=0` to the native-CPU table functionally.

---

## 11. Open questions

- `UNKNOWN`: the exact policy that chooses SAD vs SATD for `mbcmp`/`fpelcmp` lives in the
  encoder configuration, not this header.
- `UNKNOWN`: which x9 intra helpers exist on a given build is platform-dependent; callers
  must handle NULL.
