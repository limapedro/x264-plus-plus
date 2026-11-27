# `common/base.c` - parameters, allocation and shared utilities

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\base.c` (mirrored at `common/base.c`). Role in one sentence: implements the bit-depth-independent public utilities - aligned allocation, logging, picture buffers, the parameter object's defaults/presets/tunes/parser/serialiser - on which every encoder instance depends. Marker: **dirty-side behavioural specification**.

## 2. Purpose

This file is the home of the encoder's configuration object model. It converts a zeroed `x264_param_t` into a fully usable default configuration, layers named presets and comma-separated tunes over it, restricts it to a profile, parses an arbitrary list of `name=value` options with a documented error taxonomy, and serialises it back to the canonical string embedded in the bitstream's user-data SEI. It also supplies the aligned allocator used for all internal buffers and the reference logging sink. Because all of this is bit-depth independent, the symbols are exported once rather than templated per bit depth.

## 3. Public interface

- `void x264_reduce_fraction(uint32_t *n, uint32_t *d)` and `void x264_reduce_fraction64(uint64_t *n, uint64_t *d)`: divide numerator and denominator by their greatest common divisor via Euclid's algorithm; a zero operand leaves the pair unchanged.
- `void x264_log_default(void *unused, int level, const char *fmt, va_list ap)`: reference log sink; writes `x264 [<prefix>]: ` to stderr followed by the formatted message, where prefix is `error`, `warning`, `info`, `debug`, or `unknown` for an out-of-range level. On Windows it routes through a UTF-16-capable console writer when the target is a real console.
- `void x264_log_internal(int level, const char *fmt, ...)`: convenience wrapper around the default sink for messages emitted before an encoder exists (or without one).
- `void *x264_malloc(int64_t size)` / `void x264_free(void *p)`: aligned allocation pair; `p == NULL` is a no-op for free.
- `char *x264_slurp_file(const char *filename)`: reads a whole file into an allocated, always-newline-terminated, NUL-terminated buffer; returns NULL on open/seek/read/size failure and for empty files.
- `char *x264_param_strdup(x264_param_t *param, const char *src)`: internal strdup that records the pointer in a per-param opaque list; returns NULL and logs on allocation failure.
- `void x264_param_cleanup(x264_param_t *param)` (public API): frees every recorded duplicate and the list itself, then clears `param->opaque`; safe to call repeatedly.
- `void x264_picture_init(x264_picture_t *pic)`: zeroes the picture and sets `i_type = X264_TYPE_AUTO`, `i_qpplus1 = X264_QP_AUTO`, `i_pic_struct = PIC_STRUCT_AUTO`.
- `int x264_picture_alloc(x264_picture_t *pic, int csp, int width, int height)`: single allocation for all planes, with per-colorspace strides derived from a fixed-point table; -1 for unsupported colorspaces (including `V210`); `x264_picture_clean` frees plane 0 and zeroes the structure.
- `void x264_param_default(x264_param_t *param)`, `int x264_param_default_preset(x264_param_t *, const char *preset, const char *tune)`, `void x264_param_apply_fastfirstpass(x264_param_t *)`, `int x264_param_apply_profile(x264_param_t *, const char *profile)`.
- `int x264_param_parse(x264_param_t *p, const char *name, const char *value)`: returns 0 on success, `X264_PARAM_BAD_NAME`, `X264_PARAM_BAD_VALUE`, or `X264_PARAM_ALLOC_FAILED`.
- `char *x264_param2string(x264_param_t *p, int b_res)`: returns an allocated canonical options string (2000-byte nominal buffer, enlarged by the length of any zones string); `b_res` prepends resolution/fps/timebase/bitdepth.

## 4. State and data structures

The parameter object is a flat POD with nested structs for rate control, analyse, VUI, crop rectangle, mastering-display metadata and content-light-level metadata, plus an `opaque` pointer. `opaque` is deliberately repurposed as a `strdup_buffer`: an int capacity, an int used count, and a flexible array of owned pointers, initially 16 entries and doubled on demand (with overflow checks against `INT_MAX`). Only strings allocated by `x264_param_strdup` belong there; the caller must not free them individually.

`x264_param_default` zero-fills the whole object first, so any field not explicitly assigned is deterministically zero. `x264_param_default_preset` applies defaults, then a preset, then a tune; a bad preset or tune returns -1 after defaults have already been written. Profile application is a *restriction* pass: it rejects impossible combinations and silently removes features unsupported by the requested profile.

The picture-allocation table is indexed by `csp & X264_CSP_MASK`; each entry stores plane count and per-plane width/height factors in 1/256 units. High-depth input doubles every stride. Plane pointers are offsets into one contiguous block, so `x264_picture_clean` frees only `plane[0]`.

## 5. Algorithm / behaviour

**Defaults (exhaustive, grouped).** CPU/thread: `cpu = x264_cpu_detect()`, `i_threads = X264_THREADS_AUTO` (0), `i_lookahead_threads = 0`, `b_deterministic = 1`, `i_sync_lookahead = -1` (auto). Format: csp `X264_CSP_I420` unless the build fixes a chroma format, width/height 0, SAR 0:0, overscan undefined 0, videoformat 5 (undefined), fullrange -1 (input-dependent), colorprim/transfer 2 (undefined), colmatrix -1, chromaloc 0, fps 25/1, level -1, no slice limits, bitdepth from build (8 or 10). Structure: 3 reference frames, keyint max 250, keyint min auto (0), 3 B-frames, scenecut 40, B-adapt fast, B-bias 0, pyramid normal, progressive, no constrained intra. Loop filter on with alpha/beta 0; CABAC on, init idc 0. Rate control: CRF method, bitrate 0, rate tolerance 1.0, VBV maxrate/buffersize 0 with initial fill 0.9, QP constant -1, CRF 23, QP range 0..INT_MAX, QP step 4, IP factor 1.4, PB factor 1.3, AQ variance mode with strength 1.0, lookahead 40, two-pass filenames `x264_2pass.log`, qcompress 0.6, qblur 0.5, complexity blur 20, mb-tree on. Analysis: intra I4x4+I8x8, inter I4x4+I8x8+P16x8/B16x8, direct spatial, ME hex, psy-RD 1.0 on, psy-trellis 0, ME range 16, subpel refine 7, mixed refs on, chroma ME on, per-thread MV range -1 (auto), MV range -1 (from level), chroma QP offset 0, fast pskip on, weighted bipred on, weighted pred smart, DCT decimate on, 8x8dct on, trellis 1, luma deadzones 21 (inter) and 11 (intra), psnr/ssim off. Quantisation: flat CQM with all eight lists filled with 16. Bitstream: repeat headers on, Annex B on, no AUD, VFR input on, NAL HRD none, top-field-first on, no pic-struct/fake-interlacing, frame packing -1, alternative transfer 2 (undefined), OpenCL off with device index 0 and no explicit device id or clbin file, AVCIntra class 0 with Panasonic flavour. Log: `x264_log_default`, private pointer NULL, level INFO.

**Presets.** A numeric preset string is first reinterpreted as an index into the public preset-name table. Each name applies a fixed delta relative to the defaults: `ultrafast` removes almost all analysis (ref 1, no B, no CABAC, no deblock, DIA, subme 0, no AQ/trellis/8x8/weighting/mbtree, lookahead 0); `superfast` keeps intra analysis only and subme 1; `veryfast` subme 2, ref 1, no mixed refs, simple weightp, lookahead 10; `faster` ref 2, subme 4, simple weightp, lookahead 20; `fast` ref 2, subme 6, simple weightp, lookahead 30; `medium` is exactly the defaults; `slow` subme 8, ref 5, direct auto, trellis 2, lookahead 50; `slower` adds UMH, subme 9, ref 8, B-adapt trellis, P8x8, lookahead 60; `veryslow` adds me-range 24, ref 16, 8 B-frames; `placebo` adds TESA, subme 11, fast-pskip off, 16 B-frames. Unknown names log an error and fail.

**Tunes.** The tune string is split on `,./-+`; an empty token is skipped. Eight names are recognised: `film` (alpha/beta -1, psy-trellis 0.15), `animation` (refs doubled or 1, alpha/beta +1, psy-RD 0.4, AQ 0.6, two extra B-frames), `grain` (alpha/beta -2, psy-trellis 0.25, no DCT decimate, PB/IP 1.1, AQ 0.5, deadzones 6/6, qcompress 0.8), `stillimage` (alpha/beta -3, psy-RD 2.0, psy-trellis 0.7, AQ 1.2), `psnr` (AQ off, psy off), `ssim` (auto-variance AQ, psy off), `fastdecode` (no deblock, no CABAC, no weighted bipred/pred), `zerolatency` (lookahead 0, sync-lookahead 0, no B, sliced threads, CFR input, no mb-tree), `touhou` (refs doubled, alpha/beta -1, psy-trellis 0.2, AQ 1.3, P8x8 enabled when P16x8 is). At most one *psy* tune (film/animation/grain/stillimage/psnr/ssim/touhou) may be used; later ones produce a warning and are ignored. Non-psy tunes may be combined freely. Unknown tokens log an error and cause the overall call to fail.

**Fast first pass.** Only when a stats file is written and none is read: ref 1, no 8x8dct, no inter analysis, DIA, subme capped at 2, trellis 0, fast pskip on.

**Profile restriction.** The string is matched case-insensitively to a profile ID; unknown profiles fail. Before feature changes, four hard checks reject lossless with a profile below High 4:4:4, 4:4:4 with a profile below High 4:4:4, 4:2:2 below High 4:2:2, bit depth > 8 below High10, and 4:0:0 below High. Baseline additionally forbids interlacing and fake interlacing, forces no B-frames, no CABAC, no 8x8dct, flat CQM and no weighted prediction; Main forces no 8x8dct and flat CQM.

**Parser.** Name handling: underscores are converted to hyphens; a leading `no` / `no-` inverts a boolean value; a null value means "true" and a value beginning with `=` has that character stripped. Boolean values accept 1/true/yes and 0/false/no; integers use base 0 via `strtol`; floats use `strtod`; the parser flags trailing garbage as an error. Enum options resolve through public name arrays. Options are grouped as: CPU/threads (`asm`, `threads`, `lookahead-threads`, `sliced-threads`, `sync-lookahead`, `deterministic`, `cpu-independent`); stream metadata (`level`, `bluray-compat`, `avcintra-class/flavor`, `sar`, `overscan`, `videoformat`, `fullrange`, `colorprim`, `transfer`, `colormatrix`, `chromaloc`, `mastering-display`, `cll`, `alternative-transfer`, `fps`); structure (`ref`, `dpb-size`, `keyint`, `min-keyint`, `scenecut`, `intra-refresh`, `bframes`, `b-adapt`, `b-bias`, `b-pyramid`, `open-gop`, `nf`, `deblock`, slice controls, `cabac`, interlacing flags, `constrained-intra`); quant matrices (`cqm`, `cqmfile`, `cqm4/8` and per-list variants, comma-separated coefficients 1..255); analysis (`analyse`, `8x8dct`, `weightb`, `weightp`, `direct`, `chroma-qp-offset`, `me`, `merange`, `mvrange`, `mvrange-thread`, `subme`, `psy-rd`, `psy`, `chroma-me`, `mixed-refs`, `trellis`, `fast-pskip`, `dct-decimate`, deadzones, `nr`); rate control (`bitrate`, `qp`, `crf`, `crf-max`, `rc-lookahead`, qp bounds/step, `ratetol` with `inf` mapped to 1e9, VBV, IP/PB factors, AQ, `pass` mapped to write/read bits 1/2, `stats`, `qcomp`, `mbtree`, `qblur`, `cplxblur`, `zones`, `crop-rect`, `psnr`, `ssim`); output (`aud`, `sps-id`, `global-header`/`repeat-headers`, `annexb`, `force-cfr`, `nal-hrd`, `filler`, `pic-struct`, `fake-interlaced`, `frame-packing`, `stitchable`, `opencl`, `opencl-clbin`, `opencl-device`). Notably `bitrate`, `qp` and `crf` each switch `i_rc_method` to ABR/CQP/CRF; `pass` sets both two-pass flags; `crop-rect` expects four comma-separated integers. The error code is `BAD_NAME` only for unrecognised names, `ALLOC_FAILED` only for internal allocation failures, otherwise `BAD_VALUE`; a null value on a non-boolean option is also an error.

**Serialisation.** `x264_param2string` emits a stable, space-separated `key=value` string in a fixed order: optional resolution/fps/timebase/bitdepth prefix, then CABAC, refs, deblock, analysis masks in hex, ME name, subme, psy settings, mixed refs, ME range, chroma ME, trellis, 8x8dct, CQM preset, deadzones, fast pskip, chroma QP offset, thread counts, slice settings, nr/decimate, interlacing mode, bluray/stitchable/constrained-intra, B-frame settings, weightp, keyint/scenecut/intra-refresh, conditional rc-lookahead, rate-control mode and parameters, conditional NAL HRD/filler, crop rect, HDR metadata, frame packing, and IP/AQ/zones (omitted entirely for lossless CQP). Some fields are emitted only when active, so the string is a human-readable summary rather than a lossless round-trip encoding. UNKNOWN: whether any consumer reparses this string; it is intended for SEI user data and logs.

## 6. Dependencies

Depends on `base.h` (macros, enums, Scan8), `osdep.h` (file/console wrappers, `REALIGN_STACK`), `cpu.h` (detection), public `x264.h` (option name arrays, error codes, parameter structures), and libc (`ctype`, `malloc`, `stdio`). Called by the CLI and by the encoder during option application and SEI generation. Does not depend on bit-depth-templated modules.

## 7. Constants and tables

Huge-page threshold: 2 MiB page size, allocation considered large at 7/8 of that (1.75 MiB). strdup buffer initial capacity 16 pointers. `x264_param2string` initial capacity 2000 bytes. The picture-format table maps: I400/YUV 4:0:0 = 1 plane with full-size factors; I420/YV12 = 3 planes with half-width/half-height chroma; NV12/NV21 = 2 planes with full chroma width and half height; I422/YV16 = half-width/full-height chroma; NV16 = 2 planes full-size; YUYV/UYVY = 1 plane double width; I444/YV24 = 3 full planes; BGR/RGB = 1 plane triple width; BGRA = quadruple width. All factors are expressed as integers times 256 so division is a shift.

## 8. Performance notes

- **Allocation strategy:** `x264_malloc` requests `NATIVE_ALIGN` (64 bytes on x86) alignment via `memalign`, or over-allocates and stores the original pointer immediately before the aligned address on platforms without `malloc.h`. Large allocations use 2 MiB alignment and an `madvise(MADV_HUGEPAGE)` hint, cutting TLB pressure for frame buffers; the threshold is heuristic, not a correctness contract.
- **Cache layout:** `x264_picture_alloc` packs all planes into one block, giving predictable offsets and a single free; stride computation in 1/256 units avoids floating point and yields even strides suitable for SIMD row loads.
- **False sharing:** none of the parameter utilities create shared counters; keep the `strdup_buffer` owned by exactly one parameter object (it is not thread-safe for concurrent `x264_param_parse` calls).
- **Dispatch hierarchy:** the parser is cold-path only; presets are applied once. The allocator is on the hot allocation path for frames, so its huge-page branch matters for high-resolution, many-reference encodes.
- Public references: Drepper, *What Every Programmer Should Know About Memory* (2007), sections on TLB reach and huge pages; Linux `madvise(2)` and `posix_memalign(3)` man pages; Intel SDM Vol. 1 on cache-line sizes.

## 9. References

- Public `x264.h`: `x264_param_t`, `x264_picture_t`, `x264_param_parse` documentation, preset/tune/name arrays, error codes.
- ITU-T H.264 Annex A (profile constraints) and Annex D (SEI user data) for the fields restricted by `x264_param_apply_profile` and emitted by `x264_param2string`.
- x264 `doc/threads.txt`, `doc/ratecontrol.txt`, `doc/vui.txt` for the meaning of the corresponding defaults.
- Linux man pages `madvise(2)`, `memalign(3)`; MSDN `MultiByteToWideChar`, `WriteConsoleW`.

## 10. Verification checklist

- After `x264_param_default`, a black-box dump of every field must match the default table in section 5 (including the -1 sentinels and the eight flat CQM lists filled with 16).
- Applying preset `medium` must leave the defaults unchanged; applying `ultrafast` must set exactly the listed deltas; applying a numeric preset index must equal applying the corresponding name.
- Applying a tune twice or combining two psy tunes must warn and ignore the second psy tune, while non-psy tunes combine.
- `x264_param_apply_profile("baseline")` on a 4:4:4 or interlaced configuration must return -1; on a default configuration it must set CABAC, B-frames, 8x8dct and weighted prediction off.
- Parser checks: `no-cabac` sets CABAC false; `crf=20` sets method CRF; `qp=0` sets method CQP; `bitrate=1000` sets ABR; `pass=3` sets both two-pass flags; `fps=30000/1001` yields numerator 30000 and denominator 1001; `sar=4:3` and `sar=4/3` both work; unknown names return `X264_PARAM_BAD_NAME`, values such as `cabac=maybe` return `X264_PARAM_BAD_VALUE`.
- Repeated `x264_param_parse` calls with string options followed by `x264_param_cleanup` must leave no leaks and set `opaque` to NULL; a second cleanup must be harmless.
- `x264_slurp_file` on a file without a trailing newline must append exactly one; on an empty file it must return NULL.
- `x264_param2string(...,0)` must contain `cabac=`, `ref=`, `me=`, `subme=`, `rc=` and, for lossless CQP, must omit `ip_ratio=`.
- `x264_picture_alloc` with `X264_CSP_NV12` must report two planes with equal strides and half-height chroma; freeing through `x264_picture_clean` must not double-free.

## 11. Open questions

- UNKNOWN: whether `x264_param2string` output is ever parsed back by any component; no parser for it appears in this file.
- UNKNOWN: the exact behaviour of `x264_param_parse("asm", "")` when `atobool` fails and `strdup` succeeds is an empty CPU mask - this follows from the code but has no documented contract.
- UNKNOWN: whether callers may assume `opaque` remains unused by the encoder after `x264_param_cleanup`; the field may be reused by the caller after cleanup per the public header, but this file does not enforce it.
