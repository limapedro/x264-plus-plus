# `encoder/set.h`

**Role:** Declares the header-writing (SPS/PPS/SEI) interface implemented in `encoder/set.c`.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/set.h`
- Role: public (within the encoder) declaration of parameter-set derivation, bitstream writing, SEI emission, filler, and level-validation functions.
- Marker: dirty-side behavioural specification.

Scope note: this header declares *interfaces*; the field values it derives and the exact bit layouts it serialises are specified in `encoder/set.c.md` and, for the underlying structures, in `common/set.h`. This document focuses on the contracts an independent implementation must honour: which fields are derived at which stage, what each writer consumes, and what stays valid across reconfiguration.

---

## 2. Purpose

This header is the compile-time contract between `encoder.c` (which drives NAL emission) and `encoder/set.c` (which knows the H.264 bitstream syntax). Every declared function is wrapped in the `x264_template` macro mechanism so the same source compiles into bit-depth-specific symbols (`x264_8_sps_write`, `x264_10_sps_write`, ...), allowing `api.c` to dispatch by `param->i_bitdepth`.

For an I-slice implementation, this header matters because the first thing any decoder needs is a valid SPS and PPS. The SPS fixes geometry, profile, level, chroma format, bit depth and VUI timing; the PPS fixes entropy mode, initial QP, chroma QP offsets and deblocking controls. The CAVLC reader in a decoder uses `pic_init_qp` and `chroma_qp_index_offset` from the PPS before it can interpret the first macroblock's `mb_qp_delta`, so a wrong value here corrupts the whole slice even if the CAVLC writer is correct.

The two-stage SPS lifecycle (`init` then optional `init_reconfigurable`) reflects a real constraint: most SPS fields are consumed by the decoder only once per stream, while crop and SAR may be updated in place. An implementation that treats all SPS fields as reconfigurable would need to re-emit a different `seq_parameter_set_id` each time, which changes the PPS reference and breaks the simple id-0 contract used by the reference.

---

## 3. Public interface

Parameter-set derivation:

- `void x264_sps_init( x264_sps_t *sps, int i_id, x264_param_t *param )` — derive all non-reconfigurable SPS fields from parameters and stamp the SPS id. Must be called before writing.
- `void x264_sps_init_reconfigurable( x264_sps_t *sps, x264_param_t *param )` — refresh crop offsets, crop flag, and SAR presence; safe to call after an `x264_encoder_reconfig`.
- `void x264_sps_init_scaling_list( x264_sps_t *sps, x264_param_t *param )` — resolve `scaling_list[8]` pointer targets from the CQM preset.
- `void x264_sps_write( bs_t *s, x264_sps_t *sps )` — serialize the SPS RBSP; realigns `s`, appends `rbsp_trailing_bits`, flushes.
- `void x264_pps_init( x264_pps_t *pps, int i_id, x264_param_t *param, x264_sps_t *sps )` — derive PPS fields, linking to the given SPS id.
- `void x264_pps_write( bs_t *s, x264_sps_t *sps, x264_pps_t *pps )` — serialize the PPS RBSP (needs the SPS to decide High-profile scaling-list syntax).

SEI / filler writers (all write into a `bs_t`, most via the generic `x264_sei_write`):

- `void x264_sei_recovery_point_write( x264_t *h, bs_t *s, int recovery_frame_cnt )`
- `int x264_sei_version_write( x264_t *h, bs_t *s )` — allocates the option string; returns nonzero on allocation failure.
- `void x264_sei_buffering_period_write( x264_t *h, bs_t *s )`
- `void x264_sei_pic_timing_write( x264_t *h, bs_t *s )`
- `void x264_sei_dec_ref_pic_marking_write( x264_t *h, bs_t *s )`
- `void x264_sei_frame_packing_write( x264_t *h, bs_t *s )`
- `void x264_sei_mastering_display_write( x264_t *h, bs_t *s )`
- `void x264_sei_content_light_level_write( x264_t *h, bs_t *s )`
- `void x264_sei_alternative_transfer_write( x264_t *h, bs_t *s )`
- `int x264_sei_avcintra_umid_write( x264_t *h, bs_t *s )` — returns 0 on success.
- `int x264_sei_avcintra_vanc_write( x264_t *h, bs_t *s, int len )` — returns −1 if `len` is negative or larger than the internal buffer.
- `void x264_sei_write( bs_t *s, uint8_t *payload, int payload_size, int payload_type )` — the generic payload-type/size extension encoder.
- `void x264_filler_write( x264_t *h, bs_t *s, int filler )` — emits `filler` bytes of 0xFF plus RBSP trailing bits.

Validation:

- `int x264_validate_levels( x264_t *h, int verbose )` — nonzero means at least one level limit is exceeded; `verbose` controls warning logs.

Every symbol is defined as `#define x264_foo x264_template(foo)` plus its prototype; consumers call the un-templated name and the preprocessor specializes it.

