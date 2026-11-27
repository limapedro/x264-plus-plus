# configure — rebuild specification

## 1. Header

- Source path: `configure` (root of the reference tree), a POSIX/Bash shell script with `#!/bin/bash` interpreter line.
- Role: probes the build host, translates user options into generated build files, and emits the feature summary that drives source selection in the `Makefile`.
- Marker: **dirty-side behavioural specification** — describes the observable configuration contract, not the script's text.

## 2. Purpose

`configure` is the single source of truth for what gets built. It turns a portable source tree into a host-specific build by: selecting compiler and assembler conventions, detecting thread backends and optional libraries, deciding which bit depths and chroma formats to compile, deriving optimization flags, and writing `config.h`, `x264_config.h`, `config.mak`, `x264.pc` and `config.log`. The `Makefile` consumes those outputs; no source file is compiled until this script has run. Its main value to a reimplementation is the exact set of knobs and the exact macro names that gate conditional code.

## 3. Public interface

### Invocation

- `./configure [options]` — configure an in-tree or out-of-tree build; exits non-zero on fatal conditions and prints a summary to stdout.
- `./configure -h` or `./configure --help` — print the usage block and exit with status `1` (a deliberate quirk: help is an error exit).

### Option surface

Standard options: `--prefix`, `--exec-prefix`, `--bindir`, `--libdir`, `--includedir`, `--extra-asflags`, `--extra-cflags`, `--extra-ldflags`, `--extra-rcflags`.

Configuration options: `--disable-cli`, `--system-libx264`, `--enable-shared`, `--enable-static`, `--disable-bashcompletion`, `--enable-bashcompletion`, `--bashcompletionsdir=DIR`, `--disable-opencl`, `--disable-gpl`, `--disable-thread`, `--disable-win32thread`, `--disable-interlaced`, `--bit-depth=8|10|all`, `--chroma-format=400|420|422|444|all`.

Advanced options: `--disable-asm`, `--enable-lto`, `--enable-debug`, `--enable-gprof`, `--enable-strip`, `--enable-pic`.

Cross-compilation: `--host=HOST`, `--cross-prefix=PREFIX`, `--sysroot=SYSROOT`.

External libraries: `--disable-avs`, `--disable-swscale`, `--disable-lavf`, `--disable-ffms`, `--disable-gpac`, `--disable-lsmash`. An undocumented `--disable-vsx` also exists. Unknown options are reported as ignored rather than fatal.

Environment variables honoured: `CC`, `AS`, `AR`, `RANLIB`, `STRIP`, `STRINGS`, `INSTALL`, `PKGCONFIG`, `RC`, and the `CFLAGS`/`LDFLAGS`/`ASFLAGS`/`RCFLAGS` families that are appended rather than replaced. Cross tools default to the `--cross-prefix` plus the canonical tool name.

### Generated artifacts (observable outputs)

- `config.h` — internal preprocessor definitions: `HAVE_*` feature flags, `ARCH_*`, `SYS_*`, `STACK_ALIGNMENT`, `WORDS_BIGENDIAN` (big-endian hosts), `CHROMA_FORMAT` (when restricted), `PTW32_STATIC_LIB`, and `fseek`/`ftell` aliases chosen from the platform's 64-bit variants.
- `x264_config.h` — the *public* configuration header included by `x264.h`: `X264_GPL`, `X264_INTERLACED`, `X264_BIT_DEPTH` (`0` means all), `X264_CHROMA_FORMAT` (`0` means all, otherwise an `X264_CSP_I*` value) plus the four macros emitted by `version.sh` (`X264_VERSION`, `X264_POINTVER`, and conditionally `X264_REV`, `X264_REV_DIFF`).
- `config.mak` — make variables: paths, compiler/linker/assembler commands, `CFLAGS`/`CFLAGSSO`/`CFLAGSCLI`, `LDFLAGS`/`LDFLAGSCLI`, `ASFLAGS`, `RCFLAGS`, `LIBX264`, `CLI_LIBX264`, `SONAME`/`SOFLAGS`, `EXE`, `DEVNULL`, `HAVE_GETOPT_LONG`, `HAVE_OPENCL`, `PROF_*` flags, dependency-generation flags and the selected `default:`/`install:` targets.
- `x264.pc` — pkg-config metadata with Name/Description/Version/Libs/Libs.private/Cflags; Version is derived from `X264_POINTVER`.
- `config.log` — every check with its outcome, failed command lines and failed probe sources, followed by a human-readable summary block (platform, byte order, system, cli, libx264, shared/static, bashcompletion, asm, interlaced, avs, lavf, ffms, mp4, gpl, thread, opencl, filters, lto, debug, gprof, strip, PIC, bit depth, chroma format).

