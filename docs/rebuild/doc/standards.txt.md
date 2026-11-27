# `doc/standards.txt` — dirty-side behavioural specification

## 1. Header

- Source path: `doc/standards.txt`.
- Role: a compact statement of the language dialect, compiler assumptions and platform-specific guarantees the source tree relies on.
- Marker: this file is a **dirty-side behavioural specification** of those assumptions, paraphrased in our own words.

## 2. Purpose

The file exists to make explicit the gap between what ISO C guarantees and what the implementation actually assumes. A clean-room implementation written to a stricter or different dialect can compile successfully yet fail at runtime if it misses these implicit dependencies. The note therefore functions as a portability contract: it lists the exact language intersection, the compiler baseline and the non-C99 hardware facts that the code takes for granted.

## 3. Public interface

No declarations. The "interface" is a set of constraints on the build environment:

| Constraint | Statement |
| --- | --- |
| Source language | C, specifically the intersection of C99 and GCC 3.4-or-newer behaviour. |
| checkasm language | GCC dialect only; no compatibility effort is made for other compilers. |
| Signed integers | Two's complement representation is assumed. |
| Signed right shift | Arithmetic (sign-extending) behaviour is assumed. |
| `int` width | At least 32 bits. |
| x86 stack | 16-byte aligned on entry to the library and on each thread entry, and the compiler is expected to preserve that alignment afterwards. |
| MMX/FPU state | `emms` is called before floating-point operations and before returning from the library, not after each MMX operation. |

## 4. State and data structures

No data structures. The relevant state is machine state:

- **FPU/MMX tag word**: the note asserts that the code transitions out of MMX state at specific points (before float use, before library return) rather than after each MMX instruction. A compiler that inserts floating-point operations between MMX instructions violates this assumption.
- **Stack pointer alignment**: on x86 the ABI contract is established at library entry and thread entry; stack alignment after that is the compiler's responsibility.
- **Shift behaviour**: sign extension of negative right shifts is relied upon in arithmetic code; compilers with logical signed shifts would change results.

## 5. Algorithm / behaviour

The document is declarative rather than algorithmic. Its behavioural content can be restated as a set of invariants a conforming implementation must preserve:

1. **Dialect invariant.** Source files may use any construct that is valid both in C99 and in GCC ≥ 3.4. Constructs that are only defined by a later C standard, or only by non-GCC compilers, are outside the contract. The clean implementation should either stay inside this intersection or deliberately document a wider requirement.
2. **Checkasm exception.** The conformance tool is explicitly excluded from the portability contract: it is written for GCC and may use GCC-only constructs. This is why its ABI wrappers and inline assembly are written in a compiler-specific style.
3. **Integer representation invariant.** Signed overflow-adjacent code, bit tricks and sign-extension patterns assume two's complement and arithmetic right shift. An implementation targeting a one's-complement or sign-magnitude machine would need revision, but such machines are outside the stated scope.
4. **Minimum width invariant.** No code assumes `int` is exactly 32 bits; 32 bits is a lower bound. Code that needs exactly 32 bits must use the fixed-width compatibility types described in `extras/stdint.h.md`.
5. **x86 stack invariant.** The library entry point and each thread entry point align the stack to 16 bytes. Thereafter the compiler must not break alignment. This matters for aligned SIMD loads/stores that use alignment-requiring instructions.
6. **x86 FPU invariant.** The `emms` instruction is issued at controlled boundaries, not after every MMX operation. Therefore floating-point instructions must not be injected into MMX code regions by the compiler; a compiler that does so can corrupt the FPU state.

## 6. Dependencies

- The build system and configuration step, which select the compiler dialect and architecture macros used throughout the tree.
- The assembly sources and the `x86inc.asm` macro layer, which rely on the stack alignment guarantee.
- The inline-assembly macros in `common/x86/util.h` (or equivalent) that issue `emms`.
- The compatibility headers `extras/stdint.h.md` and `extras/inttypes.h.md`, which supply fixed-width types when the host C library lacks C99 support.
- `tools/checkasm.c.md` and the per-architecture checkasm assembly files, which are explicitly exempt from the portability rule.

## 7. Constants and tables

| Value | Meaning |
| --- | --- |
| C99 | Baseline language standard. |
| GCC 3.4 | Minimum compiler version for the common subset. |
| 32 bits | Minimum assumed width of `int`. |
| 16 bytes | Required x86 stack alignment. |
| `emms` | x86 instruction used to leave MMX state before float operations / library return. |

No numeric tolerance or rate constants appear in this file.

## 8. Performance notes

The assumptions in this note exist partly for performance reasons:

- Aligned stack and aligned buffers let SIMD kernels use aligned load/store instructions, which are faster than their unaligned counterparts on older x86 cores.
- Deferring `emms` to controlled boundaries avoids an expensive state transition after every MMX instruction; the code trades a rare, larger cost for a much lower steady-state cost.
- Assuming two's complement and arithmetic shift lets the implementation use single-instruction sign-extension and arithmetic-shift idioms instead of portable multi-instruction sequences.

### 8.1 Using checkasm-style verification when changing low-level code

When replacing a C routine with a SIMD version, the assumptions above become test obligations:

- Verify callee-saved register preservation with the ABI wrapper described in `tools/checkasm-a.asm.md`; this directly checks the compiler/assembly contract.
- Verify stack alignment behaviour with the page-alignment helper described there, especially for kernels that assume 16-byte alignment.
- Use the stack-clobber trick in `tools/checkasm.c.md` to catch code that wrongly assumes 32-bit arguments are zero-extended to 64 bits — a classic violation of the "int is at least 32 bits" rule on x86-64.
- Run the per-ISA flag sweep so each new feature tier is validated separately.

References: the source note itself; the GNU C language documentation for the C99/GCC intersection; the Intel 64 and IA-32 Architectures Software Developer's Manual for `emms` and stack alignment; the System V AMD64 ABI and Microsoft x64 ABI for register/stack conventions.

## 9. References

- The source note itself, `doc/standards.txt`.
- ISO/IEC 9899:1999 (C99) for the language baseline.
- GCC documentation for the compiler baseline and dialect extensions.
- Intel 64 and IA-32 Architectures Software Developer's Manual: MMX state and `emms`.
- System V AMD64 ABI and Microsoft x64 calling conventions for stack alignment and callee-saved registers.
- `doc/threads.txt.md` for the thread-entry stack alignment guarantee in practice.

## 10. Verification checklist

- The clean implementation compiles under a C99-conformant toolchain with no reliance on post-C99 features.
- No source file requires a compiler older than GCC 3.4 semantics unless it is explicitly checkasm-only.
- Signed right shifts of negative values behave arithmetically on the target.
- `int` is at least 32 bits; fixed-width types are used where exact width matters.
- On x86, library entry and thread entry align the stack to 16 bytes and that alignment is preserved.
- `emms` is issued before floating-point use and before returning to the caller.
- The checkasm tool builds with GCC (or an explicitly GCC-compatible compiler) and is not expected to build elsewhere.

## 11. Open questions

- UNKNOWN: whether the stated GCC 3.4 baseline still applies to the current reference revision, or whether it is historical.
- UNKNOWN: which specific compiler flags the note assumes when it says the compiler "must preserve alignment".
- UNKNOWN: whether any non-x86 architecture has analogous implicit state assumptions not listed here; the note only mentions x86.
