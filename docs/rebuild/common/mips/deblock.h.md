# `common/mips/deblock.h` — MSA deblocking declarations

**Dirty-side behavioural specification.** Source path: `E:\_x264_dirty_stash\x264-source\common\mips\deblock.h`. Role: declares the MSA loop-filter (deblocking) entry points used by the architecture dispatcher.

## 1. Purpose

Provides the declaration-only interface for the SIMD deblocking filter implemented in `mips/deblock-c.c`. The deblocking stage operates on reconstructed macroblock edges and is a significant fraction of per-frame time at medium/high quality, so x264 installs these 128-bit MSA implementations when the CPU supports MSA. All declarations are wrapped in `#if !HIGH_BIT_DEPTH` because the algorithms are 8-bit-specific.

## 2. Public interface

Each declaration is name-mangled through `x264_template(...)`. Functions:

**Inter (non-intra) luma/chroma vertical and horizontal edge filters**
- `void x264_deblock_v_luma_msa(uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)`
- `void x264_deblock_h_luma_msa(uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)`
- `void x264_deblock_v_chroma_msa(uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)`
- `void x264_deblock_h_chroma_msa(uint8_t *pix, intptr_t stride, int alpha, int beta, int8_t *tc0)`

**Intra (stronger) edge filters without `tc0`**
- `void x264_deblock_v_luma_intra_msa(uint8_t *pix, intptr_t stride, int alpha, int beta)`
- `void x264_deblock_h_luma_intra_msa(uint8_t *pix, intptr_t stride, int alpha, int beta)`
- `void x264_deblock_v_chroma_intra_msa(uint8_t *pix, intptr_t stride, int alpha, int beta)`
- `void x264_deblock_h_chroma_intra_msa(uint8_t *pix, intptr_t stride, int alpha, int beta)`

**Boundary-strength computation**
- `void x264_deblock_strength_msa(uint8_t nnz[X264_SCAN8_SIZE], int8_t ref[2][X264_SCAN8_LUMA_SIZE], int16_t mv[2][X264_SCAN8_LUMA_SIZE][2], uint8_t bs[2][8][4], int mvy_limit, int bframe)`

Contracts: the filter functions modify `pix` in place along a 16-pixel (luma) or 8-pixel (chroma) edge with row pitch `stride`. `alpha`/`beta` are the standard loop-filter thresholds; `tc0` points to four per-4x8-segment clipping values, where a negative value disables filtering for that segment. `deblock_strength_msa` writes the 2×8×4 boundary-strength array, with `bs[dir][edge][quarter]`.

## 3. State and data structures

No state. Declarations only; include guard `X264_MIPS_DEBLOCK_H`. The array dimensions used in the strength prototype come from generic headers:

- `X264_SCAN8_SIZE` — size of the scan-8 macroblock-indexed arrays (including chroma/dummy slots).
- `X264_SCAN8_LUMA_SIZE` — number of 4x4 luma blocks addressable through scan-8 indices.
- `bs[2][8][4]` — boundary strength indexed by direction (0 = vertical edges, 1 = horizontal edges in x264's convention), then four edges across the macroblock, then four quarters of each 16-pixel edge.

## 4. Algorithm / behaviour

Nothing is executed here. The header intentionally exposes only the externally visible functions; the many static edge-filter helpers in `deblock-c.c` (luma intra hor/ver, chroma interleaved intra hor/ver, inter edge hor/ver, and the SIMD strength kernel) are private.

The name mangling via `x264_template` maps each `*_msa` base name to the bit-depth-specific symbol, exactly as in `dct.h`. Because the filter API includes `int8_t *tc0` that is read but not written, and `alpha`/`beta` passed by value, all entry points are re-entrant and safe for frame-parallel threading as long as callers pass non-overlapping edge buffers.

## 5. Dependencies

- **Included by:** `common/mips/deblock-c.c` and `common/deblock.c` (which installs the pointers when `X264_CPU_MSA` is set).
- **Requires:** `X264_SCAN8_SIZE`, `X264_SCAN8_LUMA_SIZE` from `common/macroblock.h`/`common/frame.h` (transitively via `common/common.h` in the dispatcher); `x264_template` from the build config; `intptr_t` from `<stdint.h>`.
- gated by `HIGH_BIT_DEPTH == 0`.

## 6. Constants and tables

None in this header. The threshold semantics (`alpha`, `beta`, `tc`) are defined by the H.264 deblocking formula; see `deblock-c.c.md` for the exact rounding and clipping used by the MSA kernels.

## 7. Performance notes

No runtime cost. For rebuild, note that the four `*_intra_*` functions omit the `tc0` parameter and use a different (stronger) filter path, while the four inter functions receive per-quarter `tc` values. Allocating them separate slots in the dispatcher avoids a branch per edge.

## 8. Verification checklist

1. Include the header in isolation under an 8-bit config and a 10-bit config; verify the declarations vanish in the latter.
2. Confirm each declared symbol exists in the compiled `deblock-c.c` object.
3. Verify the strength-function prototype's array dimensions match the generic `x264_deblock_strength_t` typedef — a mismatch here is a classic source of stack corruption.
4. Check that no `static` helper is accidentally exported.

## 9. Open questions

- UNKNOWN: exact meaning of `dir` indices in `bs[2][8][4]` at the dispatcher call site; the naming convention in the implementation reads `bs[1]` first for horizontal edges, so verify against `common/deblock.c` before relying on it.
- UNKNOWN: whether this header is included directly by any third-party code outside x264's own tree.
