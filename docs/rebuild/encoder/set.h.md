# `encoder/set.h`

**Role:** Declares the header-writing (SPS/PPS/SEI) interface implemented in `encoder/set.c`.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/set.h`
- Role: public (within the encoder) declaration of parameter-set derivation, bitstream writing, SEI emission, filler, and level-validation functions.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

This header is the compile-time contract between `encoder.c` (which drives NAL emission) and `encoder/set.c` (which knows the H.264 bitstream syntax). Every declared function is wrapped in the `x264_template` macro mechanism so the same source compiles into bit-depth-specific symbols (`x264_8_sps_write`, `x264_10_sps_write`, ...), allowing `api.c` to dispatch by `param->i_bitdepth`.

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
- `int x264_sei_avcintra_vanc_write( x264_t *h, bs_t *s, int len )` — returns -1 if `len` is negative or larger than the internal buffer.
- `void x264_sei_write( bs_t *s, uint8_t *payload, int payload_size, int payload_type )` — the generic payload-type/size extension encoder.
- `void x264_filler_write( x264_t *h, bs_t *s, int filler )` — emits `filler` bytes of 0xFF plus RBSP trailing bits.

Validation:

- `int x264_validate_levels( x264_t *h, int verbose )` — nonzero means at least one level limit is exceeded; `verbose` controls warning logs.

Every symbol is defined as `#define x264_foo x264_template(foo)` plus its prototype; consumers call the un-templated name and the preprocessor specializes it.

---

## 4. State and data structures

The header itself defines no structures; it consumes `x264_sps_t`, `x264_pps_t`, `x264_t`, `x264_param_t`, and `bs_t` from `common/common.h` / `common/bitstream.h`. The contracts assume:

- `x264_sps_t.scaling_list[8]` holds pointers valid at write time (static tables or user arrays).
- `x264_t.sh_backup` carries the slice-header fields read by `x264_sei_dec_ref_pic_marking_write`.
- `x264_t.sps` is the active SPS read by the buffering-period and picture-timing writers.
- `bs_t` must have enough remaining capacity for the serialized header; callers provide a correctly sized buffer.

---

## 5. Algorithm / behaviour

The header is declarative; behaviour lives in `set.c`. The interface enforces a two-stage SPS lifecycle: derive (`sps_init`) then, after any reconfiguration of crop/SAR, refresh (`sps_init_reconfigurable`) before re-serializing. PPS derivation requires a valid, already-derived SPS because several PPS fields depend on SPS id and chroma format. SEI writers follow a common pattern: build the payload in a temporary bitwriter, byte-align with the SEI trailing-one convention, then hand the bytes and a payload type id to `x264_sei_write`. Callers are responsible for starting a new NAL and setting NAL type/ref-idc around these writes; this module never emits start codes itself.

---

## 6. Dependencies

Included by `encoder/set.c` (definition site) and by `encoder/encoder.c` (call site). Depends on `common/common.h` for the opaque types and `common/bitstream.h` for `bs_t`. Symbols are specialized by the `x264_template` macro from `common/base.h`.

---

## 7. Constants and tables

No constants are defined in the header. Payload-type identifiers (e.g. `SEI_RECOVERY_POINT`, `SEI_USER_DATA_UNREGISTERED`, `SEI_BUFFERING_PERIOD`, `SEI_PIC_TIMING`, `SEI_FRAME_PACKING`, `SEI_MASTERING_DISPLAY`, `SEI_CONTENT_LIGHT_LEVEL`, `SEI_ALTERNATIVE_TRANSFER`, `SEI_DEC_REF_PIC_MARKING`) come from `common/set.h` / H.264 Annex D.

---

## 8. Performance notes

Headers are written only at stream start and on reconfiguration; SEI messages are written per relevant frame but are tiny. No hot-loop concerns and no SIMD. A clean implementation may cache serialized SPS/PPS blobs and emit SEI payloads from preformatted byte templates without any observable change; see `set.c.md` section 8.

---

## 9. References

- ITU-T H.264 Annex D (SEI), Annex E (SPS), Annex F.1 (VUI).
- x264 public API (`x264.h`) and `x264_param_t` documentation.

---

## 10. Verification checklist

- Compile-time check: every declared function has exactly one bit-depth-specialized definition (`x264_8_*` / `x264_10_*`) and the templated macro resolves correctly for both build configurations.
- Declare-only check: no function in this header allocates or frees memory visible to the caller (except via the documented string allocation in `x264_sei_version_write`, which frees internally).
- Confirm the AVC-Intra VANC writer's length guard rejects out-of-range `len` values through a black-box call.

---

## 11. Open questions

- Whether any additional SEI writers are exposed in other builds (e.g. newer x264 revisions) is UNKNOWN; this header reflects the declared set in the reference snapshot.
