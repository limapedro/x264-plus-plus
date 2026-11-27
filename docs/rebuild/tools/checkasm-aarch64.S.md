# `tools/checkasm-aarch64.S` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/checkasm-aarch64.S`.
- Role: the AArch64 assembly support layer for the conformance tool: register poisoning, argument relocation, call/return handling, NEON and SVE register checks, stack junking and SVE length reporting.
- Marker: this file is a **dirty-side behavioural specification**; behaviour is described in our own words.

## 2. Purpose

AArch64 has a larger callee-saved set than 32-bit ARM: x19–x28 and the lower 64 bits of v8–v15 must survive a call. This file provides a wrapper that fills those registers with distinctive values before calling a kernel and verifies them afterwards. It also relocates arguments so all fifteen are passed in a controlled stack layout, and reports the hardware SVE length so benchmark output can name the SVE tier precisely.

## 3. Public interface

| Symbol | Signature / role |
| --- | --- |
| `x264_checkasm_stack_clobber` | `void (uint64_t clobber, ...)`. Fills the stack region below the current stack pointer with the given pattern. |
| `x264_checkasm_call` | `intptr_t (intptr_t (*func)(), int *ok, ...)`. Calls the kernel, checks callee-saved GPRs and NEON registers, clears `ok` and prints a message on failure, returns the kernel result. |
| `x264_checkasm_sve_length` | `int ()`. Returns the SVE vector length in bits. Only present when SVE support is compiled in. |

The source includes the architecture's shared assembly macro header, which supplies `function`/`endfunc`, `const`/`endconst`, `movrel` and the `EXT` name-mangling helper for `puts`.

## 4. State and data structures

- `register_init`: an aligned constant table of eighteen 64-bit values. The first eight are loaded into v8–v15; the next ten are loaded into x19–x28. Values are chosen to be unlikely to coincide accidentally with kernel results.
- `error_message`: a NUL-terminated string reporting failure to preserve a register.
- `MAX_ARGS` is 15, matching the C tool's bound.
- `CLOBBER_STACK` is `(8*MAX_ARGS + 15) & ~15`, i.e. the argument area rounded up to 16-byte alignment.
- `ARG_STACK` is `((8*(MAX_ARGS-8) + 15) & ~15)` — space for the seven arguments beyond the eight passed in registers, rounded to 16 bytes.
- No mutable global state; all scratch values live on the stack frame.

## 5. Algorithm / behaviour

### 5.1 Stack clobber

1. Save the original stack pointer in a temporary register.
2. Set a countdown to `CLOBBER_STACK`.
3. Repeatedly store the two argument registers as a pair at `[sp, #-16]!`, decrement the countdown by 16 and loop while positive.
4. Restore the original stack pointer and return.

This writes junk below the stack pointer, where the C caller has placed the real arguments for the subsequent ABI-check call.

### 5.2 ABI checker

1. **Prologue**: create a frame and save x29/x30, x19–x28 and d8–d15 in descending order, each pair occupying 16 bytes.
2. **Poison**: using `movrel` to obtain the address of `register_init`, load the first eight quadwords into v8–v15 and the next ten into x19–x28 with post-incrementing pair loads.
3. **Save the ok pointer**: push it onto the stack in a single slot.
4. **Relocate arguments**: reserve `ARG_STACK`, then for each argument beyond the first eight, load it from the caller's frame at the documented offset and store it into the new stack area. The offsets deliberately skip the first eight arguments, which will be loaded into registers.
5. **Prepare the call**: save the function pointer in a scratch register, load the first eight arguments from the caller's frame into x0–x7, call the kernel with `blr`, then release the argument area and reload the `ok` pointer and return address.
6. **Check NEON**: reset the source pointer to `register_init` and zero an accumulator vector. For each pair of v-registers, interleave their 64-bit lanes into a single vector, XOR with the corresponding constant from the table and OR into the accumulator. Saturate the 16 halfwords to bytes and extract a 64-bit value.
7. **Check GPRs**: for each pair x19/x20 … x27/x28, load the expected constants, XOR with the actual registers and OR the differences into the accumulator.
8. **Branch on result**: if the accumulator is zero, skip the failure path.
9. **Failure path**: write zero through the saved `ok` pointer, obtain the failure string with `movrel`, call `puts` through the architecture's external-name helper, then fall through to restoration.
10. **Epilogue**: pop the saved registers in reverse order, restore the frame pointer and link register, and return.

