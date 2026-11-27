# `tools/checkasm-loongarch.S` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/checkasm-loongarch.S`.
- Role: the LoongArch assembly support layer for the conformance tool: stack junking, a callee-saved GPR/FPR checker with argument relocation, and the failure-report path.
- Marker: this file is a **dirty-side behavioural specification**; the mechanisms are described in our own words.

## 2. Purpose

The LoongArch ABI requires s0–s8 and fs0–fs7 to be preserved across calls. This file provides a wrapper that loads those seventeen registers with distinctive constants before invoking a kernel and verifies them afterwards. It also supplies a stack-clobber helper used by the C tool on LSX builds to expose 32-bit zero-extension assumptions, mirroring the x86 and AArch64 approach.

## 3. Public interface

| Symbol | Signature / role |
| --- | --- |
| `x264_checkasm_stack_clobber` | `void (uint64_t clobber, ...)`. Fills the stack region below the current stack pointer with the given pattern. |
| `x264_checkasm_call` | `intptr_t (intptr_t (*func)(), int *ok, ...)`. Calls the kernel, checks callee-saved GPRs and FPRs, clears `ok` and prints a message on failure, returns the kernel result. |

The file includes the LoongArch shared assembly macro header, which supplies `function`/`endfunc`, `const`/`endconst` and `la.local`.

Note on naming: the exported names here include the `x264_` prefix in the source text, unlike the AArch64 file where the macro layer adds the prefix. The C tool declares the same names in its LoongArch branch.

## 4. State and data structures

- `register_init`: an aligned table of seventeen 64-bit constants — nine for s0–s8 and eight for fs0–fs7. Values are chosen to be unlikely to match kernel results accidentally.
- `error_message`: NUL-terminated failure text.
- `MAX_ARGS` is 15.
- `CLOBBER_STACK` is `(8*MAX_ARGS + 15) & ~15`, the argument region rounded to 16 bytes.
- `ARG_STACK` is `(8*(MAX_ARGS-8) + 15) & ~15`, the space for seven stack-passed arguments.
- The wrapper reserves 136 bytes to save s0–s8 (9×8) and fs0–fs7 (8×8), plus 16 bytes for the `ok` pointer and return address.
- No mutable global state.

## 5. Algorithm / behaviour

### 5.1 Stack clobber

1. Save the stack pointer in a temporary register.
2. Set a countdown to `CLOBBER_STACK`.
3. Store each of the two argument registers in successive stack slots using pre-decrement addressing, decrement the countdown by 16 and loop until the comparison indicates completion.
4. Restore the original stack pointer and return.

As on other architectures, the junked region lies below the stack pointer where the caller has placed the real arguments for the subsequent call.

### 5.2 ABI checker

1. **Prologue**: save the incoming stack pointer in a temporary register, subtract 136 bytes and store s0–s8 and fs0–fs7 into that area.
2. **Poison**: compute the address of `register_init` with a local-address instruction, then load s0–s8 and fs0–fs7 from the table with doubleword loads and floating-point doubleword loads.
3. **Save control state**: subtract another 16 bytes, store the `ok` pointer and the return address.
4. **Relocate arguments**: reserve `ARG_STACK`. Starting at an offset of eight times the number of register-passed arguments in the caller's frame, copy each remaining argument into the new stack area with indexed doubleword loads and stores.
5. **Prepare the call**: move the function pointer to a scratch register, load the eight register-passed arguments from the caller's frame into a0–a7, and call the kernel with an indirect jump that links the return address.
6. **Restore control state**: release the argument area, reload the `ok` pointer and return address, and release the 16-byte control slot.
7. **Check GPRs**: reset the table pointer, zero an accumulator and for each of s0–s8 load the expected constant, XOR with the actual register and OR into the accumulator.
8. **Check FPRs**: for each of fs0–fs7, move the floating-point value to a general register with `movfr2gr.d`, XOR with the expected constant and OR into the accumulator.
9. **Branch on result**: if the accumulator is zero, skip the failure path.
10. **Failure path**: write zero through the `ok` pointer, load the failure string address, save the return address in a scratch slot, call `puts`, restore the return address and release the scratch slot.
11. **Epilogue**: restore all saved GPRs and FPRs from the 136-byte area, release it and return.

