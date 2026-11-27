# `tools/gas-preprocessor.pl` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/gas-preprocessor.pl`.
- Role: a Perl filter that takes modern GNU-assembler-syntax assembly (as used by x264, FFmpeg and VLC), runs it through a C preprocessor and a custom macro/directive expander, and emits syntax accepted by older Apple assemblers, clang's integrated assembler, or Microsoft's armasm.
- Marker: this file is a **dirty-side behavioural specification**; the many transformations are described in our own words.

## 2. Purpose

Apple's assembler historically lacked directives such as `.rept` and had an incompatible macro syntax, and clang's integrated assembler and armasm have their own gaps. Rather than maintain separate assembly sources per toolchain, the project keeps one GNU-syntax source and uses this script as a translation layer. The source note explicitly says the implementation covers the subset used by x264, FFmpeg and VLC and that other projects may need additions.

## 3. Public interface

Invocation pattern: the assembler command line is passed to the script, with script options separated from the compiler/assembler command by `--`.

| Option | Effect |
| --- | --- |
| `-help` | Print usage text and exit. |
| `-arch <name>` | Force target architecture; accepted names are canonicalised (e.g. `arm64` → `aarch64`, `ppc` → `powerpc`). Unknown names are fatal. |
| `-as-type <type>` | Select target assembler: one of `gas`, `apple-gas`, `clang`, `apple-clang`, `armasm` (and the plain forms). Unknown types are fatal. |
| `-fix-unreq` / `-no-fix-unreq` | Enable or disable a workaround that emits both lower- and upper-case `.unreq` forms; enabled by default on Darwin. |
| `-force-thumb` | Treat input as Thumb regardless of directives; documented as incomplete and only tested with specific sources. |
| `--` | Everything after this token is the assembler command, not script options. |

Behaviour by input type:

- A `.c` input is compiled to assembly by adding `-S`.
- A `.s` or `.S` input is passed through the C preprocessor by adding `-E`.
- `-v`, `-h`, `--version` or `-dumpversion` requests are passed straight through via `exec` for toolchain probing.
- Anything else is fatal with "Unrecognized input filetype".

Output goes to the assembler via a pipe, or to a temporary `.asm` file for armasm, which is then assembled by a separate invocation and deleted at exit.

## 4. State and data structures

The script maintains a substantial set of parser tables:

- **Macro state**: current macro name, nesting level, per-macro line arrays, argument names, default values, a unique expansion counter, `altmacro` flag, and `irp` state.
- **Repetition state**: repetition count, accumulated body lines, irp arguments and parameter.
- **Conditional stack**: nested `.if` results, including `.ifb`, `.ifc`, `.ifeq`, `.iflt` and their negations.
- **Symbol table**: values assigned by `.set` / `.equ`, used in expression evaluation.
- **Section stack**: for `.previous` emulation.
- **Literal-pool table**: expressions used by `ldr reg, =expr`, mapped to generated labels, plus the literal width (`.word` or `.quad`).
- **Thumb state**: whether Thumb mode is active, labels seen, call targets and import symbols.
- **Register-alias tables**: NEON `.dn`/`.qn` aliases and AArch64 `.req` aliases, with recursive resolution.
- **Temporary-label tables**: local numeric labels mapped to unique generated names, for armasm.
- **Canonical architecture map** and **per-architecture comment prefix** (`//` for AArch64, `@` for ARM, `#` for PowerPC).

## 5. Algorithm / behaviour

### 5.1 Command-line and toolchain setup

