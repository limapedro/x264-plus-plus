# `tools/msvsdepend.sh` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/msvsdepend.sh`.
- Role: a POSIX shell helper that emits a Makefile dependency rule for a C source file by asking Microsoft's compiler to list its included headers, normalising the result into Unix-style escaped paths.
- Marker: this file is a **dirty-side behavioural specification**; the workflow is described in our own words.

## 2. Purpose

When building with MSVC through a Unix-style makefile, the build system still needs accurate header dependencies. MSVC's `-showIncludes` option prints include paths in a human-readable diagnostic format with Windows path separators and system headers mixed in. This script translates that output into a single make rule suitable for inclusion by the build.

## 3. Public interface

- Invocation: `msvsdepend.sh $(CC) $(CFLAGS) $(SRC) $(OBJ)` — four arguments in that order.
- Exit status: 1 when argument 1, 3 or 4 is missing. Argument 2 (flags) may be empty.
- Standard output: one Makefile rule of the form `target: dependency dependency ...`.
- Standard error: compiler diagnostics are suppressed except where explicitly redirected into the capture pipeline.

## 4. State and data structures

- `deps`: a newline-separated list of dependency paths assembled from compiler output, then transformed in stages.
- `target`: the escaped object-file name used as the rule's target.
- Shell options: `set -f` disables pathname expansion so Windows paths containing glob-like characters are not expanded.
- `IFS` is temporarily set to a newline during path conversion so `cygpath` can process all lines at once.

## 5. Algorithm / behaviour

1. **Argument validation.** Require arguments 1 (compiler), 3 (source) and 4 (object). Argument 2 may be empty.
2. **Ask the compiler for includes.** Run the compiler with the supplied flags plus options that suppress logo output, request include listing, disable warnings, and perform syntax checking only (no code generation or linking). Merge stderr into stdout.
3. **Extract include paths.** Keep only lines beginning with the compiler's "including file:" diagnostic prefix, strip that prefix and surrounding whitespace, and convert backslashes to forward slashes.
4. **Drop system headers.** Remove any path containing `/Program Files` (case-insensitive on the "Program Files" word) as a crude heuristic for system includes.
5. **Append the source file.** The source path from argument 3 is added as an additional dependency.
6. **Normalise path syntax.** If `cygpath` is available, convert all paths to Unix form. Otherwise, if running under WSL (detected by the kernel release string containing "Microsoft"), rewrite a leading drive letter such as `C:` into `/mnt/c`. Other platforms keep the Windows-style paths.
7. **Escape for make.** Escape spaces, `#` and `$` in each dependency so the generated rule is syntactically valid.
8. **Strip working-directory prefixes.** Remove a leading absolute working-directory prefix and a leading `./` from each path.
9. **Deduplicate and collapse.** Sort, remove duplicates, escape again, collapse newlines and carriage returns into spaces, and trim leading/trailing whitespace.
10. **Escape the target.** Apply the same escaping to the object-file argument.
11. **Print the rule.** Emit `target: deps` followed by a newline.

Behavioural notes:

- The `-Zs` syntax-check-only option means no object file is produced; this script only generates dependency metadata.
- The system-header heuristic is explicitly a hack in the source; it may wrongly drop a project header located under a path containing that string.
- WSL conversion uses a GNU sed extension for case-insensitive drive-letter lowering; the source notes this is acceptable because WSL has GNU sed.

## 6. Dependencies

- External tools: POSIX shell, the MSVC compiler driver, `grep`, `sed`, `sort`, `uniq`, `tr`, `printf`, optionally `cygpath`, and `pwd`.
- **Consumers**: the makefile that builds the Windows/MSVC configuration of the encoder.
- **Related**: `tools/gas-preprocessor.pl.md`, which also strips MSVC-specific compiler flags when preprocessing assembly for armasm; both scripts bridge Unix build conventions and MSVC tools.

## 7. Constants and tables

| Item | Meaning |
| --- | --- |
| Required args | compiler, (flags), source, object. |
| Compiler options | no logo, show includes, no warnings, syntax check only. |
| Include marker | The compiler's "including file:" diagnostic prefix. |
| System path marker | `Program Files`, matched case-insensitively. |
| WSL mount prefix | `/mnt/<lowercase drive letter>`. |
| Escaped characters | space, `#`, `$`. |
| Output form | One `target: deps` line. |

## 8. Performance notes

- The script runs once per source file at build time; its cost is a single compiler syntax-check invocation, which is much cheaper than a full compile.
- Deduplication via `sort | uniq` keeps the generated rule short, reducing make's dependency-checking work.
- `set -f` prevents accidental glob expansion of paths containing brackets or asterisks, which is important for generated or vendored headers.

### 8.1 Relation to faster-kernel development

When adding a new assembly or C helper header used by a SIMD kernel, the dependency rule must be regenerated or the build will not rebuild dependents when the header changes. A stale dependency file can make a benchmark appear unchanged when in fact the kernel source header was edited. Recommended loop: regenerate dependencies, do a clean rebuild, then validate the kernel with `tools/checkasm.c.md` before measuring speed with `--bench`.

References: MSVC documentation for `-showIncludes` and `-Zs`; GNU make manual on generated dependency rules; `tools/gas-preprocessor.pl.md` for the analogous toolchain bridge.

## 9. References

- The source file itself, `tools/msvsdepend.sh`.
- Microsoft C/C++ compiler documentation for `-showIncludes`, `-Zs`, `-nologo` and `-W0`.
- GNU make manual for dependency rule syntax and escaping.
- Cygwin documentation for `cygpath`; Microsoft WSL documentation for the `/mnt/<drive>` layout.
- `tools/gas-preprocessor.pl.md` for the related MSVC/armasm flag handling.

## 10. Verification checklist

- Invoking with fewer than three arguments exits with status 1.
- The output begins with the escaped object-file name followed by a colon.
- Every header reported by the compiler appears exactly once in the dependency list.
- The source file itself appears in the dependency list.
- Paths under Program Files are absent.
- Backslashes in paths are converted to forward slashes.
- Spaces, `#` and `$` in paths are escaped in the emitted rule.
- On Cygwin, paths are converted to Unix form; on WSL, drive prefixes become `/mnt/<letter>`.
- A leading `./` or working-directory prefix is stripped from dependencies.

## 11. Open questions

- UNKNOWN: exact behaviour with paths containing colons other than a drive letter.
- UNKNOWN: whether the case-insensitive Program Files filter is still sufficient for localized Windows installations.
- UNKNOWN: how the build system invokes this script and whether it retries on failure; the script itself does not report partial success.
