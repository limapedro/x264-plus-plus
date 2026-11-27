# x264cli.h — rebuild specification

## 1. Header

- Source path: `x264cli.h` (root of the reference tree).
- Role: small shared header that defines the CLI-side helper types, logging entry points, name-table declarations and tiny inline utilities used by `x264.c` and the CLI support modules.
- Marker: **dirty-side behavioural specification** — describes behaviour and structure, never source code.

## 2. Purpose

The command-line front end consists of many translation units (`x264.c`, `autocomplete.c`, the `input/*` and `output/*` drivers, the `filters/video/*` modules). This header is their common vocabulary. It provides: a single opaque handle type used by demuxers and muxers, the list of string tables that map user-facing option values to indices, three small header-only helpers (greatest common divisor, least common multiple, filename-extension discovery), the two logging printers, the autocomplete entry point, a Windows-only console-title and filename conversion hook, and two error-propagation macros. Keeping these declarations in one place lets the CLI modules avoid depending on the encoder's internal headers.

## 3. Public interface

### Constants

- `UPDATE_INTERVAL` — `250000`. Expressed in microseconds; a quarter of a second. The progress printer in `x264.c` suppresses updates closer together than this interval.
- `MAX_RESOLUTION` — `16384`. Largest accepted width or height for a CLI input; `x264.c` rejects `info.width`/`info.height` above it.

### Types and macros

- `typedef void *hnd_t` — opaque handle for an open input or output module. Every demuxer and muxer stores its private state behind this pointer; the CLI only passes it around.
- `range_enum` — values `RANGE_AUTO = -1`, `RANGE_TV = 0`, `RANGE_PC = 1`. Used for input/output color-range options where "auto" is distinct from a forced limited/full range.
- `RETURN_IF_ERR(cond, name, ...)` — a statement macro: when `cond` is true it emits an error-level CLI log tagged with `name` and the supplied printf-style message, then returns the caller-provided `ret` value.
- `FAIL_IF_ERR(cond, name, ...)` — specialization of the above that returns `-1`; the standard "report and fail" idiom throughout the CLI.

### Extern string tables

The header declares (but does not define) these null-terminated `const char * const` arrays, all of which are defined in `x264.c` or in encoder-side files:

`x264_avcintra_class_names`, `x264_cqm_names`, `x264_log_level_names`, `x264_partition_names`, `x264_pulldown_names`, `x264_range_names`, `x264_output_csp_names`, `x264_valid_profile_names`, `x264_demuxer_names`, `x264_muxer_names`.

Each array is a display/parse vocabulary: index `i` corresponds to the numeric option value `i`, and a final null pointer terminates the list. Some entries are empty strings (reserved values that must not be offered to users or parsed by name).

### Functions

- `void x264_cli_log(const char *name, int i_level, const char *fmt, ...)` — print a level-tagged diagnostic to `stderr`; see `x264.c.md` for the exact format.
- `void x264_cli_printf(int i_level, const char *fmt, ...)` — print an untagged message to `stderr`, suppressed when `i_level` exceeds the current CLI log level.
- `int x264_cli_autocomplete(const char *prev, const char *cur)` — produce shell completion candidates for the word `cur` following `prev`; returns `0` when it printed candidates, `1` when the shell should fall back to filename completion. Implemented in `autocomplete.c`.

### Inline helpers

- `uint64_t gcd(uint64_t a, uint64_t b)` — Euclid's algorithm on unsigned operands: repeatedly replace `(a,b)` with `(b, a mod b)` until the remainder is zero, then return `b`. The intermediate remainder is held in a signed 64-bit variable; for the values used by the CLI this is immaterial.
- `uint64_t lcm(uint64_t a, uint64_t b)` — `(a / gcd(a,b)) * b`. Dividing before multiplying reduces the chance of intermediate overflow, though overflow is still possible for very large inputs.
- `char *get_filename_extension(char *filename)` — scan backwards from the terminating NUL until a `.` is found or the start of the string is reached; then advance one character if a dot was actually found. Returns a pointer to the character after the last dot, or to the string start when there is no dot. With a pathname containing dots in directories, the search is over the whole string, not just the basename.

### Windows-only declarations

When `_WIN32` is defined:

- `void x264_cli_set_console_title(const char *title)` — set the console window title from a UTF-8 string (implemented in `x264.c`).
- `int x264_ansi_filename(const char *filename, char *ansi_filename, int size, int create_file)` — convert a UTF-8 filename to the ANSI code page, optionally creating the file as a side effect; implemented outside this header.

On other platforms `x264_cli_set_console_title(title)` is defined away to nothing.

## 4. State and data structures

The header itself owns no mutable state. However it fixes two pieces of shared state by convention:

- `cli_log_level` lives in `x264.c` and is consulted by `x264_cli_log`/`x264_cli_printf`. Its initial value is `X264_LOG_INFO`; `--quiet`, `-v` and `--log-level` change it.
- The opaque `hnd_t` values are owned respectively by the selected input and output driver; the CLI closes them through function pointers in `cli_input_t`/`cli_output_t`, never by dereferencing them here.

The string tables are immutable process-lifetime data. They are consumed by `parse_enum_name`/`parse_enum_value` in `x264.c` and by the completion engine in `autocomplete.c`.

