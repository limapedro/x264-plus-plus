# `extras/getopt.h` — dirty-side behavioural specification

## 1. Header

- Source path: `extras/getopt.h`.
- Role: the GNU-style declaration header for the portable `getopt` / `getopt_long` / `getopt_long_only` command-line option parser, including the `struct option` descriptor used by long options.
- Marker: this file is a **dirty-side behavioural specification**; the declarations and their contracts are described in our own words.

## 2. Purpose

On platforms whose C library lacks GNU-style long-option parsing, the project ships its own implementation. This header is the public contract between that implementation and the CLI: it declares the global communication variables, the option descriptor structure and the parser entry points, while carefully avoiding conflicts with a native glibc `getopt` declaration.

## 3. Public interface

### Global variables

| Variable | Type | Contract |
| --- | --- | --- |
| `optarg` | `char *` | Points to the argument of the option just found, or, in `RETURN_IN_ORDER` mode, to a non-option argv element returned as if it were an option argument. |
| `optind` | `int` | Index in `argv` of the next element to scan. Callers set it to 1 before the first call; on return −1 it indexes the first non-option element. |
| `opterr` | `int` | Non-zero enables automatic error messages for unrecognised options; set to zero to suppress them. |
| `optopt` | `int` | Set to the option character that was unrecognised or missing an argument. |

### Long-option descriptor

`struct option` has four fields:

- `name`: the long option name; a zero name terminates the array.
- `has_arg`: one of `no_argument` (0), `required_argument` (1) or `optional_argument` (2). The field is deliberately an `int` rather than an enum because some compilers reject type mismatches.
- `flag`: if non-null, points to an integer that is set to `val` when the option is found, while the parser returns 0.
- `val`: the value returned when `flag` is null; conventionally the equivalent single-letter option.

### Functions

- `getopt(argc, argv, optstring)`: parse short options. Returns the next option character, 0 for a flag-style long option, −1 when done, `'?'` for an error, or `':'` for a missing argument when the option string begins with a colon.
- `getopt_long(argc, argv, optstring, longopts, longind)`: as above plus long options; `longind` receives the index of the matched long option when non-null.
- `getopt_long_only(argc, argv, optstring, longopts, longind)`: as above, but a single dash may introduce a long option.
- `_getopt_internal(...)`: the shared implementation; documented as internal and not for direct caller use.

### Option-string syntax

- A bare letter is a flag option.
- One following colon means a required argument.
- Two following colons mean an optional argument (a GNU extension).
- A leading `+` selects stop-at-first-non-option ordering; a leading `-` selects return-in-order behaviour; a leading `:` suppresses error messages and changes the missing-argument return value.

## 4. State and data structures

The header itself declares only storage classes for the four global variables; their definitions live in `extras/getopt.c.md`. The descriptor array is caller-owned and must be terminated by an entry with an empty name.

Include guard behaviour: `_GETOPT_H` is defined unless `__need_getopt` is defined, in which case only the minimal declarations are exposed. The header includes `<ctype.h>` when `__GNU_LIBRARY__` is not defined, because ctype is guaranteed to exist and pulls in glibc feature macros without polluting the namespace too much. C++ linkage is wrapped in `extern "C"`.

## 5. Algorithm / behaviour

The header performs no algorithms. Its behavioural content is the contract enforced by the implementation:

1. Callers initialise `optind` to 1 (the documented 1003.2 value) and call the parser repeatedly until it returns −1.
2. An option element is an argv element beginning with `-`, except the exact strings `-` and `--`.
3. Short options may be clustered in one element; the parser resumes at the next character via internal state.
4. `--` terminates option scanning regardless of ordering mode.
5. Long options may be abbreviated as long as the abbreviation is unique or is an exact match of some defined option. If the long option has an argument, it follows `=` in the same element or occupies the next element.
6. A long option with a non-null `flag` sets `*flag = val` and returns 0; otherwise it returns `val`.
7. An unrecognised option or missing argument sets `optopt` and returns `'?'` (or `':'` for a leading-colon option string when the argument is missing).

These rules are elaborated in `extras/getopt.c.md`.

## 6. Dependencies

- **Includes**: `<ctype.h>` when not compiling against glibc; the include is part of the header's feature probing strategy.
- **Included by**: `extras/getopt.c.md`, the CLI source and any module using long options.
- **Related**: prototype for `getopt` is only emitted when `__GNU_LIBRARY__` is defined, because other libraries declare incompatible `const` variants in `<stdlib.h>`.

## 7. Constants and tables

| Constant | Value | Meaning |
| --- | --- | --- |
| `no_argument` | 0 | Long option takes no argument. |
| `required_argument` | 1 | Long option requires an argument. |
| `optional_argument` | 2 | Long option takes an optional argument. |
| `optind` initial | 1 | Standard requires this before the first call. |
| `optopt` initial | `'?'` | Implementation default, described in the .c specification. |

## 8. Performance notes

No runtime cost. Design note for implementers: keep long-option arrays static and terminated correctly; a missing terminator makes the parser read past the array.

### 8.1 Relation to faster-kernel development

The conformance tool's own CLI uses a simple `--bench` prefix check rather than this parser, so kernel work rarely touches this header. When adding a new tool that exposes many tuning flags, use the long-option descriptor rather than ad-hoc string comparisons; the framework in `tools/digress/cli.py.md` shows a similar option-group pattern in Python.

References: POSIX `getopt` specification for the variable contracts; GNU libc documentation for long-option extensions.

## 9. References

- The source file itself, `extras/getopt.h`, carrying the GNU C Library copyright notice.
- POSIX.1-2008 `getopt()` specification for `optarg`, `optind`, `opterr` and `optopt`.
- GNU libc documentation for `getopt_long` and `getopt_long_only`.
- `extras/getopt.c.md` for the implementation of these declarations.

## 10. Verification checklist

- `struct option` has exactly four fields with the documented types.
- `no_argument`, `required_argument` and `optional_argument` expand to 0, 1 and 2.
- Including the header twice is harmless.
- `__need_getopt` exposes only the minimal declarations and is undefined at the end of the header.
- In C++, all declarations have C linkage.
- With glibc detected, the header does not re-declare `getopt` with conflicting constness.
- Callers can set `opterr = 0` and still receive `'?'` for bad options.

## 11. Open questions

- UNKNOWN: which CLI modules in the reference tree actually include this header; the file itself does not say.
- UNKNOWN: whether a native `getopt_long` is preferred on some platforms; selection is build-system controlled.
- UNKNOWN: exact behaviour of `getopt_long_only` beyond the declaration; see `extras/getopt.c.md`.
