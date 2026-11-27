# `tools/cltostr.sh` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/cltostr.sh`.
- Role: a small POSIX shell filter that converts standard input into two C character arrays — one holding the input text and one holding its MD5 hash — for embedding an OpenCL kernel source string into a C file.
- Marker: this file is a **dirty-side behavioural specification**; the behaviour is described in our own words.

## 2. Purpose

The encoder's OpenCL path needs the kernel source text available as a compiled-in C string, plus a compact digest that can be used to detect mismatches between the built-in source and an external source file. This script automates the generation of both arrays from a source file supplied on standard input, avoiding hand-escaping of quotes and newlines.

## 3. Public interface

- Invocation: `cltostr.sh <output-file>` with the input text piped on standard input.
- Exit status: 1 when no output file argument is supplied; otherwise the status of the last command in the pipeline.
- Output file: a C fragment containing two `static const char` array definitions.
- Standard error: a temporary file is created next to the output file during processing and removed on exit.

No functions are exported to callers; `sanitize`, `dump` and `hash` are shell functions local to the script.

## 4. State and data structures

- Temporary file `<output-file>.temp`: holds the sanitised input text so its hash can be computed.
- Output file `<output-file>`: holds the generated C arrays.
- `trap` on EXIT: removes the temporary file regardless of success or failure.

## 5. Algorithm / behaviour

1. **Argument check.** If the first positional parameter is empty, exit with status 1. No default output name is provided.
2. **Sanitise the input.** A `sed` expression removes leading whitespace from every line, deletes empty lines, and deletes lines beginning with `//`. This drops blank lines and simple line comments from the embedded source.
3. **Write the first array.** The sanitised stream is written to the temporary file via `tee` and simultaneously passed to the dump function. The dump function emits a declaration of a static constant character array with a fixed name, then converts the byte stream with `od` into comma-separated hexadecimal byte literals, and finally appends a zero byte and closes the array. The result is a NUL-terminated C string constant.
4. **Compute the hash.** The hash function tries three platform-specific MD5 utilities in sequence — the GNU `md5sum`, BSD `md5 -q`, and Solaris `digest -a md5` — discards all error output, keeps only the first 32 characters of the result, and removes newline and carriage-return characters. Keeping exactly 32 characters extracts the hexadecimal digest even from tools that also print a file name.
5. **Write the second array.** The digest string is passed to the same dump function, producing a second NUL-terminated character array with a different name, appended to the output file.
6. **Cleanup.** The EXIT trap removes the temporary file.

Behavioural details:

- The two arrays are named so that one holds the OpenCL source and the other its hash; these names are consumed by the OpenCL dispatcher code.
- The hash covers the **sanitised** text, not the original input, because the temporary file receives the post-`sed` stream.
- The output is a C fragment, not a complete translation unit; it is intended to be included from a generated C file.
- The script assumes `sed`, `od`, `tee`, `cut`, `tr` and at least one MD5 implementation are available. `md5sum` is not standardised, hence the fallback chain.

## 6. Dependencies

- External tools: a POSIX shell, `sed`, `od`, `tee`, `cut`, `tr`, and one of `md5sum`, `md5`, `digest`.
- **Consumers**: the OpenCL source-generation step of the build, which includes the generated C fragment.
- **Related**: `extras/cl.h.md` and `extras/cl_platform.h.md` define the OpenCL API used by the code that consumes these strings; `extras/intel_dispatcher.h.md` is a separate CPU-side dispatcher and is not involved.

## 7. Constants and tables

| Item | Value / meaning |
| --- | --- |
| First array name | Static character array holding the OpenCL kernel source. |
| Second array name | Static character array holding the MD5 digest as text. |
| Digest length | 32 hexadecimal characters. |
| Terminator | A single zero byte appended to each array. |
| `od` options | Verbose output, no addresses, one byte per hexadecimal value. |
| `sed` filters | Strip leading whitespace; delete empty lines; delete `//` comment lines. |
| Temporary suffix | `.temp` appended to the output file name. |

## 8. Performance notes

- The script runs once at build time, not at encode time; its cost is irrelevant to runtime performance.
- `tee` lets the sanitised stream feed both the C-array generator and the hash calculation in one pass, avoiding a second read of the input.
- Hash-string extraction deliberately removes line breaks so the resulting C array contains only digest characters plus the terminator.

### 8.1 Relation to faster-kernel development

Embedded OpenCL kernels are a candidate target for optimisation alongside SIMD kernels. When changing a kernel:

1. Regenerate this C fragment so the embedded source and its digest stay in sync — a stale digest is a common source of confusing runtime behaviour.
2. Verify that the generated string still compiles as OpenCL C; the sanitisation step removes blank lines and `//` comments, which can expose layout assumptions in the kernel source.
3. Use the framework in `tools/checkasm.c.md` for CPU-side kernels and the decoder-comparison test in `doc/regression_test.txt.md` for OpenCL output equivalence; neither substitutes for checking the embedded digest.

References: OpenCL specification for program creation from source; `extras/cl.h.md` for `clCreateProgramWithSource`.

## 9. References

- The source file itself, `tools/cltostr.sh`.
- POSIX shell and utility specifications for `sed`, `od`, `tee`, `cut`, `tr` and `trap`.
- RFC 1321 / FIPS 180-4 for MD5 digest semantics.
- `extras/cl.h.md` for the OpenCL API that consumes the generated source string.

## 10. Verification checklist

- Invoking with no argument exits with status 1 and produces no output file.
- The generated output contains exactly two `static const char` array definitions.
- Decoding the first array's bytes yields the sanitised input followed by a zero byte.
- Leading whitespace, blank lines and `//` comment lines are absent from the embedded source.
- Decoding the second array yields a 32-character hexadecimal string followed by a zero byte.
- The hash matches an independent MD5 computation of the sanitised text.
- The temporary file is removed after both success and failure.

## 11. Open questions

- UNKNOWN: which generated C file includes this fragment in the current build system; the script does not name a consumer.
- UNKNOWN: behaviour when none of the three MD5 utilities is available; the pipeline would produce an empty second array.
- UNKNOWN: whether block comments (`/* ... */`) are intentionally preserved; only `//` lines are filtered.
