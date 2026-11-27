# `common/cpu.c` - CPU feature and processor-count detection

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\cpu.c` (mirrored at `common/cpu.c`). Role in one sentence: detects, per architecture, which SIMD instruction sets and microarchitectural quirks are usable in the current process, and reports how many logical processors the process may use. Marker: **dirty-side behavioural specification**.

## 2. Purpose

A single binary must run correctly and quickly across many CPUs. This file turns CPUID (x86), sysctl/sigill probing (PowerPC, ARM), auxiliary-vector queries (AArch64, LOONGARCH) and OS affinity APIs into a single 32-bit flag mask. It additionally tags known-slow instruction paths with quirk flags so the function-table dispatcher can prefer an alternative implementation even when the instruction is technically available.

## 3. Public interface

- `uint32_t x264_cpu_detect(void)`: see `cpu.h.md`. Architecture-dependent implementation, one definition per supported architecture selected at build time.
- `int x264_cpu_num_processors(void)`: affinity-aware processor count; see section 5 for the platform matrix.
- `const x264_cpu_name_t x264_cpu_names[]`: ordered, zero-terminated name/flags table (content detailed in `cpu.h.md` section 7).
- Internal helpers on x86: `int x264_cpu_cpuid_test(void)` (only on 32-bit x86), `void x264_cpu_cpuid(uint32_t op, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)` and `uint64_t x264_cpu_xgetbv(int xcr)`; on ARM: `x264_cpu_neon_test()` and `x264_cpu_fast_neon_mrc_test()`. These are private to the architecture-specific code.

## 4. State and data structures

Stateless with respect to the caller; each call re-detects from scratch. On PowerPC/ARM (when NEON is not compiled in) a static `sigjmp_buf` and a `volatile sig_atomic_t canjump` flag implement the illegal-instruction probe: the SIGILL handler restores the previous handler and either re-raises the signal (when the jump is not armed) or longjmps back to the detection site. AArch64 Linux uses a local `detect_flags()` helper reading `AT_HWCAP` and `AT_HWCAP2`. The name table is a static const array.

## 5. Algorithm / behaviour

**x86 / x86-64.** On 32-bit builds, first test whether CPUID is supported; if not, return 0. Query leaf 0: the returned maximum basic leaf is zero means "no CPUID", so return 0. Vendor string is assembled from the EBX/EDX/ECX registers of leaf 0. Query leaf 1:
- EDX bit 23 (MMX) is mandatory; without it detection stops and returns 0.
- EDX bit 25 sets MMX2 and SSE; bit 26 sets SSE2.
- ECX bit 0 sets SSE3; bit 9 sets SSSE3 *and* the "SSE2 is fast" hint; bit 19 sets SSE4.1; bit 20 sets SSE4.2.
- If ECX bit 27 (XSAVE/OSXSAVE) is set, execute XGETBV(0). Only when bits 1 and 2 (XMM and YMM state) are both enabled may AVX (ECX bit 28) and FMA3 (ECX bit 12) be reported. This models the OS-support requirement: CPUID alone is insufficient.
- With XMM/YMM enabled and maximum basic leaf at least 7, query leaf 7: EBX bit 3 -> BMI1, bit 8 -> BMI2, bit 5 -> AVX2. AVX-512 additionally requires XCR0 bits 5,6,7 (opmask, ZMM_hi256, Hi16_ZMM) plus EBX bits 18,20,21,22 and 28 (the AVX-512 subset set), then sets the AVX512 flag.

Extended leaf 0x80000000 gives the maximum extended leaf. If at least 0x80000001, query it:
- ECX bit 5 -> LZCNT; bit 6 (AMD SSE4a) -> "SSE2 fast", but family 0x14 (Bobcat) instead clears "fast", sets "SSE2 slow" and "palignr slow", and family 0x16 (Jaguar) sets "pshufb slow".
- With AVX already set, ECX bit 11 -> XOP and bit 16 -> FMA4.
- On AuthenticAMD, EDX bit 22 additionally grants MMX2; any SSE2-capable AMD CPU with no "fast" hint gets "slow".
- On GenuineIntel, recompute leaf 1 to derive family (`((eax>>8)&0xf)+((eax>>20)&0xff)`) and model (`((eax>>4)&0xf)+((eax>>12)&0xf0)`). Model 28 (Atom) sets "slow atom" and "pshufb slow"; SSSE3-without-SSE4 and model < 23 (early Conroe/Penryn/Nehalem without SSE4) sets "slow shuffle".
- On Intel or Cyrix without SSE4.2, cache-line size is inferred from three possible sources in priority order: leaf 1 EBX bits 8-15 times 8 (CLFLUSH line size), extended leaf 0x80000006 ECX low byte, and leaf 2 cache/TLB descriptors matched against a 32-byte and a 64-byte descriptor-ID list. A 32-byte result sets CACHELINE_32, a 64-byte result sets CACHELINE_64, otherwise a warning is logged.
- If compile-time stack alignment is below 16 bytes, set STACK_MOD4.

**PowerPC (Altivec).** On BSD-like systems, query sysctl (`hw.altivec`, `machdep.altivec`, or the Mach-O vector-unit selector) and set ALTIVEC when the value is nonzero. On Linux, install a SIGILL handler, arm the jump buffer, attempt to enable the vector unit and execute a vector instruction, then restore the handler; success sets ALTIVEC. Builds configured with no FPRs return 0. Other platforms return 0.

**ARM (ARMv6).** Start with ARMV6 set. When NEON is not compiled in, install the SIGILL handler, jump-arm, call the assembly NEON probe; on SIGILL the handler returns 0 with only ARMV6 set, otherwise NEON is set. Unless building for Mach-O, call the fast-MRC probe (Cortex-A9 cycle-counter access) and set FAST_NEON_MRC when it succeeds. The comment explicitly warns of a race if another program reconfigures the performance counters.

**AArch64.** Start with ARMV8, add NEON if compiled in, add SVE/SVE2 if the compiler enabled the corresponding features, then on Linux OR in the hwcap-derived bits (SVE bit 22 of AT_HWCAP, SVE2 bit 1 of AT_HWCAP2).

**MIPS / LOONGARCH / fallback.** MSA builds return MSA unconditionally. LOONGARCH reads `AT_HWCAP` bits 4 (LSX) and 5 (LASX). Any other configuration returns 0.

**Processor count.** If threading is compiled out, return 1. Windows delegates to `x264_pthread_num_processors_np` (Win32 affinity-aware) or to pthread-win32. Cygwin/Solaris/OpenBSD use `sysconf(_SC_NPROCESSORS_ONLN)`. Linux uses `sched_getaffinity(0, ...)`, failing to 1; Android uses `_SC_NPROCESSORS_CONF` because the NDK lacks the affinity call. BeOS queries `system_info.cpu_count`; macOS/FreeBSD use `sysctlbyname("hw.ncpu")`, falling back to 1. Unknown platforms return 1.

## 6. Dependencies

Includes `base.h` (which supplies `cpu.h`, `osdep.h` and the log functions). Platform includes are conditional: `unistd.h`, `sched.h`, `kernel/OS.h`, `sys/sysctl.h`, `machine/cpu.h`, `signal.h`, `setjmp.h`, `sys/auxv.h`. It calls `x264_log_internal` to report an indeterminate cache line, and is called by `base.c` and by encoder function-table initialisation.

## 7. Constants and tables

CPUID bit masks used: 0x00800000 (MMX), 0x02000000 (SSE/MMX2), 0x04000000 (SSE2), ECX 0x1/0x200/0x80000/0x100000 (SSE3/SSSE3/SSE4.1/SSE4.2), ECX 0x08000000 (XSAVE), 0x10000000 (AVX), 0x00001000 (FMA3), XCR0 0x6 and 0xE0, leaf 7 EBX bits 0x8/0x100/0x20 (BMI1/BMI2/AVX2), 0xD0030000 (AVX-512 subset), extended ECX 0x20/0x40/0x800/0x10000, AMD family 0x14/0x16, Intel model 28 and threshold 23. Cache descriptor byte lists are internal constant strings of descriptor IDs for 32- and 64-byte lines. AArch64 hwcap bits: (1<<22) for SVE, (1<<1) for SVE2; LOONGARCH (1<<4)/(1<<5).

## 8. Performance notes

- **Dispatch hierarchy:** detection output is consumed once to build per-encoder function tables; the branchy CPUID sequence is never repeated in the hot path. Cache-line and quirk flags influence *which* kernel is installed, so a false positive on "slow pshufb" can cost measurable speed and a false negative can crash.
- **Cache layout:** CACHELINE_32/64 feed `frame.c` stride alignment; using the wrong one can cause unaligned SIMD loads that split cache lines. Prefer the reference priority order (CLFLUSH size, extended leaf, leaf-2 descriptors) when reimplementing.
- **False sharing:** detection uses only stack locals and read-only statics; no shared counters.
- **Allocation strategy:** no allocations except platform internals; keep it that way so this can be called before the allocator is fully initialised.
- Public references: Intel SDM Vol. 2 CPUID/XGETBV, AMD APM CPUID, Linux `getauxval(3)`, ARM ARM hwcap definitions, Intel *Optimization Reference Manual* section on CPU dispatch.

## 9. References

- Intel 64 and IA-32 Architectures Software Developer's Manual Vol. 2A (CPUID), Vol. 3A (XSAVE/XCR0).
- AMD64 Architecture Programmer's Manual Vol. 3 (CPUID, family/model encoding).
- Linux man pages `getauxval(3)`, `sched_getaffinity(2)`, `sysconf(3)`; Microsoft `GetProcessAffinityMask` and `GetNativeSystemInfo` documentation.
- ARM Architecture Reference Manual and Linux `asm/hwcap.h`.
- Public `x264.h` flag definitions; x264 `doc/threads.txt` for why processor count matters.

## 10. Verification checklist

- On a machine with AVX but an OS that does not enable YMM state (or a VM configured accordingly), verify no AVX-family flag is reported.
- Compare reported flags against `lscpu`/`cpuid` output for SSE2, SSSE3, SSE4.1, SSE4.2, AVX, AVX2, BMI1/2, LZCNT; no reported feature may be absent.
- On a multi-core Linux host, restrict affinity to two CPUs and verify `x264_cpu_num_processors()` returns 2; restore affinity and verify it returns the full count.
- Case-insensitively pass each name in `x264_cpu_names` to `x264_param_parse` with `asm=` and verify the resulting mask equals the table's flags (plus the documented SSSE3 implication).
- Verify that on a 32-bit build without CPUID support the detector returns 0 without executing an illegal instruction (emulate via a stub `cpuid_test` in a harness).
- Verify that a cache-line size that cannot be determined emits exactly one warning and leaves both cacheline flags clear.

## 11. Open questions

- UNKNOWN: whether the "optimal" huge-page or cache-line heuristics are calibrated for CPUs released after the reference source version; the detector has no version guard.
- UNKNOWN: behaviour of the ARM fast-MRC probe when performance counters are unavailable; the code only comments on a possible race, not an error code.
- UNKNOWN: the exact AVX-512 subset required by the hand-written kernels versus the four-bit mask checked here; only the mask is visible in this file.