The return value in x0/x1 is saved across the failure path and restored before returning, so the C side can still report the kernel's numeric result.

### 5.3 SVE length

When SVE is compiled in, the file switches assembly to an SVE-enabled architecture variant and defines a function that reads the SVE byte count with `cntb`, multiplies by eight to convert to bits and returns it. The C tool uses this value to print `SVE (N bits)` and `SVE2 (N bits)` tier names.

## 6. Dependencies

- **Includes**: `../common/aarch64/asm.S`, providing the architecture macro layer.
- **Declares**: `puts` through the `EXT` helper so symbol names are correct across platforms.
- **Called by**: `tools/checkasm.c.md` under `HAVE_AARCH64`; the C file also uses the stack-clobber variant on non-Apple AArch64.
- **Related**: `tools/gas-preprocessor.pl.md` may translate this file for older Apple assemblers and clang's integrated assembler; `doc/standards.txt.md` states the general ABI assumptions.

## 7. Constants and tables

| Constant | Value / meaning |
| --- | --- |
| `MAX_ARGS` | 15 |
| `CLOBBER_STACK` | `(8*15+15) & ~15` = 128 bytes rounded to 16. |
| `ARG_STACK` | `(8*7+15) & ~15` = 64 bytes rounded to 16. |
| `register_init` entries | 18 quadrwords: 8 NEON + 10 GPR sentinels. |
| Saved registers | x19–x28, d8–d15, x29, x30. |
| SVE length conversion | `cntb << 3` gives bits. |

## 8. Performance notes

- The wrapper is conformance-only; its register save/restore and comparisons add overhead that is excluded from benchmark samples.
- Loading all sentinels with post-incrementing pair loads is efficient and keeps the poison setup short.
- The NEON check interleaves lanes and folds with `uqxtn`, avoiding a scalar loop over sixteen bytes.
- SVE length is architecture-dependent; benchmark tables should always include it in the tier name so results from different machines are not conflated.

### 8.1 Using checkasm-style verification to accelerate faster kernels

For AArch64 kernel work:

- The wrapper checks both NEON and GPR callee-saved state; a new kernel that uses x19–x28 as scratch without saving them will be caught immediately.
- Run with each tier enabled separately. The SVE and SVE2 rows include the vector length, so a kernel tuned for 128-bit SVE must be re-evaluated on a 256-bit or 512-bit implementation.
- On Apple platforms the C tool does not use the stack-clobber path, so zero-extension bugs may need an additional explicit test there.
- Combine with the benchmark mode: because wide vectors may be power-managed, compare only after warmup and on an idle machine.

References: AAPCS for callee-saved registers; ARM NEON and SVE architecture references for register widths; `tools/checkasm.c.md` for the C side and flag sweep.

## 9. References

- The source file itself, `tools/checkasm-aarch64.S`, attributed to Martin Storsjo.
- ARM Architecture Procedure Call Standard (AAPCS64) for x19–x28 and v8–v15 preservation.
- ARM NEON and Scalable Vector Extension architecture documentation for register naming and `cntb`.
- `tools/gas-preprocessor.pl.md` for assembler compatibility handling.
- `tools/checkasm.c.md` for the C-side declarations and tier sweep.

## 10. Verification checklist

- `x264_checkasm_call` returns the kernel's return value when all registers are preserved.
- Clobbering any of x19–x28 or v8–v15 causes `ok` to be cleared and the failure message printed.
- The first eight arguments arrive in x0–x7 and the remaining seven in the relocated stack area with the correct values.
- `x264_checkasm_stack_clobber` writes across the full `CLOBBER_STACK` region and restores the original stack pointer.
- `x264_checkasm_sve_length` returns a multiple of 128 bits and matches the hardware's SVE length.
- The failure path preserves x0/x1 so the caller still sees the kernel result.
- All saved registers are restored exactly, with no stack imbalance.

## 11. Open questions

- UNKNOWN: why the stack-clobber path is disabled on Apple platforms in the C tool; a platform-specific ABI or OS behaviour is likely but not documented here.
- UNKNOWN: whether the `register_init` constants are shared with other architectures by design or coincidence; their values match the AArch64 and LoongArch tables.
- UNKNOWN: behaviour on implementations with SVE2 but no SVE flag set; the C tier sweep treats them as separate flags.