### 3.1 Templating contract

`x264_template(foo)` expands to `x264_<BIT_DEPTH>_foo`, so `x264_sps_write` becomes `x264_8_sps_write` or `x264_10_sps_write`. This is the same mechanism used throughout `common.h.md`. A clean implementation must provide one definition per bit depth (or one templated definition compiled twice); a single untemplated definition will fail to link when both libraries are present. The `api.c` dispatch layer selects the right symbol from `param->i_bitdepth`.

### 3.2 Writer side-effect contracts

| Writer | Consumes | Side effects on `bs_t` | Return |
|--------|----------|------------------------|--------|
| `x264_sps_write` | fully derived SPS | byte-align, write RBSP trailing bits, flush | void |
| `x264_pps_write` | SPS + PPS | byte-align, trailing bits, flush | void |
| `x264_sei_write` | payload bytes, type id | payload-type/size extension encoding | void |
| `x264_filler_write` | filler byte count | 0xFF bytes + trailing bits | void |
| `x264_sei_version_write` | `h->param` option string | allocates then frees temporary string | nonzero on malloc failure |
| `x264_sei_avcintra_vanc_write` | VANC length | rejects out-of-range length | −1 on invalid `len` |

None of these writers emit NAL start codes or set NAL type/ref-idc; the caller in `encoder.c` does that. This separation is why the same writer can be used inside Annex-B and length-prefixed (AVCC) byte streams.

---

## 4. State and data structures

The header itself defines no structures; it consumes `x264_sps_t`, `x264_pps_t`, `x264_t`, `x264_param_t`, and `bs_t` from `common/common.h` / `common/bitstream.h`. The contracts assume:

- `x264_sps_t.scaling_list[8]` holds pointers valid at write time (static tables or user arrays).
- `x264_t.sh_backup` carries the slice-header fields read by `x264_sei_dec_ref_pic_marking_write`.
- `x264_t.sps` is the active SPS read by the buffering-period and picture-timing writers.
- `bs_t` must have enough remaining capacity for the serialized header; callers provide a correctly sized buffer.

### 4.1 SPS field contract (derived by `x264_sps_init`)

The `x264_sps_t` structure (declared in `common/set.h`) carries the following fields. This table states which stage sets each group and what invariant holds at write time:

| Group | Fields | Set by | Invariant at write |
|-------|--------|--------|--------------------|
| Identity | `i_id`, `i_profile_idc`, `i_level_idc` | `sps_init` / `validate_levels` | id stamped; profile/level cover geometry and features |
| Constraints | `b_constraint_set0..3` | `sps_init` | consistent with profile (Baseline/Main/High flags) |
| Numbering | `i_log2_max_frame_num`, `i_poc_type`, `i_log2_max_poc_lsb` | `sps_init` | log2 in 4..16; POC type 0 or 2 (type 1 unused) |
| Geometry | `i_mb_width`, `i_mb_height`, `b_frame_mbs_only`, `b_mb_adaptive_frame_field`, `b_direct8x8_inference` | `sps_init` | MB dimensions match padded picture; MBAFF flag consistent with interlace setting |
| References | `i_num_ref_frames`, `b_gaps_in_frame_num_value_allowed` | `sps_init` / level validation | reference count ≤ level limit |
| Crop | `b_crop`, `crop.{left,right,top,bottom}` | `sps_init` + `sps_init_reconfigurable` | crop margins ≤ MB-padded minus visible |
| VUI | `b_vui` and nested timing/HRD/bitstream fields | `sps_init` | enabled when timing, SAR or HRD present |
| High profile | `b_qpprime_y_zero_transform_bypass`, `i_chroma_format_idc`, `i_cqm_preset`, `scaling_list[8]` | `sps_init` / `sps_init_scaling_list` | scaling pointers non-null when High profile uses them |
| AVC-Intra | `b_avcintra_hd`, `b_avcintra_4k` | `sps_init` | selects AVC-Intra SEI and CQM presets |

The VUI sub-structure contains: SAR presence and dimensions, overscan, signal-type presence (video format, full-range, colour description with primaries/transfer/matrix), chroma location info, timing info (`i_num_units_in_tick`, `i_time_scale`, `b_fixed_frame_rate`), NAL/VCL HRD presence and the nested HRD record, picture-structure presence, and bitstream-restriction fields (`b_motion_vectors_over_pic_boundaries`, max bytes per picture, max bits per MB, max MV lengths, `i_num_reorder_frames`, `i_max_dec_frame_buffering`). A clean implementation should derive these from `x264_param_t` once and treat them as read-only afterwards.

### 4.2 PPS field contract (derived by `x264_pps_init`)

| Field | Meaning | Contract |
|-------|---------|----------|
| `i_id`, `i_sps_id` | PPS id and parent SPS id | `i_sps_id` references the SPS stamped by `sps_init` |
| `b_cabac` | entropy coding mode | 0 means CAVLC (Baseline); 1 means CABAC |
| `b_pic_order` | POC type selection | consistent with SPS `i_poc_type` |
| `i_num_slice_groups` | FMO slice groups | reference uses 1 (single group) |
| `i_num_ref_idx_l0_default_active`, `l1` | active reference counts | ≤ SPS `i_num_ref_frames` |
| `b_weighted_pred`, `b_weighted_bipred` | weighted prediction flags | set from analyse flags |
| `i_pic_init_qp` | initial picture QP | CAVLC/CABAC QP delta base; QP+26 convention |
| `i_pic_init_qs` | SP/SI initial QS | typically 0 for normal streams |
| `i_chroma_qp_index_offset` | chroma QP offset | applied before chroma QP table lookup |
| `b_deblocking_filter_control` | deblocking control present flag | gates slice-level deblock syntax |
| `b_constrained_intra_pred` | constrained intra flag | from param |
| `b_redundant_pic_cnt` | redundant picture count present | from param |
| `b_transform_8x8_mode` | 8x8 transform enable | High-profile only; gates CAVLC transform-size flag |

`i_pic_init_qp` and `i_chroma_qp_index_offset` are the two PPS fields the entropy decoders need immediately; an independent implementation should verify them first when debugging a first-slice failure.

---

## 5. Algorithm / behaviour

The header is declarative; behaviour lives in `set.c`. The interface enforces a two-stage SPS lifecycle: derive (`sps_init`) then, after any reconfiguration of crop/SAR, refresh (`sps_init_reconfigurable`) before re-serializing. PPS derivation requires a valid, already-derived SPS because several PPS fields depend on SPS id and chroma format. SEI writers follow a common pattern: build the payload in a temporary bitwriter, byte-align with the SEI trailing-one convention, then hand the bytes and a payload type id to `x264_sei_write`. Callers are responsible for starting a new NAL and setting NAL type/ref-idc around these writes; this module never emits start codes itself.

### 5.1 SEI writer contract table