The kernel's return value is not explicitly preserved across the `puts` call in this implementation; the C side relies on the `ok` flag to detect failure. This differs from the x86 and AArch64 wrappers, which save the return registers explicitly.

## 6. Dependencies

- **Includes**: `../common/loongarch/loongson_asm.S`, providing the LoongArch macro layer.
- **Declares**: `puts` as an external symbol.
- **Called by**: `tools/checkasm.c.md` in its `ARCH_LOONGARCH` branch; when LSX is present the C tool also invokes the stack-clobber helper before each candidate call.
- **Related**: `tools/gas-preprocessor.pl.md` does not cover LoongArch; this file is assembled directly with a GNU-style assembler.

## 7. Constants and tables

| Constant | Value / meaning |
| --- | --- |
| `MAX_ARGS` | 15 |
| `CLOBBER_STACK` | `(8*15+15) & ~15` = 128 bytes rounded to 16. |
| `ARG_STACK` | `(8*7+15) & ~15` = 64 bytes rounded to 16. |
| Saved-register area | 136 bytes |
| Control slot | 16 bytes (`ok` pointer + return address) |
| Poison constants | 17 doublewords |
| Checked registers | s0–s8, fs0–fs7 |

## 8. Performance notes

- The wrapper is conformance-only; benchmark samples call kernels directly.
- Floating-point registers are checked by moving them to general registers, which is a small fixed cost independent of kernel work.
- The stack-clobber helper is enabled only for LSX builds, because that is where the C tool's zero-extension check applies.
- Extensibility tip: if a future LoongArch extension adds callee-saved vector registers, extend both the poison load sequence and the check sequence, and increase the saved-register area accordingly.

### 8.1 Using checkasm-style verification to accelerate faster kernels

For LoongArch LSX/LASX kernel work:

- The wrapper catches both GPR and FPR preservation mistakes, which are common when hand-writing vector code with temporary register reuse.
- The stack-clobber path mirrors x86-64 and non-Apple AArch64; use it when a kernel accepts 32-bit arguments and might read the upper half of a 64-bit stack slot.
- Benchmark each tier separately: the C tool prints LSX and LASX as distinct rows, so the incremental benefit of LASX can be measured directly.
- Because the failure path does not preserve the return register explicitly, do not rely on the numeric result of a failed kernel; use the `ok` flag as the authority.

References: LoongArch ABI documentation for callee-saved registers; LSX/LASX intrinsics documentation for vector register conventions; `tools/checkasm.c.md` for the C-side tier sweep and stack-clobber invocation.

## 9. References

- The source file itself, `tools/checkasm-loongarch.S`, attributed to Xiwei Gu.
- LoongArch architecture and ABI documentation for s0–s8 and fs0–fs7 preservation.
- LoongArch SIMD extension (LSX/LASX) documentation.
- `tools/checkasm.c.md` for the C-side declarations and flag sweep.

## 10. Verification checklist

- `x264_checkasm_call` returns the kernel's result when all registers are preserved.
- Clobbering any of s0–s8 or fs0–fs7 causes `ok` to be cleared and the failure message printed.
- The first eight arguments arrive in a0–a7 and the remaining seven in the relocated stack area with correct values.
- `x264_checkasm_stack_clobber` writes the full `CLOBBER_STACK` region and restores the original stack pointer.
- All saved registers are restored exactly on both success and failure paths, and the stack pointer returns to its original value.
- The failure path calls `puts` without corrupting the `ok` pointer.

## 11. Open questions

- UNKNOWN: whether saving the kernel's return registers across `puts` is unnecessary for LoongArch kernels or an oversight; the x86 and AArch64 wrappers do save them.
- UNKNOWN: exact rationale for enabling the stack-clobber helper only when LSX is present in the C tool.
- UNKNOWN: whether the `x264_` prefix is added by the macro layer or written literally in all declarations; source text and C declarations should be cross-checked when porting.