1. Parse options until the assembler command is collected. `--` explicitly starts the command; otherwise the first non-option token starts it and consumes the rest.
2. For armasm, replace the compiler driver with `cpp`, add `-undef`, manually define `_WIN32`, remove `-nologo`, and strip `-ignore <value>` pairs and `-MM`. A `-MM` request is executed directly and exits.
3. If compiling with `-c` but no `-o`, synthesise an output name by replacing the source extension with `.o`.
4. Rewrite the preprocessor's `-o` argument to `-` so make dependency targets specified with `-MT` are not changed.
5. For non-armasm, replace source-file arguments in the assembler command with `-x assembler -`. For armasm, strip many compiler-only flags (including `-G`, `-W`, `-Z`, `-fp`, `-EHsc`, `-O`, `-D`, `-U`, `-m`, `-M`, `-c`, `-I`) and substitute a temporary `.asm` file for the `.S` input.
6. Determine architecture from `-arch` if given, otherwise from the compiler binary name, otherwise from a `-arch` flag in the command, otherwise from the host `arch` command. Canonicalise and reject unknown values.
7. Set the comment prefix from the canonical architecture, switching to `;` for armasm.

### 5.2 Two-pass parsing

**Pass 1** reads the preprocessed input, strips preprocessor line markers (`#`-leading lines) and architecture-specific comments, removes carriage returns, splits on semicolons, and calls `parse_line` on each sub-line. This pass primarily captures macro definitions, `.rept` bodies and conditional structure.

**Pass 2** is performed implicitly as macro expansions and repetition unrollings call `parse_line` recursively, which in turn calls `expand_macros` and finally emits lines through `handle_serialized_line`.

### 5.3 Conditional handling

`.if` variants are evaluated with Perl's expression evaluator after substituting known symbols:

- `.ifb` tests whether the expression (with whitespace removed) is empty.
- `.ifc` compares two comma-separated strings.
- Plain `.if` evaluates a numeric expression as non-zero.
- `.ifeq` and `.iflt` evaluate equality and less-than-zero.
- `.else`, `.elseif` and `.endif` manipulate the stacked result; a negative value on the stack marks an already-taken else branch so it cannot be taken twice.
- Lines in false blocks are discarded, but conditionals inside macros or repetitions are deferred until expansion.

### 5.4 Macro expansion

- `.macro` / `.endm` define named macros with optional comma separators, named arguments and `name=default` defaults. A trailing `:vararg` argument absorbs extra arguments.
- `.purgem` removes a macro definition.
- `.altmacro` and `.noaltmacro` toggle alternate mode, where `%expr` is evaluated and bare argument names are also substituted.
- Macro invocation is detected by matching a line against a known macro name. Arguments are split on commas with whitespace separation, preserving blank arguments and treating arithmetic/shift operators as part of a token.
- Substitution replaces escaped argument names, longest name first so prefixes do not corrupt longer names. `\\@` is replaced by a unique per-expansion counter.
- Nested macros are supported only when an outer macro is active; otherwise a fatal error is raised.

### 5.5 Repetition

- `.rept <expr>` repeats the accumulated body a computed number of times.
- `.irp` iterates over whitespace-separated arguments, substituting escapes of the parameter.
- `.irpc` iterates over individual characters of the argument string.
- `.endr` unrolls the body with the appropriate substitutions; state is saved and restored so nested repetitions work.

### 5.6 Serialised-line transformations

For every emitted line, in roughly this order:

