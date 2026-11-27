# input/input.h — CLI input (demuxer) plugin contract and shared picture descriptors

*Dirty-side behavioural specification. Reference source: `input/input.h`. This document describes behaviour and structure only; no source code is reproduced.*

## 1. Header

- **Source path:** `input/input.h`
- **Role:** Defines the public contract between the x264 command-line front end and every video input module (demuxer), plus the shared image/picture descriptors, the extended CLI colourspace registry, and the memory-mapped file helpers.
- **Marker:** dirty-side behavioural specification — reverse-engineered from the reference source and public documentation.

## 2. Purpose

The encoder core (`libx264`) consumes raw sample planes; it knows nothing about files, containers, or codecs. This header establishes the abstraction layer that lets the CLI feed it frames from raw planar files, YUV4MPEG2 streams, AviSynth scripts, libavformat, FFMS2, and timecode overlays through one uniform interface. It standardises four things: (a) how the caller passes options and receives source properties, (b) how a frame (planes, strides, PTS, duration, opaque token) is represented, (c) the six-function vtable every demuxer must provide, and (d) helpers to describe colourspace geometry and to map file bytes directly into frame memory. A clean-room implementation needs this contract to interoperate with any of the assigned input modules, and especially to implement the raw/Y4M path that a standalone encoder actually requires.

## 3. Public interface

### `cli_input_opt_t`
Bundle of options that only some demuxers interpret. Fields: `index_file` (path to a sidecar index), `format` (forced demuxer/container name), `resolution` (`WxH` string for raw sources), `colorspace` (name string), `bit_depth`, `timebase` (`num/den` override), `seek` (enable normal seeking vs. linear), `progress` (emit indexing progress), `output_csp`/`output_range`/`input_range` (conversion requests). Contract: any field may be `NULL`/zero when unused; demuxers must validate before use.

### `video_info_t`
Properties reported *by* a demuxer *to* the caller. Fields: `csp`, `fps_num`/`fps_den`, `fullrange`, `width`, `height`, `interlaced`, `num_frames`, `sar_width`/`sar_height`, `tff` (top-field-first), `thread_safe`, `timebase_num`/`timebase_den`, `vfr`. Contract: filled in by `open_file`; zero/unknown values mean "unspecified"; the frontend may overwrite SAR or range if the user supplied overrides.

### `cli_image_t`
`csp`, `width`, `height`, `planes`, `plane[4]`, `stride[4]`. Up to four plane pointers with matching byte strides; the count and geometry of valid planes depend on `csp`. Frame data is treated as read-only by consumers.

### `cli_pic_t`
Wraps a `cli_image_t img` plus `pts` (presentation time in encoded timebase units), `duration` (frame duration, used for VFR), and `opaque` (demuxer-private handle, e.g. an AviSynth frame or FFMS frame reference). Contract: `pts`/`duration` are meaningful only when VFR input is active; `opaque` is owned by the demuxer and released through `release_frame`.

### `cli_input_t` (the demuxer vtable)
Six function pointers, each returning 0 on success and nonzero on failure (except the void cleaner):

- `open_file(filename, *handle, *info, *opt)` — open source, probe/derive all `video_info_t` fields, allocate an opaque handle returned through `*handle`.
- `picture_alloc(*pic, handle, csp, width, height)` — prepare a `cli_pic_t` suitable for `read_frame` into; may allocate planes or leave them NULL for memory-mapped modes.
- `read_frame(*pic, handle, frame_index)` — deliver the requested frame (0-based). Sequential-only demuxers may reject backward requests.
- `release_frame(*pic, handle)` — optional; release demuxer-owned resources tied to a delivered frame. May be NULL.
- `picture_clean(*pic, handle)` — release any storage allocated by `picture_alloc`; void.
- `close_file(handle)` — tear down the handle and all associated state.

Global `cli_input_t cli_input` holds the selected module; each module exports a constant instance: `raw_input`, `y4m_input`, `avs_input`, `thread_8_input`, `thread_10_input`, `lavf_input`, `ffms_input`, `timecode_input`.

### Extended colourspace registry
`X264_CSP_CLI_MAX` equals `X264_CSP_MAX`; `X264_CSP_OTHER = 0x4000` marks a colourspace not known to libx264 (e.g. an FFmpeg pixel format). `x264_cli_csp_t` = `{ name, planes, width[4], height[4], mod_width, mod_height }`; width/height entries are floating-point scale factors relative to the luma dimensions. Global table `x264_cli_csps[]` is indexed by base colourspace.

Helpers: `x264_cli_csp_is_invalid(csp)` (rejects NONE, out-of-range, V210, and any `X264_CSP_OTHER`-flagged value), `x264_cli_csp_depth_factor(csp)` (2 when the `X264_CSP_HIGH_DEPTH` flag is set, else 1, 0 for invalid), `x264_cli_pic_plane_size(csp,w,h,plane)`, `x264_cli_pic_size(csp,w,h)`, `x264_cli_pic_alloc`, `x264_cli_pic_alloc_aligned`, `x264_cli_pic_init_noalloc`, `x264_cli_pic_clean`, `x264_cli_get_csp`.

