# autocomplete.c — rebuild specification

## 1. Header

- Source path: `autocomplete.c` (root of the reference tree).
- Role: implements the CLI's shell-completion hook, mapping the previously typed option to a list of candidate values (or to a list of options) and printing them for the shell to consume.
- Marker: **dirty-side behavioural specification** — describes behaviour, structure and interfaces in our own words.

## 2. Purpose

`x264` exposes more than a hundred command-line options. Rather than duplicating the option vocabulary in a shell script, the executable itself answers completion queries. The shell helper invokes `x264 --autocomplete <prev> <cur>`; this file classifies every option into one of four categories (value-with-suggestions, value-without-suggestions, filename-valued, standalone flag), then either emits the appropriate candidate values or falls back to filename completion. It also knows which value lists come from the encoder API (preset, tune, profile, levels, colorspaces) and which from optional external libraries (FFmpeg pixel formats and demuxers).

## 3. Public interface

- `int x264_cli_autocomplete(const char *prev, const char *cur)` — the single exported entry point (declared in `x264cli.h`).
  - `prev`: the token immediately before the word being completed. An empty string means no previous token.
  - `cur`: the partial word being completed.
  - Return `0` after printing candidates terminated by a newline, or `1` to indicate the shell should perform its default completion (filename completion).
  - Output format: candidates separated by single spaces on one line, followed by `\n`; candidates are written to standard output.

Internal helper functions (all file-static):

- `list_contains(list, s)` — true if non-empty `s` exactly matches an entry in a null-terminated string list; empty strings are never matched.
- `suggest(s, cur, cur_len)` — print `s` followed by a space if `s` is non-empty and begins with the first `cur_len` bytes of `cur` (case-sensitive prefix test).
- `suggest_lower(s, cur, cur_len)` — like `suggest`, but the prefix test is case-insensitive and the candidate is lowercased on output. Used for CPU feature names.
- `suggest_num_range(start, end, cur, cur_len)` — format each integer in the inclusive range as decimal and offer it through `suggest`.
- `suggest_token(s, delim, cur, cur_len)` — compiled only with libavformat support; split a delimiter-separated string and offer each token that is at least as long as `cur` and prefix-matches it. Used for FFmpeg demuxer names, which may contain comma-separated aliases.

Macros used for dispatch: `OPT(x)` matches `prev` exactly; `OPT2(a,b)` matches either alias; `OPT_TYPE(t)` tests membership in one of the four classification lists; short aliases for the suggest helpers bind the `cur` arguments.

## 4. State and data structures

All state is immutable, process-lifetime, file-static data:

- `level_names` — H.264 level strings offered for `--level`: `1`, `1.1`, `1.2`, `1.3`, `1b`, `2`, `2.1`, `2.2`, `3`, `3.1`, `3.2`, `4`, `4.1`, `4.2`, `5`, `5.1`, `5.2`, `6`, `6.1`, `6.2`.
- `opts_suggest` — long/short option names that take a value with known candidates: `--alternative-transfer`, `--aq-mode`, `--asm`, `--avcintra-class`, `--avcintra-flavor`, `--b-adapt`, `--b-pyramid`, `--colormatrix`, `--colorprim`, `--cqm`, `--demuxer`, `--direct`, `--frame-packing`, `--input-csp`, `--input-fmt`, `--input-range`, `--level`, `--log-level`, `--me`, `--muxer`, `--nal-hrd`, `--output-csp`, `--overscan`, `--pass`/`-p`, `--preset`, `--profile`, `--pulldown`, `--range`, `--subme`/`-m`, `--transfer`, `--trellis`/`-t`, `--tune`, `--videoformat`, `--weightp`.
- `opts_nosuggest` — value-taking options with no enumeration: `--b-bias`, `--bframes`/`-b`, `--deblock`/`-f`, `--bitrate`/`-B`, `--chroma-qp-offset`, `--chromaloc`, `--cplxblur`, the custom-matrix options (`--cqm4*` and `--cqm8*` family), `--crf`, `--crf-max`, `--crop-rect`, `--deadzone-inter`, `--deadzone-intra`, `--fps`, `--frames`, `--input-depth`, `--input-res`, `--ipratio`, `--keyint`/`-I`, `--lookahead-threads`, `--mastering-display`, `--cll`, `--merange`, `--min-keyint`/`-i`, `--mvrange`, `--mvrange-thread`, `--nr`, `--opencl-device`, `--output-depth`, `--partitions`/`-A`, `--pbratio`, `--psy-rd`, `--qblur`, `--qcomp`, `--qp`/`-q`, `--qpmax`, `--qpmin`, `--qpstep`, `--ratetol`, `--ref`/`-r`, `--rc-lookahead`, `--sar`, `--scenecut`, `--seek`, `--slices`, `--slices-max`, `--slice-max-size`, `--slice-max-mbs`, `--slice-min-mbs`, `--sps-id`, `--sync-lookahead`, `--threads`, `--timebase`, `--vbv-bufsize`, `--vbv-init`, `--vbv-maxrate`, `--video-filter`/`--vf`, `--zones`.
- `opts_filename` — options whose value is a path: `--cqmfile`, `--dump-yuv`, `--index`, `--opencl-clbin`, `--output`/`-o`, `--qpfile`, `--stats`, `--tcfile-in`, `--tcfile-out`.
- `opts_standalone` — boolean flags with no value, including both positive and negative spellings: `--8x8dct`/`--no-8x8dct`, `--aud`, `--bff`, `--bluray-compat`, `--cabac`/`--no-cabac`, `--constrained-intra`, `--cpu-independent`, `--dts-compress`, `--fake-interlaced`, `--fast-pskip`/`--no-fast-pskip`, `--filler`, `--force-cfr`, `--mbtree`/`--no-mbtree`, `--mixed-refs`/`--no-mixed-refs`, `--no-asm`, `--no-chroma-me`, `--no-dct-decimate`, `--no-deblock`, `--no-progress`, `--no-psy`, `--no-scenecut`, `--no-weightb`, `--non-deterministic`, `--open-gop`, `--opencl`, `--pic-struct`, `--psnr`, `--quiet`, `--sliced-threads`, `--slow-firstpass`, `--ssim`, `--stitchable`, `--tff`, `--thread-input`, `--verbose`/`-v`, `--weightb`.
- `opts_special` — `--fullhelp`, `--help`/`-h`, `--longhelp`, `--version`; offered only when no other option precedes the cursor.

