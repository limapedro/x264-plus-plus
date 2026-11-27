# `common/aarch64/asm-offsets.h` — AArch64 CABAC structure offsets

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/asm-offsets.h`
- Role: single authoritative list of byte offsets into `x264_cabac_t` that the AArch64 CABAC assembly must use.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

Hand-written assembly cannot use the C preprocessor's `offsetof` at runtime, so the offsets are frozen as named constants shared by the assembly source (`cabac-a.S`) and the assertion translation unit (`asm-offsets.c`). This header is the shared contract: change a constant here and both the assembly and the assertions change together.

## 3. Public interface

A set of object-like preprocessor macros, each expanding to a hexadecimal byte offset:

- `CABAC_I_LOW`, `CABAC_I_RANGE`, `CABAC_I_QUEUE`, `CABAC_I_BYTES_OUTSTANDING`
- `CABAC_P_START`, `CABAC_P`, `CABAC_P_END`
- `CABAC_F8_BITS_ENCODED`, `CABAC_STATE`

Contract: when added to the base address of an `x264_cabac_t` held in a 64-bit register, each constant yields the address of the named field. All are plain byte offsets; none require scaling. The header is idempotent (include guard) and has no runtime side effects.

## 4. State and data structures

The layout implied by the constants (all sizes inferred from the consuming assembly and Cabac semantics):

| Offset | Field | Size | Meaning |
| --- | --- | --- | --- |
| `0x00` | `i_low` | 4 | low end of current arithmetic interval |
| `0x04` | `i_range` | 4 | interval range (12-bitish, renormalized) |
| `0x08` | `i_queue` | 4 | pending output bit counter (can be negative) |
| `0x0c` | `i_bytes_outstanding` | 4 | count of 0xFF bytes awaiting carry resolution |
| `0x10` | `p_start` | 8 | start of output buffer |
| `0x18` | `p` | 8 | current output pointer |
| `0x20` | `p_end` | 8 | end of output buffer |
| `0x28` | (gap / padding, unused by assembly) | 8 | — |
| `0x30` | `f8_bits_encoded` | 4 | accumulated bit count for rate estimation |
| `0x34` | `state` | 1024 | CABAC context state array (only its base is named here) |

Invariants: `i_low` and `i_range` are adjacent (single pair access), as are `i_queue` and `i_bytes_outstanding`. The state array starts at `0x34`; individual contexts are byte-sized and indexed by a context index added to this base.

## 5. Algorithm / behaviour

The header performs no computation. Its semantic content is the *layout contract*:

- Four 32-bit scalar fields occupy the first sixteen bytes. Two of them (`i_low`/`i_range`) form a naturally aligned pair suitable for one 64-bit store, as do `i_queue`/`i_bytes_outstanding`.
- Three pointer fields occupy `0x10`–`0x27`; the assembly uses `p` at `0x18` for byte emission and treats the byte immediately before it as the carry-propagation target.
- The 64-bit gap at `0x28` is not referenced by the AArch64 kernels; a clean-room port must preserve it only if it intends to use the same constants.
- The bit counter and state array follow at `0x30`/`0x34`.

Consumers add these constants as unsigned offsets to a structure base. Because `state` is a byte array, a context index is added directly (not scaled) to `CABAC_STATE`.

## 6. Dependencies

- Included by `cabac-a.S` (to compute field addresses).
- Included by `asm-offsets.c` (to build assertions).
- Conceptually coupled to the definition of `x264_cabac_t` in the common headers — the coupling is verified, not expressed, in C.

## 7. Constants and tables

The ten offsets are listed in section 4. The `0x28` gap and the size of the state array are not named constants; they are behaviourally relevant only through the consuming code. No lookup tables are defined here.

## 8. Performance notes

Not a hot code path. For a clean-room port on x86-64, the equivalent design is either the same frozen offsets (with matching assertions) or direct C struct member access with compiler-generated addressing. SVE/SVE2 offer no benefit for structure-offset constants. Public references: ARM AAPCS64 layout rules and AArch64 assembler immediate/addressing documentation (`ldr`, `ldp`, `str`, `stp` with unsigned offsets).

## 9. References

- ARM AAPCS64 (structure member alignment and layout).
- AArch64 Architecture Reference Manual, load/store addressing modes.
- ITU-T H.264 / ISO/IEC 14496-10, CABAC arithmetic encoder state.
- `docs/rebuild/common/aarch64/asm-offsets.c.md`.

## 10. Verification checklist

- Every macro is defined exactly once and the include guard prevents redefinition.
- A test program including this header and the real Cabac struct agrees with `offsetof` for all ten fields.
- The two adjacency invariants (`i_low`→`i_range`, `i_queue`→`i_bytes_outstanding`) hold.
- Assembly built with these offsets produces the same encoded bitstream as a C reference CABAC on randomized symbol streams.

## 11. Open questions

- UNKNOWN: exact declared type and size of the unused spacer at `0x28`.
- UNKNOWN: whether `f8_bits_encoded` is written by the AArch64 assembly (the kernels shown only read/interact with low, range, queue, outstanding, pointer, and state).
