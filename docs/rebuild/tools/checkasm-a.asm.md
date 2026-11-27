# `tools/checkasm-a.asm` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/checkasm-a.asm`.
- Role: the x86/x86-64 assembly support layer for the conformance tool: a callee-saved-register checker, a stack-clobber helper, a page-aligned stack trampoline and vector-unit warmup stubs.
- Marker: this file is a **dirty-side behavioural specification**; the mechanisms are described in our own words, with names and constants as facts.

## 2. Purpose

On x86 the calling convention defines which registers a callee must preserve, but a buggy assembly kernel may violate that rule only occasionally — for example only when a particular code path is taken — so a normal test run may appear to pass. This file provides a wrapper that calls a kernel with deliberately poisoned callee-saved registers and vector registers, then checks them afterwards. It also supplies helpers used by the benchmark side of the tool: stack junking to expose 32-bit zero-extension assumptions, a controlled stack boundary for alignment tests, and warmup stubs to keep wide vector units powered on.

## 3. Public interface

All symbols use the project's `x264_` prefix through the macro layer unless noted.

| Symbol | Signature / role |
| --- | --- |
| `x264_checkasm_stack_clobber` | `void (uint64_t clobber, ...)`. Fills the area below the stack pointer with the supplied 64-bit pattern. |
| `x264_checkasm_call` | `intptr_t (intptr_t (*func)(), int *ok, ...)`. Calls `func` with all arguments forced onto the stack, verifies register preservation, writes zero through `ok` on failure, returns the function's value. |
| `x264_stack_pagealign` | `int (int (*func)(), int align)`. Calls `func` with the stack aligned to a 4 KiB boundary minus the requested offset. |
| `x264_checkasm_warmup_avx` | `void ()`. Trivial AVX-register operation. |
| `x264_checkasm_warmup_avx512` | `void ()`. Trivial AVX-512-register operation. |

The file also declares `puts` as an external naked symbol for printing the failure message. `max_args` is defined as 15, matching the maximum argument count of any x264 assembly function.

## 4. State and data structures

- **Read-only data**: a NUL-terminated failure message and, on x86-64, two sets of random-looking 64-bit constants. One set is used for integer registers (`n7`…`n14`), one for XMM registers (`x6`…`x15`). The values are chosen so accidental equality is unlikely.
- On 32-bit x86 only four 32-bit poison values named `n3`…`n6` are needed, defined as immediate constants.
- `free_regs` is 7 on Win64 and 9 on Unix64; it determines how many integer registers can be poisoned without interfering with the call.
- The benchmark stack frame reserves `(max_args+1)*8` bytes rounded up to the stack-alignment requirement, with an extra slot holding the `ok` pointer.
- All data lives in the read-only section; there is no mutable global state.

## 5. Algorithm / behaviour

### 5.1 Stack clobber

1. Reserve `(max_args+6)*8` bytes below the stack pointer.
2. Starting from the highest slot and stepping down by 8 bytes, write the supplied 64-bit pattern into every slot.
3. Restore the stack pointer and return.

Because the scratch area lies below the stack pointer, it is not live data, but it is exactly where a caller may have placed spill slots or where a 64-bit argument slot's upper half may reside after the caller wrote only 32 bits. Filling it with a repeated pattern makes wrong zero-extension assumptions observable.

### 5.2 x86-64 ABI checker

1. Establish a frame with 15 integer registers and 16 vector registers available to the macro layer, plus the reserved argument area.
2. Save the function pointer and the `ok` pointer; the latter is stored at the top of the argument area.
3. Copy all incoming arguments from the caller's stack into the newly created argument area. On the Unix ABI the first six integer arguments are re-read from their register-save area and the remaining nine from the caller stack; on the Windows ABI the first four are re-read from shadow space and the remaining eleven from the caller stack. The helper deliberately inserts four dummy zero arguments before the real ones, so every real argument resides on the stack with junk in the upper half of 32-bit slots.
4. On Windows, load XMM6 through XMM15 with the random vector constants, because those registers are callee-saved there.
5. Load the callee-saved integer registers that are not in use with distinct random constants.
6. Call the target function indirectly.
7. Compare each poisoned integer register with its original constant using exclusive-or and accumulation; on Windows also compare each poisoned XMM register, pack the results into a 16-bit mask and fold it into the accumulator.
8. If the accumulator is zero, return the function result unchanged.
9. Otherwise save the return registers, print the failure message via `puts`, write zero through the saved `ok` pointer, restore the return registers and return.

The function result is preserved across the failure path so the caller can still print a numeric mismatch if desired.

### 5.3 32-bit x86 ABI checker

The 32-bit variant has a smaller register set. It stores the `ok` pointer at the end of its argument area, copies all `max_args` arguments from the caller stack into fresh stack slots, loads four callee-saved registers with poison values, calls the target, folds the four comparisons, and on failure prints the message, clears `ok`, restores `eax`/`edx`, and returns with a `rep ret` sequence (a workaround for old AMD branch-predictor behaviour).

### 5.4 Page-aligned stack trampoline

1. Sign-extend the requested alignment offset to pointer width.
2. Save the frame pointer and set up a new frame.
3. On Win64 reserve 32 bytes of shadow space.
4. Clear the low 12 bits of the stack pointer, so it is aligned to a 4 KiB page boundary.
5. Subtract the requested offset.
6. Call the target function.
7. Restore the frame and return.