### Memory-mapping helpers
`cli_mmap_t` stores `file_size`, `align_mask`, and platform state (Windows: page mask, `PrefetchVirtualMemory` pointer, process/map handles; POSIX with `HAVE_MMAP`: file descriptor). Functions: `x264_cli_mmap_init(h, FILE*)`, `x264_cli_mmap(h, offset, size)`, `x264_cli_munmap(h, addr, size)`, `x264_cli_mmap_close(h)`.

## 4. State and data structures

The header is stateless; all state lives in the module-specific handles referenced through the opaque `hnd_t`. Ownership rules: the demuxer owns the handle and any buffers behind `opaque`; the caller owns the `cli_pic_t` structure it passes in and any planes allocated by `picture_alloc` (released with `picture_clean`). For memory-mapped frames the plane pointers point into a file mapping, so the caller must call `release_frame` before reusing or closing the map. `cli_mmap_t` is embedded inside a demuxer handle and has lifetime equal to that handle.

## 5. Algorithm / behaviour

**Colourspace geometry.** Each descriptor states how many planes a format has and the fractional width/height of each plane relative to luma. Planar 4:2:0 (`i420`, `yv12`) has three planes; chroma planes are half width and half height; mod widths/heights are 2/2. Planar 4:2:2 has half-width chroma only (mod 2/1); 4:4:4 has full-resolution chroma (mod 1/1). Monochrome `i400` has one plane (mod 1/1). Semi-planar NV12/NV21 have two planes: luma full, chroma interleaved at half height (mod 2/2); NV16 has half-height absent (mod 2/1). Packed YUYV/UYVY have a single plane occupying twice the luma width (mod 2/1); BGR/RGB use three samples per pixel, BGRA four (mod 1/1). Depth factor doubles all byte counts when `X264_CSP_HIGH_DEPTH` is set.

**Size computation.** A plane's byte size is `width * height * width_factor[plane] * height_factor[plane] * depth_factor`, computed in 64-bit arithmetic to avoid overflow on large frames. Total picture size is the sum over all valid planes. An invalid colourspace or an out-of-range plane index yields zero.

**Picture initialisation.** The internal initialiser zeroes the whole `cli_pic_t`, sets `planes` from the descriptor (or zero for invalid csp), stores the caller's csp/width/height, and computes each stride as `ALIGN(width * width_factor * depth_factor, align)`, where `align` is 1 for ordinary allocation and the platform native alignment for aligned allocation. With allocation requested, each plane receives `x264_malloc(height * height_factor * stride)` bytes; any allocation failure leaves a partially built picture that the caller must clean up. The no-allocation variant computes strides and plane counts but leaves plane pointers null — used when a demuxer will point them into a memory map. `picture_clean` frees every plane up to `planes` and zeroes the structure.

**Validity rule.** `x264_cli_csp_is_invalid` masks off flag bits (`X264_CSP_MASK`), rejects a base at or below NONE or at/above the CLI maximum, rejects V210 explicitly (it has no CLI descriptor), and rejects any format carrying `X264_CSP_OTHER` (an FFmpeg-native pixel format that must be routed through the resize filter instead).

**Memory mapping.** Initialisation obtains the file size, the OS allocation granularity (the offset-alignment mask), and on Windows the page-size mask plus an optional `PrefetchVirtualMemory` entry point. Mapping a frame rounds the requested offset *down* to the alignment boundary and grows the size correspondingly; the returned pointer is offset back up by that alignment. A fixed 64-byte padding is always added so third-party scalers that over-read do not fault. At end-of-file the padding is handled differently per platform: POSIX remaps the tail padding onto the last valid page via a fixed-address `mmap`, while Windows either maps a slightly larger view or, if padding would cross past the file end, copies the frame into a private padded anonymous mapping (because Windows cannot perform the POSIX remap trick). A successful map optionally issues an OS read-ahead hint (`MADV_WILLNEED`/`posix_madvise` or `PrefetchVirtualMemory`). Unmapping reconstructs the aligned base pointer by masking the address and releases `size + padding + intra-page offset` bytes. Close releases the OS mapping handle (Windows only).

## 6. Dependencies

- Included by every file under `input/` and by `filters/video/video.h` (hence the whole video-filter chain).
- Depends on `x264cli.h` for `hnd_t`, `x264_param_t`, logging/error macros, `ALIGN`, `NATIVE_ALIGN`, `XCHG`, `x264_malloc/x264_free`, and the `X264_CSP_*` base definitions.
- Platform layer: `<windows.h>` on Windows, `<sys/mman.h>`/`<unistd.h>` when `HAVE_MMAP`.
- The resize filter consumes `x264_cli_csp_t` and `x264_cli_csps[]` to interpret `X264_CSP_OTHER` formats.