1. **Sections**: track `.section`/`.text`/`.const_data`; `.previous` replays the second-most-recent section.
2. **Thumb mode**: `.code 16`/`.thumb` and `.code 32`/`.arm` toggle state.
3. **Literal pools**: rewrite `ldr reg, =expr` to a generated label and emit pool entries at `.ltorg` or at the end, with `.word` or `.quad` depending on architecture. Apple syntax uses `@PAGE` / `@PAGEOFF` for ADRP/LO12 relocations.
4. **Thumb immediates**: `add` with an immediate above 255 becomes `add.w`.
5. **Mach-O local labels**: leading-dot `L` labels lose the dot.
6. **`.func`** becomes `.thumb_func` in Apple/Thumb mode.
7. **Call targets**: branch and global directives are scanned to discover Thumb functions or armasm imports; care is taken not to misread instructions such as `bic` as conditional branches.
8. **PowerPC**: `@l`/`@ha` become `lo16`/`ha16`; `mftspr`/`mtspr` forms are rewritten with SPR numbers.
9. **Register aliases**: `.dn`/`.qn` and `.req` aliases are recorded, removed from output, and resolved on later lines, recursively for AArch64.
10. **AArch64 compatibility**: vector `mov` becomes `orr reg, reg, reg`; `movi` gets an explicit shift; missing `sxtl`/`uxtl` aliases become `sshll`/`ushll`; large add/sub immediates are converted to shifted form for clang and armasm; Xcode-5 fixes are gated by an environment variable; `ext` becomes `ext8`, condition codes are folded into `ccmp`/`csel`/`cinc`/`cset`, negative offsets become explicit `sub`, `prfum` is dropped, and `ldrb` with a negative offset becomes `ldurb`.
11. **armasm-specific**: `.set` symbols are substituted; `.func` becomes `PROC`; numeric local labels become unique generated labels; labels are not duplicated; non-label lines are indented; branches to local labels are rewritten and given `.w` width on ARM; alignment becomes byte-count `ALIGN`; `[r0, :128]` becomes `[r0@128]`; logical expressions with constants are folded; `movw`/`movt` pairs become a `mov32` pseudo-op; NEON type qualifiers are added where required; `THUMB`/`ARM` sections are emitted.
12. **Thumb forcing**: post-indexed loads become load plus add; `mov pc, lr` becomes `bx lr`; single-register push/pop forms become plain stores/loads; `muls` becomes `mul` plus compare; `and reg, sp, #imm` becomes `mov` plus `and`; large-shift addressing is split; `.arm` becomes `.thumb`.
13. **Directive commenting**: unsupported directives are commented out with the architecture's comment prefix or translated, depending on assembler type — `.type`, `.func`, `.endfunc`, `.ltorg`, `.size`, `.fpu`, `.arch`, `.object_arch`, `.note.GNU-stack`, `.syntax`. `.hword` becomes `.short`. Apple syntax renames `.global`→`.globl`, `.rodata`→`.const_data`, `.int`→`.long`, `.float`→`.single`. armasm renames data directives to `dcd`/`dcw`/`dcb`/`dcq`, sections to `AREA`, `.global` to `EXPORT`, and conditional branches from `b.eq` to `beq`.
14. **Unknown Mach-O sections** are fatal with a message asking for a proper section name.

### 5.7 Final emission

For non-armasm, the script appends `.text`, `.align 2`, any remaining literal pool entries and `.thumb_func` declarations for discovered call targets. For armasm it emits `IMPORT` directives for outstanding targets and a final `END`. Input and output streams are closed, with failure propagated on close. In armasm mode the temporary file is assembled by a second process unless debug mode is set, and the temporary file is removed at exit.

The `GASPP_DEBUG` environment variable sends output to standard output instead of the assembler. `GASPP_FIX_XCODE5` and `GASPP_ARMASM64_INVERT_SCALE` enable extra compatibility workarounds.

## 6. Dependencies

- **Imports**: Perl core only (`strict`), plus external `cpp`/compiler driver, assembler, `arch`, and standard utilities used by the shell around it.
- **Consumers**: the build system's assembly compilation rules for ARM, AArch64, PowerPC and Windows/armasm targets.
- **Related**: `tools/checkasm-arm.S.md`, `tools/checkasm-aarch64.S.md`, `tools/checkasm-loongarch.S.md` are assembly sources that may pass through this filter depending on the toolchain.

## 7. Constants and tables