This lets the conformance tool run a whole test sweep at a deliberately unusual stack alignment, catching kernels that assume the stack is 16-byte aligned at all times.

### 5.5 Warmup stubs

A macro emits a function that performs a single vector exclusive-or of a register with itself and returns. It is instantiated twice: once after selecting 256-bit AVX state and once after selecting 512-bit AVX-512 state. The tool calls the appropriate stub before benchmark samples to keep upper vector registers powered on and measurements stable.

## 6. Dependencies

- **Includes**: `x86inc.asm`, which provides section macros, `cglobal`, `INIT_XMM/YMM/ZMM`, register naming and ABI helpers.
- **Declares**: `puts` via `cextern_naked`.
- **Called by**: `tools/checkasm.c.md` under `HAVE_MMX`; the C file declares the corresponding prototypes and selects warmup based on detected CPU flags.
- **Related**: `doc/standards.txt.md` documents the stack-alignment and `emms` assumptions this file helps validate.

## 7. Constants and tables

| Constant | Value | Purpose |
| --- | --- | --- |
| `max_args` | 15 | Maximum arguments of any x264 assembly function. |
| Clobber reservation | `(max_args+6)*8` bytes on x86-64 | Region below the stack pointer filled with junk. |
| `free_regs` | 7 (Win64), 9 (Unix64) | Number of non-poisoned integer registers. |
| Poisoned integer registers | Up to `r14`…`r9` | Chosen from registers not needed by the wrapper. |
| Poisoned vector registers | `xmm6`…`xmm15` on Win64 | Callee-saved XMM registers in the Windows ABI. |
| Page mask | `~0xfff` | Aligns the stack to 4 KiB in the trampoline. |
| Shadow space | 32 bytes on Win64 | Required by the Windows x64 ABI before a call. |
| Warmup instruction | Vector `xor` with self | Cheap way to touch a wide register. |

## 8. Performance notes

- The wrapper is used only during conformance testing. It adds stack copying, register poisoning and comparison overhead, so its timing is not representative of production calls. Benchmark samples call kernels directly.
- The poison constants are read from read-only memory once per call; the resulting cache traffic is irrelevant for test correctness.
- `rep ret` in the 32-bit path is a historical microarchitectural workaround, not a correctness requirement.
- Extending the wrapper for a new callee-saved register requires updating both the load and compare loops and the `free_regs` count.

### 8.1 Using checkasm-style verification to accelerate faster kernels

For x86 kernel development this file is the mechanism that makes fast experimentation safe:

- After writing a new SSE/AVX/AVX-512 kernel, run the tool on a machine with the corresponding tier; the trampoline will catch register-preservation mistakes immediately instead of as intermittent corruption in long encodes.
- Use `--bench=<prefix>` to time only the new kernel; the warmup stubs ensure the measurement is not distorted by a cold upper register file.
- Use the page-align trampoline when testing kernels with alignment-sensitive loads, especially on older cores where unaligned access is slow or faults.
- Compare against the previous ISA tier row rather than C, so the benefit of wider vectors is isolated.
- Check both ABIs if the kernel is used on Windows: XMM6–XMM15 are callee-saved there, a common source of bugs that the Unix build will not reveal.

References: System V AMD64 ABI and Microsoft x64 ABI for register conventions; Intel 64 and IA-32 Architectures Optimization Reference Manual for vector power gating; `tools/checkasm.c.md` for the calling side.

## 9. References

- The source file itself, `tools/checkasm-a.asm`, attributed to Loren Merritt and Henrik Gramner.
- System V AMD64 ABI: callee-saved registers and stack alignment.
- Microsoft x64 calling convention: shadow space and callee-saved XMM6–XMM15.
- Intel 64 and IA-32 Architectures Software Developer's Manual: `lfence`, `rdtsc`, MMX state and AVX power management.
- `x86inc.asm` in the reference tree for the macro layer used here.
- `tools/checkasm.c.md` for the C-side wrapper selection.

## 10. Verification checklist

- `x264_checkasm_call` returns the target's return value when no register was clobbered.
- Deliberately clobbering any poisoned integer register causes `ok` to be zeroed and the failure message to be printed.
- On Win64, deliberately clobbering any of XMM6–XMM15 also triggers failure.
- Arguments arrive at the target in the correct order and with the correct values despite being forced onto the stack.
- A 32-bit argument whose caller value has junk in the upper 32 bits is observable to the target, confirming the zero-extension check.
- `x264_stack_pagealign` calls the target with `rsp` low 12 bits zero minus the requested offset.
- Warmup functions return without altering architectural state visible to callers.
- Both 32-bit and 64-bit code paths assemble with the project's macro layer.

## 11. Open questions

- UNKNOWN: exact identities of all poisoned integer registers on each ABI beyond the documented `free_regs` counts; the macro expansion determines them.
- UNKNOWN: whether any x264 assembly function actually uses all 15 arguments; the constant is a conservative bound.
- UNKNOWN: whether the `rep ret` workaround is still needed on current AMD processors.
- UNKNOWN: behaviour when `puts` is unavailable in a minimal embedded build; the tool is standalone and expects a C runtime.
