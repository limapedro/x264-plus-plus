# `common/mips/predict.h` — MSA intra-prediction declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\predict.h`. Role: declares the MSA intra prediction functions installed by the generic prediction dispatcher.

## 1. Purpose

Intra prediction is invoked once per candidate mode per block, so a fast SIMD implementation reduces encoder latency measurably at slow presets where many modes are evaluated. This header exposes the MSA implementation set to `common/predict.c` with x264's standard `x264_template` name mangling. It is compiled only for 8-bit builds.

## 2. Public interface

All functions are name-mangled via `x264_template(...)` and take a single destination/source pointer `uint8_t *p_src` (the current block position in the reconstruction plane, from which neighbouring samples are addressed at negative offsets), except the 8x8 variants that also take the 36-byte edge array.

**16x16 luma predictions**
- `x264_intra_predict_dc_16x16_msa(p)` — DC from both above and left neighbours.
- `x264_intra_predict_dc_left_16x16_msa(p)` — DC from left only.
- `x264_intra_predict_dc_top_16x16_msa(p)` — DC from above only.
- `x264_intra_predict_dc_128_16x16_msa(p)` — constant 128 fill.
- `x264_intra_predict_hor_16x16_msa(p)` — replicate left column.
- `x264_intra_predict_vert_16x16_msa(p)` — replicate top row.
- `x264_intra_predict_plane_16x16_msa(p)` — H.264 plane prediction.

**8x8 luma predictions (block-relative edge array)**
- `x264_intra_predict_dc_4blk_8x8_msa(p)` — four separate 4x4 DC blocks (chroma-style DC prediction for an 8x8 luma block).
- `x264_intra_predict_hor_8x8_msa(p)`, `..._vert_8x8_msa(p)`, `..._plane_8x8_msa(p)` — horizontal/vertical/plane from the reconstruction plane's neighbours.
- `x264_intra_predict_ddl_8x8_msa(p, edge[36])`, `..._dc_8x8_msa(p, edge[36])`, `..._h_8x8_msa(p, edge[36])`, `..._v_8x8_msa(p, edge[36])` — edge-array-based diagonal-DC/horizontal/vertical variants used by the fast intra metric helpers.

**4x4 luma predictions**
- `x264_intra_predict_dc_4x4_msa(p)`, `..._hor_4x4_msa(p)`, `..._vert_4x4_msa(p)`.

Contracts: each routine writes only the block at `p` with reconstruction stride `FDEC_STRIDE` (32 bytes). Neighbouring samples are read at `p−1` (left) and `p−FDEC_STRIDE` (above). The edge-array variants read exclusively from the caller-supplied `edge[36]` and write to `p`. No function returns a value.

## 3. State and data structures

No state. Include guard `X264_MIPS_PREDICT_H`. The edge array (36 bytes) is the standard x264 intra-neighbour buffer; the implementation uses offsets 16 (top row and top-left corner), 14 (left column), and 7 (left samples for 8x8 DC), consistent with the generic predictor's layout. Output uses `FDEC_STRIDE` pitch; note that `FDEC_STRIDE` (32) is twice the block width for 8x8 and 16x16 blocks, leaving room for the right-neighbour placeholder bytes used by later stages.

## 4. Algorithm / behaviour

The header performs no computation. Semantics of each mode follow H.264 §8.3.1 (4x4/16x16 luma) and §8.3.2 (chroma 8x8):

- **Vertical:** copy the row above into every output row.
- **Horizontal:** replicate each left-column sample across its row.
- **DC:** average the available neighbours with rounding: both available → `(sum_top + sum_left + N/2) >> (log2(2N))`, one available → `(sum + N/2) >> log2(N)`, neither → 128. `N` is the block width (4 or 16).
- **Plane:** compute the two slope terms `H = Σ(i+1)·(P[8+i] − P[6−i])` and `V = Σ(i+1)·(P[8+i]·stride − P[6−i]·stride)`, then `pred(x,y) = clip((A + B·(x−3) + C·(y−3) + 16) >> 5)` with `A = 16·(P[−1]+P[7·stride+7]+1)`, `B = (5·H+32)>>6`, `C = (5·V+32)>>6`. The MSA code computes these with shuffle-mask-based pairwise differences and word-lane accumulators; see `predict-c.c.md` for the exact lane mapping.
- **DDL (diagonal-down-left):** each output byte is `(2·edge[i] + edge[i+1] + edge[i+2] + 2) >> 2` with boundary replication, implemented via byte shuffles and pairwise horizontal adds.

## 5. Dependencies

- **Included by:** `common/mips/predict-c.c`, `common/mips/pixel-c.c` (the intra SAD/SATD combined helpers), and `common/predict.c`.
- **Requires:** `x264_template`, `uint8_t`, `FDEC_STRIDE`, and the `x264_predict_t`/`x264_predict8x8_t` typedefs at the dispatcher call site.
- Note: unlike the LoongArch version, this MIPS header declares no `*_init_mips` functions; the generic predictor installs these symbols directly by name.

## 6. Constants and tables

None in the header. The shuffle masks used by plane/DDL and the rounding constants (16, 32, 5, 128) are implementation details documented in `predict-c.c.md`.

## 7. Performance notes

No runtime cost. For rebuild, the important performance property is that all helpers write contiguous full rows (16 bytes for 4x4, 64/128 bits per store for 8x8/16x16), allowing 128-bit stores without read-modify-write of neighbouring macroblock data.

## 8. Verification checklist

1. Compile header alone in 8-bit and 10-bit builds; check declarations disappear in the latter.
2. Compare each MSA predictor output against the scalar reference for a deterministic neighbour pattern (e.g. `edge[i]=i`) across all block sizes.
3. Confirm all 17 declarations resolve to symbols in `predict-c.c`.
4. Verify no out-of-bounds writes: the destination stride is `FDEC_STRIDE`, not the block width.

## 9. Open questions

- UNKNOWN: exact correspondence between the `_8x8_msa(p, edge)` functions and the generic `x264_predict8x8_t` callback signature — verify the dispatcher passes `p_edge` in the same slot.
- UNKNOWN: whether some of the declared helpers are also used outside prediction (e.g. directly by analyse.c) in this x264 version.
