# `common/tables.h` - constant table declarations

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\tables.h` (mirrored at `common/tables.h`). Role in one sentence: declares all fixed lookup tables shared by analysis, quantisation, CABAC, rate distortion, motion compensation and CAVLC, plus the `vlc_t` element type. Marker: **dirty-side behavioural specification**.

Reading note: this header is the *catalogue* of table names, types and dimensions; the meanings, derivations and numeric properties are in `tables.c.md`. The generated CAVLC tables (`x264_level_token`, `x264_run_before`) are declared in `bitstream.h`, not here — see §7 for the cross-reference.

---

## 2. Purpose

The encoder trades memory for speed by precomputing non-linear functions (log/exp approximations, lambda curves, CABAC state transitions, VLC codes, scaling weights) into flat arrays. This header is the single catalogue of those arrays, so consumers do not need to know where each is defined and the build can keep them in one translation unit.

For an independent I-slice implementation, this header answers three questions that the algorithm documents assume: (1) what exact type each table element has (and therefore its fixed-point width), (2) what index space each table uses, and (3) which entries are padding rather than meaningful data. Getting (2) wrong is the classic source of CAVLC stream corruption, because the same symbol class is indexed differently in different context categories.

The header is intentionally a flat catalogue rather than a set of accessor functions: every consumer indexes arrays directly, which keeps the hot loops free of call overhead. A clean implementation can wrap lookups in inline functions for clarity, provided the compiler inlines them and the element types remain unchanged.

---

## 3. Public interface

`typedef struct { uint8_t i_bits; uint8_t i_size; } vlc_t;` — a fixed-length binary code: the right-aligned code bits and the number of bits. Used by all static CAVLC tables.

A companion type declared in `bitstream.h`, `vlc_large_t { uint16_t i_bits; uint8_t i_size; uint8_t i_next; }`, extends `vlc_t` with a larger bit field and a suffix-length selector. It is used only by the generated `x264_level_token` table. A third helper type, `x264_run_level_t { int32_t last; int32_t mask; dctcoef level[18]; }`, is the output of `coeff_level_run` and feeds the `run_before` table; its `mask` is a 16-bit occupancy bitmap (the upper bits of `last` are not part of it). The `level[18]` array has two spare entries at the end so trailing-one detection can read three positions without a bounds check.

### 3.1 Declaration catalogue by group

| Group | Symbols | Element type | Notes |
|-------|---------|--------------|-------|
| Levels | `x264_levels[]` | `x264_level_t` | `X264_API`, zero-terminated |
| Math | `x264_exp2_lut[64]` | uint8 | fractional part of 2^(i/64) ×256 |
| Math | `x264_log2_lut[128]`, `x264_log2_lz_lut[32]` | float | mantissa + leading-zero correction |
| Lambda | `x264_lambda_tab[82]`, `x264_lambda2_tab[82]`, `x264_trellis_lambda2_tab[2][82]` | uint16 / int / int | QP-indexed, saturating |
| Chroma | `x264_chroma_lambda2_offset_tab[37]` | uint16 | saturates at 65535 |
| MC | `x264_hpel_ref0[16]`, `x264_hpel_ref1[16]` | uint8 | values 0..3 |
| CQM | `x264_cqm_jvt4i[16]`, `x264_cqm_jvt4p[16]`, `x264_cqm_jvt8i[64]`, `x264_cqm_jvt8p[64]`, `x264_cqm_flat16[64]` | uint8 | standard scaling lists |
| CQM | `x264_cqm_jvt[8]` | `const uint8_t*` | eight list pointers |
| DCT | `x264_decimate_table4[16]`, `x264_decimate_table8[64]` | uint8 | heuristic cutoffs |
| DCT | `x264_dct4_weight_tab[16]`, `x264_dct8_weight_tab[64]`, `*_weight2_tab` | uint8 / uint32 | 1/256 fixed point |
| CABAC | `x264_cabac_context_init_I[1024][2]`, `x264_cabac_context_init_PB[3][1024][2]` | int8 | `(m,n)` pairs |
| CABAC | `x264_cabac_range_lps[64][4]`, `x264_cabac_transition[128][2]`, `x264_cabac_renorm_shift[64]`, `x264_cabac_entropy[128]` | uint8 / uint16 | state machine + RD cost |
| CABAC RD | seven offset tables | uint8 | context-index bases and increments |
| CAVLC | six static VLC tables | `vlc_t` | code bits + length |
| Scratch | `x264_zero[1024]` | uint8 | non-const, 64-byte aligned |

The AVCIntra preset lists are additional `const uint8_t[]` arrays (16 or 64 entries) whose names encode resolution and profile class; their exact set is UNKNOWN beyond what `tables.c.md` describes.

Declared groups (exact declarations are in the source; semantics below):

- Public levels: `X264_API extern const x264_level_t x264_levels[]` — a zero-terminated array of H.264 level limit records.
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

---

## 4. State and data structures

All objects are `const` (except `x264_zero`) and have static storage duration; no lifecycle functions exist. The `x264_level_t` records are the only publicly exported ones. `x264_zero` is writable scratch used as a convenient all-zero source for SIMD fills; callers must not rely on it retaining zeros after use if any code writes to it (the reference declares it non-const, see Open Questions).

Index-space conventions: lambda tables are indexed by QP including the bit-depth offset and the extra 18 emergency QPs; chroma-lambda offsets by the configured chroma QP offset (possibly negative, clamped at the call site); CQM pointers by preset and list index; VLC tables by frame syntax categories (`nC`, total coefficients, trailing ones, total zeros, run length).

### 4.1 `vlc_t` bit semantics (worked)

`vlc_t.i_bits` holds the code **right-aligned** in an 8-bit field, and `i_size` is its length in bits. The writer emits exactly the low `i_size` bits of `i_bits`, most significant of those first. Example: the code `00000101` (8 bits, value 0x05) is stored as `{i_bits=0x05, i_size=8}`. The code `1` (1 bit) is `{0x01, 1}`; `11` (2 bits) is `{0x03, 2}`; `01` (2 bits) is `{0x01, 2}` — note that `{0x01,1}` and `{0x01,2}` are *different* codes, so `i_size` is not redundant. A clean implementation must store both fields and write the low `i_size` bits, never the whole byte.

`vlc_large_t` uses a 16-bit `i_bits` because level escape codes can be up to 28 bits (for High Profile extension even longer prefixes are assembled by the writer, not stored). Its `i_next` is the suffix length to use for the *next* level, so one lookup returns code, length and state update together.

### 4.2 Declaration-to-use map

| Declaration | Read by | Index expression |
|-------------|---------|------------------|
| `x264_lambda_tab` | mode decision, rate control | `qp` (0..81) |
| `x264_chroma_lambda2_offset_tab` | chroma RD | clamped chroma QP offset (0..36) |
| `x264_hpel_ref0/ref1` | motion compensation | 4-bit MV fraction (0..15) |
| `x264_decimate_table4/8` | DCT decimation | zigzag coefficient index |
| `x264_dct*_weight*_tab` | trellis quantisation | zigzag coefficient index |
| `x264_cabac_context_init_*` | CABAC slice init | context index 0..1023, init idc 0..2 |
| `x264_cabac_range_lps` | CABAC encoder | state 0..63, rangeLPS variant 0..3 |
| `x264_cabac_transition` | CABAC encoder | (state<<1) | bin |
| `x264_cabac_renorm_shift` | CABAC encoder | state 0..63 |
| `x264_cabac_entropy` | CABAC RD estimation | (state<<1) | bin |
| RDO offset tables | CABAC RD coefficient coding | scan position + base |
| `x264_coeff0_token` | CAVLC zero block | nC 0..5 |
| `x264_coeff_token` | CAVLC coeff_token | nC, total−1, trailing |
| `x264_total_zeros*` | CAVLC total_zeros | total−1, total_zeros |
| `x264_run_before_init` | seed for generated run table | min(zeros,7)−1, run length |
| `x264_level_token` (bitstream.h) | CAVLC levels | suffix 0..6, level+64 |
| `x264_run_before` (bitstream.h) | CAVLC run-before | 16-bit coefficient mask |

The two generated tables are declared in `bitstream.h` because they are produced by `x264_cavlc_init` rather than statically initialised; their values still derive entirely from the standard tables catalogued here.

### 7.1 `x264_level_t` record fields

The public level record (from `x264.h`, consumed by `encoder/set.c` validation) carries these fields in order: `idc`, `max_mbps`, `max_fs`, `max_dpb_mbs`, `max_br`, `max_cpb`, `max_ref`, `min_cr`, `b_8x8dct`, `max_mv_range`, `max_mv_range_v`, `b_frames_factor`, and the unbounded-DPB flag. `encoder.open` walks the array until the all-zero terminator, selecting the smallest level whose limits cover the requested geometry, reference count, bitrate and MV range. An independent implementation should expose the same record shape because the validation logic indexes fields by name, not position.

### 7.2 CQM list-role enums

`x264_cqm_jvt[8]` is indexed by the CQM role enumerations: 4x4 intra luma (CQM_4IY), 4x4 inter luma (CQM_4PY), 4x4 intra chroma (CQM_4IC), 4x4 inter chroma (CQM_4PC), then the corresponding four 8x8 roles (CQM_8IY, CQM_8PY, CQM_8IC, CQM_8PC). The SPS scaling-list array uses the same role ordering. The reference does not allow separate Cb and Cr lists, so eight pointers suffice where the standard permits twelve. A clean implementation must keep this ordering or the wrong list will be applied to a block.

### 7.3 QP table sizing derivation

`QP_MAX_MAX = 51 + 6*2 + 18 = 81`: the 51 base QPs of H.264, plus 12 more for 10-bit (`6*(10−8)`), plus 18 emergency QPs for rate control. The chroma offset limit `MAX_CHROMA_LAMBDA_OFFSET = 36` covers the maximum legal chroma QP offset range. Tables indexed by QP therefore have 82 entries (0..81 inclusive). The lambda tables are `uint16_t` because `lambda_tab[81] = round(2^(81/6−2)) = round(2^11.5) ≈ 2896`, which fits in 16 bits; `lambda2` needs 32 bits because of the ×256 scaling.

---

## 5. Algorithm / behaviour

Consumers use these arrays as pure functions. Examples: `x264_log2` in `base.h` uses the two log tables; `x264_exp2fix8` uses the exp table; rate control looks up lambda by QP; CAVLC writers index `coeff_token[nC][total-1][trailing]`; CABAC initialisation reads the context-init pairs and converts them to initial states; RDO coefficient coding adds the RDO offsets to base context indices; half-pel reference tables map a 4-bit MV fraction index to the two neighbouring reference positions used for interpolation.

The arrays are grouped so that all read-only data lives in one translation unit, improving locality and allowing the linker to place them together. Several tables are deliberately *padded* (e.g. `significant_coeff_flag_offset_8x8[2][64]` where only 63 coefficients exist, and the 16-entry `x264_*_offset` tables where only 14 are meaningful) so SIMD code can index without bounds checks.

### 5.1 Padding entries (normative for SIMD)

| Table | Meaningful entries | Padding | Value of padding |
|-------|--------------------|---------|------------------|
| `significant_coeff_flag_offset_8x8[2][64]` | 0..62 | index 63 | 0 |
| `last_coeff_flag_offset_8x8[63]` | 0..62 | none | — |
| `significant_coeff_flag_offset[2][16]` | 0..13 | 14,15 | 0 |
| `last_coeff_flag_offset[2][16]` | 0..13 | 14,15 | 0 |
| `coeff_abs_level_m1_offset[16]` | 0..13 | 14,15 | 0 |
| `count_cat_m1[14]` | 0..13 | none | — |
| `cabac_context_init_I[1024][2]` | used contexts | unused contexts | `{0,0}` |

An implementation that removes padding to save space must still make out-of-range indexes safe for its own code; the reference relies on the zeros for SIMD loads.

### 5.2 Index-space cross-check for CAVLC

A single CAVLC block write touches up to six table families in sequence: `coeff0_token[nC]` (early exit), `coeff_token[nC][total−1][trailing]`, `level_token[suffix][level+64]` (generated, re-indexed as suffix adapts), `total_zeros[total−1][zeros]` or a chroma-DC variant, and `run_before[mask]` (generated). The index bases differ: `total` is 1-based in the syntax but 0-based in the table, the level table is biased by +64, and the run table is keyed by a bitmask rather than a count. A clean implementation should keep these base conversions explicit and verify them against the worked example in `cavlc.c.md` §5.1.

### 5.3 Type widths as fixed-point contracts

`x264_lambda_tab` is `uint16_t` (lambda ≤ ~2896 at QP81, fits), `x264_lambda2_tab` is `int` (needs 32 bits for the capped value 134,217,727), `x264_chroma_lambda2_offset_tab` is `uint16_t` (saturates at 65535), `x264_cabac_entropy` is `uint16_t` (1/256 fixed point up to ~65535), `x264_cabac_context_init_*` is `int8_t` (the standard `(m,n)` pairs are small signed integers). Using a wider type does not change values but may change SIMD load alignment; a clean scalar implementation may use `int` throughout.

### 5.4 The level record as an exported API surface

`x264_levels[]` is the only table marked `X264_API`, because external callers inspect H.264 level limits through `x264_level_t`. Its records are read by `x264_validate_levels` (in `encoder/set.c`) to check whether the configured frame size, reference count, DPB size, bitrate and MV range fit a given level. The array is zero-terminated: consumers iterate until `idc == 0`. Level 1b (idc 9) is the only out-of-order entry and carries the unbounded-DPB flag. An independent implementation that reorders the array or omits the terminator will make level validation loop forever or select the wrong level.

### 5.5 Math-table consumption paths

`x264_log2_lut` and `x264_log2_lz_lut` are consumed by the `x264_log2` inline in `base.h`: the leading-zero count of the input supplies an index into `log2_lz_lut`, and the mantissa bits index `log2_lut`, so the pair approximates `log2` without a libc call. `x264_exp2_lut` is consumed by `x264_exp2fix8`, which reconstructs `2^(x/64)` as an integer plus fractional part with a shift. Both are used in rate control where millions of evaluations would make a libm call expensive. A clean implementation may call `log2`/`exp2` directly; output decisions will differ by at most one quantiser step in edge cases.

### 5.6 CQM table ownership at write time

`x264_sps_t.scaling_list[8]` holds *pointers*, not copies, into these static arrays (or into user-supplied arrays for custom CQM). `set.c` resolves the pointers from `param->i_cqm_preset` at SPS derivation time; the pointers must remain valid when `x264_sps_write` serialises the scaling-list syntax. This is why the tables have static storage duration and why the header declares them `const`. A clean implementation that copies lists into the SPS instead of pointing at them would be bit-identical but would change the ownership contract documented in `set.h.md`.

### 5.7 Index-base summary for implementers

The following base conversions appear across the algorithm documents and are fixed by these declarations:

| Syntax quantity | Table index | Conversion |
|-----------------|-------------|------------|
| `TotalCoeff` | `coeff_token` second axis | `TotalCoeff − 1` |
| `TotalTrailing` | `coeff_token` third axis | direct, 0..3 |
| level value | `level_token` second axis | `level + LEVEL_TABLE_SIZE/2` (+64) |
| coefficient occupancy | `run_before` axis | 16-bit mask, direct |
| `TotalZeros` | `total_zeros` second axis | direct, within `[0, total−1]` |
| QP | lambda tables | direct, 0..81 |
| chroma QP offset | chroma lambda offset | direct after clamp, 0..36 |
| coefficient scan position | RDO offset tables | position + base constant |

A clean implementation should centralise these conversions in named helpers so they are applied consistently across the CAVLC writer, the analyser and the RD estimators.

---

## 6. Dependencies

Included by `base.h` and by all algorithm modules. Depends on `x264.h` for `x264_level_t` and `X264_API`, and on `osdep.h` for `ALIGNED_64`. Implemented by `tables.c`; no runtime dependencies. `QP_MAX_MAX` and `MAX_CHROMA_LAMBDA_OFFSET` are defined in this header and used to size arrays in `tables.c`. `vlc_large_t`, `x264_run_level_t`, `LEVEL_TABLE_SIZE`, `x264_level_token` and `x264_run_before` are declared in `bitstream.h` and included transitively through `common.h`.

---

## 7. Constants and tables

`QP_MAX_MAX = 81`, `MAX_CHROMA_LAMBDA_OFFSET = 36`, `NUM` of CQM lists = 8, CABAC context count = 1024, CAVLC context categories = 6, level table size = 128, run-before table size = 65536. Semantics and derivations are in `tables.c.md`.

The two generated tables live in `bitstream.h`:

- `vlc_large_t x264_level_token[7][LEVEL_TABLE_SIZE]` — suffix length × biased level.
- `uint32_t x264_run_before[1<<16]` — occupancy mask → packed `(bits<<5)|length`.

They are not declared here because they are not statically initialised; this difference is visible to any implementation that tries to place all tables in read-only data. Only the static `vlc_t` tables in this header can be pure constants.

---

## 8. Performance notes

- **Cache layout:** keep the hottest tables (lambda, CABAC entropy, hpel refs) small and contiguous; the large CAVLC `coeff_token` table is roughly 6·16·4·2 = 768 bytes and fits in L1.
- **False sharing / allocation:** read-only static data is shared cleanly; `x264_zero` is the only writable array and must not be used concurrently for different purposes.
- **Dispatch hierarchy:** these tables are CPU-independent; SIMD kernels consume the same numbers as C kernels, which is what makes bitstreams identical across architectures.
- Public references: ITU-T H.264 tables for CABAC/CAVLC; Drepper (2007) on read-only table locality.
- **Declaration grouping:** keeping all declarations in one header lets the compiler see complete array sizes at every call site, enabling bounds- and alignment-aware optimisations. A clean implementation should preserve the exact dimensions so that `sizeof(table)` checks remain meaningful.
- **Element size discipline:** the `uint8_t` `vlc_t` fields pack into 2 bytes; widening them to `int` would triple the `coeff_token` footprint and push it out of L1. Keep the declared widths in any performance-sensitive implementation.
- **Read-only sharing:** because every declared table except `x264_zero` is `const`, multiple encoder instances and threads share the same physical pages. A clean implementation should keep them in read-only data sections so the OS can share them across processes.
- **Predicate-friendly bounds:** the padding in §5.1 exists so SIMD loads of a full vector never fault at the last meaningful index. A scalar implementation may omit it, but should keep the same dimension macros so that any later SIMD addition does not require changing call sites.
- Public references (additional): ITU-T H.264 clause 9.3.1 (CABAC context initialisation from `(m,n)`); Intel *Optimization Reference Manual* on fixed-width table-lookup instructions and alignment.

---

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10, Annex A (levels), clause 9.2 (CAVLC), 9.3 (CABAC), 8.5 (quantisation), Annex E (scaling lists).
- Public `x264.h` `x264_level_t` and level-limit semantics.
- `tables.c.md`, `vlc.c.md`, `set.c.md` in this directory.
- Drepper, *What Every Programmer Should Know About Memory* (2007).
- Intel *Optimization Reference Manual* — fixed-width table loads and alignment.

---

## 10. Verification checklist

- Verify every declared symbol is defined exactly once in a linked library (no missing or duplicate symbols) for both bit depths.
- Verify `sizeof(vlc_t) == 2` in both builds and that every static VLC code fits in its declared `i_size` bits.
- Verify `QP_MAX_MAX + 1 == 82` entries in all QP-indexed tables and that the last entries are saturated/valid.
- Verify `x264_zero` is 64-byte aligned and contains only zeros before any consumer writes to it.
- Verify the level list is terminated by an all-zero record and that level values are strictly increasing except the 1b special case.
- Confirm no table is modified during encoding by hashing the data segment before and after an encode (excluding `x264_zero`).

Additional black-box checks:

- Verify the §4.1 `vlc_t` semantics: write `{0x01,1}`, `{0x01,2}`, `{0x03,2}` into a test bit writer and confirm emitted bit strings `1`, `01`, `11` respectively.
- Verify `sizeof(vlc_large_t) == 4` and that `x264_level_token[0][64+2]` has length 3, bits 1, `i_next == 1` after `x264_cavlc_init`.
- Verify §5.1 padding: every declared padding entry is zero and meaningful entries are non-zero for at least one base offset.
- Verify §5.2 by driving one CAVLC block write and confirming the six index bases (total−1, level+64, mask, nC) are applied exactly once each.
- Verify the declaration-to-use map §4.2: each declared table has at least one reader in a linked build (no dead declarations), confirmed by a symbol cross-reference.
- Verify the generated tables are bit-depth independent by hashing them in 8-bit and 10-bit builds and confirming identical hashes.
- Verify §7.1 by walking `x264_levels[]` until the terminator: every record's `max_fs`, `max_mbps` and `max_ref` are positive except the all-zero terminator, and idc 9 (level 1b) carries the unbounded-DPB flag.
- Verify §7.2 by checking that `x264_cqm_jvt[0..7]` point to the expected 16/64-entry lists in the documented role order and that no two roles accidentally alias the same array (unless intended for luma/chroma symmetry).
- Verify §7.3 by asserting `QP_MAX_MAX == 81`, `MAX_CHROMA_LAMBDA_OFFSET == 36`, and that `sizeof(x264_lambda_tab)/sizeof(uint16_t) == 82`.
- Verify §5.5 by comparing `x264_log2` against `log2()` over 1000 random positive inputs and confirming the absolute error stays within the documented approximation bound.
- Verify §5.6 by mutating a user-supplied CQM array after SPS derivation and confirming the written SPS reflects the mutation (pointer semantics), then repeat with a copied-list implementation to confirm bit-identical output.

---

## 11. Open questions

- UNKNOWN: why `x264_zero` is non-const; no writer is visible in this header, so it may be historical or used by assembly as scratch.
- UNKNOWN: whether the AVCIntra CQM presets are intended for external use or only selected by profile flags; only their declarations are visible here.
- UNKNOWN: the exact count and names of the AVCIntra preset arrays beyond those listed here; they are declared for several resolutions and profiles but the full set is not enumerated in this document.
- UNKNOWN: whether any consumer treats `i_bits` as sign-extended in `vlc_large_t`; codes are unsigned by construction, but the field is 16-bit and escape codes can approach 28 bits.
- UNKNOWN: the exact names and count of the AVCIntra preset arrays; this header declares "several resolutions and profiles" but the full enumeration is not reproduced here.
- UNKNOWN: whether the `flat16[64]` JVT array is also used as a 4x4 flat list by truncation, or a separate 4x4 flat array exists; only the 64-entry form is visible here.

This header declares storage, not behaviour. It should be read together with `tables.c.md` (derivations and numeric properties) and `bitstream.h.md` (the generated tables and the bit writer that consumes `vlc_t`).
