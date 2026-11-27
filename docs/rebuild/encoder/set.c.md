# `encoder/set.c`

**Role:** Derives and serializes the H.264/AVC sequence and picture parameter sets (SPS/PPS), all registered SEI messages, and level-limit validation for the encoder.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/set.c`
- Role: build the `x264_sps_t`/`x264_pps_t` parameter-set structures from `x264_param_t` and write their RBSP representations plus SEI NAL payloads onto a bit writer (`bs_t`).
- Marker: this document is a dirty-side behavioural specification derived from the reference implementation and public H.264 literature.

---

## 2. Purpose

An H.264 decoder can only interpret slice data after receiving an SPS (resolution, profile, level, chroma format, reference/DPB sizes, VUI) and a PPS (entropy mode, QP initialisation, loop-filter/deblocking control). This module is the single place where those structures are populated and where the exact bit ordering of the RBSP is defined. It also emits the optional SEI messages (recovery point, buffering period, picture timing, frame packing, HDR metadata, AVC-Intra UMID/VANC, encoder version string) and the filler NAL. Level validation compares the chosen configuration against the standard's per-level maxima so an invalid stream can be rejected before encoding starts.

Two-phase initialization is deliberate: fields that never change are set once in `sps_init`, while crop rectangle and SAR — which may be reconfigured mid-stream — are refreshed by `sps_init_reconfigurable`.

---

## 3. Public interface

All entry points are template-generated (bit-depth-specific) via the `x264_template` macro scheme shared by the rest of the encoder. Signatures and contracts:

- `void x264_sps_init( x264_sps_t *sps, int i_id, x264_param_t *param )` — fully derive an SPS from parameters; assigns the SPS id.
- `void x264_sps_init_reconfigurable( x264_sps_t *sps, x264_param_t *param )` — recompute crop offsets and sample-aspect-ratio presence only.
- `void x264_sps_init_scaling_list( x264_sps_t *sps, x264_param_t *param )` — instantiate the eight scaling lists from a CQM preset.
- `void x264_sps_write( bs_t *s, x264_sps_t *sps )` — emit SPS RBSP (with `rbsp_trailing_bits`), realigning the writer first.
- `void x264_pps_init( x264_pps_t *pps, int i_id, x264_param_t *param, x264_sps_t *sps )` — derive a PPS, linking it to `sps->i_id`.
- `void x264_pps_write( bs_t *s, x264_sps_t *sps, x264_pps_t *pps )` — emit PPS RBSP.
- `void x264_sei_write( bs_t *s, uint8_t *payload, int size, int type )` — generic SEI wrapper: writes payload type and size with the 0xFF continuation extension, copies payload bytes, appends RBSP trailing bits, flushes.
- `void x264_sei_recovery_point_write( x264_t *h, bs_t *s, int cnt )`, `int x264_sei_version_write( x264_t *h, bs_t *s )`, `void x264_sei_buffering_period_write( x264_t *h, bs_t *s )`, `void x264_sei_pic_timing_write( x264_t *h, bs_t *s )`, `void x264_sei_frame_packing_write( x264_t *h, bs_t *s )`, `void x264_sei_mastering_display_write( x264_t *h, bs_t *s )`, `void x264_sei_content_light_level_write( x264_t *h, bs_t *s )`, `void x264_sei_alternative_transfer_write( x264_t *h, bs_t *s )`, `void x264_sei_dec_ref_pic_marking_write( x264_t *h, bs_t *s )`, `int x264_sei_avcintra_umid_write( x264_t *h, bs_t *s )`, `int x264_sei_avcintra_vanc_write( x264_t *h, bs_t *s, int len )`, `void x264_filler_write( x264_t *h, bs_t *s, int filler )` — specialized SEI/filler emitters.
- `int x264_validate_levels( x264_t *h, int verbose )` — returns nonzero if any level constraint is violated; logs warnings when `verbose`.

Internal (static) helpers: `transpose()` (in-place square matrix transpose), `scaling_list_write()`.

---

## 4. State and data structures

- `x264_sps_t` — holds `i_id`, macroblock dimensions (`i_mb_width`, `i_mb_height`), `b_frame_mbs_only`, `i_chroma_format_idc`, `i_profile_idc`, the four `b_constraint_setN` flags, `i_level_idc`, `i_log2_max_frame_num`, `i_poc_type`, `i_log2_max_poc_lsb`, `i_num_ref_frames`, cropping (`crop`, `b_crop`), `b_qpprime_y_zero_transform_bypass`, `b_avcintra_hd`, `b_avcintra_4k`, `i_cqm_preset`, `scaling_list[8]`, and a `vui` substructure (`i_num_reorder_frames`, `i_max_dec_frame_buffering`, aspect ratio, overscan, video signal, chroma location, timing, HRD, pic_struct, bitstream restriction fields). Ownership: embedded in `x264_t`; `scaling_list[]` entries are pointers either to static constant tables or to caller-supplied arrays — never owned/freed here.
- `x264_pps_t` — holds `i_id`, `i_sps_id`, `b_cabac`, `b_pic_order`, `i_num_slice_groups`, default reference indices, weighted-prediction flags, `i_pic_init_qp`, `i_pic_init_qs`, `i_chroma_qp_index_offset`, deblocking/constrained-intra/redundant flags, `b_transform_8x8_mode`.
- `x264_level_t` (from `common/common.h`) — table keyed by `level_idc` with `frame_size`, `dpb`, `bitrate`, `cpb`, `mv_range`, `mbps`, `frame_only`.
- `bs_t` — bit writer; functions call `bs_realign` before and `bs_flush` after header/SEI writes.

Invariants: `sps_init` must run before `sps_write`; `pps_init` must receive an already-derived SPS. Scaling-list pointers must be valid through SPS/PPS write time.

---

## 5. Algorithm / behaviour

### 5.1 SPS derivation (`x264_sps_init`)

Dimensions: macroblock width/height are `ceil(width/16)` and `ceil(height/16)`. If either interlaced mode is active the frame is field-coded, so `b_frame_mbs_only = 0` and `i_mb_height` is rounded up to an even number of macroblocks. Chroma format idc is chosen from the colour-space family: 4:4:4, 4:2:2, 4:2:0, or monochrome (400) in descending order.

QP-prime-zero bypass: when rate control is constant-QP with QP exactly 0, `b_qpprime_y_zero_transform_bypass` is set.

Profile selection (highest applicable wins, evaluated in this order):
1. `HIGH444_PREDICTIVE` if QP-prime bypass or chroma 4:4:4.
2. `HIGH422` if chroma 4:2:2.
3. `HIGH10` if compiled bit depth > 8.
4. `HIGH` if 8x8 transforms enabled, a non-flat CQM preset is used, or monochrome.
5. `MAIN` if CABAC, B-frames, interlacing, or weighted prediction is in use.
6. Otherwise `BASELINE`.

Constraint flags: `set0` is set only for Baseline; `set1` for Baseline/Main (set0 and set1 are mutually exclusive in practice because x264 does not use Baseline-only features such as arbitrary slice order or slice groups); `set2` is always zero; `set3` is set for level 1b (a requested level of 9 combined with Baseline/Main, which is rewritten to level 11) and for all-intra ("intra profiles": keyint max 1 with High-or-higher profile).

Reference/DPB sizing: `num_reorder = pyramid ? 2 : (bframes ? 1 : 0)`. `num_ref_frames = max_dec_frame_buffering = min(X264_REF_MAX, max(frame_reference, 1+num_reorder, pyramid?4:1, dpb_size))`; strict B-pyramid subtracts one from `num_ref_frames`. For all-intra, both are forced to zero. The maximum frame number is `max_dec_frame_buffering * (pyramid?2:1) + 1`, raised to at least `min(mb_width-1, keyint_max) + bframes - 1 + 1` when intra refresh is enabled (so the recovery-point distance fits). `log2_max_frame_num` starts at 4 and increments while `2^log2 <= max_frame_num`.

Picture order count: type 0 is used whenever B-frames, interlacing, or AVC-Intra class is present; otherwise type 2 (POC derived directly from frame_num). For type 0, `max_delta_poc = (bframes+2) * (pyramid?2:1) * 2` and `log2_max_poc_lsb` starts at 4, incrementing while `2^log2 <= max_delta_poc*2`.

Other SPS fields: `b_vui = 1`, gaps-in-frame-num not allowed, MB-adaptive frame/field equals interlaced, direct-8x8 inference always on. VUI defaults: overscan present only for values 1–2; video format clamped to 0–5 else 5; full range defaulted to 1 for BGR input else 0; colour primaries clamped 0–12 else 2; transfer clamped 0–18 else 2; matrix clamped 0–14 else 0 for BGR / 2 otherwise; colour description present if primaries/transfer/matrix differ from 2; signal-type present if format != 5 or full range or colour description present. Chroma-location present only for values 1–5 with 4:2:0. Timing info present when both timebase numerator and denominator are positive; `num_units_in_tick = timebase_num`, `time_scale = timebase_den*2`, fixed-frame-rate is the inverse of VFR input. NAL HRD presence follows `i_nal_hrd`; VCL HRD is never used; pic_struct presence follows `b_pic_struct`. Bitstream restriction is signalled unless constraint_set3 with High-or-higher profile; when present it sets MV-over-boundaries = 1, zero denominators, and `log2_max_mv_length = floor(log2(max(1, mv_range*4-1))) + 1`. AVC-Intra flags: `b_avcintra_hd` for class 1–200, `b_avcintra_4k` for class > 200. Finally `i_cqm_preset` is copied.

Reconfigurable part: crop right/bottom are the user crop values plus the padding introduced by rounding to whole macroblocks (`crop_rect.right + mb_width*16 - width`, similarly for height); crop is flagged if any edge is nonzero. SAR presence requires both dimensions positive.

### 5.2 Scaling lists

Eight logical lists: four 4x4 (luma intra, luma inter, chroma intra, chroma inter) and four 8x8. FLAT preset points all eight at a flat list; JVT points them at the JVT default tables; CUSTOM transposes each user-supplied 4x4/8x8 matrix (to match the encoder's transposed DCT and zigzag ordering), assigns the pointers, and falls back to the JVT table for any list containing a zero element.

When writing a list: `len` is 16 for 4x4 lists, 64 for 8x8; elements are traversed in the appropriate zigzag scan order. The "default list" for chroma lists is the corresponding luma list (so chroma may inherit luma), otherwise the JVT default. Three cases: (a) equal to default → `scaling_list_present_flag = 0`; (b) equal to JVT → flag 1 plus `se(-8)` as the "use default" escape; (c) otherwise flag 1, then delta values `se(int8(list[i] - previous))` with an implicit previous of 8, with run-length compression of trailing equal values when the `-last_value` escape is cheaper than writing them.

### 5.3 SPS/PPS bitstream layout

SPS: profile_idc (8 bits), four constraint flags (1 bit each), 4 reserved zero bits, level_idc (8), seq_parameter_set_id (ue). For High-or-higher profiles: chroma_format_idc (ue), separate_colour_plane_flag for 444 (1), bit-depth-minus-8 for luma and chroma (ue each), qpprime-y-zero bypass (1), and a seq_scaling_matrix_present_flag that is only set for AVC-Intra HD, in which case a fixed subset of scaling lists and explicit "no inter" flags are written to match AVC-Intra exactly. Then log2_max_frame_num_minus4, pic_order_cnt_type (ue, with log2_max_poc_lsb_minus4 for type 0), max_num_ref_frames (ue), gaps flag (1), pic_width_in_mbs_minus1, a height field divided by two for field coding, frame_mbs_only flag, optional mb_adaptive flag, direct8x8 inference flag, crop flag and four crop offsets (shifted right by chroma subsampling and field polarity), VUI flag. The VUI block writes, in order and each gated by its presence flag: aspect ratio (a short lookup table of common SARs, with an extended 16+16 bit form), overscan, video signal (format/fullrange/colour description), chroma location, timing (32+32 bits plus fixed-frame-rate), NAL HRD parameters (CPB count, bit-rate and CPB-size scales and values, CBR flag, four delay-length fields), VCL HRD flag, low-delay HRD flag when HRD is present, pic_struct flag, and bitstream restriction fields (reorder count, max DPB). RBSP trailing bits and a flush complete the NAL.

PPS: pps_id and sps_id (ue), entropy-coding-mode (CABAC) flag, bottom-field-ordering flag, slice-group count minus one, default active reference counts for list 0/1 minus one, weighted-pred and 2-bit weighted-bipred flags, `pic_init_qp_minus26` and `pic_init_qs_minus26` (se), chroma QP index offset (se), deblocking-filter-control / constrained-intra / redundant-pic-count flags. The optional High-profile tail is emitted when 8x8 transform mode or non-flat scaling lists are signalled: transform-8x8 flag, scaling-list flag, the scaling lists (with AVC-Intra-specific "Cr = Cb" / "no inter" shortcuts), and the second chroma QP offset; then RBSP trailing bits.

`pic_init_qp`: ABR rate control or stitchable streams use neutral `26 + QP_BD_OFFSET`; constant-QP uses the specified QP. `pic_init_qs` is always neutral.

### 5.4 SEI messages

Generic wrapper: the payload type is written as a run of 0xFF bytes (one per full 255) followed by the remainder; the payload size uses the same scheme; payload bytes follow verbatim; then RBSP trailing bits and flush. All specialized writers first serialize their payload into a small stack buffer with `bs_realign`, write the payload bit fields, call `bs_align_10` to byte-align with the standard SEI trailing-one pattern, and pass the byte count to the generic wrapper.

- Recovery point: recovery_frame_cnt (ue), exact_match = 1, broken_link = 0, changing_slice_group = 0.
- Version: a 16-byte UUID constant followed by a human-readable string "x264 - core N[VER] - ... - options: <param string>" where the copyright verb is selected by GPL build; the string is NUL-terminated and included in the payload.
- Buffering period: SPS id, initial CPB removal delay and offset using the SPS delay length; used with HRD.
- Picture timing: CPB removal delay and DPB output delay (when HRD), then pic_struct index minus one (4 bits) and zero clock_timestamp_flag bits (count indexed by a 10-entry `num_clock_ts` table).
- Frame packing: arrangement id 0, cancel 0, arrangement type (7 bits, from `i_frame_packing`), quincunx flag (type 0), content interpretation (type != 6), several zero flags, alternating `current_frame_is_frame0_flag` for frame-packing type 5, optional grid positions, reserved byte, and a repetition period of 0/1 except type 5.
- HDR: mastering-display writes eight 16-bit chromaticity coordinates plus two 32-bit luminance bounds; content-light-level writes two 16-bit values; alternative-transfer writes one byte.
- Dec-ref-pic-marking (for repeated B-refs on Blu-ray): original IDR 0, original frame_num, optional field flag, and a single MMCO type-1 command per prior MMCO with `difference_of_pic_nums - 1`, terminated by MMCO 0.
- AVC-Intra UMID/VANC: a fixed UUID plus "UMID"/"VANC" markers and filler bytes; VANC validates length (0 ≤ len ≤ 6000) and fails with a log otherwise.
- Filler: a run of 0xFF bytes followed by RBSP trailing bits.

### 5.5 Level validation

Compute `mbs = mb_width * mb_height`, `dpb = mbs * max_dec_frame_buffering`, and a profile-dependent CPB/bitrate factor `cbp_factor`: 16 for High 4:2:2, 12 for High 10, 5 for High, 4 otherwise. Locate the `x264_level_t` entry matching `i_level_idc`. Fail if frame size exceeds the level limit (also checking `8*frame_size >= width_mbs^2` and `>= height_mbs^2`), if DPB exceeds its limit, if VBV bitrate exceeds `bitrate*cbp_factor/4`, if VBV buffer exceeds `cpb*cbp_factor/4`, if MV range exceeds the limit, if interlaced/fake-interlaced is not permitted, or (when fps denominator > 0) if `mbs * fps_num / fps_den` exceeds `mbps`. Each failure logs a warning only when `verbose` and sets a nonzero return code.

---

## 6. Dependencies

Calls `bs_*` primitives from `common/bitstream.h`, `x264_param2string`, `x264_log`, `x264_cqm_*` constant tables, `x264_levels`, `x264_zigzag_scan4/8` from `common/tables.h`, and `x264_clip3` macros. `sps_init` is called from encoder open/reconfig; `sps_write`/`pps_write` are invoked by the NAL-emission path in `encoder.c`; SEI writers are invoked from `encoder.c` (headers/frame output) and, for buffering-period/pic-timing, from rate-control HRD logic. `x264_validate_levels` is called during parameter validation.

---

## 7. Constants and tables

- `num_clock_ts[10] = {0,1,1,1,2,2,3,3,2,3}` — clock timestamp count per pic_struct value (index 0 = Auto).
- AVC-Intra UUID: 16 fixed bytes identifying the unregistered SEI.
- Version UUID: a second 16-byte constant (ISO-11578 style).
- SAR lookup: 16 common aspect ratios coded as `aspect_ratio_idc` 1–16, terminated by an extended (255) entry; values 17–254 are reserved.
- Level table `x264_levels` from `common/common.h` (limits per level).
- CQM tables: `x264_cqm_flat16`, `x264_cqm_jvt[8]`, plus user arrays in `x264_param_t`.
- Bit-depth macros `BIT_DEPTH`, `QP_BD_OFFSET`, `SPEC_QP`, chroma-format and profile idc enums from `common/common.h`.

---

## 8. Performance notes

This is a once-per-stream (and occasionally once-per-reconfig) control-path module; it is never in the per-frame hot loop, so raw speed is unimportant relative to correctness.

Realizable opportunities:
- **Header caching**: the SPS/PPS RBSP bytes depend only on a small subset of `x264_param_t`. An implementation can memoize the serialized byte blobs keyed by that subset and reuse them across reconfigs that do not touch header-relevant fields, avoiding repeated bit-writing and the `x264_param2string` call in the version SEI. Impact: none on bitstream or quality; purely a startup latency win.
- **Constant-table compression**: the SAR lookup and CQM defaults can be prepared as pre-encoded bytes rather than written bit-by-bit. No output change.
- **SEI buffer reuse**: the writers allocate a 100-byte stack scratch buffer each call; pooling a single scratch buffer is trivial but marginal.

Honesty: none of these affect decode correctness or RD quality; the dominant cost in `x264_sei_version_write` is formatting the option string, which a build can skip entirely if the version SEI is disabled. No SIMD applies. Branch behaviour is irrelevant given negligible call frequency.

---

## 9. References

- ITU-T H.264 (AVC) recommendation, Annex E (SPS), Annex F.1 (VUI), Annex D (SEI), Annex A (profiles and levels).
- x264 public documentation and `x264.h` API reference (`x264_param_t` semantics).
- AVC-Intra specification (Panasonic) for the UMID/VANC/AVC-Intra-specific scaling-list behaviour.
- SMPTE ST 2086 / CEA-861.3 for mastering display and content light level SEI semantics.

---

## 10. Verification checklist

- Parse the emitted SPS with an independent bitstream parser: verify profile/level, chroma format, `log2_max_frame_num`, POC type, reference count and VUI fields match the documented formulas for a matrix of parameter sets.
- Compare SPS/PPS RBSP bytes bit-for-bit against a known-good encoder for identical parameters (including all-intra, interlaced, 4:2:2/4:4:4, high bit depth, CQM presets).
- Check level 9 + Baseline/Main is rewritten to level 11 with constraint_set3; check intra-profile constraint_set3.
- Validate crop/SAR reconfiguration emits a new SPS with updated crop offsets but unchanged non-reconfigurable fields.
- Verify all SEI payloads decode to their documented fields (recovery point flags, version string, HDR coordinates, frame packing type).
- Confirm `x264_validate_levels` returns nonzero and logs exactly when a level limit is exceeded, using boundary-value parameter sets.
- Check scaling-list round-trip: FLAT/JVT/CUSTOM, including the chroma-inherits-luma fallback and the zero-element JVT fallback.

---

## 11. Open questions

- The exact AVC-Intra scaling-list write sequence for 4:4:4 4K (beyond what is described above) is UNKNOWN without AVC-Intra documentation.
- Whether `x264_sei_avcintra_umid_write` uses the `s` argument or always `h->out.bs` (the reference writes to the latter) is an implementation detail; external behaviour is a fixed payload.
- The `b_stitchable` flag's interaction with `pic_init_qp` is defined in `x264_param_t`; precise conditions under which it is set are UNKNOWN here.
