# `extras/getopt.c` — dirty-side behavioural specification

## 1. Header

- Source path: `extras/getopt.c`.
- Role: a portable implementation of the GNU `getopt` family, providing short and long option parsing with argv permutation, environment-controlled ordering and POSIX compatibility modes.
- Marker: this file is a **dirty-side behavioural specification**; the parsing rules below are paraphrased from the implementation.

## 2. Purpose

The project needs a single option-parsing behaviour across platforms, including support for abbreviated long options and options placed after positional arguments. The GNU implementation supplies that, but glibc already provides it on Linux; therefore the code is conditionally elided when the host glibc exposes a compatible interface version. On other platforms the compiled implementation is used.

## 3. Public interface

The file defines the variables declared in `extras/getopt.h.md` (`optarg`, `optind`, `opterr`, `optopt`) and implements:

| Function | Contract |
| --- | --- |
| `_getopt_internal(argc, argv, optstring, longopts, longind, long_only)` | Shared engine used by all public entry points. |
| `getopt(argc, argv, optstring)` | Short-option parser; delegates with no long-option table. |
| `getopt_long(argc, argv, optstring, longopts, longind)` | Adds long options introduced by `--`. |
| `getopt_long_only(argc, argv, optstring, longopts, longind)` | Allows a single dash to introduce long options too. |

Additional internal helpers: `_getopt_initialize`, `exchange`, `my_index`. When built as part of glibc, extra non-option-flag machinery is compiled in; that path is not used by standalone builds.

## 4. State and data structures

- `optind`: index of the next argv element to scan; initialised to 1 because the standard requires it, with the implementation also accepting 0 as a reset trigger.
- `nextchar`: pointer into the current argv element where the next short-option character lives; null or empty means advance to the next element.
- `first_nonopt` / `last_nonopt`: bounds of the run of skipped non-option arguments used by permutation.
- `ordering`: one of `REQUIRE_ORDER`, `PERMUTE`, `RETURN_IN_ORDER`.
- `posixly_correct`: value of the `POSIXLY_CORRECT` environment variable.
- `__getopt_initialized`: avoids re-initialising on every call.
- `print_errors`: derived from `opterr` and a leading colon in the option string.
- For glibc builds only: non-option flag arrays and saved argc/argv.

## 5. Algorithm / behaviour

### 5.1 Elision rule

If compiling outside glibc with a glibc version whose getopt interface matches the declared interface version constant, the implementation is elided entirely and the host's library is used. Otherwise it is compiled.

### 5.2 Initialisation

`_getopt_initialize` sets the non-option bounds to the current `optind`, clears `nextchar`, reads the environment variable, and selects ordering:

- A leading `-` in the option string selects `RETURN_IN_ORDER` and is stripped from the string.
- A leading `+` selects `REQUIRE_ORDER` and is stripped.
- Otherwise, if `POSIXLY_CORRECT` is set, choose `REQUIRE_ORDER`.
- Otherwise choose `PERMUTE`.

### 5.3 Permutation

When the current element is a non-option:

- In `PERMUTE` mode, already-seen options are exchanged with already-skipped non-options so that options collect at the front; the scan then skips additional non-options and extends the skipped range.
- In `REQUIRE_ORDER` mode, parsing stops immediately.
- In `RETURN_IN_ORDER` mode, the non-option is returned as if it were an option with character code 1, with `optarg` pointing at it.

`exchange` swaps two adjacent subsequences of argv: the skipped non-options and the options processed since then, updating the bounds accordingly.

### 5.4 `--` handling

The exact string `--` terminates option scanning: `optind` is advanced, skipped non-options are exchanged back into place, and the remaining elements are treated as non-options. In `RETURN_IN_ORDER` mode only `--` can cause termination with `optind != argc`.

### 5.5 Long-option matching

A long option is detected when the element starts with `--`, or when `long_only` is set and the single-dash form does not correspond to a valid short option. This rule preserves access to short options such as `-f` even when a long option starting with the same letter exists.

1. Find the end of the option name, stopping at `=` or end of string.
2. Compare the supplied name against each descriptor with a prefix comparison.
3. An exact match wins immediately.
4. A first non-exact match is remembered; a second distinct one sets an ambiguity flag. Distinctness is determined by `has_arg`, `flag` or `val` differences, or by `long_only`.
5. Ambiguity without an exact match prints a diagnostic, advances past the element, sets `optopt` to 0 and returns `'?'`.
6. If an argument is present after `=`: accept it only when the descriptor allows an argument; otherwise print a diagnostic and return `'?'`.
7. If no `=` is present and the descriptor requires an argument, take the next argv element or report a missing argument.
8. Write the matched index to `longind` if provided.
9. If `flag` is non-null, set `*flag = val` and return 0; otherwise return `val`.

