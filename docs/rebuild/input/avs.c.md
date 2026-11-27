# input/avs.c — AviSynth script demuxer

*Dirty-side behavioural specification. Reference source: `input/avs.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `input/avs.c`
- **Role:** Implements the `avs_input` demuxer, which loads AviSynth/AviSynth+ dynamically, opens a `.avs` script (or wraps a media file with a source filter), and exposes its frames through the CLI input contract.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

AviSynth is a scriptable frame server widely used on Windows for preprocessing. This module lets x264 consume any AviSynth clip without linking against AviSynth at build time: the library is loaded at runtime and all required entry points are resolved by name. It also maps AviSynth's many pixel formats onto CLI colourspaces, optionally performs colourspace/range conversion inside AviSynth when libswscale is unavailable, and handles field-based clips by weaving them into frames.

## 3. Public interface

Exports `const cli_input_t avs_input` with the standard six callbacks:

- `open_file` requires a regular file, loads the AviSynth library, creates a script environment at interface version 2 (AviSynth 2.5, required for YV12), opens either the script via `Import` or a source filter chosen from the filename extension, and fills `video_info_t`.
- `picture_alloc` allocates only a descriptor (no planes): plane pointers come directly from AviSynth frames.
- `read_frame` fetches frame N and points the picture at AviSynth's plane data and pitches.
- `release_frame` releases the AviSynth frame object.
- `picture_clean` simply zeroes the descriptor (no owned memory).
- `close_file` releases clip, environment, and library.

Supported AviSynth pixel types are enumerated in section 7. Unsupported pixel types cause an error that includes the value reported by AviSynth's `PixelType` function.

## 4. State and data structures

`avs_hnd_t` holds: `clip` (the AviSynth clip), `env` (script environment), `library` (dynamic-library handle), `num_frames`, and a `func` substructure holding function pointers for every AviSynth entry point used. Entry points are resolved with two policies: mandatory (failure aborts) for clip/environment/frame/get-info/invoke/value-management functions, optional for format-query helpers and AviSynth+ extensions. Optional AviSynth+ symbols are tried under both their normal names and Windows stdcall-decorated aliases. A set of wrapper macros falls back to statically linked helpers when a dynamic symbol is absent, and a separate macro set detects AviSynth+ versus classic AviSynth so the same logic works with both.

The handle owns the clip and environment; individual `AVS_VideoFrame` objects are stored in `pic->opaque` and released per frame.

## 5. Algorithm / behaviour

**Library loading.** On Windows the module loads `avisynth`; on macOS `libavisynth.dylib`; elsewhere `libavisynth.so`. It resolves mandatory and optional symbols as described, unloads the library on any mandatory failure, and creates a script environment at interface version 2. If an environment error query is available it is checked immediately. The AviSynth version is obtained by invoking `VersionNumber`, which must exist and return a float; the value is logged at debug level.

**Filename handling.** AviSynth cannot accept Unicode filenames, so on Windows the UTF-8 argument is converted to the system ANSI code page; if that fails (unrepresentable characters), a legacy 8.3 short path is obtained via `GetShortPathNameW` and converted instead. Failure to produce an ANSI path is fatal.

**Opening the clip.** If the extension is `.avs`, the `Import` function is invoked with the filename. The module then checks `GetMTMode`; a mode strictly between 0 and 5 indicates a multi-threaded script, and `Distributor` is applied to make frame access thread-safe (adapted from AviSynth's own VfW interface). For non-script files, a filter sequence is built from the extension: `.avi` tries `AVISource`, `.d2v` tries `MPEG2Source`, `.dga` tries `AVCSource` (Windows), followed by general-purpose source filters (`FFmpegSource2`/`DSS2`/`DirectShowSource` on Windows, `FFVideoSource` elsewhere). Each candidate is logged and tried in order; an indexing message is printed for FFmpegSource filters; the first non-error result wins, and exhausting the list is an error.

**Clip validation and field handling.** The result must be a clip with video data. If the clip is field-based, a warning is logged and `Weave` is invoked to combine fields into frames; the clip is replaced, `interlaced` is set, and `tff` is taken from the woven clip.

**Conversion (when built without libswscale).** When the requested output colourspace differs from the clip's, the module invokes `ConvertTo<Format>` with appropriate arguments. AviSynth 2.6 is required for i400/i422/i444 output; width/height divisibility rules are enforced (width even for subsampled formats, height even, height multiple of 4 for interlaced 4:2:0). Format names differ between classic AviSynth (`Y8`, `YV12`, `YV16`, `YV24`) and AviSynth+ (`Y`, `YUV420`, `YUV422`, `YUV444`). For RGB↔YUV conversions on AviSynth 2.56+, a `matrix` argument is supplied using Rec.601 or PC.601 coefficients according to the requested range, and the input range is marked as converted. Separately, if the source is YUV and a TV↔PC range change is requested, `ColorYUV` is invoked with the corresponding levels string and the input range is updated. Every conversion replaces the clip via a shared "update clip" helper that releases the previous clip and value, takes a new clip from the result, and refreshes the video-info pointer.

**Info extraction.** Width, height, fps numerator/denominator, and frame count are copied from the final clip's `AVS_VideoInfo`; `thread_safe = 1` and `vfr = 0` are reported. Colourspace is selected by an ordered chain of format predicates: 64-bit RGB → BGRA high-depth; 32-bit RGB → BGRA (both with vertical-flip flag, because AviSynth stores RGB top-down-inverted relative to x264's expectation); 48-bit RGB → BGR high-depth; 24-bit RGB → BGR; then 4:4:4/4:2:2/4:2:0 planar 16-bit and 8-bit variants, monochrome 16/8-bit, YUY2, and (with libswscale) YUV411 as an `X264_CSP_OTHER` format. Anything else is an error reporting the pixel type name.

**Frame delivery.** `picture_alloc` calls the shared allocator with `X264_CSP_NONE` (descriptor only), then sets the requested csp and plane count from the CLI table (three for `X264_CSP_OTHER` YUV411, one for Y8/YUY2). `read_frame` rejects out-of-range indices, fetches the AviSynth frame into `opaque`, checks for a clip error, and assigns each plane pointer/pitch from AviSynth's planar accessors in Y/U/V order. No copy occurs. `release_frame` releases the stored frame object; `close_file` releases clip, environment, and library.

## 6. Dependencies

- `input/input.h` (vtable, colourspace helpers), `extras/avisynth_c.h` (AVS C API declarations, included with `AVSC_NO_DECLSPEC` so symbols can be resolved dynamically).
- Platform dynamic loading: `LoadLibraryW`/`GetProcAddress`/`FreeLibrary` on Windows, `dlopen`/`dlsym`/`dlclose` elsewhere.
- Optional `libavutil/pixfmt.h` for the YUV411 `X264_CSP_OTHER` path.
- Wrapped by `thread_*` when input threading is enabled.

## 7. Constants and tables

- `AVS_INTERFACE_25 = 2`: minimum AviSynth interface version (YV12 support).
- `AVS_MAX_SEQUENCE = 5`: maximum number of candidate source filters tried for non-script files.
- Windows source-filter candidates: `AVISource` (.avi), `MPEG2Source` (.d2v), `AVCSource` (.dga), then `FFmpegSource2`, `DSS2`, `DirectShowSource`. Other platforms: `FFVideoSource`.
- Format mapping (AviSynth → CLI): RGB64→`BGRA|VFLIP|HIGH_DEPTH`, RGB32→`BGRA|VFLIP`, RGB48→`BGR|VFLIP|HIGH_DEPTH`, RGB24→`BGR|VFLIP`, YUV444P16→`I444|HIGH_DEPTH`, YV24→`I444`, YUV422P16→`I422|HIGH_DEPTH`, YV16→`I422`, YUV420P16→`I420|HIGH_DEPTH`, YV12→`I420`, Y16→`I400|HIGH_DEPTH`, Y8→`I400`, YUY2→`YUYV`, YV411 (with swscale)→`YUV411P|OTHER`.
- Error tag: `"avs"`.

## 8. Performance notes

- Library loading and script opening are one-off; per-frame work is one `GetFrame` plus pointer assignment.
- **Zero-copy:** plane pointers alias AviSynth-owned memory; never copy. The descriptor-only `picture_alloc` is essential.
- **Threading:** the module reports `thread_safe = 1` only after applying `Distributor` to MT scripts; this is why the distributor step exists.
- **Reservation sizing:** frame objects are allocated by AviSynth's own cache; no CLI-side frame pool is needed.
- **Branch behaviour:** the long format-detection chain is cold (once at open); the per-frame predicate macros resolve to direct function-pointer calls.
- References: AviSynth C API documentation; x264 CLI input threading notes.

## 9. References

- AviSynth/AviSynth+ C API (`avisynth_c.h`) documentation: <http://avisynth.nl/index.php/Avisynthplus>.
- AviSynth `Select`/`Weave`/`ConvertTo*`/`ColorYUV` filter documentation.
- `input/input.h`, `input/thread.c` (companion specifications).

## 10. Verification checklist

1. Open a simple `.avs` returning a `BlankClip` and confirm width/height/fps/frame count propagate.
2. Confirm each supported pixel type maps to the documented CLI csp, including the vertical-flip flag on RGB.
3. With a field-based clip, confirm a warning is logged, `Weave` is applied, and `interlaced`/`tff` are set.
4. Confirm the `Distributor` is applied when `GetMTMode` returns 1..4 and not otherwise.
5. For a non-script file, confirm candidate filters are tried in extension order and the first success is used.
6. Confirm unsupported pixel types produce an error naming the AviSynth pixel type.
7. Verify plane pointers/pitches match AviSynth's `GetReadPtr`/`GetPitch` for Y/U/V.
8. Verify clean shutdown: clip, environment, and library are all released.

## 11. Open questions

- UNKNOWN: exact behaviour if AviSynth's `GetFrame` returns an error after a partially delivered picture — the source checks `avs_clip_get_error` once and aborts.
- UNKNOWN: which source filters are compiled/exist on a given non-Windows system; only `FFVideoSource` is attempted there.
