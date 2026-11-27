# `common/predict.h` — intra prediction interface and mode enums

**Role:** declares predictor function-pointer types, the intra mode numbering for every
block size, the mode fix-up tables, and the per-size initialization entry points.
**Status:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/predict.h`.
- Role in one sentence: define the vocabulary (mode enums, fix-up maps, callback types) used
  by `predict.c`, `pixel.c` and the macroblock analyser for intra prediction.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

The header pins down mode numbering so that the encoder's analysis code, the entropy coder,
the SIMD predictors and the metric helpers all agree on which number means which predictor.
It also exposes the small lookup tables that make an intra mode legal when its required
neighbours are unavailable — a key part of H.264's in-loop prediction semantics.

---

## 3. Public interface

Types:

- `x264_predict_t` — `void (*)(pixel *src)`, a predictor that reads its neighbours directly
  from the decoded picture around `src`.
- `x264_predict8x8_t` — `void (*)(pixel *src, pixel edge[36])`, a predictor that reads a
  prepared 36-byte edge array.
- `x264_predict_8x8_filter_t` — `void (*)(pixel *src, pixel edge[36], int i_neighbor,
  int i_filters)`, the edge builder for the 8x8 luma predictors.

Mode enums (numeric values are part of the contract):

- `intra_chroma_pred_e`: `DC=0, H=1, V=2, P=3`, then fallback values `DC_LEFT=4`,
  `DC_TOP=5`, `DC_128=6`. Used for both 8x8 and 8x16 chroma.
- `intra16x16_pred_e`: `V=0, H=1, DC=2, P=3`, fallbacks `DC_LEFT=4`, `DC_TOP=5`,
  `DC_128=6`.
- `intra4x4_pred_e` and `intra8x8_pred_e` share numbering: `V=0, H=1, DC=2, DDL=3, DDR=4,
  VR=5, HD=6, VL=7, HU=8`, fallbacks `DC_LEFT=9, DC_TOP=10, DC_128=11`. The comment
  "must use the same numbering as intra4x4_pred_e" on the 8x8 enum is a hard constraint.

Fix-up tables:

- `x264_mb_chroma_pred_mode_fix[7]`: maps modes 0..3 to themselves and modes 4..6 to `DC`.
- `x264_mb_pred_mode16x16_fix[7]`: maps V/H/DC/P to themselves and all fallbacks to `DC`.
- `x264_mb_pred_mode4x4_fix[13]`: indexed by `(mode+1)` via the macro
  `x264_mb_pred_mode4x4_fix(t)`; entry 0 is `-1` (meaning "no valid prediction"), entries
  1..9 are V,H,DC,DDL,DDR,VR,HD,VL,HU, and entries 10..12 map the DC fallbacks to `DC`.

Initialization functions:

- `void x264_predict_16x16_init(uint32_t cpu, x264_predict_t pf[7])`
- `void x264_predict_8x8c_init(uint32_t cpu, x264_predict_t pf[7])`
- `void x264_predict_8x16c_init(uint32_t cpu, x264_predict_t pf[7])`
- `void x264_predict_4x4_init(uint32_t cpu, x264_predict_t pf[12])`
- `void x264_predict_8x8_init(uint32_t cpu, x264_predict8x8_t pf[12],
  x264_predict_8x8_filter_t *predict_filter)`

Portable implementations exported individually (for metrics and testing):
`x264_predict_16x16_{dc,h,v,p}_c`, `x264_predict_4x4_{dc,h,v}_c`,
`x264_predict_8x8_{dc,h,v}_c`, `x264_predict_8x8c_*_c`, `x264_predict_8x16c_*_c`.

---

## 4. State and data structures

- The `pf` arrays are pure dispatch tables: the caller supplies storage; the init function
  fills it. No hidden state is retained.
- The enum values double as array indices; e.g. `pf[I_PRED_16x16_V]` is the vertical
  predictor. The fallback slots are filled even though analysis normally selects them only
  through the fix table.
- The 8x8 edge array (`pixel edge[36]`) is caller-owned scratch; its layout contract is
  documented in `predict.c.md`.
- Availability bit flags (`MB_LEFT`, `MB_TOP`, `MB_TOPRIGHT`, `MB_TOPLEFT`) are defined in
  `macroblock.h` and are passed to the 8x8 filter, not stored here.

---

## 5. Algorithm / behaviour

The header defines no algorithms, but encodes these behavioural rules:

1. **Mode availability:** a mode may only be used if every neighbour in
   `x264_pred_i4x4_neighbors[mode]` is available. Otherwise the mode is remapped through the
   fix tables to a legal predictor (almost always a DC variant).
2. **Fallback numbering:** modes 4..6 (16x16/chroma) and 9..11 (4x4/8x8) are not transmitted
   in the bitstream; they exist so the same callback interface can express "DC using only
   left", "DC using only top" and "DC using mid-grey".
3. **Signed fix table trick:** the 4x4 fix table has a leading `-1` because the macro indexes
   it with `mode+1`; an input of `-1` (no mode) therefore yields the sentinel.
4. **Dispatch:** initialization installs the C functions first and then lets architecture
   overrides replace entries; every slot ends up non-null.
5. **8x8 special case:** unlike the other sizes, 8x8 luma needs an extra filter callback
   because its directional modes operate on a filtered neighbour array.

---

## 6. Dependencies

- Depends on `pixel`, `uint8_t`, `uint32_t` and `int8_t` typedefs from `common.h`/`base.h`.
- Included by `predict.c`, `pixel.c`, `encoder/macroblock.c`, `encoder/analyse.c`, and the
  architecture-specific predictor headers.
- The fix-up tables are consumed by the bitstream/mode-decision code to sanitize modes.

---

## 7. Constants and tables

- Enum values listed in section 3.
- Three fix-up arrays with the exact mappings listed in section 3.
- The 36-byte edge-array size is a fixed constant shared by all 8x8 luma predictors.

---

## 8. Performance notes

- Because the enums are dense small integers, predictors can be selected by a single
  indirect call through `pf[mode]` with no switch.
- Keeping the fix tables in `rodata` (they are `static const`) makes mode sanitization a
  single byte lookup.
- The 8x8 filter pointer is separated from the predictor array so it can be dispatched
  independently by SIMD without disturbing the predictor slots.
- See `predict.c.md` section 8 for the predictor-level optimization discussion and literature.

---

## 9. References

- ITU-T H.264 clause 8.3 and Table 8-2/8-3 (intra prediction modes and neighbour
  availability).
- x264 public source: `common/predict.h`, `common/macroblock.h`.

---

## 10. Verification checklist

1. Check enum values by asserting the documented numbers (0..3/4..6 and 0..8/9..11).
2. Verify each fix table maps fallback modes to DC and leaves real modes unchanged; check the
   `mode+1` indexing macro for the 4x4 table including the `-1` input.
3. Compile a unit that fills each `pf` array via every init function and assert all entries
   non-null, including the 8x8 filter pointer.
4. Verify that 4x4 and 8x8 mode enums have identical numeric values (call a predictor
   selected by the same index on both block sizes).
5. On a SIMD machine, compare the function pointers chosen by `cpu=0` versus the native mask
   for functional equivalence using the predictors from `predict.c.md` test vectors.

---

## 11. Open questions

- `UNKNOWN`: whether any future mode (e.g. additional 8x8 directional variants) changes the
  array sizes; the documented tables are exactly 7 and 12 entries.
- `UNKNOWN`: the precise meaning of `-1` in the 4x4 fix table at call sites outside this
  header is defined by the analyser, not by this file.
