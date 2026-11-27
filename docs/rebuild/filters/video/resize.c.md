# filters/video/resize.c — resize / colourspace conversion filter

*Dirty-side behavioural specification. Reference source: `filters/video/resize.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `filters/video/resize.c`
- **Role:** Implements `resize_filter`, which uses libswscale to rescale frames, convert pixel formats/ranges, and adjust SAR, either from explicit user options or to normalise a demuxer's `X264_CSP_OTHER` format.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Sources report pixel formats in different vocabularies (FFmpeg `AVPixelFormat` marked `X264_CSP_OTHER`, AviSynth planar order YVU, various sample ranges), and users may request arbitrary output resolutions/aspect ratios. This filter is the universal normaliser: it maps CLI colourspaces to swscale pixel formats, builds/rebuilds a swscale context when input properties change, handles YVU↔IYUV chroma swapping, and supports fast paths for monochrome conversion. Without swscale it degrades to a minimal pass/fail check.

## 3. Public interface

Exports `cli_vid_filter_t resize_filter = { "resize", help, init, get_frame, release_frame, free_filter, NULL }`.

- `help` documents the option grammar `[width,height][,sar][,fittobox][,csp][,method]` and lists valid csp/method names.
- `init`: decides whether work is needed; parses options or uses encoder parameters; validates formats/resolution; sets up the swscale context; updates `info`.
- `get_frame`: delegates, optionally rebuilds the context for variable inputs, handles chroma swapping, scales into the buffer (or takes a fast path), and swaps back.
- `release_frame` delegates; `free_filter` frees swscale context, buffer, and previous filter.

## 4. State and data structures

`resizer_hnd_t` fields: `prev_hnd`, `prev_filter`, `buffer` (allocated destination picture), `buffer_allocated`, `dst_csp`, `input_range`, `ctx` (SwsContext), `ctx_flags`, `pre_swap_chroma`/`post_swap_chroma`, `fast_mono`, `variable_input`, `working`, `dst` and `scale` (each a `frame_prop_t` of width/height/pix_fmt/range). `frame_prop_t` is compared with `memcmp` to detect input property changes. Ancillary structures map csp↔pix_fmt and select supported formats.

## 5. Algorithm / behaviour

**Need check.** `full_check` ORs four inequalities: source csp vs. `param->i_csp`, width, height, and fullrange vs. `param->vui.b_fullrange`. With no option string, `init` no-ops if none differ. With the special option `"normcsp"`, it no-ops when the csp has no `X264_CSP_OTHER` flag. Otherwise parsing proceeds.

**Option handling.** Options: `width`, `height`, `sar`, `fittobox`, `csp`, `method`. Method names map to swscale flags (fastbilinear, bilinear, bicubic default, experimental, point, area, bicublin, gauss, sinc, lanczos, spline). The output csp option accepts `name[:depth]` with depth 8/16; an empty name keeps the current base format.

Resolution/SAR semantics:
- Width+height given without SAR: SAR is adjusted to preserve display aspect, using `new_sar = (old_w*new_h)/(old_h*new_w)` reduced and multiplied into the old SAR.
- SAR given without resolution: width or height is reduced (rounded to the csp's modulus) so the display aspect stays constant — width shrinks if SAR got wider, height if narrower.
- `fittobox=width|height|both`: the largest resolution that fits the box and preserves the (input→output) SAR is computed, rounded down to mod values, comparing width- and height-limited cases using ratios involving the SARs.
- Csp only: dimensions and SAR are preserved.
- An invalid SAR string accepts `n:m` or `n/m`.

**Format mapping.** `convert_csp_to_pix_fmt` maps CLI formats to swscale pixel formats, respecting the high-depth flag: i400→GRAY8/16; i420/yv12→YUV420P/16; i422/yv16→YUV422P/16; i444/yv24→YUV444P/16; RGB/BGR/BGRA→RGB24/48, BGR24/48, BGRA/BGRA64; NV12/NV21/YUYV/UYVY→only 8-bit variants; NV16→none; `X264_CSP_OTHER` passes its masked value through. `pick_closest_supported_csp` does the reverse via FFmpeg pix-format descriptors: RGB/BGR family → BGRA/BGR/RGB with high-depth from component depth; monochrome → I400; 4:2:0 (planar or semiplanar) → NV12 or I420; 4:2:2 → I422 (NV16 excluded because unsupported); full chroma → I444.

**Flags and validation.** Unless the method is fastbilinear, the context flags gain full-chroma interpolation and accurate-rounding bits. The destination pix format is derived; a source/destination resolution ≤ MAX_RESOLUTION and positive is required. The reverse-depth pix format is checked so a helpful error can name the format + depth when the chosen depth is unsupported. swscale input/output support is verified; interlaced vertical resizing is rejected; output dimensions must satisfy the csp modulus. Log messages announce resizing, csp conversion, and range conversion. A VFLIP flag on the source is preserved in the destination csp.

A fast path (`fast_mono`) is enabled when converting a chroma-bearing YUV format to I400 with unchanged resolution and range: the luma plane is simply relabelled and chroma ignored.

YV12/YV16/YV24 sources or destinations require swapping planes 1 and 2 before/after scaling because swscale expects I420/I422/I444 ordering.

**Context setup/rebuild.** `init_sws_context` allocates/frees a context and sets options: sws_flags, destination width/height/format/range, source width/height/format/range, and colorspace coefficients (default matrix with the given ranges, brightness/contrast unity). A FIXME notes the matrix is always the default, so RGB↔YUV uses Rec.601-ish coefficients. `check_resizer` compares the current input picture's properties to the cached `scale`; if they differ, it warns (at the current PTS when already working), clears `fast_mono`, (re)allocates the aligned destination buffer, and rebuilds the context. Variable-input mode (normcsp) checks per frame; otherwise the context is built once at init with a synthetic input picture.

**Frame delivery.** Delegate; if variable input, check/rebuild; set `working`; swap chroma if needed; if a context exists and not fast_mono, call `sws_scale` into `h->buffer.img` and replace the output descriptor with the buffer; otherwise just relabel `output->img.csp`; swap chroma back if needed. The return value of `get_frame` is not propagated from `sws_scale` (assumed success).

**No-swscale build.** `init` only passes when nothing needs doing; otherwise it fails with "not compiled with swscale support"; all frame callbacks are NULL.

## 6. Dependencies

- libswscale/libavutil (`swscale.h`, `opt.h`, `pixdesc.h`), `filters/video/video.h`, `filters/filters.h`, `input/input.h`, error macros tagged `"resize"`.
- Used as both an explicit user filter and an implicit normaliser for `X264_CSP_OTHER` formats from lavf/ffms/avs.

## 7. Constants and tables

- Method→flag mapping table (11 methods + bicubic default).
- csp→pix_fmt mapping table described in section 5.
- `MAX_RESOLUTION` from `x264cli.h` bounds output dimensions.
- Filter name: `"resize"`.

## 8. Performance notes

- Resampling is potentially the most expensive filter; quality method has a large cost spread (fastbilinear cheapest, spline/lanczos most expensive).
- **I/O buffering:** not applicable; **zero-copy:** only the fast_mono and no-op cases avoid a copy.
- **Reservation sizing:** the destination picture is allocated once, aligned (`x264_cli_pic_alloc_aligned`) for SIMD-friendly strides; the swscale context is reused across frames.
- **Threading:** swscale contexts are not thread-safe; one context per filter instance.
- **Alignment:** aligned destination strides are important for swscale's SIMD inner loops.
- **Branch behaviour:** variable-input checks add a `memcmp` per frame (cheap) and only on formats that can change.
- Reference: libswscale documentation and x264 CLI `--vf resize` documentation.

## 9. References

- libswscale API documentation: <https://ffmpeg.org/doxygen/>.
- x264 CLI filter documentation: <https://x264.readthedocs.io/>.
- `filters/filters.c`, `input/input.c`, `filters/video/internal.h` (companion specifications).

## 10. Verification checklist

1. Resize 1920x1080 → 1280x720 with no SAR option; verify SAR adjusts to preserve display aspect.
2. Supply only a SAR and verify width/height shrink to the nearest valid modulus.
3. Test `fittobox=width/height/both` and verify the fitted dimensions.
4. Convert YV12 → I420 and confirm chroma planes are swapped correctly (U/V not interchanged in output).
5. Convert 8-bit → 10-bit via csp option and verify sample scaling.
6. Force `fast_mono` (YUV → I400, same size/range) and verify luma is unchanged and no buffer copy occurs.
7. Feed a stream whose pixel format changes mid-stream (normcsp) and confirm a warning and context rebuild.
8. Confirm interlaced vertical resizing is rejected and modulus violations are rejected.
9. Confirm no-swscale builds fail only when a conversion is actually needed.

## 11. Open questions

- UNKNOWN: exact behaviour of the default colorspace matrix for RGB↔YUV (a FIXME states it is not the correct matrix) — verify against a reference if colour accuracy matters.
- UNKNOWN: whether `sws_scale` failures are ever surfaced — the reference ignores its return value.