## 4. State and data structures

The script's "state" is a set of shell variables that are resolved in a strict order:

1. Defaults: CLI enabled, internal libx264, no shared/static library by default, bash completion auto, all external libraries auto, GPL on, threads auto, asm auto, interlaced on, LTO off, debug/gprof/strip/PIC off, bit depth all, chroma all, compiler style GNU, OpenCL on, VSX auto.
2. Option parsing overrides those defaults (with validation: bit depth must be 8/10/all; chroma format must be one of 400/420/422/444/all). It is fatal to disable CLI, shared and static simultaneously.
3. Tool selection and host normalization: `config.guess` supplies a triplet when `--host` is absent, `config.sub` normalizes it, and the triplet is split into CPU/vendor/OS. Temporary probe files (`conftest*`) are removed on exit via a trap.
4. Platform, architecture and compiler-style branching produce `SYS`, `ARCH`, assembler format/extension, `EXE`, `DEVNULL`, stack alignment and OS-specific link flags.
5. Feature detection fills `HAVE_*` macros. Every name in the declared `CONFIG_HAVE` list is explicitly defined to `0` if not detected, so source code can always test `#if HAVE_X`.
6. Final derived values (`x264_gpl`, `x264_interlaced`, `bit_depth`, `chroma_format`, `opencl`, `bashcompletion`, `filters`) are written to the generated files.

License override logic: with `--system-libx264`, the script reads an installed `x264_config.h` and refuses to continue if its GPL flag conflicts; it also overrides local bit depth, chroma format and interlaced settings to match the installed library.

## 5. Algorithm / behaviour

Detection and decision flow, in order:

1. **Help short-circuit** — print usage and exit `1`.
2. **Workspace validation** — delete stale generated files; compute a relative source path; refuse out-of-tree builds when the source path contains whitespace or when `config.h`/`x264_config.h` already exist in the source directory.
3. **Option parsing** — as described above; `--sysroot` appends to both compile and link flags; `--gprof` appends `-pg` to both.
4. **Compiler identification** — special handling for Windows Intel Compiler, MSVC `cl` and MinGW; GNU-style compilers are tested for C99 loop scoping and get `-std=gnu99 -D_GNU_SOURCE` or `-std=c99` with POSIX/BSD feature macros, otherwise configuration dies. Intel/CL get their own warning-flag translation tables.
5. **OS classification** — maps host OS to internal `SYS` names (BEOS, MACOSX, FREEBSD, NETBSD, OPENBSD, LINUX, HURD, CYGWIN, WINDOWS, SunOS, QNX, HAIKU, others fatal), sets `libm`, `DEVNULL`, resource compiler behavior and getopt availability. Solaris and QNX set `HAVE_GETOPT_LONG=0`, causing the bundled `extras/getopt.c` to be compiled.
6. **Architecture classification** — sets `ARCH` and assembler type. x86 uses NASM (`-f elf32`/`-f elf64`/`-f win32`/`-f win64`/macho variants); ARM/AArch64/MIPS/LoongARCH typically use the C compiler as assembler; PowerPC uses Altivec/VSX flags. On 32-bit x86 with auto asm, `-march=i686 -mfpmath=sse -msse -msse2` and `-m32` are added. On x86-64, `-m64` and on Windows additional ASLR/DEP flags. Stack alignment is probed and raised toward 64 bytes when the compiler supports `force_align_arg_pointer`, otherwise toward 16.
7. **Assembler feature detection** — x86 requires a NASM new enough for AVX-512 syntax (minimum 2.13) and defines `HAVE_MMX`/`HAVE_X86_INLINE_ASM`. ARM checks ARMv6/ARMv6T2/NEON inline assembly; AArch64 checks NEON and optionally SVE/SVE2; MIPS requires MSA; LoongARCH requires LSX; PowerPC checks Altivec/VSX. `--disable-asm` clears the assembler entirely.
8. **Endianness check** — compiles a probe object and inspects strings for the magic markers; defines `WORDS_BIGENDIAN` only for detected big-endian hosts, and dies if neither ordering can be confirmed.
9. **Thread backend** — Windows can use pthreads-win32 (with several library-name variants; static variants define `PTW32_STATIC_LIB`); otherwise BeOS, Win32 native, POSIX (linking `-lpthread` or libc), or none. Any enabled backend defines `HAVE_THREAD`.
10. **Small libc probes** — `log2f`, `strtok_r`, `clock_gettime` (with `-lrt` fallback), `mmap`, Linux transparent huge pages, `fseeko`/`fseeko64`/`_fseeki64` aliases, vector extensions, 64-bit stdio functions.
11. **External libraries** — swscale enables libavformat and FFmpeg Source; libavformat requires swscale; FFMS requires version 2.21.0 or newer; L-SMASH requires version 1.5+; GPAC requires 0.8.0+ and is only probed when L-SMASH is unavailable. MP4 output is selected as L-SMASH first, else GPAC. Avisynth is auto-enabled on Windows/Cygwin/Linux/macOS/BSD/Haiku. Filter list becomes `resize crop select_every` when swscale is present, otherwise `crop select_every`.
12. **Optimization flags** — unless `--enable-debug`: `-O3 -ffast-math`, `-fomit-frame-pointer`, optional `-flto` (with `-O3 -flto` on the link line), `-fno-tree-vectorize` when supported, `-fvisibility=hidden` for GNU compilers, and optional `-Wshadow`/`-Wno-maybe-uninitialized`. Debug builds use `-O1 -g` and `-DDEBUG` for the resource compiler. `--enable-pic` adds `-fPIC`/`-DPIC` and tries `-Wl,-Bsymbolic`; shared builds force PIC.
13. **Bit depth and chroma** — `all` defines both `HAVE_BITDEPTH8` and `HAVE_BITDEPTH10`; a single depth defines only that one and disables OpenCL for 10-bit. Restricted chroma defines `CHROMA_FORMAT CHROMA_<format>`.
14. **GPL/interlaced/OpenCL** — define `HAVE_GPL`, `HAVE_INTERLACED`, and `HAVE_OPENCL "(BIT_DEPTH==8)"` when applicable. OpenCL adds `-ldl` on Linux/macOS.
15. **Generated files** — write `config.h`, append `version.sh` output to `x264_config.h`, compose `config.mak`, add `default:`/`install:` targets for each requested artifact, add bash-completion install when enabled, emit `x264.pc`, write the summary, create output subdirectories and symlink the `Makefile` for out-of-tree builds.

