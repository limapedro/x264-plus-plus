# common/x86/cpu-a.asm — behavioural specification

**Role:** provides the small assembly primitives through which the C runtime probes CPU capabilities and controls SIMD/FPU state: CPUID, XGETBV, EMMS and SFENCE, plus a 32-bit CPUID-presence test.
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/cpu-a.asm`
- Role: thin assembly wrappers for privileged/special instructions that cannot be expressed in portable C; called from `common/cpu.c` during `x264_cpu_detect()` and from the fence/emms macros.
- All symbols are emitted via `cglobal` and therefore use the private prefix and no CPU suffix (they are always available on x86).

---

## 2. Purpose

Feature detection must be reliable before any ISA-specific kernel is selected. The C detector needs to issue `CPUID` with arbitrary leaf numbers and collect all four register results, to read extended control register `XCR0` (to verify the OS has enabled AVX/AVX-512 state), and on 32-bit to first check whether `CPUID` exists at all. It also needs `EMMS` to return from MMX code to floating-point code and `SFENCE` to order non-temporal stores. This file supplies exactly those five primitives.

---

## 3. Public interface

All signatures follow the C declarations in `common/cpu.c`:

- `void x264_cpu_cpuid( uint32_t op, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx );`
  - `op` is the CPUID leaf; the four output pointers each receive the corresponding 32-bit register result. ECX is zeroed before the instruction (required for some leaves).
- `uint64_t x264_cpu_xgetbv( int xcr );`
  - Reads extended control register `xcr` (called with 0). Returns the 64-bit value: EDX in the high 32 bits, EAX in the low 32. On 32-bit, the C ABI returns only EAX (high half implicitly unavailable — UNKNOWN whether callers ever need it there).
- `void x264_cpu_emms( void );` — executes `emms`, clearing the MMX/FPU tag-word state.
- `void x264_cpu_sfence( void );` — executes `sfence`, ordering preceding store instructions.
- `int x264_cpu_cpuid_test( void );` — **x86-32 only**. Returns 0 if CPUID is unsupported, nonzero otherwise, by attempting to toggle the ID flag (bit 0x200000) in EFLAGS via pushfd/popfd.

---

## 4. State and data structures

- No persistent state. Each function is a leaf; registers used beyond the ABI allowance are saved on the stack.
- `cpu_cpuid` uses the `rN` virtual register naming from `x86inc`: `r0`=op, `r1..r4`=output pointers. It declares five argument registers and seven total registers (`cglobal cpu_cpuid, 5,7`).
- On x86-32, EBX is callee-saved and must be preserved; the function pushes it, plus the four output pointers in reverse order so they can be popped one at a time into a scratch register for storing.

---

## 5. Algorithm / behaviour

### 5.1 CPUID wrapper
1. Save EBX (and the pointer registers as needed by the ABI).
2. Move the leaf number `op` into EAX and zero ECX.
3. Execute `cpuid`, producing EAX/EBX/ECX/EDX.
4. For each of the four output pointers in order: pop the saved pointer into a scratch register and store the corresponding result dword to `[pointer]`.
5. Restore EBX and return.

The order matters: the pointers are pushed so the last pushed is the first popped, matching the eax→edx output order. Because the outputs are written after the single CPUID, all four values come from the same leaf invocation.

### 5.2 XGETBV wrapper
`ecx` is loaded from the first argument (`r0m` — the memory/register slot of arg 0, via `movifnidn`), `xgetbv` is executed, and the 64-bit result is assembled as `(EDX << 32) | EAX` on x86-64. On x86-32 the high half is not combined; the function simply returns EAX (the C prototype is still 64-bit, so the upper bits are UNKNOWN/undefined there — callers only test the low bits). A plain `ret` is used rather than `REP_RET` since there is no epilogue state to restore.

### 5.3 EMMS / SFENCE
Single-instruction leaves: `emms` then `ret`, and `sfence` then `ret`. `x264_sfence` is the C-visible macro mapped directly to this symbol (see `common/cpu.h`); `x264_emms` may be an inline-asm sequence instead when `HAVE_X86_INLINE_ASM` is set.

### 5.4 CPUID presence test (x86-32)
1. Push the current EFLAGS (`pushfd`) twice: once to preserve and once to modify.
2. Pop the copy into EAX, save it in EBX, XOR with `0x200000` (the ID flag), push it back and `popfd` to load the modified flags.
3. Push/pop EFLAGS again into EAX and XOR with the saved original: a nonzero result means the ID flag was toggled, i.e. CPUID exists.
4. Restore all saved GP registers and the original flags, return the flag difference in EAX.
The `xor eax, ebx` result is the return value; callers test it for zero.

---

## 6. Dependencies

- Includes `x86inc.asm` for `cglobal`, `DECLARE_REG` register mapping and `RET`.
- Called from `common/cpu.c: x264_cpu_detect()` (CPUID leaves 0, 1, 7, 0x80000000, 0x80000001, 0x80000006 and XGETBV(0)), and from the `x264_emms`/`x264_sfence` macros in `common/cpu.h`.
- The returned bit patterns are interpreted against `X264_CPU_*` flags in `x264.h` (see the `x86inc`/`mc-c` specs for the dispatch hierarchy).

---

## 7. Constants and tables

- CPUID leaf numbers (used by the C caller, not this file): 0 (max leaf + vendor), 1 (feature flags in ECX/EDX), 7 (AVX2/AVX-512 sub-features in EBX), 0x80000000/0x80000001 (extended), 0x80000006 (cache line size).
- XCR0 bits tested by the C caller: `0x6` (XMM+YMM state, required for AVX) and `0xE0` (opmask/ZMM state, required for AVX-512).
- CPUID bit masks of interest: EDX bit 0x00800000 (MMX), 0x02000000 (MMX2/SSE), 0x04000000 (SSE2); ECX bit 0x00000001 (SSE3), 0x00000200 (SSSE3), 0x00080000 (SSE4.1), 0x00100000 (SSE4.2), 0x08000000 (XSAVE), 0x10000000 (AVX), 0x00001000 (FMA3); leaf-7 EBX bits 0x8 (BMI1), 0x100 (BMI2), 0x20 (AVX2), 0xD0030000 (AVX-512 F/CD/BW/DQ/VL).
- EFLAGS ID flag mask `0x200000` used by the 32-bit test.

---

## 8. Performance notes

These functions run once at startup, not in the hot path. Correctness dominates; the only performance consideration is keeping the probe small and free of dependencies on previously detected features.

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- No vectorization applies to these primitives. However, their output gates all AVX/AVX-512 dispatch: the OS-support check via `XGETBV` is mandatory before any AVX code is executed (Intel SDM Vol. 3, "Detection of AVX/AVX-512 support"; x264 `common/cpu.c`).
- A known pitfall: checking CPUID.1:ECX[28] (AVX) without checking XCR0[1:2] causes illegal-instruction faults on OSes that do not enable YMM state — the reference correctly performs both checks.
- AVX-512 detection additionally needs leaf-7 EBX bits and XCR0[5:7]; a rebuild must preserve the conjunction (Intel SDM).
- `CPUID` serialises the pipeline; calling it repeatedly in the hot path would be a bottleneck, so caching the result (as the C layer does) is essential.

---

## 9. References

- Intel SDM Vol. 2: CPUID, XGETBV, EMMS, SFENCE instruction definitions.
- Intel SDM Vol. 3: OS support detection for XSAVE/AVX/AVX-512.
- AMD APM Vol. 3: CPUID and extended feature leaves.
- System V / Win64 ABI: callee-saved register rules (EBX, RBX, RBP, etc.).

---

## 10. Verification checklist

1. Call `x264_cpu_cpuid(0, ...)` and verify the returned vendor string matches the host and that EAX equals the maximum basic leaf.
2. Call leaf 1 and compare feature bits against an independent implementation (e.g. `/proc/cpuinfo` on Linux, `__cpuidex` on Windows).
3. Call `x264_cpu_xgetbv(0)` and verify the low 6 bits match the OS-enabled SIMD state; on a machine with AVX, `(xcr0&6)==6`.
4. On x86-32, verify `cpu_cpuid_test()` returns nonzero on a modern CPU.
5. Verify EMMS clears the FPU tag word by mixing MMX and floating-point computation; verify SFENCE orders a non-temporal store before a subsequent read.
6. Disassemble `cpu_cpuid` and confirm all four results are written from the same CPUID invocation and EBX is restored on all paths.

---

## 11. Open questions

- Exact upper-32-bit return behaviour of `cpu_xgetbv` on x86-32 is UNKNOWN (the assembly does not combine EDX there; the C prototype is 64-bit).
- Whether any caller relies on CPUID leaves beyond those listed (e.g. leaf 0x80000006 for cache-line size — used by `cpu.c` but not by this file) is covered by the C layer.
- The `cglobal cpu_xgetbv` declaration has no explicit argument/register counts in the source; the exact prologue defaults are therefore those of `cglobal` with no parameters (UNKNOWN if any implicit registers are assumed beyond `r0m`).