The file also consumes externally defined tables from `x264.h` and `x264.c`: transfer/colormatrix/colorprim names, direct-prediction names, B-pyramid names, NAL-HRD names, AVC-Intra flavor names, overscan/videoformat names, motion-estimation names, log-level names, preset/tune names, partition names, valid profile names, pulldown names, output-CSP names, range names, and the CPU feature table `x264_cpu_names`.

## 5. Algorithm / behaviour

The entry point computes `cur_len = strlen(cur)` and then walks a long ordered chain of `prev` comparisons. For each recognized option it emits a specific candidate set:

- `--alternative-transfer`, `--transfer`: all `x264_transfer_names` entries that are non-empty.
- `--aq-mode`: integers `0..3`.
- `--asm`: every CPU feature name from `x264_cpu_names`, lowercased, using case-insensitive prefix matching.
- `--avcintra-class`: `x264_avcintra_class_names`; `--avcintra-flavor`: the flavor names.
- `--b-adapt`: `0..2`; `--b-pyramid`: its name table; `--colormatrix`/`--colorprim`: their name tables.
- `--cqm`: `x264_cqm_names`; `--demuxer`/`--muxer`: the CLI demuxer/muxer tables.
- `--direct`: direct-prediction names; `--frame-packing`: `0..7`.
- `--input-csp`: all names from the CLI colorspace table (excluding `NONE` and stopping at `X264_CSP_CLI_MAX`); with libavformat, additionally every FFmpeg pixel-format descriptor name.
- `--input-fmt`: with libavformat, iterate demuxers and offer their comma-separated name tokens.
- `--input-range`, `--range`: the range names; `--level`: the local level list; `--log-level`: log-level names; `--me`: motion-estimation names; `--nal-hrd`: HRD names.
- `--output-csp`: the output-CSP name table; `--output-depth`: `8` if the build has 8-bit support and `10` if it has 10-bit support (possibly both).
- `--overscan`, `--partitions`/`-A`, `--preset`, `--profile`, `--pulldown`, `--tune`, `--videoformat`: the corresponding name tables.
- `--pass`/`-p`: `1..3`; `--subme`/`-m`: `0..11`; `--trellis`/`-t`: `0..2`; `--weightp`: `0..2`.

If no specific branch matched, a fallback runs unless `prev` belongs to `opts_nosuggest` or `opts_special`:

1. If `prev` is in `opts_filename`, or `cur` does not begin with `--`, return `1` immediately — the shell performs filename completion.
2. Otherwise print all options from `opts_suggest`, `opts_nosuggest`, `opts_filename` and `opts_standalone` whose names prefix-match `cur`.
3. Print `opts_special` entries only when `prev` is empty (i.e. the very first word is being completed).

Finally the function prints a newline and returns `0`.

Ordering and edge cases:

- The classification lists are checked before generating option candidates, so after `--bitrate` the shell completes filenames rather than offering other options; after a standalone flag the shell offers the full option list.
- Matching is prefix-based, so an empty `cur` matches every candidate in a list; `cur` values are never lowercased except in the CPU-name case.
- Empty-string entries in name tables are silently skipped (the helper tests `*s`).
- With no libavformat support, `--input-fmt` produces no candidates and the `--input-csp`/av-pix-fmt branches compile away.
- The numeric ranges are fixed and do not depend on encoder validation; for example `--subme` offers `0..11` even though lower values may be rejected by the encoder in combination with other settings.

