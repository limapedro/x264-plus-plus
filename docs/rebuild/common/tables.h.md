# `common/tables.h` - constant table declarations

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\tables.h` (mirrored at `common/tables.h`). Role in one sentence: declares all fixed lookup tables shared by analysis, quantisation, CABAC, rate distortion, motion compensation and CAVLC, plus the `vlc_t` element type. Marker: **dirty-side behavioural specification**.

## 2. Purpose

The encoder trades memory for speed by precomputing non-linear functions (log/exp approximations, lambda curves, CABAC state transitions, VLC codes, scaling weights) into flat arrays. This header is the single catalogue of those arrays, so consumers do not need to know where each is defined and the build can keep them in one translation unit.

## 3. Public interface

`typedef struct { uint8_t i_bits; uint8_t i_size; } vlc_t;` - a fixed-length binary code: the right-aligned code bits and the number of bits. Used by all static CAVLC tables.

Declared groups (exact declarations are in the source; semantics below):

- Public levels: `X264_API extern const x264_level_t x264_levels[]` - a zero-terminated array of H.264 level limit records.
- Math: `x264_exp2_lut[64]` (uint8), `x264_log2_lut[128]` (float), `x264_log2_lz_lut[32]` (float).
- Lambda: `x264_lambda_tab[QP_MAX_MAX+1]` (uint16), `x264_lambda2_tab[QP_MAX_MAX+1]` (int), `x264_trellis_lambda2_tab[2][QP_MAX_MAX+1]` (int), `x264_chroma_lambda2_offset_tab[MAX_CHROMA_LAMBDA_OFFSET+1]` (uint16). `QP_MAX_MAX = 51+6*2+18 = 81`, `MAX_CHROMA_LAMBDA_OFFSET = 36`.
- Half-pel reference selection: `x264_hpel_ref0[16]`, `x264_hpel_ref1[16]` (uint8).
- Custom quant matrices: JVT defaults `x264_cqm_jvt4i[16]`, `jvt4p[16]`, `jvt8i[64]`, `jvt8p[64]`, `flat16[64]`, an array of eight pointers `x264_cqm_jvt[8]`, plus AVCIntra preset lists for several resolutions and profiles.
- DCT decimation: `x264_decimate_table4[16]`, `x264_decimate_table8[64]` (uint8).
- Trellis weights: `x264_dct4_weight_tab[16]`, `x264_dct8_weight_tab[64]`, and squared/inverse variants `x264_dct4_weight2_tab`, `x264_dct8_weight2_tab` (uint32).
- CABAC: `x264_cabac_context_init_I[1024][2]` and `x264_cabac_context_init_PB[3][1024][2]` (int8), `x264_cabac_range_lps[64][4]`, `x264_cabac_transition[128][2]`, `x264_cabac_renorm_shift[64]` (uint8), `x264_cabac_entropy[128]` (uint16).
- CABAC RDO context offsets: `x264_significant_coeff_flag_offset_8x8[2][64]`, `x264_last_coeff_flag_offset_8x8[63]`, `x264_coeff_flag_offset_chroma_422_dc[7]`, `x264_significant_coeff_flag_offset[2][16]`, `x264_last_coeff_flag_offset[2][16]`, `x264_coeff_abs_level_m1_offset[16]`, `x264_count_cat_m1[14]`.
- CAVLC static codes: `x264_coeff0_token[6]`, `x264_coeff_token[6][16][4]`, `x264_total_zeros[15][16]`, `x264_total_zeros_2x2_dc[3][4]`, `x264_total_zeros_2x4_dc[7][8]`, `x264_run_before_init[7][16]` (all `vlc_t`).
- Scratch: `extern uint8_t x264_zero[1024]`, a 64-byte-aligned block of zeros.

## 4. State and data structures

All objects are `const` (except `x264_zero`) and have static storage duration; no lifecycle functions exist. The `x264_level_t` records are the only publicly exported ones. `x264_zero` is writable scratch used as a convenient all-zero source for SIMD fills; callers must not rely on it retaining zeros after use if any code writes to it (the reference declares it non-const, see Open Questions).

Index-space conventions: lambda tables are indexed by QP including the bit-depth offset and the extra 18 emergency QPs; chroma-lambda offsets by the configured chroma QP offset (possibly negative, clamped at the call site); CQM pointers by preset and list index; VLC tables by frame syntax categories (`nC`, total coefficients, trailing ones, total zeros, run length).

## 5. Algorithm / behaviour

Consumers use these arrays as pure functions. Examples: `x264_log2` in `base.h` uses the two log tables; `x264_exp2fix8` uses the exp table; rate control looks up lambda by QP; CAVLC writers index `coeff_token[nC][total-1][trailing]`; CABAC initialisation reads the context-init pairs and converts them to initial states; RDO coefficient coding adds the RDO offsets to base context indices; half-pel reference tables map a 4-bit MV fraction index to the two neighbouring reference positions used for interpolation.

The arrays are grouped so that all read-only data lives in one translation unit, improving locality and allowing the linker to place them together. Several tables are deliberately *padded* (e.g. `significant_coeff_flag_offset_8x8[2][64]` where only 63 coefficients exist, and the 16-entry `x264_*_offset` tables where only 14 are meaningful) so SIMD code can index without bounds checks.

## 6. Dependencies

Included by `base.h` and by all algorithm modules. Depends on `x264.h` for `x264_level_t` and `X264_API`, and on `osdep.h` for `ALIGNED_64`. Implemented by `tables.c`; no runtime dependencies.

## 7. Constants and tables

`QP_MAX_MAX = 81`, `MAX_CHROMA_LAMBDA_OFFSET = 36`, `NUM` of CQM lists = 8, CABAC context count = 1024, CAVLC context categories = 6, level table size = 128. Semantics and derivations are in `tables.c.md`.

## 8. Performance notes

- **Cache layout:** keep the hottest tables (lambda, CABAC entropy, hpel refs) small and contiguous; the large CAVLC `coeff_token` table is roughly 6*16*4*2 = 768 bytes and fits in L1.
- **False sharing / allocation:** read-only static data is shared cleanly; `x264_zero` is the only writable array and must not be used concurrently for different purposes.
- **Dispatch hierarchy:** these tables are CPU-independent; SIMD kernels consume the same numbers as C kernels, which is what makes bitstreams identical across architectures.
- Public references: ITU-T H.264 tables for CABAC/CAVLC; Drepper (2007) on read-only table locality.

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10, Annex A (levels), clause 9.2 (CAVLC), 9.3 (CABAC), 8.5 (quantisation), Annex E (scaling lists).
- Public `x264.h` `x264_level_t` and level-limit semantics.
- `tables.c.md`, `vlc.c.md`, `set.c.md` in this directory.
- Drepper, *What Every Programmer Should Know About Memory* (2007).

## 10. Verification checklist

- Verify every declared symbol is defined exactly once in a linked library (no missing or duplicate symbols) for both bit depths.
- Verify `sizeof(vlc_t) == 2` in both builds and that every static VLC code fits in its declared `i_size` bits.
- Verify `QP_MAX_MAX + 1 == 82` entries in all QP-indexed tables and that the last entries are saturated/valid.
- Verify `x264_zero` is 64-byte aligned and contains only zeros before any consumer writes to it.
- Verify the level list is terminated by an all-zero record and that level values are strictly increasing except the 1b special case.
- Confirm no table is modified during encoding by hashing the data segment before and after an encode (excluding `x264_zero`).

## 11. Open questions

- UNKNOWN: why `x264_zero` is non-const; no writer is visible in this header, so it may be historical or used by assembly as scratch.
- UNKNOWN: whether the AVCIntra CQM presets are intended for external use or only selected by profile flags; only their declarations are visible here.
