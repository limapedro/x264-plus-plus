# `encoder/api.c`

**Role:** Provides the bit-depth-independent public x264 API: a dispatcher that selects 8-bit or 10-bit implementations and exposes the opaque `x264_t` handle.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `encoder/api.c`
- Role: implement the externally visible `x264_encoder_*` functions by routing each call through a vtable-like structure to the appropriate bit-depth-specific implementation.
- Marker: dirty-side behavioural specification.

---

## 2. Purpose

x264 is compiled with configurable bit-depth support (8-bit only, 10-bit only, or both). The public API must present one symbol set regardless of build, while internally each depth has its own `x264_t` layout and functions. This file bridges them: `x264_encoder_open` allocates a small dispatch object, chooses the 8 or 10-bit implementation based on `param->i_bitdepth`, stores function pointers, and returns the dispatch object cast to the opaque `x264_t*`. Every subsequent public call casts the handle back and forwards through the stored pointers.

---

## 3. Public interface

Exported symbols (all `REALIGN_STACK`-annotated where relevant):

- `x264_t *x264_encoder_open( x264_param_t *param )` — allocate the dispatch object and open a depth-specific encoder; returns NULL on allocation failure, unsupported bit depth, or inner open failure.
- `void x264_encoder_close( x264_t *h )` — invoke the inner close, then free the dispatch object.
- `void x264_nal_encode( x264_t *h, uint8_t *dst, x264_nal_t *nal )` — forward to the depth-specific NAL encoder.
- `int x264_encoder_reconfig( x264_t *h, x264_param_t *param )` — forward runtime reconfiguration.
- `void x264_encoder_parameters( x264_t *h, x264_param_t *param )` — copy the encoder's current parameters out.
- `int x264_encoder_headers( x264_t *h, x264_nal_t **pp_nal, int *pi_nal )` — emit SPS/PPS/SEI header NALs.
- `int x264_encoder_encode( x264_t *h, x264_nal_t **pp_nal, int *pi_nal, x264_picture_t *pic_in, x264_picture_t *pic_out )` — encode one picture.
- `int x264_encoder_delayed_frames( x264_t *h )`, `int x264_encoder_maximum_delayed_frames( x264_t *h )` — query buffering depth.
- `void x264_encoder_intra_refresh( x264_t *h )`, `int x264_encoder_invalidate_reference( x264_t *h, int64_t pts )` — force refresh / drop a reference by PTS.

Global symbol: `const int x264_chroma_format = X264_CHROMA_FORMAT` — advertises the build's chroma capability.

---

## 4. State and data structures

- `x264_api_t` — private struct holding `x264_t *x264` (the real depth-specific encoder context) and nine function pointers matching the entry points above. It is allocated with `calloc`, so all pointers start NULL. The caller only ever sees `(x264_t *)api`; the real `x264_t` is never exposed.
- Depth-specific symbols: `x264_8_encoder_open`, `x264_10_encoder_open`, and matching `*_nal_encode`, `*_encoder_reconfig`, `*_encoder_parameters`, `*_encoder_headers`, `*_encoder_encode`, `*_encoder_close`, `*_encoder_delayed_frames`, `*_encoder_maximum_delayed_frames`, `*_encoder_intra_refresh`, `*_encoder_invalidate_reference`. Each is declared with a `void *` second argument to `*_encoder_open` (the API object, used by the inner code to report errors / access vtables).

Invariants: a handle is valid from successful `open` until `close`; the function pointers are set before the inner `open` is called so inner code can call back into the API if needed; both build-time depth macros guard their branches.

---

## 5. Algorithm / behaviour

`x264_encoder_open`: allocate and zero an `x264_api_t`; return NULL if allocation fails. If this build supports 8-bit and `param->i_bitdepth == 8`, fill all function pointers with the `x264_8_*` variants and call `x264_8_encoder_open(param, api)`. Otherwise, if 10-bit is supported and depth is 10, use the `x264_10_*` variants and call `x264_10_encoder_open`. For any other depth, log an error: "not compiled with %d bit depth support". If the inner open returns NULL, free the dispatch object and return NULL. On success return the dispatch object cast to `x264_t *`. The `REALIGN_STACK` attribute ensures stack alignment across the API boundary (important for SIMD code in the inner implementation).

Every other function is a thin, stateless forwarder: cast the handle to `x264_api_t *`, then call the corresponding stored function pointer with the inner `x264` pointer and the original arguments. `x264_encoder_close` is special: it first calls the inner close (which frees all encoder state) and then frees the dispatch object itself; using the handle afterwards is invalid.

The two-pass ABI has no failure return on `x264_nal_encode`/`parameters`/`close`; `reconfig`, `headers`, `encode`, `delayed_frames`, `maximum_delayed_frames` and `invalidate_reference` propagate the inner integer status.

---

## 6. Dependencies

Declares/forwards to the bit-depth-specific implementations in `encoder/encoder.c` (which define `x264_8_*`/`x264_10_*` via the `x264_template` mechanism). Uses `x264_log_internal` for the unsupported-depth error, `common/base.h` for macros and `x264.h` for public types. No direct dependency on other encoder modules.

---

## 7. Constants and tables

- `x264_chroma_format` is a build-time constant (`X264_CHROMA_FORMAT`).
- No numeric constants beyond bit depths 8 and 10 and the `HAVE_BITDEPTH8`/`HAVE_BITDEPTH10` guards.

---

## 8. Performance notes

This is a one-time-dispatch layer: overhead is one indirect call per public call, negligible relative to encoding. No SIMD, no threading. There is no meaningful optimisation opportunity, and changing dispatch semantics would break the public ABI. A clean implementation should keep the opaque-handle + vtable design so that both bit depths can coexist in one library build.

---

## 9. References

- `x264.h` public API documentation (opaque `x264_t`, parameter/picture/NAL types, function contracts).
- x264 build system documentation on bit-depth and chroma-format variants.

---

## 10. Verification checklist

- In a dual-depth build, open encoders at depth 8 and 10 simultaneously and confirm independent operation and correct dispatch.
- Verify `x264_encoder_open` returns NULL (and leaks nothing) for an unsupported `i_bitdepth`.
- Verify `x264_encoder_close` frees both inner state and the dispatch object (no leaks under a memory checker).
- Confirm `x264_encoder_parameters` returns the effective parameters after reconfiguration.
- Confirm all integer-returning entry points propagate the inner implementation's return value unchanged.
- Check that calling any function after `close` is invalid (documented; not tested by the implementation).

---

## 11. Open questions

- Whether the commercial/proprietary build exposes additional API entry points is UNKNOWN; this describes the GPL snapshot's symbol set.
- The exact second `void *` argument contract to `x264_8/10_encoder_open` is resolved in `encoder.c`; only its presence is visible here.
