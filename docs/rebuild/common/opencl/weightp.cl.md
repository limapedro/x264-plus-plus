# `common/opencl/weightp.cl`

**Role:** Applies weighted-prediction gain and offset to a reference frame's downscaled images and packed half-pel plane so that motion search uses the same weighted reference the encoder would use.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `common/opencl/weightp.cl`
- Role: two small elementwise kernels that transform reference-plane samples by an affine per-sample mapping before lookahead motion estimation.
- Marker: this document is a dirty-side behavioural specification derived from the reference implementation and public H.264 weighted-prediction literature.

---

## 2. Purpose

H.264 weighted prediction allows a reference picture to be scaled and offset before being subtracted from the current picture. The encoder's normal analysis would therefore search against a modified reference. To keep the GPU lookahead consistent, these kernels pre-apply the same affine transform to every pyramid level and to the packed HPEL plane of the reference frame. The results are stored in shared buffers so multiple frames can reuse them without recomputing.

Two kernel variants are needed because the two image layouts differ: the pyramid images are four-channel `uint8` texels, while the HPEL image stores four 8-bit phases packed into one 32-bit word.

---

## 3. Public interface

- **`weightp_scaled_images( read_only image2d_t in_plane, write_only image2d_t out_plane, uint offset, uint scale, uint denom )`** — one work item per texel. Reads a `uint4`, applies the same affine transform to each of the four channels, writes a `uint4`.
- **`weightp_hpel( read_only image2d_t in_plane, write_only image2d_t out_plane, uint offset, uint scale, uint denom )`** — one work item per texel. Reads one 32-bit word, unpacks its four bytes into a `uint4`, applies the affine transform, repacks into a 32-bit word and writes it.

Both kernels are launched with global dimensions `[width, height]` in texels (the host uses the lowres pixel dimensions `[8*mb_width, 8*mb_height]` and halves them per pyramid level).

---

## 4. State and data structures

- `in_plane` / `out_plane`: 2D images. Pyramid images use `CL_RGBA`/`CL_UNSIGNED_INT8`; the HPEL image uses `CL_R`/`CL_UNSIGNED_INT32`.
- The shared output buffers are `h->opencl.weighted_scaled_images[NUM_IMAGE_SCALES]` and `h->opencl.weighted_luma_hpel`, allocated once in `x264_opencl_lowres_init()`. Because they are shared across frames, a new weighting operation overwrites the previous contents; only frames with weighted prediction enabled use them.
- The affine parameters come from `x264_weight_t` computed by `x264_weights_analyse()` on the CPU: `i_offset`, `i_scale`, `i_denom`. The host passes them as `int32_t` values to kernel arguments declared `uint`; see §11 for the signed-value caveat.

---

## 5. Algorithm / behaviour

### 5.1 Affine transform

For each 8-bit sample `v` the transform is:

`out = offset + ((scale * v) >> denom)`

performed in unsigned 32-bit arithmetic on each channel independently. There is no clipping in the kernel: values outside the 8-bit range wrap modulo 2³² before being stored into an 8-bit channel (which itself keeps only the low eight bits). The H.264-weighted sample range is limited by the encoder's choice of parameters so the result normally stays representable.

The three parameters are passed as scalars to all work items; no per-macroblock weighting is used in this lookahead path.

### 5.2 `weightp_scaled_images`

Each work item reads the texel at `(gx,gy)` into a four-channel `uint4`. A vectorized multiply, shift and add apply the transform to all four channels at once. The result `uint4` is written to the same coordinate in `out_plane`. Since the pyramid image's four channels hold four consecutive lowres luma samples (see `downscale.cl`), this transforms four samples per work item.

### 5.3 `weightp_hpel`

Each work item reads one 32-bit word, masks and shifts it into four byte-sized lanes (F/H/V/C), applies the same vectorized transform, then reassembles the four lanes into a 32-bit word with shifts of 0/8/16/24 and writes it. This is a scalar unpack/repack around a vectorized transform, chosen because the HPEL word is stored as a single unsigned integer channel.

### 5.4 Host dispatch contract

`x264_opencl_motionsearch()` in `encoder/slicetype-cl.c`:

- Checks `w && w->weightfn`. Weighted prediction is only prepared when the caller supplies a non-trivial weight, which happens for the list0 search of a B frame when `b == p1` and `i_weighted_pred` is enabled; the list1 search is always called with `NULL`.
- For each pyramid level `i` from 0 to `NUM_IMAGE_SCALES-1`: sets arguments `(fref->scaled_image2Ds[i], weighted_scaled_images[i], w->i_offset, w->i_scale, w->i_denom)` and launches with the current `gdims`; then halves `gdim[0]` and `gdim[1]`, breaking when either falls below 16.
- Launches `weightp_hpel` with `(fref->luma_hpel, weighted_luma_hpel, offset, scale, denom)` at the full lowres dimensions.
- Selects `weighted_scaled_images[]` and `weighted_luma_hpel` as the reference planes for the subsequent HME and subpel kernels; otherwise it selects the unweighted per-frame planes.
- The CPU-side `x264_weights_analyse()` supplies the same `w` used by the corresponding CPU path.

### 5.5 GPU versus CPU output; host selection

These kernels produce weighted reference planes used only for lookahead motion search. The GPU path is selected by `b_opencl`; weighted prediction is applied on the GPU only when the CPU analysis produced a weight with a non-null `weightfn`. The transform is intentionally identical to the CPU weighting formula, so the weighted samples should match. UNKNOWN: the effect of unsigned argument interpretation when `i_offset` or `i_scale` is negative (H.264 permits negative offset in some configurations).

---

## 6. Dependencies

- **Uses from `x264-cl.h`:** `pixel`/`uint` aliases and the program sampler (for image reads in the scaled-images kernel).
- **Input:** per-frame `scaled_image2Ds[]` and `luma_hpel` from `downscale.cl`; `x264_weight_t` parameters from `x264_weights_analyse()` in the CPU encoder.
- **Output:** shared `weighted_scaled_images[]` and `weighted_luma_hpel` consumed by `motionsearch.cl` and `subpel.cl`.
- **Host:** kernels created in `common/opencl.c`; dispatched in `encoder/slicetype-cl.c`.

---

## 7. Constants and tables

- `NUM_IMAGE_SCALES` = 4: number of weighted pyramid levels.
- Minimum launch dimension 16: decimation stops when either axis drops below 16 texels.
- Byte shifts 0/8/16/24 and mask `0x00ff`: HPEL lane extraction and repacking.
- Affine parameters are scalar per kernel launch; no lookup tables are used.

---

## 8. Performance notes

- **Coalescing:** consecutive work items read and write consecutive texels; the scaled-images kernel moves 16 bytes per work item, the HPEL kernel 4 bytes.
- **Arithmetic intensity:** purely elementwise, memory-bandwidth bound; no local memory, barriers or atomics.
- **Occupancy:** trivial register usage, so occupancy is limited only by the driver's work-group choice (no explicit local size is supplied).
- **Shared buffers:** reusing `weighted_*` buffers avoids per-frame allocation but serializes frames that need different weights; the host launches the weighting immediately before the corresponding search, so ordering is preserved by the in-order command queue.
- **Platform limitations:** none specific; this is plain OpenCL 1.1 image I/O.

---

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10, clause 8.4 on weighted sample prediction: `clip((w*pred + offset) >> denom)` motivating the affine transform.
- Khronos Group, *OpenCL C Specification*: `read_imageui`, `write_imageui`, `uint4` vector arithmetic, bitwise shifts.
- x264 documentation on lookahead weighted prediction (`x264_weights_analyse`).

---

## 10. Verification checklist

- With `scale = 2^denom` and `offset = 0`, confirm output equals input for both kernels.
- With `scale = 0` and `offset = C`, confirm every sample becomes `C`.
- For a known ramp input and parameters `(offset,scale,denom)`, confirm each output sample equals `offset + ((scale*v)>>denom)` exactly, including rounding behaviour for fractional results.
- Verify the HPEL kernel preserves byte order F/H/V/C after the transform.
- Verify that all four pyramid levels are processed until the 16-texel cutoff and that the HPEL plane is transformed exactly once.
- Verify that when no weight is supplied, the unweighted reference planes are used and the `weighted_*` buffers are not read.
- Verify GPU weighted samples match the CPU-side weighted reference for a representative frame.

---

## 11. Open questions

- **UNKNOWN:** interpretation of negative `i_offset`/`i_scale` when passed to `uint` kernel arguments; the reference's build path may guarantee non-negative values, but this is not verified here.
- **UNKNOWN:** absence of clipping means out-of-range results wrap when stored into 8-bit channels; whether weight parameters always keep results in `[0,255]` is not established.
- **UNKNOWN:** whether the shared `weighted_*` buffers can be overwritten by a concurrently executing lookahead thread before the search kernel reads them (relies on in-order queue and lookahead threading model).