| Writer | Payload type | When emitted | Key input |
|--------|--------------|--------------|-----------|
| `sei_recovery_point_write` | recovery point | after a seek/recovery point | `recovery_frame_cnt` |
| `sei_version_write` | user data unregistered | once at stream start | option string from `h->param` |
| `sei_buffering_period_write` | buffering period | at IDR/SPS activation | `h->sps->vui.hrd` |
| `sei_pic_timing_write` | picture timing | when timing info present | `h->sps` timing, POC |
| `sei_dec_ref_pic_marking_write` | dec ref pic marking | IDR and memory-management control ops | `h->sh_backup` |
| `sei_frame_packing_write` | frame packing arrangement | when frame-packing configured | param frame-packing mode |
| `sei_mastering_display_write` | mastering display colour volume | when metadata configured | param mastering-display struct |
| `sei_content_light_level_write` | content light level | when metadata configured | param MaxCLL/MaxFALL |
| `sei_alternative_transfer_write` | alternative transfer characteristics | when configured | param transfer characteristics |
| `sei_avcintra_umid_write` | AVC-Intra UMID | AVC-Intra profiles | internal UMID buffer |
| `sei_avcintra_vanc_write` | user data registered | AVC-Intra profiles | length-guarded VANC buffer |

`x264_sei_write` implements the Annex-D payload-type/size encoding: a byte is emitted for the type and size; when a value is ≥255, additional `0xFF` extension bytes precede a final residue byte. The payload is followed by an `rbsp_trailing_bits` convention specific to SEI (a `1` bit then zero padding, with the payload size counting the alignment bits). An independent implementation must get this extension rule right or long option strings will be misparsed.

### 5.2 Filler and validation

`x264_filler_write(h, s, filler)` emits exactly `filler` bytes of `0xFF` (filler data NAL payload) and then RBSP trailing bits. It is used by rate control to pad an access unit to a target size. `x264_validate_levels(h, verbose)` walks `x264_levels[]` (from `tables.h.md`) and checks frame size, MB rate, reference count, DPB size, bitrate/CPB and MV range against each candidate level; it returns nonzero if *no* level satisfies all constraints. With `verbose` set it logs which limit failed. The caller typically reconfigures bitrate/refs or reduces geometry when validation fails.

### 5.3 Worked SPS derivation example

For a 1920x1080 4:2:0 8-bit progressive source at level 4.0:

```
i_mb_width  = ceil(1920/16) = 120
i_mb_height = ceil(1088/16) = 68   (height padded to 1088)
b_frame_mbs_only = 1
i_chroma_format_idc = 1            (4:2:0)
i_log2_max_frame_num = 4           (if keyint <= 16; larger otherwise)
i_pic_init_qp (PPS) = 26 + qp      (qp=0 baseline)
crop.bottom = 1088 − 1080 = 8      (only if b_crop enabled)
```

The SPS writes `profile_idc` (66 for Baseline when CAVLC and no 8x8dct), constraint flags, `level_idc` (40), `seq_parameter_set_id`, then the profile-specific fields, `log2_max_frame_num_minus4`, POC fields, `max_num_ref_frames`, geometry (`pic_width_in_mbs_minus1 = 119`, `pic_height_in_map_units_minus1 = 67`), `frame_mbs_only_flag`, direct8x8 inference, crop, VUI, then `rbsp_trailing_bits`. A decoder parsing this SPS will know to expect CAVLC if the active PPS has `b_cabac = 0`.

### 5.4 Reconfiguration lifecycle

`x264_encoder_reconfig` may change crop margins, SAR and rate-control parameters without changing geometry or profile. The `sps_init_reconfigurable` entry point exists precisely for these fields: it rewrites `b_crop`, the crop offsets and SAR presence in the existing SPS, and the caller re-emits the SPS in a new access unit. Fields such as `i_mb_width`, `i_profile_idc`, `i_chroma_format_idc` and `i_log2_max_frame_num` are *not* reconfigurable; changing them requires a new encoder instance. A clean implementation that rebuilds the whole SPS on reconfig would be bit-identical but would change the pointer identity that other modules may cache.

