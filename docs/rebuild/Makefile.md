# Makefile — rebuild specification

## 1. Header

- Source path: `Makefile` (root of the reference tree).
- Role: the GNU Make build graph that turns the outputs of `configure` into the CLI executable, static/shared libraries, API example and assembly-test binaries.
- Marker: **dirty-side behavioural specification** — documents the observable build contract, not script text.

## 2. Purpose

The Makefile defines what "a build" means. It lists the portable C sources, the architecture-specific C and assembly sources, the CLI support sources, and the conditionally compiled modules; it encodes the bit-depth duplication scheme; and it defines targets for installation, cleaning and profile-guided optimization. Because `configure` has already resolved feature flags into `config.h` and `config.mak`, the Makefile's conditional logic is mostly a second-stage translation of those flags into file lists and compiler switches. A clean-room implementation must reproduce the same artifacts from the same inputs.

## 3. Public interface

### Make targets

- `all` / `default` — delegates to whatever `config.mak` appended as its `default:` target (CLI, shared library and/or static library). A raw `make` with no config file triggers `./configure` through the `config.mak` rule.
- `cli` (`x264.exe` on Windows, `x264` elsewhere) — builds the command-line encoder from `OBJCLI` plus `CLI_LIBX264`, with `LDFLAGSCLI` followed by `LDFLAGS`.
- `lib-static` — creates `libx264.a` (or `libx264.lib` with MSVC) by archiving `OBJS` and `OBJASM`, then runs `ranlib` when configured.
- `lib-shared` — creates `$(SONAME)` by linking `OBJS`, `OBJASM` and `OBJSO` with `SOFLAGS` and `LDFLAGS`.
- `example` — builds `example.exe` (or `example`) from `example.o` and `LIBX264`.
- `checkasm` / `checkasm8` / `checkasm10` — build the assembly conformance test binaries; enabled per bit depth according to `HAVE_BITDEPTH8`/`HAVE_BITDEPTH10`.
- `fprofiled` — profile-guided rebuild; requires `VIDS="file1 file2 ..."` and runs eight representative command profiles at `--threads 1` to generate training data, then rebuilds with the profile-use flags.
- `install` / `install-cli` / `install-lib-static` / `install-lib-shared` / `install-lib-dev` / `install-bashcompletion` — copy binaries, headers (`x264.h`, generated `x264_config.h`), `x264.pc` and the completion script into the configured directories.
- `uninstall` — remove those installed files.
- `clean` — remove objects, generated headers, dependency file, libraries, executables and profiling data.
- `distclean` — `clean` plus removal of `config.mak`, `x264_config.h`, `config.h`, `config.log`, `x264.pc`, `x264.def` and probe leftovers.
- `depend` / `.depend` — regenerate and include the dependency file (MSVC uses `tools/msvsdepend.sh`; other compilers use the compiler's `-MM`/`-MT` flags).
- `etags` / `TAGS` — generate editor tag files.

### Variables consumed from `config.mak`

`SRCPATH`, `SYS_ARCH`, `SYS`, `CC`, `CFLAGS`, `CFLAGSSO`, `CFLAGSCLI`, `COMPILER`, `DEPMM`, `DEPMT`, `LD`, `LDFLAGS`, `LDFLAGSCLI`, `LIBX264`, `CLI_LIBX264`, `AR`, `RANLIB`, `STRIP`, `INSTALL`, `AS`, `ASFLAGS`, `RC`, `RCFLAGS`, `EXE`, `HAVE_GETOPT_LONG`, `DEVNULL`, `PROF_*`, `HAVE_OPENCL`, `SONAME`, `SOFLAGS`, `IMPLIBNAME`, `BASHCOMPLETIONSDIR`, `prefix`/`bindir`/`libdir`/`includedir`.

`CFLAGSPROF` and `LDFLAGSPROF` are appended to the global compile/link flags and are the hook used by the PGO flow.

## 4. State and data structures

The Makefile maintains derived *lists* rather than runtime state:

- `SRCS` — unconditionally compiled library sources: OS abstraction, base utilities, CPU detection, tables and the public API shim.
- `SRCS_X` — sources compiled once per enabled bit depth: motion compensation, prediction, pixel ops, macroblock/frame/dct/cabac/common/rectangle/set/quant/deblock/vlc/mvpred/bitstream, plus encoder analyse/me/ratecontrol/set/macroblock/cabac/cavlc/encoder/lookahead. Threads add `common/threadpool.c` here.
- `SRCS_8` — additional 8-bit-only sources, populated by OpenCL (`common/opencl.c`, `encoder/slicetype-cl.c`).
- `SRCCLI` — CLI sources: `x264.c`, `autocomplete.c`, input/raw/y4m/timecode, output/raw/matroska/matroska_ebml/flv/flv_bytestream, and filter framework plus source/resize/fix_vfr_pts/select_every/crop. `input/thread.c` is added per bit depth when threading is enabled; bundled getopt is added when the host lacks `getopt_long`.
- `SRCSO` — shared-library-only sources: on Windows, `x264dll.c`.
- `SRCCHK_X` — `tools/checkasm.c`, compiled per bit depth for the checkasm targets.
- `OBJASM` — assembled objects, with architecture- and bit-depth-specific suffixes.

`CONFIG := $(shell cat config.h)` is the string from which all `HAVE_*` decisions are made: a feature is enabled when the literal substring `HAVE_X 1` appears.

## 5. Algorithm / behaviour

Source and object composition:

1. **vpath directives** let sources be found in `$(SRCPATH)` for out-of-tree builds.
2. **Optional modules** are added by testing `CONFIG` for `HAVE_AVS 1`, `HAVE_THREAD 1`, `HAVE_WIN32THREAD 1`, `HAVE_LAVF 1`, `HAVE_FFMS 1`, `HAVE_GPAC 1`, `HAVE_LSMASH 1`. Threading adds both a library threadpool and the CLI threaded-input wrapper; MP4 output selects exactly one of GPAC or L-SMASH source files.
3. **Assembler selection** only runs when `AS` is non-empty:
   - X86 (32-bit): NASM `dct-32.asm`, `pixel-32.asm`.
   - X86-64: NASM `dct-64.asm`, `trellis-64.asm`.
   - Both: the shared x86 assembly set (bitstream, constants, CABAC, DCT, deblock, MC, MC-aux, pixel, prediction, quant) plus C fallbacks `mc-c.c` and `predict-c.c`; `cpu-a.asm` is unconditional.
   - PPC: C intrinsics sources for DCT/deblock/MC/pixel/predict/quant.
   - ARM: NEON `.S` set plus C fallbacks and `cpu-a.S`.
   - AArch64: NEON `.S` set, optional SVE/SVE2 additions, `asm-offsets.c` plus C fallbacks.
   - MIPS: MSA C sources when `HAVE_MSA`; LoongARCH: LSX `.S` set plus C fallbacks when `HAVE_LSX`.
4. **Bit-depth duplication** — for each enabled depth, every source in `SRCS_X` (and `SRCS_8` for 8-bit) is compiled twice with explicit defines: 8-bit objects get `-DHIGH_BIT_DEPTH=0 -DBIT_DEPTH=8`; 10-bit objects get `-DHIGH_BIT_DEPTH=1 -DBIT_DEPTH=10`. Assembly objects get matching `-D` values; x86 assembly additionally uses a depth-private symbol prefix (`x264_8`/`x264_10`) so both versions can coexist in one archive. `.S` files receive the same depth defines but no private prefix. Post-compilation, local symbols may be stripped with `strip -x` (best-effort, prefixed with `-@` so failure is ignored).
5. **Per-target flag overlays** — all library objects receive `CFLAGSSO`; CLI objects receive `CFLAGSCLI`. All objects depend on `.depend`.
6. **Link recipes** — static archive via `$(AR)$@ ...`; shared object via `$(LD)$@ ... $(SOFLAGS) $(LDFLAGS)`; CLI via `$(LD)$@ $(OBJCLI) $(CLI_LIBX264) $(LDFLAGSCLI) $(LDFLAGS)`. `CLI_LIBX264` is normally the internal static archive but can point to a system library as configured.
7. **Windows resources** — a `.rc` file is compiled into the CLI object list; when a DLL is produced, a separate resource object with `-DDLL` is added to the shared-library objects.
8. **OpenCL generated header** — when `HAVE_OPENCL=yes`, `common/oclobj.h` is regenerated by concatenating the OpenCL header source and all `.cl` files through `tools/cltostr.sh`; it is added to `GENERATED` and its C sources join `SRCS_8`.
9. **Dependency generation** — `.depend` is deleted and rebuilt from every C source (per depth) using either `msvsdepend.sh` (MSVC) or the compiler's dependency flags; included at the end if present.
10. **PGO flow** — `fprofiled` starts from `clean`, builds a first-pass binary with `CFLAGSPROF=$(PROF_GEN_CC)` and `LDFLAGSPROF=$(PROF_GEN_LD)`, runs all eight option profiles on each supplied video with `--threads 1`, deletes the first-pass objects (or just the executable for MSVC, whose PGD timestamps break object changes), rebuilds with `PROF_USE_*`, then removes profile data. Without `VIDS`, it prints the required usage and does nothing.
11. **Clean boundaries** — `clean` preserves configuration; `distclean` removes it. Installation is decomposed so library installs include the development headers and pkg-config file.

Speed-relevant composition facts:

- Single-depth builds compile roughly half the `SRCS_X` object set and omit one assembly variant per file.
- Panels of representative PGO profiles cover rate-control modes, motion-estimation methods, B-frame settings, slicing and lossless coding; this breadth is what makes `fprofiled` effective.
- The extra `-DHIGH_BIT_DEPTH`/`-DBIT_DEPTH` defines are the mechanism by which one source tree yields optimized 8- and 10-bit code paths without runtime branching on depth.
- Assembly objects strip local symbols to reduce profiler noise, not for speed.

## 6. Dependencies

- Input: `config.mak` (generated by `configure`) is included unconditionally; missing it triggers `./configure`.
- Input: `config.h` is read as text to make `HAVE_*` decisions.
- Tools: C compiler, assembler, archiver, linker, resource compiler (Windows), `strip`, `etags`, `install`, shell utilities (`rm`, `cat`, `ln`, `mkdir`).
- Scripts: `version.sh` (indirectly, via `x264_config.h`), `tools/cltostr.sh` (OpenCL), `tools/msvsdepend.sh` (MSVC dependency generation), `tools/bash-autocomplete.sh` (install target).
- Outputs consumed by: the CLI executable links `CLI_LIBX264`; `checkasm` links `LIBX264`; external applications link the installed library using `x264.pc`.

## 7. Constants and tables

| Item | Value / meaning |
| --- | --- |
| `GENERATED` | List of generated files removed by `clean`; includes `common/oclobj.h` when OpenCL is enabled |
| Object suffixes | `%.o` (single compile), `%-8.o` (8-bit), `%-10.o` (10-bit), `%.dll.o` (DLL resource) |
| Depth defines | `HIGH_BIT_DEPTH=0/BIT_DEPTH=8`, `HIGH_BIT_DEPTH=1/BIT_DEPTH=10` |
| x86 private prefixes | `x264_8`, `x264_10` |
| PGO profiles | Eight profiles `OPT0`–`OPT7` spanning: low-quality/fast settings, mid CRF with slicing, CQM/noise-reduction/2-pass, high-quality UMH+all partitions, exhaustive/trellis/slices, short fast-motion runs, and lossless runs |
| PGO threads | `--threads 1` for all training runs, to keep profiles deterministic |
| EXE | `.exe` on Windows, empty elsewhere |
| Default output names | `x264`, `libx264.a`/`libx264.lib`, `libx264.so.<API>`/`.dylib`/`.dll`, `checkasm8`, `checkasm10`, `example` |

The eight PGO profiles are semantically: (0) very fast low-quality with SSIM; (1) mid quality with hex ME, no 8x8 DCT and slice caps; (2) CRF 26 with JVT matrices, denoising, PSNR and B-adapt trellis; (3) CRF 18 with UMH, all partitions, normal B-pyramid and no fast skip; (4) CRF 22 with ESA, psy-RD and four slices; (5) short 50-frame TESA run; (6) short lossless high-subme run; (7) short lossless low-complexity run.

## 8. Performance notes

- The Makefile itself is fast; the expensive part is the number of compiled objects. Use `--bit-depth` in `configure` to halve it when 10-bit is unnecessary.
- `make fprofiled VIDS="..."` is the main build-time speed investment for runtime speed. Use several diverse clips; the eight profiles exercise different encoder paths, so a single clip under-trains the compiler.
- Keep NASM/GAS enabled. The Makefile's asm selection is where every SIMD kernel enters the binary; a `--disable-asm` build can be several times slower.
- LTO is passed through `CFLAGS`/`LDFLAGS` from `configure`, not set here; combined with PGO it gives the best runtime at the cost of long link times.
- Incremental builds are supported via `.depend`; regenerate it after changing generated headers with `make depend`.
- Public references: x264 wiki on PGO builds, GCC/Clang documentation for profile-guided optimization, NASM documentation for object formats and symbol prefixes, and the project's `doc/threads.txt` for how threading-related source inclusion affects behaviour.

## 9. References

- Reference tree `configure` — generator of every variable consumed here.
- Reference tree `tools/cltostr.sh`, `tools/msvsdepend.sh`, `tools/bash-autocomplete.sh` — helper scripts invoked by specific targets.
- GNU Make manual for `vpath`, pattern rules, phony targets and `$(shell ...)`.
- x264 project build documentation for `fprofiled` and bit-depth/chroma selection.

## 10. Verification checklist

- After a default `configure`, run `make` and confirm the `x264` executable and `libx264.a` are produced.
- With `--enable-shared`, confirm `make` produces `$(SONAME)` and that the CLI links to the internal static archive (or system library as configured).
- With `--bit-depth=all`, confirm both `%-8.o` and `%-10.o` variants exist for `SRCS_X` files and that the archive contains both `x264_8`/`x264_10` prefixed asm symbols.
- With a single bit depth, confirm only one variant is compiled and no unneeded depth defines appear.
- Confirm `make checkasm` builds both `checkasm8` and `checkasm10` when both depths are enabled.
- Confirm `make example` produces a working example binary.
- Confirm `make clean` leaves `config.mak`/`config.h` intact and `make distclean` removes them; that a subsequent `make` re-runs `configure`.
- With `VIDS` set, confirm `make fprofiled` performs the two builds, runs all eight profiles per video, and removes profile artifacts.
- Confirm installation places `x264.h` and `x264_config.h` in `includedir`, `x264.pc` in `libdir/pkgconfig`, and the completion script in `BASHCOMPLETIONSDIR` when enabled.
- On Windows with MSVC, confirm `CC_O=-Fo$@`, `AR=lib.exe -nologo -out:`, `LD=link.exe -out:` and the DLL resource object are used.

## 11. Open questions

- The exact set of symbols stripped by `strip -x` is assembler/platform dependent and not observable from this file.
- Whether all `OPT*` PGO profiles remain valid for every source type (the help text notes YUV-with-resolution, y4m or Avisynth) depends on the configured demuxers.
- The interaction between `--system-libx264` and the `example`/`checkasm` link steps is defined by `CLI_LIBX264` but not exercised in a default build.
- The number and names of object files produced by NASM for `.asm` templates are generated by `x86inc.asm` macros and are only indirectly visible here.
