# filters/video/depth.c — bit-depth conversion filter

*Dirty-side behavioural specification. Reference source: `filters/video/depth.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/depth.c`
- **Role:** Implements `depth_8`/`depth_10`, which convert between 8-bit and 16-bit sample representations using either a left-shift up-conversion or Sierra-2-4A error-diffusion dithering down-conversion.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

The encoder supports specific bit depths; source material may be 8-bit or 9..16-bit. This filter adjusts the `X264_CSP_HIGH_DEPTH` flag and sample width so frames match the encoder's configured depth. Up-conversion is a simple left shift (lossless). Down-conversion dithers with error diffusion so that a source previously up-converted by the same algorithm round-trips losslessly. It is compiled twice (8/10 intra variants) because the target `pixel` type differs.

## 3. Public interface

Exports `cli_vid_filter_t depth_8_filter` / `depth_10_filter` with names `"depth_8"` / `"depth_10"` and the standard vtable.

- `init`: computes whether the high-depth flag must change; optionally parses `bit_depth=<n>` from `opt_string`; validates the requested depth equals this variant's `BIT_DEPTH`; installs the filter only if a conversion is needed.
- `get_frame`: after delegating, either dither down (high-depth → 8-bit target) or scale up (8-bit source → high-depth), then replaces the picture descriptor with the internal buffer.
- `release_frame` delegates; `free_filter` frees previous filter, buffer, and handle.

## 4. State and data structures

`depth_hnd_t` fields: `prev_hnd`, `prev_filter`, `bit_depth` (target), `dst_csp`, `buffer` (an allocated `cli_pic_t` for converted output), `error_buf` (a `int16_t` scratch array of `width+1` entries, allocated immediately after the handle in one allocation). The error buffer is shared across planes/rows and zeroed per plane.

Supported colourspaces (masked): I400, I420, I422, I444, YV12, YV16, YV24, NV12, NV21, NV16, BGR, RGB, BGRA. Interleaved sample counts per plane are format-specific: NV12/NV21/NV16 chroma plane has 2 interleaved samples; BGR/RGB have 3; BGRA has 4; planar formats have 1.

## 5. Algorithm / behaviour

**Init logic.** The "change format" flag is the XOR of the source csp's high-depth bit and the encoder parameter csp's high-depth bit. A target csp is derived by toggling that bit. The initial bit depth is `8 * depth_factor(csp)`. If options are supplied, `bit_depth=<n>` overrides (8..16), the target csp's high-depth bit is set/cleared accordingly, and `change_fmt` is recomputed. Invalid depth or a depth not equal to this template's `BIT_DEPTH` is rejected. The filter is inserted only when `change_fmt` is set or the source's byte depth differs from the target depth; the target csp must be in the supported set. The handle is allocated with extra space for the error buffer, a destination picture is allocated via `x264_cli_pic_alloc`, and `info->csp` is updated to the target csp.

**Up-conversion (`scale_image`).** For each plane, every 8-bit source sample is written into the corresponding 16-bit destination sample shifted left by `BIT_DEPTH - 8`. Processing is row-by-row with source/destination strides; destination stride is interpreted in 16-bit units (`stride/2`). This is the same operation as the raw/Y4M demuxers' non-byte-aligned up-conversion.

**Down-conversion (`dither_image`).** A family of four row routines (interleaving pitch 1/2/3/4) implements Sierra-2-4A-style error diffusion:

- The error row is zeroed at the start of each plane.
- For each pixel, an accumulator is formed from twice the previous horizontal error plus the stored errors at the current and next column (the next column is the history slot for the row above in the diffusion schedule).
- The 16-bit source sample is shifted left by 2, the accumulator and a rounding half are added, and the result is shifted right by `16 - BIT_DEPTH + 2`, clipped to `[0, 2^BIT_DEPTH - 1]`, and stored as the `pixel` output.
- The residual error — source sample minus output value shifted back up by `16 - BIT_DEPTH` — is saved into the error row for the current column and becomes the next pixel's horizontal error.
- Because the same shift-up is used by the demuxers and `scale_image`, dithering down after up-conversion reproduces the original 8-bit values exactly.

For interleaved formats each interleaved component (e.g. B, G, R or U, V) is dithered independently with its own sub-plane stride; the alpha component of BGRA is dithered too (the source comment notes it could probably be skipped).

**Frame delivery.** After the previous filter fills `output`, if the target is < 16 bits and the frame is high-depth, `dither_image` writes into `h->buffer.img` and the descriptor is replaced by the buffer; if the target is > 8 bits and the frame is 8-bit, `scale_image` does the same. If no conversion is needed for a particular frame, the descriptor passes through unchanged.

## 6. Dependencies

- `filters/video/video.h`, `common/common.h` (`x264_clip3`, `x264_malloc`/`x264_free`, `pixel`, `SIZEOF_PIXEL`), `filters/filters.h` for option parsing, `input/input.h` geometry, error macros tagged `depth_8`/`depth_10`.
- Inserted by the frontend when encoder bit depth differs from source format.

## 7. Constants and tables

- Dither constants derived from `BIT_DEPTH`: `lshift = 16 - BIT_DEPTH`, `rshift = 18 - BIT_DEPTH`, `half = 1 << (17 - BIT_DEPTH)`, `pixel_max = (1 << BIT_DEPTH) - 1`.
- Interleave pitches: 1 (planar), 2 (NV chroma), 3 (BGR/RGB), 4 (BGRA).
- Supported csp list in section 4.
- Filter names: `"depth_8"`, `"depth_10"`.

## 8. Performance notes

- Down-conversion is O(pixels) with a small per-pixel arithmetic chain; up-conversion is a simple shift loop. These are among the heavier filters.
- **Reservation sizing:** the error row is allocated once with the handle; the output picture is allocated once in `init`.
- **Zero-copy:** not possible — sample width changes require a new buffer.
- **SIMD status:** the reference uses scalar C; a clean-room implementation can vectorise the up-conversion easily, while error diffusion is serial per row (harder to vectorise, though multiple rows/interleaves are independent).
- **Branch behaviour:** the dithering routine is selected by interleaving count at init (constant per frame).
- Reference: Sierra-2-4A error diffusion (published dithering algorithm).

## 9. References

- Sierra-2-4A error diffusion / Floyd–Steinberg-family dithering literature.
- ITU-T H.264 / ISO 14496-10 for bit-depth and sample-range conventions.
- `input/raw.c`, `input/y4m.c`, `filters/video/internal.h` (companion specifications).

## 10. Verification checklist

1. Feed an 8-bit frame and request 10-bit output; verify each 16-bit sample equals the 8-bit value shifted left by 2 (for the 10-bit variant).
2. Feed a 10-bit frame produced by step 1 and request 8-bit output; verify the original 8-bit values are recovered exactly (round-trip property).
3. Verify BGR/RGB/BGRA interleaved dithering processes each component independently.
4. Verify unsupported colours are rejected.
5. Verify `bit_depth` option outside 8..16 or not matching the variant is rejected.
6. Verify the filter is not installed when no depth change is needed (info unchanged).
7. Verify clean teardown with ASAN (single combined allocation for handle+error buffer).

## 11. Open questions

- UNKNOWN: behaviour for bit depths other than 8/10 with the two available templates (e.g. requesting 12-bit when only 8/10 variants are built) — init rejects depth != BIT_DEPTH.