## 6. Dependencies

- Header dependencies: `x264cli.h` (declares the entry point and several name tables), `input/input.h` (CLI colorspace table), and, conditionally, `<libavformat/avformat.h>` and `<libavutil/pixdesc.h>`. The libavformat include is preceded by undefining the alignment macro to avoid a clash with FFmpeg's own definition.
- Compile-time switches from `config.h`: `HAVE_LAVF`, `HAVE_BITDEPTH8`, `HAVE_BITDEPTH10`.
- Data dependencies: `x264_cpu_names` from `common/cpu.c`; name tables defined in `x264.h` and `x264.c`.
- Called from: `main()` in `x264.c` through the special four-argument path `x264 --autocomplete prev cur`.
- Complementary script: `tools/bash-autocomplete.sh`, installed by `make install-bashcompletion`, which supplies the `prev`/`cur` words and consumes the output.

## 7. Constants and tables

The level list, numeric ranges and four option-classification lists are the file's constants; see section 4 for their full contents. Key numeric ranges: `--aq-mode` 0–3, `--b-adapt` 0–2, `--frame-packing` 0–7, `--pass` 1–3, `--subme` 0–11, `--trellis` 0–2, `--weightp` 0–2, output depth 8/10.

The four option categories form a state machine over the previous token:

| Previous token category | Result |
| --- | --- |
| Known value option with suggestions | Print enumerated/numeric candidates |
| Value option without suggestions | Fall through to shell filename completion |
| Filename option | Shell filename completion |
| Standalone flag or ordinary token | Offer all non-special options matching `cur` |
| Nothing (`prev == ""`) | Offer all options including `--help`/`--version` family |

## 8. Performance notes

Completion is a one-shot, interactive operation; there is no hot path. The implementation iterates small static lists and a handful of FFmpeg iterators, so latency is dominated by process startup and dynamic linking. Still, design choices matter:

- Early `return 1` for filename options avoids walking and printing hundreds of option names.
- Output is emitted with unbuffered character/string writes (`putchar`/`printf`) into a line; buffering the whole line would be equivalent but is unnecessary at this scale.
- CPU-name completion lowercases with a bitwise OR of `0x20` on uppercase ASCII characters instead of calling locale-sensitive functions — deterministic and locale-independent.
- To make completion fast and complete, keep the option classification lists in sync with the `long_options` table in `x264.c`; a missing entry silently degrades completion to filename mode.

Public reference: the project's `tools/bash-autocomplete.sh` completion script describes the `prev`/`cur` calling convention used by shells.

## 9. References

- Reference tree `x264.h` — name tables and CPU feature constants.
- Reference tree `x264.c` — `main()`'s `--autocomplete` fast path and the definitions of the CLI name tables.
- Reference tree `tools/bash-autocomplete.sh` — the shell side of the protocol.
- Reference tree `input/input.h` — `x264_cli_csps` and `X264_CSP_CLI_MAX`.
- FFmpeg/libavformat API documentation for `av_pix_fmt_desc_next` and `av_demuxer_iterate`.

## 10. Verification checklist

- Invoke `x264 --autocomplete "" --` and verify the output starts with `--alternative-transfer` and includes the full option list, ending with a newline.
- Invoke with `prev="--preset"`, `cur="v"` and verify only presets beginning with `v` (`veryfast`, `veryslow`) are offered.
- Invoke with `prev="--subme"`, `cur=""` and verify integers `0` through `11`.
- Invoke with `prev="--asm"`, `cur="S"` and verify lowercase CPU names that begin with `s` (case-insensitive).
- Invoke with `prev="-o"`, `cur=""` and verify return code `1` with no printed candidates (shell filename completion).
- Invoke with `prev="--bitrate"`, `cur=""` and verify return code `1`.
- Invoke with `prev="--help"`, `cur=""` and verify no candidates (special options suppressed after another option).
- With a 10-bit-only build, verify `--output-depth` offers only `10`; with a dual build, both `8` and `10`.
- With `--level`, verify `1b` appears and that `6.2` is the last level.
- Verify that all candidates are separated by one space and that output terminates with exactly one newline.

## 11. Open questions

- The exact contents of `X264_CSP_CLI_MAX` and the CLI colorspace table are defined in `input/input.h`; they are not fully listed in this file.
- Whether shells other than bash use the same calling convention is **UNKNOWN**; only the bundled bash script is shipped.
- The behaviour of completion when `cur` contains embedded whitespace or shell metacharacters is delegated to the shell and is **UNKNOWN** from this source.