### 5.5 Buffer-capacity contract

`x264_sps_write` and `x264_pps_write` assume the caller has provided a `bs_t` with sufficient `p_end − p`. The header does not check capacity. `encoder.c` allocates a small header buffer sized for the maximum SPS/PPS (including scaling-list syntax when High profile is active) before calling these writers. An independent implementation should either preserve that contract or add an explicit capacity check; silently truncating an SPS is unrecoverable for the decoder.

---

## 6. Dependencies

Included by `encoder/set.c` (definition site) and by `encoder/encoder.c` (call site). Depends on `common/common.h` for the opaque types and `common/bitstream.h` for `bs_t`. Symbols are specialized by the `x264_template` macro from `common/base.h`. It also relies on `x264_param_t` from the public `x264.h` and on `x264_level_t` (declared in `tables.h.md`) for level validation.

---

## 7. Constants and tables

No constants are defined in the header. Payload-type identifiers (e.g. `SEI_RECOVERY_POINT`, `SEI_USER_DATA_UNREGISTERED`, `SEI_BUFFERING_PERIOD`, `SEI_PIC_TIMING`, `SEI_FRAME_PACKING`, `SEI_MASTERING_DISPLAY`, `SEI_CONTENT_LIGHT_LEVEL`, `SEI_ALTERNATIVE_TRANSFER`, `SEI_DEC_REF_PIC_MARKING`) come from `common/set.h` / H.264 Annex D.

The CQM role enumeration (CQM_4IY, CQM_4PY, CQM_4IC, CQM_4PC, CQM_8IY, CQM_8PY, CQM_8IC, CQM_8PC) from `common/set.h` indexes `x264_sps_t.scaling_list[8]`; the enum ordering is the SPS scaling-list syntax order, so a clean implementation must preserve it. `x264_sps_init_scaling_list` resolves these eight pointers from `x264_cqm_jvt[8]` (documented in `tables.h.md` §7.2) or from user-supplied arrays.

When `i_cqm_preset` selects the flat lists, all eight pointers resolve to flat-16/flat-64 arrays and the SPS may omit explicit scaling-list syntax depending on profile; with JVT or user lists the syntax is written. The SEI payload-type constants are likewise fixed Annex-D values; they are not defined in this header precisely because they are standard facts shared with the decoder side.

---

## 8. Performance notes

Headers are written only at stream start and on reconfiguration; SEI messages are written per relevant frame but are tiny. No hot-loop concerns and no SIMD. A clean implementation may cache serialized SPS/PPS blobs and emit SEI payloads from preformatted byte templates without any observable change; see `set.c.md` section 8.

- **Caching opportunity:** because SPS/PPS change only at stream start or reconfig, serializing them once into a byte buffer and reusing the bytes avoids rebuilding Exp-Golomb fields on every keyframe. The bitstream is identical.
- **SEI payload templates:** most SEI payloads are constant for a stream; precomputing the payload-type/size bytes (including the `0xFF` extension rule for sizes ≥255) removes a small per-frame cost and a source of off-by-one bugs.
- Public references (additional): ITU-T H.264 Annex D (SEI payload-type/size encoding), Annex E (SPS syntax), x264 `doc/standards.txt` for profile/level constraints. Drepper (2007) is not applicable here — the data volume is negligible.

---

## 9. References

- ITU-T H.264 Annex D (SEI), Annex E (SPS), Annex F.1 (VUI).
- x264 public API (`x264.h`) and `x264_param_t` documentation.
- `encoder/set.c.md` in this directory for bit layouts and derivation details.
- `tables.h.md` §7.1–7.2 for the level record and CQM role ordering.

---

## 10. Verification checklist

