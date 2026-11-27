# `common/aarch64/asm-offsets.c` — AArch64 assembly layout assertions

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/asm-offsets.c`
- Role: compile-time verification that the C layout of `x264_cabac_t` matches the hard-coded byte offsets used by the AArch64 CABAC assembly.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The hand-written CABAC assembly addresses fields of `x264_cabac_t` using numeric byte offsets instead of C field names. If a compiler, ABI change, or an unrelated edit ever changes the structure layout, the assembly would silently corrupt encoder state. This translation unit exists solely to make such a mismatch a build-time (compile-time) failure rather than a runtime data-corruption bug. It emits no functions and no data at runtime; its entire purpose is a set of negative-array-size assertions.

## 3. Public interface

No functions, variables, or symbols are exported. The file is a "constant-checking" translation unit: it is compiled as part of the build and contributes no object code (all declared structs are never instantiated at runtime).

Semantic contract: for every checked field, `offsetof(struct, field)` must equal the corresponding macro from `asm-offsets.h`. Additionally two pairs of fields must be adjacent with the second member exactly one `int` wide after the first (so a single pair-load/store can touch both).

## 4. State and data structures

- Uses the real `x264_cabac_t` definition (via `common/common.h`).
- Uses the numeric constants from `asm-offsets.h`.
- Declares dummy tag types named after each checked field pair; each such type has one member whose declared array size is `+1` when the assertion is true and `-1` when false. The C language rejects a negative array bound, so a violation is diagnosed by the compiler. No object of these tag types is ever created.

## 5. Algorithm / behaviour

Two assertion macros are defined in the file (their names and exact spelling are implementation facts):

1. An offset assertion macro takes a structure type, a member name, and an expected byte offset. It defines a tag structure containing one static-assertion array whose size is one element when `offsetof(type, member) == expected`, and illegal otherwise.
2. A relative-offset assertion macro takes a structure type and two member names plus an intervening type. It asserts `offsetof(type, first) + sizeof(type) == offsetof(type, second)`, i.e. the second field immediately follows the first and the first is exactly the given type.

The following ten absolute offsets are checked against the Cabac structure:

| Field | Expected offset |
| --- | --- |
| `i_low` | `0x00` |
| `i_range` | `0x04` |
| `i_queue` | `0x08` |
| `i_bytes_outstanding` | `0x0c` |
| `p_start` | `0x10` |
| `p` | `0x18` |
| `p_end` | `0x20` |
| `f8_bits_encoded` | `0x30` |
| `state` | `0x34` |

Note the deliberately sparse layout: between `p_end` (`0x20`) and `f8_bits_encoded` (`0x30`) there are sixteen bytes that the assembly does not interpret, and the state array begins immediately after the 32-bit bit counter at `0x34`.

Two relative assertions encode further assumptions of the assembly:

- `i_low` is immediately followed by `i_range` (both 32-bit) — the assembly uses one pair load/store (`stp`/`ldp`) for them.
- `i_queue` is immediately followed by `i_bytes_outstanding` (both 32-bit) — again one pair access.

Ordering/edge cases: assertions are evaluated at translation time; there is no runtime branch. The resulting object file is empty. Any future change to the Cabac structure that preserves field offsets but changes field *types* is not caught unless it also breaks one of the two adjacency checks.

## 6. Dependencies

- `common/common.h`: definition of `x264_cabac_t`, `offsetof` availability.
- `asm-offsets.h`: the expected numeric constants (see the companion specification).
- Built only when the AArch64 assembly CABAC is enabled. Not called by anything.

## 7. Constants and tables

The numeric constants are documented in `asm-offsets.h.md`; no new tables are introduced here. The two `2 * !(condition) - 1` style array-size expressions yield `1` for a satisfied assertion and `-1` for a violated one.

## 8. Performance notes

No runtime cost. The file is not hot and has no SIMD content. For a clean-room port the important point is to preserve the *semantic* guarantee: if Cabac state is redesigned, the assembly offsets must be regenerated or the equivalent assertions ported to the new ABI. A modern port may prefer C11 `_Static_assert` or language-native `static_assert` to express the same facts without the negative-array trick.

Public references: ISO C standard (`offsetof`, array-bound rules); ARM AAPCS64 (structure layout); x264 source distribution build notes.

## 9. References

- ISO/IEC 9899 (C language), `offsetof` and array declarators.
- ARM AAPCS64 (Procedure Call Standard for the Arm 64-bit Architecture).
- ITU-T H.264 / ISO/IEC 14496-10 (CABAC encoder state semantics).
- `docs/rebuild/common/aarch64/asm-offsets.h.md` (constant definitions).

## 10. Verification checklist

- Compiling this file against the intended `x264_cabac_t` succeeds with no diagnostics.
- Deliberately perturbing one expected offset or swapping two fields must cause a compiler error, proving the assertion mechanism is effective.
- The emitted object contains no functions and no writable data.
- A clean-room implementation can confirm equivalence by checking that its Cabac structure, when compiled with the same assertions, produces the same offsets; or by asserting the same invariants in its own language.

## 11. Open questions

- UNKNOWN: whether the reference build always compiles this file for 10-bit builds as well as 8-bit builds (the offsets themselves are bit-depth independent).
- UNKNOWN: the compiler versions and ABIs against which the layout was originally validated.