Speed-relevant contracts to reproduce:

- Default non-debug optimization is `-O3 -ffast-math -fomit-frame-pointer -fno-tree-vectorize`; PGO support is exposed as `PROF_GEN_CC`/`PROF_GEN_LD` = `-fprofile-generate` and `PROF_USE_CC`/`PROF_USE_LD` = `-fprofile-use` for GNU, or `-GL`/`-LTCG:PGINSTRUMENT`/`-LTCG:PGOPTIMIZE` for MSVC, or `-Qprof-gen`/`-Qprof-use` for Intel.
- Link-time optimization is opt-in (`--enable-lto`) and auto-detected for GNU compilers.
- Assembly is on by default; disabling it is the largest single performance regression.
- Bit depth `all` compiles every architecture-specific routine twice (see `Makefile.md`), so a single-depth build is roughly half the compile time and binary size.
- The compiler/architecture flags are appended before user `--extra-cflags`, so user flags can override defaults.

## 6. Dependencies

- Helper scripts in the source tree: `config.guess`, `config.sub`, `version.sh`, `tools/cltostr.sh` (referenced at build time by the Makefile for OpenCL), `tools/bash-autocomplete.sh` (install target).
- External tools: a C compiler, optional assembler (NASM/GAS), optional resource compiler, `pkg-config`, `git` (used by `version.sh`), `ar`/`ranlib`, `strip`, `strings`, `install`, `make`.
- Optional libraries discovered through pkg-config: libswscale/libavutil, libavformat/libavcodec/libavutil, ffms2, liblsmash, gpac, bash-completion.
- Consumed by: `Makefile` includes `config.mak` and reads `config.h` through `CONFIG := $(shell cat config.h)`; all C sources include the generated `x264_config.h` via `x264.h` or `common/base.h`.

## 7. Constants and tables

| Item | Value / rule |
| --- | --- |
| Default prefix | `/usr/local`; `bindir`, `libdir`, `includedir` derived from prefix/exec-prefix |
| Default library mode | static archive `libx264.a`; shared name `libx264.so.<X264_BUILD>` (or `.dylib`/`.dll` per OS) |
| `X264_BUILD` | Read from `x264.h`; used in SONAME and as the encoded-API version |
| `CONFIG_HAVE` | Full list of feature macros forced to 0 when undetected: MALLOC_H, ALTIVEC, ALTIVEC_H, MMX, ARMV6, ARMV6T2, NEON, AARCH64, BEOSTHREAD, POSIXTHREAD, WIN32THREAD, THREAD, LOG2F, SWSCALE, LAVF, FFMS, GPAC, AVS, GPL, VECTOREXT, INTERLACED, CPU_COUNT, OPENCL, THP, LSMASH, X86_INLINE_ASM, AS_FUNC, INTEL_DISPATCHER, MSA, LSX, MMAP, WINRT, VSX, ARM_INLINE_ASM, STRTOK_R, CLOCK_GETTIME, BITDEPTH8, BITDEPTH10, SVE, SVE2 |
| Minimum NASM | 2.13 (AVX-512 instruction probe) |
| FFMS minimum | 2.21.0.0 |
| L-SMASH minimum | 1.5 (rev.895+) |
| GPAC minimum | 0.8.0 |
| PGO command sets | Eight `OPT0`–`OPT7` profiles consumed by `make fprofiled` (documented in `Makefile.md`) |

