# filters/video/crop.c — crop filter

*Dirty-side behavioural specification. Reference source: `filters/video/crop.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/crop.c`
- **Role:** Implements `crop_filter`, which removes pixels from the left/top/right/bottom edges of each frame by adjusting the reported dimensions and plane pointer origins — no data is copied.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Cropping is a zero-copy operation: the underlying frame buffer is unchanged; only the visible rectangle and plane starting addresses change. This filter validates crop amounts against the colourspace's alignment constraints, updates `video_info_t` with the new dimensions, and shifts each plane pointer by the appropriate byte offset at `get_frame` time. It is one of the most commonly used filters and is straightforward for a clean-room implementation.

## 3. Public interface

Exports `cli_vid_filter_t crop_filter = { "crop", help, init, get_frame, release_frame, free_filter, NULL }`.

- `help` prints `crop:left,top,right,bottom` and, in long form, a one-line description.
- `init`: parses four named options via `x264_split_options`, validates them, computes the output rectangle, no-ops if all crops are zero, otherwise installs the filter and updates `info->width`/`height`.
- `get_frame`: delegates to the previous filter, then adjusts `output->img.width/height` and each plane pointer.
- `release_frame`: delegates directly to the previous filter (no un-shifting).
- `free_filter`: frees the previous filter and the handle.

## 4. State and data structures

`crop_hnd_t` fields: `prev_hnd`, `prev_filter`, `dims[4]` (left, top, output width, output height — note slots 2/3 are overwritten from margins to final dimensions), `csp` (pointer to the colourspace descriptor). The option list is `{"left","top","right","bottom",NULL}`.

## 5. Algorithm / behaviour

**Init and validation.** The input csp must be valid. The option string is parsed into named values; each of the four values must be present, non-negative, and a multiple of the relevant alignment modulus. The modulus for horizontal margins (left/right) is `csp->mod_width`; for vertical margins (top/bottom) it is `csp->mod_height << info->interlaced` — doubling the vertical modulus for interlaced sources so each field's chroma alignment is respected. Output width/height are computed as `input_width - left - right` and `input_height - top - bottom`; both must be positive. If neither dimension changed, the handle is freed and the filter is not installed (a true no-op). Otherwise `info` is updated and the wrapper installed.

**Frame delivery.** The previous filter produces the frame. `output->img.width/height` are set to the cropped dimensions. For each plane, the pointer is advanced by `stride[i] * top * height_factor[i] + left * width_factor[i] * depth_factor` bytes — i.e. down `top` scaled rows and right `left` scaled samples. Note the output stride is *not* changed; downstream consumers read only the cropped width from each row, and the skipped columns between rows remain part of the stride. This is why cropping cannot copy pixels: it simply reinterprets the existing buffer with a smaller visible window.

**Release.** The frame is handed straight to the previous filter's `release_frame`. The source comment notes that no filter should depend on plane pointers for release, so un-shifting is unnecessary.

## 6. Dependencies

- `filters/video/video.h`, `filters/filters.h` (option helpers), `input/input.h` (colourspace geometry), error macros tagged `"crop"`, `printf` for help.

## 7. Constants and tables

- Filter name: `"crop"`.
- Option names: `left`, `top`, `right`, `bottom`.
- Alignment rule: horizontal margins multiple of `mod_width`; vertical margins multiple of `mod_height << interlaced`.
- No pixel constants; geometry from the csp descriptor.

## 8. Performance notes

- Per frame: one delegation, two integer assignments, and up to four pointer arithmetic operations — essentially free.
- **Zero-copy:** the canonical example; never copy pixels for cropping.
- **Reservation sizing:** none needed (no owned buffers).
- **Branch behaviour:** the no-op case is decided once at init and removes the filter entirely.
- Reference: standard planar image cropping by pointer offset.

## 9. References

- `filters/filters.c`, `filters/video/video.h`, `input/input.c` (companion specifications).

## 10. Verification checklist

1. Crop I420 1920x1080 by left=2,top=2,right=2,bottom=2 and confirm 1916x1076 with correctly shifted luma/chroma pointers.
2. Confirm chroma pointer offsets are half the luma offsets for 4:2:0.
3. Confirm a crop amount not divisible by `mod_width`/`mod_height` is rejected.
4. Confirm interlaced sources double the vertical modulus.
5. Confirm all-zero crops do not install the filter (info unchanged).
6. Confirm release passes through without modifying the plane pointers.
7. Confirm negative/zero output dimensions are rejected.

## 11. Open questions

- UNKNOWN: whether downstream filters that allocate buffers based on `img.width`/`height` but use the original stride handle the shifted pointer correctly in every case (e.g. resize reading beyond the cropped width); this is expected but not proven here.