| Constant | Meaning |
| --- | --- |
| Canonical architectures | `aarch64`, `arm`, `powerpc` with aliases `arm64`, `ppc`. |
| Comment prefixes | `//` AArch64, `@` ARM, `#` PowerPC, `;` armasm. |
| `MAX` macro arg count | Not fixed a priori; extra arguments require `:vararg`. |
| Literal expression | `.word`, or `.quad` for AArch64. |
| Environment flags | `GASPP_DEBUG`, `GASPP_FIX_XCODE5`, `GASPP_ARMASM64_INVERT_SCALE`. |
| Condition codes | The standard ARM condition mnemonics used to disambiguate branches. |
| SPR numbers | `ctr` = 9, `vrsave` = 256 for PowerPC. |

## 8. Performance notes

- The script runs at build time; its multi-pass parsing is irrelevant to encode performance.
- It streams preprocessed input line by line and buffers only macro/repetition bodies, so memory use scales with source complexity, not file size.
- Expression evaluation uses Perl's `eval`, which is fast but means symbol substitution must be careful to avoid evaluating unsafe content; the input is trusted build-time source.
- Extensibility tip: when a new assembly directive appears in a kernel, add a targeted transformation in `handle_serialized_line` rather than changing the source to toolchain-specific syntax, so all assembler targets keep working.

### 8.1 Relation to faster-kernel development

New SIMD kernels often use recent instruction aliases or large immediates. Before benchmarking:

1. Run the new assembly through this preprocessor for each target assembler type to confirm it translates cleanly — a kernel that only builds with GNU as is not portable across the project's supported toolchains.
2. Verify the generated assembly with the conformance harness (`tools/checkasm.c.md` plus the per-architecture wrappers) on each ISA tier.
3. Only then trust benchmark numbers; a translation that silently drops an instruction (for example `prfum`) changes performance characteristics.

References: `tools/checkasm-arm.S.md`, `tools/checkasm-aarch64.S.md`, `tools/checkasm.c.md`; GNU as and ARM assembler documentation for the source and target syntaxes.

## 9. References

- The source file itself, `tools/gas-preprocessor.pl`, attributed in-file to David Conrad and licensed GPLv2-or-later.
- GNU assembler documentation for `.macro`, `.rept`, `.irp`, `.irpc`, conditionals and relocations.
- Apple assembler / clang integrated assembler documentation noted in the source's usage text.
- ARM ARM and AAPCS for condition codes, Thumb encodings and NEON syntax; AArch64 ISA documentation for the listed aliases.
- Microsoft armasm documentation for `PROC`, `AREA`, `EXPORT`, `IMPORT`, `ALIGN` and data directives.

## 10. Verification checklist

- `-help` prints usage and exits successfully.
- An unknown architecture or assembler type is rejected with a fatal message.
- A `.c` input is compiled with `-S`; a `.s`/`.S` input is preprocessed with `-E`.
- Macro definitions with defaults, named arguments and `:vararg` expand correctly.
- Nested conditionals and `.elseif` behave as documented, including the "already taken" rule.
- `.rept`, `.irp` and `.irpc` unroll with correct substitutions.
- `ldr reg, =expr` is replaced by a literal-pool label and the pool is emitted at `.ltorg` and at end of file.
- `.previous` restores the second-most-recent section.
- `\\@` expands to a unique number per macro invocation.
- Thumb mode converts post-indexed loads, `muls` and wide `add` as described.
- armasm output contains `PROC`, `ENDP`, `AREA`, `EXPORT`, `IMPORT` and `END` where appropriate.
- Unsupported directives are commented out with the correct per-architecture comment prefix.
- `GASPP_DEBUG` prevents assembly and emits the transformed text on standard output.

## 11. Open questions

- UNKNOWN: exact behaviour for macro definitions nested more than one level deep beyond the documented "master macro" restriction.
- UNKNOWN: whether every listed Xcode/AArch64 workaround is still needed with current toolchains.
- UNKNOWN: how the build system selects `-as-type` for each configuration; the script does not auto-detect it.
- UNKNOWN: which projects other than x264, FFmpeg and VLC have been tested; the usage text does not enumerate them.
