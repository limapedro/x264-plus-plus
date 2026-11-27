# input/lavf.c — libavformat/libavcodec demuxer

*Dirty-side behavioural specification. Reference source: `input/lavf.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `input/lavf.c`
- **Role:** Implements the `lavf_input` demuxer, which uses FFmpeg's libavformat to demux and libavcodec to decode arbitrary container/codec inputs.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

When the source is not a raw/Y4M file, x264 can rely on FFmpeg to open containers and decode compressed video. This module bridges FFmpeg's `AVFrame`/`AVPacket` model to the CLI picture model: it selects the first video stream, configures a decoder from stream parameters, decodes frames on demand, reports source properties (including VFR timestamps), and exposes decoded plane pointers without copying. It is not required by a minimal clean-room encoder, but its contract matters for timestamp/VFR handling documentation.

## 3. Public interface

Exports `const cli_input_t lavf_input` with the standard callbacks, with two notable differences: `release_frame` is **NULL** (decoded frame storage is managed internally by libavcodec/FFmpeg), and `picture_alloc` always creates a descriptor with four planes regardless of the actual format.

- `open_file`: maps `"-"` to `"pipe:"`, accepts `opt->resolution`/`opt->colorspace` as demuxer options for raw video, optionally forces an input format with `opt->format`, identifies the first video stream, creates and opens a decoder, prefetches frame 0 to confirm flags and properties, and fills `video_info_t`.
- `read_frame`: decodes until frame `i` is available, then copies linesizes/data pointers into the picture and fills PTS when VFR input is enabled.

## 4. State and data structures

`lavf_hnd_t` holds: `lavf` (format context), `lavc` (codec context), `frame` (decoded `AVFrame`), `pkt` (demuxed packet), `stream_id` (index of selected video stream), `next_frame` (index of next frame the decoder will emit), `vfr_input` (local copy of `info->vfr`), and `first_pic` (a cached `cli_pic_t` holding the prefetched frame 0 and its PTS). Ownership: FFmpeg objects are owned by the handle; `first_pic` owns a descriptor allocated via the module's `picture_alloc` but not any plane memory (planes alias `AVFrame`).

**Thread safety:** explicitly reported as `thread_safe = 0`, because `av_read_frame` invalidates previously returned packet data; input threading must not wrap this demuxer.

## 5. Algorithm / behaviour

**Open.** After allocating the handle and FFmpeg frame/packet objects, the module builds a demuxer option dictionary: when a resolution was supplied it sets `video_size` and a `pixel_format` (the user's colourspace name or a default of YUV420P). A forced input format is resolved by name if requested. `avformat_open_input` and `avformat_find_stream_info` follow; failure is fatal. The first stream whose codec type is video is selected; absence is an error.

A decoder context is created from the stream's codec parameters (`avcodec_find_decoder` + `avcodec_alloc_context3` + parameter copy), and opened. Source properties are recorded from the stream and codec: average frame rate and timebase are copied into `info`; `thread_safe` is cleared; `vfr_input` mirrors `info->vfr`.

The module then prefetches frame 0 into `first_pic` using the internal read routine with an `info` pointer, which also fills `fullrange`, `interlaced`, and `tff`. After that, width/height/csp come from the decoder context and prefetched picture, frame count from the stream's `nb_frames`, SAR from the decoder's sample aspect ratio, and full-range is OR-ed with the codec's colour-range flag. For `.avs` files containing BGRA/BGR24, a vertical-flip flag is added because AviSynth-originated RGB is inverted.

**Decoding loop (internal read).** If the first-frame cache is still present and this is not the probing call, it is consumed: for frame 0 the image and PTS are transferred to the caller via a structure swap; the cache is then cleaned and freed regardless of which frame was requested. If the request was not frame 0, decoding proceeds normally.

For every frame index from the current `next_frame` up to the requested one, the routine repeatedly calls `avcodec_receive_frame`. When the decoder reports "need more input" (`EAGAIN`), packets are read from the format context until one belongs to the selected stream (unrelated packets are discarded); the packet is sent to the decoder and unreferenced. End-of-stream is signaled by sending a null packet. Any other error logs a decoding-failure warning and aborts. Once a frame is received, `next_frame` advances by one.

The decoded frame's linesizes and data pointers are copied into the picture descriptor; width/height/csp are taken from the decoder (`csp` is marked `X264_CSP_OTHER` so the resize filter normalises it), and `handle_jpeg` maps deprecated JPEG-range pixel formats (YUVJ420P/422P/444P) to their limited-range equivalents while setting `fullrange`. When probing, full-range/interlaced/tff are written into `info`.

**Timestamps.** When VFR input is active, PTS and duration start at zero; the frame's `pts` is used if valid, otherwise `pkt_dts` is used for AVI files. If neither exists, VFR is disabled both locally and in `info` and the routine returns early (with `fullrange`/interlace fields not refreshed in that case).

**Cleanup.** `picture_alloc` creates a descriptor-only picture with four planes (FFmpeg frames can have up to four planes, e.g. alpha). `picture_clean` just zeroes it; there is no `release_frame` because decoded frames remain owned by the codec context until the next receive. `close_file` frees the codec and format contexts, packet, frame, and handle.

## 6. Dependencies

- FFmpeg headers: `libavformat/avformat.h`, `libavcodec/avcodec.h`, `libavutil/dict.h`, `error.h`, `mem.h`, `pixdesc.h`.
- `input/input.h` helpers; `get_filename_extension`, logging/error macros tagged `lavf`.
- The resize filter normalises the reported `X264_CSP_OTHER` format to a CLI/libx264 csp.

## 7. Constants and tables

- `"-"` is translated to `"pipe:"` for stdin.
- Default pixel format for raw-video demuxing: `AV_PIX_FMT_YUV420P`.
- Deprecated JPEG formats mapped to limited-range equivalents: YUVJ420P→YUV420P, YUVJ422P→YUV422P, YUVJ444P→YUV444P (each sets `fullrange = 1`).
- Error/log tag: `"lavf"`.
- No internal table of codecs; decoder selection is delegated to libavcodec.

## 8. Performance notes

- Decoding dominates; the wrapper itself adds one structure copy of pointers/linesizes per frame.
- **I/O buffering:** FFmpeg handles demuxer buffering; do not add a second layer.
- **Zero-copy:** decoded planes are aliased, not copied; keep `picture_alloc` descriptor-only.
- **Threading:** not thread-safe; never wrap in `thread_*`. FFmpeg-internal threading is configured by the frontend/libx264 parameters, not here.
- **Reservation sizing:** the one-frame cache (`first_pic`) is allocated once; avoid allocating descriptors per decoded frame.
- Reference: FFmpeg API documentation for `avcodec_send_packet`/`avcodec_receive_frame` and `av_read_frame`.

## 9. References

- FFmpeg/libav documentation: <https://ffmpeg.org/doxygen/> (AVFormatContext, AVCodecContext, AVFrame, AVPacket lifecycle).
- `input/input.h`, `input/thread.c`, `filters/video/resize.c` (companion specifications).

## 10. Verification checklist

1. Open a container with a video stream and confirm width/height/fps/timebase/SAR and frame count are reported.
2. Confirm deprecated JPEG-range formats yield `fullrange = 1` and a normal limited pixel format.
3. Confirm VFR PTS come from `AVFrame.pts`, with `pkt_dts` fallback on AVI.
4. Confirm that a stream with no usable timestamp disables VFR.
5. Confirm decoding of a code stream with B-frames delivers frames in presentation order at the requested indices.
6. Confirm `thread_safe = 0` is reported and that input threading is therefore not applied.
7. Verify clean close with no FFmpeg object leaks (valgrind or ASAN).

## 11. Open questions

- UNKNOWN: exact behaviour when `avcodec_receive_frame` returns a non-EAGAIN, non-EOF error mid-stream — the module logs and returns failure but does not document recovery.
- UNKNOWN: whether forcing `opt->format` affects raw-video probing when both a format and resolution are given; the option dictionary and format argument are independent.