## 7. Constants and tables

- `X264_CSP_OTHER = 0x4000`: flag for a pixel format outside the libx264 set.
- `X264_CSP_CLI_MAX = X264_CSP_MAX`: sentinel end of the CLI colourspace table.
- `MMAP_PADDING = 64`: fixed safety overread allowance for frame mappings.
- Colourspace table (base → name / plane count / chroma width factor / chroma height factor / mod width / mod height):
  - `i400`: 1 plane, 1×1, mod 1/1.
  - `i420`, `yv12`: 3 planes, 1 / ½ / ½ width, 1 / ½ / ½ height, mod 2/2.
  - `i422`, `yv16`: 3 planes, 1 / ½ / ½ width, 1 / 1 / 1 height, mod 2/1.
  - `i444`, `yv24`: 3 planes, all 1×1, mod 1/1.
  - `nv12`, `nv21`: 2 planes, 1 / 1 width, 1 / ½ height, mod 2/2.
  - `nv16`: 2 planes, 1 / 1 width, 1 / 1 height, mod 2/1.
  - `yuyv`, `uyvy`: 1 plane, 2× width, mod 2/1.
  - `bgr`, `rgb`: 1 plane, 3× width, mod 1/1; `bgra`: 4× width.
  The exact values are facts of the format geometry, not copied code.

## 8. Performance notes

- This header is on the hot path only through the helper calls made per frame; the geometry loops are tiny (≤4 planes) and compile to constant-folded code for known formats.
- **I/O buffering / zero-copy:** the mmap helpers are the mechanism for zero-copy input. A clean implementation should map whole frames (as raw.c and y4m.c do) rather than reading into heap buffers, so the encoder never copies sample data. Keep the 64-byte padding — skipping it saves nothing but risks faults with SIMD/scaler overreads.
- **Readahead:** the `WILLNEED`/`PrefetchVirtualMemory` hints matter for spinning media and for large 4K frames; issue them per mapping, not per plane.
- **Allocation:** `x264_cli_pic_alloc_aligned` should be used whenever a scaler will read the buffer with SIMD; aligned strides reduce the scaler's need for unaligned head loads.
- **Reservation sizing:** plane buffers should be allocated once at `picture_alloc` time and reused for every frame; per-frame `malloc` is a measurable regression.
- Reference optimization guidance: x264 CLI documentation on threaded input/frame threading (<https://x264.readthedocs.io/en/stable/>); OS man pages for `mmap(2)`, `madvise(2)`, and Windows `MapViewOfFile`/`PrefetchVirtualMemory`.

## 9. References

- x264 CLI source documentation and build configuration (`x264cli.h`, `common/common.h`) — interface definitions.
- ITU-T H.264 / ISO IEC 14496-10 — definition of YUV coloursample formats and bit depths used by the descriptors.
- `mmap(2)`, `madvise(2)`, `posix_madvise(3)` (POSIX); `CreateFileMapping`, `MapViewOfFile`, `PrefetchVirtualMemory` (Win32) — mapping semantics.
- YUV4MPEG2 format description (for how the CLI descriptors map to Y4M chroma formats).

## 10. Verification checklist

1. For each base colourspace, ask `x264_cli_pic_size` for a small frame and confirm the byte count equals the analytic sum of plane areas times depth factor.
2. Confirm `x264_cli_csp_is_invalid` returns false for all table entries and true for `X264_CSP_NONE`, `X264_CSP_V210`, and any value with `X264_CSP_OTHER` set.
3. Confirm `x264_cli_csp_depth_factor` is 2 exactly when the high-depth flag is set.
4. `x264_cli_pic_alloc_aligned` must produce strides divisible by the platform native alignment; `x264_cli_pic_init_noalloc` must leave plane pointers null yet report correct strides and plane count.
5. `x264_cli_pic_clean` must free all allocated planes and zero the structure (a second clean must be harmless).
6. Map a frame at a non-page-aligned offset and verify the returned pointer's low bits match the requested offset, that 64 bytes past the frame are readable, and that unmapping leaves the process able to map again.
7. Map the final frame of a file whose size is not a multiple of the page size and confirm no fault occurs when reading the padded region.

## 11. Open questions

- UNKNOWN: exact value of `NATIVE_ALIGN` on non-x86 platforms (it is platform/build dependent).
- UNKNOWN: whether `x264_cli_csp_is_invalid` treats `X264_CSP_VFLIP`-flagged values specially — the flag is masked off, so they are accepted, but interaction with `X264_CSP_OTHER` in the resize filter is only partially documented.
- UNKNOWN: behaviour of the Windows mmap fallback when `CreateFileMapping` fails — the source returns a null pointer; whether callers retry with buffered I/O is a frontend concern not shown here.