- Compile-time check: every declared function has exactly one bit-depth-specialized definition (`x264_8_*` / `x264_10_*`) and the templated macro resolves correctly for both build configurations.
- Declare-only check: no function in this header allocates or frees memory visible to the caller (except via the documented string allocation in `x264_sei_version_write`, which frees internally).
- Confirm the AVC-Intra VANC writer's length guard rejects out-of-range `len` values through a black-box call.

Additional black-box checks:

- Parse the emitted SPS with an independent H.264 bitstream parser and verify every field in §4.1 against the source `x264_param_t`, especially `pic_width_in_mbs_minus1`, `pic_height_in_map_units_minus1`, `chroma_format_idc` and crop margins.
- Parse the emitted PPS and verify §4.2, in particular `pic_init_qp_minus26 + 26 == i_pic_init_qp` and `chroma_qp_index_offset == i_chroma_qp_index_offset`.
- Verify the §5.3 worked example end to end: for 1920x1080 4:2:0, the parser reports 120x68 macroblocks, chroma_format_idc 1, and a bottom crop of 8 when cropping is enabled.
- Verify the SEI payload-type/size extension rule by emitting a user-data payload larger than 255 bytes and confirming the parser recovers the correct size and payload bytes.
- Verify reconfiguration: change crop/SAR via reconfig, confirm a new SPS is emitted with updated crop/SAR while `i_mb_width`, profile and level are unchanged.
- Verify level validation: request a geometry/ref count exceeding level 4.0 limits and confirm `x264_validate_levels` returns nonzero and logs the failing limit when `verbose` is set.
- Verify `x264_filler_write(h, s, n)` emits exactly `n` 0xFF bytes followed by trailing bits, for n = 0, 1 and a large value.
- Verify buffer-capacity behaviour: give `sps_write` a `bs_t` with just enough room and one byte less, and confirm the documented contract (no check in the writer) rather than expecting graceful truncation.
- Verify §4.1 constraint flags: for a Baseline profile build, `b_constraint_set0` must be set and 8x8 transform (`b_transform_8x8_mode` in the PPS) must be zero; for High profile, confirm the chroma-format and scaling-list fields are written.
- Verify §4.2 reference indexing: `i_num_ref_idx_l0_default_active` and `l1` must never exceed the SPS `i_num_ref_frames`, and both must be at least 1 for any P/B slice.
- Verify the §5.1 SEI ordering at stream start: `version` appears in the first access unit, `buffering_period` and `pic_timing` are present when VUI timing is enabled, and `dec_ref_pic_marking` accompanies IDR pictures.

---

## 11. Open questions

- Whether any additional SEI writers are exposed in other builds (e.g. newer x264 revisions) is UNKNOWN; this header reflects the declared set in the reference snapshot.
- The precise ordering of SPS/PPS emission relative to the first IDR slice NAL is driven by `encoder.c`, not by this header; only the writer contracts are specified here.

Anything not stated about bit layouts is deliberately left UNKNOWN rather than guessed; the standard and `set.c.md` are the authoritative sources.
- The exact numeric payload-type identifiers for the SEI messages are standard Annex-D facts and are not reproduced here; a clean implementation must source them from H.264.
- Whether `x264_sps_init_reconfigurable` is called for SAR-only changes in all reconfiguration paths is UNKNOWN; the header documents the capability, not every call site.
- The exact initial values of `i_pic_init_qs` and `b_pic_order` for non-SI/SP streams are standard defaults and are not enumerated in this header.
- Whether `x264_sei_avcintra_umid_write` is compiled in non-AVC-Intra builds is UNKNOWN; its guard condition is not visible in this header.
- The exact buffer size used for the VANC payload in `x264_sei_avcintra_vanc_write` is UNKNOWN beyond the stated rejection of out-of-range `len`.

This header is a declaration surface only. It intentionally leaves the bit-level syntax (Exp-Golomb field order, VUI sub-syntax, SEI payload bytes) to `encoder/set.c.md` and the H.264 standard; an independent implementation should consult both rather than infer layouts from field order in `x264_sps_t`.