## 5. Algorithm / behaviour

Contract details that an independent implementation must preserve:

- `gcd` terminates when the modulo result becomes zero; `gcd(a,0) == a` and `gcd(0,b)` is `b` for non-zero `b`, while `gcd(0,0)` returns `0` (no division by zero occurs because the loop returns before another modulo).
- `lcm` follows the mathematical `|a*b|/gcd(a,b)` identity without absolute values; it is intended for positive SAR/rate values.
- `get_filename_extension` handles dotfiles such as `.y4m` by returning the string start (the dot is the first character, so the loop stops there and the "advance if dot" step moves past it — note that this yields the substring after the leading dot), and handles names without a dot by returning the original pointer. The caller in `select_output` compares the returned text case-insensitively against known container names.
- The `FAIL_IF_ERR` family always logs at error level with the module name as the tag, so messages originating from the CLI appear as `x264 [error]: ...` while messages from an input module carry that module's name.
- Completion returns `1` only for filename-valued options and for words not beginning with `--`; returning `1` is how the completion script knows to delegate to the shell's default filename completion.
- The header includes `common/base.h`, so it inherits that header's integer types, alignment macros, `X264_MIN`/`X264_MAX` helpers and `X264_LOG_*` constants; it does not itself include `x264.h`.

## 6. Dependencies

- Included by: `x264.c`, `autocomplete.c` (and through them effectively all CLI modules that log).
- Includes: `common/base.h` only.
- Calls into: `x264_cli_log`/`x264_cli_printf` are implemented in `x264.c`; `x264_cli_autocomplete` is implemented in `autocomplete.c`; `x264_cli_set_console_title` is implemented in `x264.c` for Windows.
- Shared globals: the name tables listed in section 3 are defined in `x264.c` (or `common/base.c` for encoder-owned tables) and referenced here.

## 7. Constants and tables

| Constant | Value | Meaning |
| --- | --- | --- |
| `UPDATE_INTERVAL` | 250000 | Microseconds between progress-line refreshes |
| `MAX_RESOLUTION` | 16384 | Upper bound for CLI width/height |
| `RANGE_AUTO` | -1 | Let the source decide the color range |
| `RANGE_TV` | 0 | Limited / MPEG range |
| `RANGE_PC` | 1 | Full range |

The name-table externs are the authoritative vocabularies for `--log-level` (none/error/warning/info/debug), `--range` (auto/tv/pc), `--pulldown` (none/22/32/64/double/triple/euro), `--cqm` (flat/jvt), `--partitions`, `--output-csp`, demuxers, muxers, AVC-Intra classes and valid profiles. Their concrete contents are enumerated in `x264.c.md` and `autocomplete.c.md`.

## 8. Performance notes

This header is not on any hot path; all three inline helpers are tiny and called only during option parsing or filename handling. `gcd`/`lcm` are used a handful of times to reduce SAR and frame-rate fractions. `get_filename_extension` is a short backward scan over a pathname. The logging macros perform a level test before formatting, so disabled levels cost only an integer comparison.

Concrete optimization guidance (for an implementer, not a caller):

- Keep the inline helpers markable as `static inline` so the compiler can fold the short loops; avoid turning them into out-of-line calls in a hot parse loop.
- Do not put `printf` work behind `RETURN_IF_ERR` when the condition is false — the macro's log call is inside the conditional branch.
- Public references: the x264 CLI documentation (`doc/vui.txt` in the reference tree) explains the range/VUI values these helpers support; general C guidance on branchless/inline small utilities applies.

## 9. References

- Reference tree `common/base.h` — immediate include, source of `X264_LOG_*`, integer types and min/max macros.
- Reference tree `x264.c` — definitions of every declared function and table.
- Reference tree `autocomplete.c` — the consumer of the completion contract.
- H.264 Annex E (VUI) for the semantics of the color-range values in `range_enum`.

## 10. Verification checklist

- Compile a small translation unit that includes only `x264cli.h` (plus `stdint.h` as required by `common/base.h`) and instantiates all three inline helpers.
- Check `gcd(48,18) == 6`, `gcd(a,0) == a`, `lcm(4,6) == 12`.
- Check `get_filename_extension("movie.mkv")` points at `"mkv"`, and that a name without a dot returns the original pointer.
- Verify `RANGE_AUTO == -1` and `UPDATE_INTERVAL == 250000`.
- Verify that `FAIL_IF_ERR(1, "x264", "msg")` returns `-1` from the enclosing function and prints `x264 [error]: msg`.
- On non-Windows builds, verify `x264_cli_set_console_title("x")` expands to nothing and generates no call.
- Verify completion entry point returns `1` for a filename option and `0` after printing candidates.

## 11. Open questions

- `x264_ansi_filename` is declared here but its implementation is in another module; the exact failure codes and the meaning of `create_file` beyond the name are **UNKNOWN** from this file alone.
- The precise set of empty-string reserved slots in the extern name tables is defined at the definition sites; this header only declares the arrays.
- Whether `gcd`'s signed intermediate ever matters for the maximum 64-bit input is **UNKNOWN** in practice; the inputs used by the CLI are small positive fractions.