## 8. Performance notes

This section concerns the performance of the *produced build*, not of the script:

- Always keep assembly enabled; every architecture-specific DCT, MC, pixel, CABAC and SAD kernel depends on it.
- Use `--enable-lto` for a modest whole-program gain, accepting longer link times and larger debug-info requirements.
- `make fprofiled` with representative clips is the strongest speed lever after preset choice; the generated `PROF_*` flags feed the Makefile's profile-guided flow.
- Restricting `--bit-depth` to a single value halves the compiled kernel set and improves instruction-cache locality of the resulting binary; build both only when 10-bit output is needed.
- `--disable-interlaced`, `--disable-gpl` and `--disable-opencl` reduce functionality but can simplify generated code; they are not general speed wins.
- Avoid `--enable-debug` for production: it drops from `-O3 -ffast-math` to `-O1 -g`.
- Public references: the x264 build documentation and the project's wiki pages on assembly, LTO and PGO builds; GCC documentation for `-O3`, `-flto`, `-fprofile-generate/-fprofile-use`, `-fomit-frame-pointer` and `-fno-tree-vectorize`.

## 9. References

- Reference tree `Makefile` — consumer of all generated variables and `CONFIG` string.
- Reference tree `version.sh` — appends public version macros to `x264_config.h`.
- Reference tree `x264.h` — `X264_BUILD`, read back for SONAME generation.
- x264 project README/build instructions and wiki for option semantics.
- GNU Autoconf/Qt-style `config.guess`/`config.sub` documentation for host triplet normalization.
- FFmpeg/L-SMASH/GPAC pkg-config metadata conventions.

## 10. Verification checklist

- Run `./configure --help` and confirm it lists exactly the documented option groups and exits `1`.
- Run a default configure on a POSIX host and confirm `config.h` contains `HAVE_THREAD`, `HAVE_POSIXTHREAD`, `HAVE_BITDEPTH8`, `HAVE_BITDEPTH10`, `HAVE_MMX` on x86, and `HAVE_* 0` for undetected features.
- Confirm `x264_config.h` contains `X264_BIT_DEPTH 0` for `all`, `X264_GPL 1`, `X264_INTERLACED 1` by default, and four version macros including `X264_POINTVER`.
- With `--bit-depth=10`, confirm only `HAVE_BITDEPTH10`, `X264_BIT_DEPTH 10`, and that OpenCL is disabled.
- With `--chroma-format=420`, confirm `CHROMA_FORMAT CHROMA_420` and `X264_CHROMA_FORMAT X264_CSP_I420`.
- With `--disable-asm`, confirm `AS` is empty in `config.mak` and no `HAVE_MMX`/`HAVE_NEON` is defined.
- With `--enable-shared`, confirm `SONAME`, `SOFLAGS`, `default: lib-shared` and `CFLAGSSO` containing `-DX264_API_EXPORTS`.
- With `--system-libx264`, confirm a GPL mismatch aborts, and that bit depth/chroma are overridden from the installed header.
- With `--disable-cli`, confirm demuxers/muxers/swscale are forced off in the summary.
- Confirm `config.log` records each probe result and that the final summary block has one line per feature.
- Confirm out-of-tree builds work when the source path has no spaces and no stale config headers exist in the source directory, and that they fail loudly otherwise.
- Confirm `x264.pc` Version equals the `X264_POINTVER` value stripped of the revision suffix.

## 11. Open questions

- Some `HAVE_*` macros are set through compiler-specific branches not exercised on the reference host (WinRT, Intel dispatcher, Solaris XPG objects); their exact interaction is described from probe structure and is **UNKNOWN** in edge cases.
- The precise effect of `--extra-cflags` on warning suppression tables depends on the compiler; for non-GNU/CL/ICC compilers it is **UNKNOWN**.
- `--disable-vsx` is accepted but not documented in the help output; its interaction with PowerPC Altivec builds is partially inferred.
- Version strings from `version.sh` depend on the local git repository state of the reference tree and are not reproducible from the source alone.
