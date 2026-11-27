# `common/set.h` - quantization setup interface

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\set.h` (mirrored at `common/set.h`). Role in one sentence: declares the SPS/PPS mirror structures and the custom-quantization-matrix (CQM) lifecycle functions used while configuring an encoder. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Before encoding starts, the encoder needs an internal, mutable representation of the sequence and picture parameter sets and a set of derived quantisation tables. This header defines those structures and the three functions that load, build and tear down custom quant matrices. Separating them from `common.h` keeps the SPS/PPS layout reusable by the bitstream writer and the rate controller.

## 3. Public interface

Enums: `enum cqm4_e` with `CQM_4IY = 0`, `CQM_4PY = 1`, `CQM_4IC = 2`, `CQM_4PC = 3`; `enum cqm8_e` with the analogous `CQM_8IY = 0`, `CQM_8PY = 1`, `CQM_8IC = 2`, `CQM_8PC = 3`. These are indices into the `scaling_list` array and into `h->*_mf` matrices; IY/PY denote intra/inter luma, IC/PC intra/inter chroma.

`typedef struct ... x264_sps_t`: internal sequence parameter set. Fields: `i_id`, `i_profile_idc`, `i_level_idc`, four constraint flags, `i_log2_max_frame_num`, `i_poc_type`, `i_log2_max_poc_lsb`, `i_num_ref_frames`, gap flag, `i_mb_width/height`, frame-mbs-only and MBAFF flags, direct-8x8-inference flag, crop flag and four crop offsets, VUI block (SAR, overscan, video format/full range, colour description, chroma location, timing with `i_num_units_in_tick` and `i_time_scale`, fixed frame rate, NAL/VCL HRD presence and the HRD sub-struct with cpb count, rate/size scales and values, CBR flag and five delay lengths, pic-struct flag, bitstream-restriction flags including reorder count and max DPB frames), `b_qpprime_y_zero_transform_bypass`, `i_chroma_format_idc`, AVCIntra HD/4k flags, `i_cqm_preset`, and `const uint8_t *scaling_list[8]` (four 4x4 lists followed by four 8x8 lists; only eight although H.264 permits twelve, because separate Cb and Cr lists are not supported).

`typedef struct ... x264_pps_t`: internal picture parameter set. Fields: `i_id`, `i_sps_id`, `b_cabac`, `b_pic_order`, `i_num_slice_groups`, default active reference counts for both lists, weighted-pred/bipred flags, `i_pic_init_qp`, `i_pic_init_qs`, `i_chroma_qp_index_offset`, deblocking-control/constrained-intra/redundant-pic flags, `b_transform_8x8_mode`.

Functions (all bit-depth templated via `x264_template`):

- `int x264_cqm_init(x264_t *h)`: builds all derived quantisation tables from the SPS scaling lists and the encoder's deadzone settings; returns 0 or -1, cleaning up on failure.
- `void x264_cqm_delete(x264_t *h)`: frees the derived tables, respecting pointer aliasing between identical lists.
- `int x264_cqm_parse_file(x264_t *h, const char *filename)`: reads a JM-style custom matrix file and fills `param.cqm_*`; returns 0 or a nonzero error accumulation, sets `i_cqm_preset = X264_CQM_CUSTOM`.

## 4. State and data structures

The SPS/PPS structs are value-owned inside `x264_t` (one each, `h->sps[1]`, `h->pps[1]`) and are written by encoder-open logic, not by these functions except for the scaling-list pointers. `scaling_list` entries point either at static const JVT/flat/AVCIntra arrays in `tables.c` or at arrays embedded in the parameter object (`cqm_4iy`, ...). Ownership of those arrays belongs elsewhere; `cqm_init` never frees them.

The derived matrices owned by `cqm_init` and stored in `x264_t` are grouped by width: four 4x4 sets and up to four 8x8 sets. Each set contains a decode dequant matrix `[6][w*w]` of `int`, a trellis unquant matrix `[QP_MAX_SPEC+1][w*w]` of `int`, a quant factor matrix `[QP_MAX_SPEC+1][w*w]` of `udctcoef`, and two bias matrices (`quant*_bias`, `quant*_bias0`) of `udctcoef`. Two sets may intentionally share an allocation when their scaling lists (and, for biases, deadzone) coincide; deletion must therefore compare pointers before freeing.

## 5. Algorithm / behaviour

`x264_cqm_init` performs these passes:

1. Establish deadzones: inter luma = `32 - analyse.i_luma_deadzone[1]`, intra luma = `32 - analyse.i_luma_deadzone[0]`, inter chroma = 21, intra chroma = 11 (i.e. `32-11`, `32-21`). Determine how many 8x8 lists are needed: four for 4:4:4, two when 8x8 transforms are enabled, zero otherwise.
2. Allocate each distinct list's four/five matrices; if a list's bytes equal an earlier list's bytes, reuse that earlier pointer instead of allocating. Bias reuse additionally requires the deadzone value to match.
3. Build the flat reference scales for QP modulo 6. For 4x4, the scale depends only on `(i&1) + ((i>>2)&1)`, i.e. the horizontal+vertical frequency class, giving a 3-value pattern per QP. For 8x8, an index derived from the coefficient position selects one of six scale values (the pattern is a small fixed permutation table applied to `((i>>1)&12) | (i&3)`).
4. Multiply each reference dequant scale by the user's scaling-list coefficient; compute the reciprocal quant scale as `(reference_quant*16 + d/2)/list_coefficient` with round-half-up division.
5. For each QP from 0 to `QP_MAX_SPEC`: compute the unquant multiplier as a 64-bit reciprocal `(1 << (QP/6 + 15 + 8)) / quant_factor` (8x8 uses `+16+8`), and the quant multiplier by shifting the QP-mod-6 factor by `QP/6 - 1` (8x8 by `QP/6`) with rounding. A zero quant multiplier marks an unusable QP (`min_qp_err`). Bias is `(deadzone<<10)/factor` rounded, capped at `(1<<15)/factor`; `bias0` is exactly `(1<<15)/factor`. Luma/chroma maximum usable QPs are tracked when the quant factor exceeds `min(0xffff, (1<<(25-BIT_DEPTH))-1)`.
6. Build emergency-mode noise-reduction offsets for QPs above the specification maximum: categories are luma 4x4, luma 8x8, chroma 4x4, chroma 8x8 (skipped when 8x8 transforms are off). The final QP slot sets every offset to `(1<<(7+BIT_DEPTH))-1`, removing all coefficients. Earlier QPs ramp from zero with an exponential bias based on an unquant multiplier at `QP_MAX_SPEC`, with separate thresholds for DC, luma and chroma; chroma ramps first, then luma, then DC.
7. Unless lossless, widen the legal QP range: raise `i_qp_min` until the chroma-mapped QP exceeds the maximum chroma error, lower `i_qp_max` below the minimum error QP, raise it above the maximum luma error, and, for CAVLC below High profile, keep raising `i_qp_max` until the chroma-mapped QP and the QP itself exceed 12 (avoiding long level codes). If the bounds cross, log an error and fail.

`x264_cqm_delete` releases each matrix only when no lower-index list shares its pointer, then frees the emergency offsets and nulls them.

`x264_cqm_parse_file` slurps the file, replaces comment text after '#' with spaces, sets `X264_CQM_CUSTOM`, then searches for JM variable names such as `INTRA4X4_LUMA`, `INTER4X4_LUMA`, `INTRA4X4_CHROMA` (an optional trailing `U`/`V` is skipped), and the 8x8 luma names; in 4:4:4 mode it also parses the 8x8 chroma names. For each name: if absent, fill the list with 16; if the first coefficient is 0, copy the corresponding JVT default list; otherwise read comma/space/newline-separated integers in 1..255, rejecting out-of-range or too-short lists. Chroma names are applied to both Cb and Cr lists. Errors are accumulated with bitwise OR, so all lists are attempted.

## 6. Dependencies

Includes `common.h`; uses `base.h` macros (`CHECKED_MALLOC`, `X264_MIN`), `tables.c` JVT matrices via `tables.h`, `x264_slurp_file`, `x264_log`, `x264_emms`, `x264_free`, and the chroma-QP mapping table in `h->chroma_qp_table`. Called from encoder open (after SPS/VUI setup, before quantisation use) and from parameter parsing of `cqmfile`.

## 7. Constants and tables

JVT flat value 16 for all lists. Deadzone constants 21/11 inter/intra luma and 32-based complements. Maximum quant factor `min(0xffff, (1<<(25-BIT_DEPTH))-1)`. Emergency offset maximum `(1<<(7+BIT_DEPTH))-1`. Scale tables `dequant4_scale`, `quant4_scale`, `dequant8_scale`, `quant8_scale` are 6x3 and 6x6 arrays of small integers defined in `set.c`; their rows correspond to QP modulo 6 and their columns to frequency classes. The 8x8 position-to-class index table is a 16-byte permutation. JM variable names and the default-list selection are listed in section 5.

## 8. Performance notes

- These tables are built once per encoder open; the QP-indexed matrices are read in the trellis and rate-control hot paths, so their memory layout matters. Store them contiguously and keep the frequently used QP rows in cache; the `[cqm][qp][coef]` ordering means a given QP's 16/64 coefficients are adjacent.
- **Cache layout / allocation strategy:** sharing identical lists avoids duplicate cache footprints as well as allocations; preserve this aliasing and the pointer-based deletion rule.
- **False sharing:** none; tables are read-only after init.
- **Dispatch hierarchy:** the tables are consumed by both C and SIMD quantisation kernels; keep element widths (`int` vs `udctcoef`) exactly as declared so assembly loads stay valid.
- Public references: ITU-T H.264 clause 8.5.1 (quantisation and scaling lists), Annex E (custom matrices), x264 `doc/ratecontrol.txt` for QP-range interactions.

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10, clause 7.4.2.1.1 (SPS), 7.4.2.2 (PPS), 8.5.1 (scaling lists and dequantisation), Annex E.
- JM reference software custom-matrix file format documentation.
- Public `x264.h`: `X264_CQM_*` presets and CQM parameter fields.
- Drepper (2007) on read-only table locality.

## 10. Verification checklist

- With the default flat matrix, verify that all eight scaling lists are 16 and that derived dequant values for a given QP follow the documented frequency-class pattern (compare against independently computed H.264 dequant values for several QPs).
- Verify that identical scaling lists share pointers: after `x264_cqm_init`, two equal lists must have equal `quant*_mf` pointers; after delete there must be no double free (run under a memory checker).
- Feed a JM file with a missing list and verify that list becomes all 16; feed a first coefficient of 0 and verify the JVT default is used.
- Feed a list with a coefficient of 0 or 256 and verify an error is reported; feed a short list and verify an error.
- In a 4:2:0 build, verify only two 8x8 lists are allocated; in a 4:4:4 build, four.
- Verify that a pathological custom matrix can raise `i_qp_min` or lower `i_qp_max`, and that impossible constraints produce an error rather than an out-of-range QP.
- Verify emergency offsets at the last QP slot saturate to the documented maximum for every category.

## 11. Open questions

- UNKNOWN: exact numeric contents of the six-by-three and six-by-six scale tables are not reproduced here; they are fixed H.264 constants and must be taken from the standard's dequantisation tables.
- UNKNOWN: why 8x8 chroma lists are only parsed in 4:4:4 mode is not documented beyond the code's conditional; treat as an implementation limitation.
- UNKNOWN: the tuning rationale for the exponential emergency-denoise formula is labelled heuristic in the source; no public derivation is available.
