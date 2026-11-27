# `common/cpu.h` - CPU capability interface

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\cpu.h` (mirrored at `common/cpu.h`). Role in one sentence: declares the public CPU feature-detection API, the x86 MMX state helpers and the name-to-flag table used by the `asm` parameter. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Hand-written SIMD kernels must be selected at run time because a single binary has to run on machines with different instruction sets. This header defines the small surface through which the rest of the encoder asks "what may I execute?" and "how many processors are available?". Keeping it tiny and non-templated allows it to be called from `x264_param_default` before any encoder context exists.

## 3. Public interface

- `X264_API uint32_t x264_cpu_detect(void)`: returns a bit mask of capabilities of the current machine; the value is a union of the `X264_CPU_*` flags defined in the public `x264.h`. The contract is "features safe to execute in this process right now", which includes OS state support for AVX/AVX-512, not merely CPUID bits.
- `X264_API int x264_cpu_num_processors(void)`: returns the number of logical processors available to the *current process* (affinity-aware where possible); returns 1 when threading is disabled or the query fails. Never returns 0 in practice.
- `void x264_cpu_emms(void)`: emits the `emms` instruction on x86 to mark MMX/SSE floating-point state as clean; a no-op elsewhere.
- `void x264_cpu_sfence(void)`: issues a store fence on architectures that require it; aliased as `x264_sfence`.
- `x264_emms()` macro: on x86 with inline assembly support, emits `emms` with memory and x87 stack clobbers so the compiler is less likely to reorder floating-point work across it; otherwise calls `x264_cpu_emms()` or expands to nothing. The header documents the residual theoretical risk that a compiler may place float instructions before the EMMS.
- `typedef struct { const char *name; uint32_t flags; } x264_cpu_name_t;` and `X264_API extern const x264_cpu_name_t x264_cpu_names[]`: a null-name/0-flags terminated table mapping user-facing feature names (case-insensitive in the parser) to flag masks.

## 4. State and data structures

No mutable state. `x264_cpu_names` is a static const array whose final entry is `{"", 0}`. Each entry's `flags` value is a *set* of bits: choosing a name such as `SSE4.2` implies all lower features, so the parser can OR it into a user-supplied mask. The table is also the loop terminator for the parameter parser: an unrecognised token lands on the empty entry with zero flags and is reported as an error.

## 5. Algorithm / behaviour

Detection is delegated to `cpu.c`. The calling contract is: call once at default-parameter creation (or when the user sets `asm=auto`), store the result in `param.cpu`, and never re-detect per frame. Manual `asm` strings are parsed by `x264_param_parse` against `x264_cpu_names`; a numeric value is used verbatim as a bit mask. The parser has one derived rule: SSSE3 without the "SSE2 is slow" flag implies "SSE2 is fast", so enabling SSSE3 by name upgrades the SSE2 dispatch choice.

The EMMS helper exists because MMX instructions leave the x87 tag register marked full; subsequent floating-point code can then produce NaNs or exceptions. Kernels that use only SSE do not need it, but legacy MMX paths and mixed scalar/SIMD code do. The store-fence helper is used where a producer thread writes data that a consumer reads without a mutex.

## 6. Dependencies

Relies on `osdep.h`/`base.h` for `X264_API`, `ARCH_*`, `HAVE_MMX` and `HAVE_X86_INLINE_ASM` configuration macros (in practice the header is reached through `base.h`). `x264_cpu_detect` is implemented per architecture in `cpu.c`; `x264_cpu_names` is defined there. Called by `base.c` (parameter defaults and parser), by the encoder at function-table initialisation, and by the CLI when reporting capabilities.

## 7. Constants and tables

The flag values are public ABI and live in `x264.h`. x86 bits: MMX 1<<0, MMX2/MMXEXT 1<<1, SSE 1<<2, SSE2 1<<3, LZCNT 1<<4, SSE3 1<<5, SSSE3 1<<6, SSE4/SSE4.1 1<<7, SSE4.2 1<<8, AVX 1<<9, XOP 1<<10, FMA4 1<<11, FMA3 1<<12, BMI1 1<<13, BMI2 1<<14, AVX2 1<<15, AVX512 1<<16, cacheline-32 1<<17, cacheline-64 1<<18, SSE2-is-slow 1<<19, SSE2-is-fast 1<<20, slow-shuffle 1<<21, stack-mod4 1<<22, slow-atom 1<<23, slow-pshufb 1<<24, slow-palignr 1<<25. Other architectures reuse low bits: Altivec/MSA/LSX = 1<<0; ARMv6/ARMv8 = 1<<0 with NEON 1<<1, fast-NEON-MRC 1<<2, SVE 1<<4, SVE2 1<<5; LASX = 1<<1.

Aliases in the name table: `MMXEXT` equals `MMX2`; `SSE4` equals `SSE4.1`; `SSE2Slow`, `SSE2Fast`, `Cache32`, `Cache64`, `SlowAtom`, `SlowPshufb`, `SlowPalignr`, `SlowShuffle`, `UnalignedStack` expose microarchitectural quirk flags.

## 8. Performance notes

- Detection is a one-time cold cost; never invoke it in a per-frame or per-macroblock path.
- **Dispatch hierarchy:** the flag mask is consumed once per encoder open to populate `h->pixf`, `h->mc`, `h->dctf`, `h->quantf`, `h->loopf` and `h->bsf` (see `common.h.md`); hot code then calls through those tables. Adding a feature test inside the hot loop would negate the design.
- **Cache layout:** the `CACHELINE_32/64` flags feed stride-alignment choices in `frame.c` and PREALLOC alignment, so a correct detection result can change memory layout and therefore speed; the quirk flags can select slower but safer kernels on Atom/Bobcat/Jaguar.
- **False sharing:** none in this header; the mask is read-only after parameter setup.
- Public references: Intel SDM Vol. 2A CPUID instruction description and Vol. 3A on XGETBV/OSXSAVE; AMD64 APM CPUID; ARM architecture hwcap documentation; Intel *Optimization Reference Manual* on dispatch and ISAs.

## 9. References

- Public `x264.h` `X264_CPU_*` definitions and `x264_cpu_detect`/`x264_cpu_num_processors` declarations.
- Intel 64 and IA-32 Architectures Software Developer's Manual, CPUID and XGETBV descriptions.
- AMD64 Architecture Programmer's Manual, Volume 3, CPUID.
- Linux `getauxval(3)` and `sched_getaffinity(2)` man pages; Microsoft `GetProcessAffinityMask` documentation.
- Hennessy & Patterson, *Computer Architecture*, on instruction-set dispatch.

## 10. Verification checklist

- On x86, compare `x264_cpu_detect()` with an independent CPUID-based probe for at least SSE2, SSSE3, AVX and AVX2; all bits reported by x264 must be truly executable (run a tiny kernel for each feature as an observable check).
- Verify AVX/AVX512 are not reported when the OS has not enabled the corresponding XMM/YMM/ZMM state, even if the CPUID feature bits are set (can be checked in a VM with XSAVE disabled).
- `x264_cpu_num_processors()` must equal the process affinity mask population count (or 1 when restricted to one CPU), verified with `taskset` on Linux or processor affinity on Windows.
- Passing each name from `x264_cpu_names` through `x264_param_parse(p,"asm",name)` must succeed and produce the documented flag mask; an unknown name must fail.
- `x264_emms()` must not change observable results of surrounding scalar floating-point computation (compile-and-run smoke test).
- A build with threading disabled must return 1 from `x264_cpu_num_processors()`.

## 11. Open questions

- UNKNOWN: exact behaviour of `x264_cpu_sfence()` on non-x86 architectures (the declaration exists, implementation is architecture-dependent and not in this header).
- UNKNOWN: whether the name table is guaranteed sorted or unique; the parser uses a linear scan, so duplicates would be silently accepted.