### 5.6 Short-option parsing

1. Read the next character and advance `nextchar`. Increment `optind` when that was the last character of the element.
2. If the character is absent from the option string or is a colon, print an invalid/illegal option diagnostic (wording differs under POSIXLY_CORRECT), set `optopt` and return `'?'`.
3. Special case: a descriptor entry of `W;` in the option string implements POSIX `-W foo` as long option `--foo`. It collects the argument, performs the same exact/ambiguous matching as above, and returns the long option's value or 0; if no match is found it returns `'W'` for the application to handle.
4. A single colon after the option letter means a required argument: take the rest of the current element if present, otherwise the next argv element, otherwise report a missing argument and return `':' or '?'` depending on the leading-colon option string.
5. Two colons mean an optional argument: take the rest of the current element if present, otherwise set `optarg` to null.
6. Return the option character.

### 5.7 Error message control

Error messages are suppressed when `opterr` is zero or the option string begins with `:`. A leading colon also changes the missing-argument return from `'?'` to `':'`. Messages include the program name from `argv[0]` and distinguish `--option`, `-option` and `+option` forms.

### 5.8 Public wrappers

- `getopt` calls the internal engine with no long-option table and `long_only = 0`.
- `getopt_long` passes the table and `long_only = 0`.
- `getopt_long_only` passes the table and `long_only = 1`.

## 6. Dependencies

- **Includes**: `<stdio.h>`, `getopt.h`, conditionally `<stdlib.h>`/`<unistd.h>`/`<string.h>`/`<libintl.h>`/`<gnu-versions.h>`.
- **Imported by**: the CLI and any module parsing command-line options; `extras/getopt.h.md` declares the interface.
- **Localisation**: the `_()` macro maps diagnostics through `gettext` when NLS is enabled, otherwise to identity.

## 7. Constants and tables

| Constant | Meaning |
| --- | --- |
| `GET_INTERFACE_VERSION` constant | Used with glibc's version header to decide whether to elide this implementation. |
| Option string prefixes | `-`, `+`, `:` as described above. |
| `W;` entry | POSIX `-W` synonym for long options. |
| Return codes | option character, 0, 1 (returned non-option), `'?'`, `':'`, −1. |
| `optopt` initial | `'?'` so it has a defined value before any error. |

## 8. Performance notes

- The parser mutates argv by permutation, avoiding a second copy of the argument vector; this is why the header pretends argv is const while the implementation casts away const.
- Long-option matching is a linear scan over the descriptor array, acceptable for a handful of options at startup.
- Repeated calls resume via `nextchar` and `optind` rather than restarting, so clustered short options are parsed incrementally.

### 8.1 Relation to faster-kernel development

A benchmarking tool that adds many tuning flags benefits from long-option abbreviation and permutation: options can follow positional inputs without breaking the command line. When adding such a tool, remember that `POSIXLY_CORRECT` in the environment changes ordering behaviour; benchmark scripts should either unset it or pass options before positional arguments. For the existing conformance tool's simpler CLI, see `tools/checkasm.c.md`.

References: `extras/getopt.h.md`, POSIX getopt specification, GNU libc documentation.

## 9. References

- The source file itself, `extras/getopt.c`, carrying the GNU C Library copyright notice.
- POSIX.1-2008 `getopt()` specification.
- GNU libc documentation for `getopt_long`, `getopt_long_only`, `POSIXLY_CORRECT` and argv permutation.
- `extras/getopt.h.md` for the declarations and `struct option`.

## 10. Verification checklist

- `getopt` parses clustered short options and required/optional arguments correctly.
- A leading `+` stops parsing at the first non-option; a leading `-` returns non-options with code 1.
- Without modifiers, `POSIXLY_CORRECT` selects stop-at-first-non-option and otherwise argv is permuted so options come first.
- `--` terminates scanning and leaves later elements as non-options.
- Long options are matched by unique abbreviation; ambiguous abbreviations return `'?'`.
- An exact long-option match wins over a non-exact one.
- A long option with `flag` set writes `val` and returns 0.
- A long option with an argument accepts `=` form or the following element.
- Missing required arguments return `':'` when the option string starts with `:` and `'?'` otherwise.
- `-W foo` is treated as `--foo` when `W;` is in the option string.
- Setting `opterr = 0` suppresses diagnostics without changing return values.

## 11. Open questions

- UNKNOWN: whether any CLI in the project uses `getopt_long_only`; the implementation exists but usage is not stated here.
- UNKNOWN: behaviour when `optind` is reset to 0 between calls with a different argv array; the implementation reinitialises but the source notes this is fragile.
- UNKNOWN: whether the glibc elision path is active in any supported build configuration; depends on configure checks.
