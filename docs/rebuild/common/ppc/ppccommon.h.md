# `common/ppc/ppccommon.h` — PowerPC Altivec utility macros

## 1. Header

- **Source path:** `common/ppc/ppccommon.h`.
- **Role in one sentence:** defines vector types, unions, endianness-aware conversions, load/store helpers, and SIMD building blocks shared by all PowerPC back-end files.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

Altivec/VMX interfaces differ between big-endian and little-endian PowerPC, between compilers, and across ISA generations (AltiVec, VSX, POWER9). This header hides those differences behind a consistent set of macros so the kernel files can be written once.

## 3. Public interface

The header defines, at assembly/inline level:

- Vector type aliases: `vec_u8_t`, `vec_s8_t`, `vec_u16_t`, `vec_s16_t`, `vec_u32_t`, `vec_s32_t`, and, when VSX is available, `vec_u64_t`/`vec_s64_t`.
- Unions such as `vec_u8_u`, `vec_s16_u`, `vec_u32_u` that allow scalar access to vector lanes.
- `LOAD_ZERO` and the `zero_*v` family of zero vectors.
- Width conversion macros: `vec_u8_to_u16_h/l`, `vec_u8_to_s16_h/l`, `vec_u16_to_u32_h/l`, `vec_u16_to_s32_h/l`, and the inverse packing macros `vec_*_to_*`.
- `VEC_STORE8` for storing eight bytes.
- `VEC_TRANSPOSE_8` and `VEC_TRANSPOSE_4` for matrix transposition of 16-bit vectors.
- Difference helpers `PREP_DIFF`, `VEC_DIFF_H`, `VEC_DIFF_HL`, `VEC_DIFF_H_8BYTE_ALIGNED` with their `PREP_DIFF_8BYTEALIGNED` variant.
- Fallback definitions of `vec_vsx_ld`/`vec_vsx_st` for non-VSX builds using `vec_perm` and lane-shift masks.
- `vec_absd` fallback as `max - min`.
- `xxpermdi` implementations for compilers that do not expose the intrinsic, with endian-specific corrections.

## 4. State and data structures

All definitions are compile-time. The unions provide a view of a 128-bit vector as 2, 4, 8, or 16 scalar lanes. The zero vector must be declared once per function with `LOAD_ZERO` before using the `zero_*v` macros.

## 5. Algorithm / behaviour

**Endianness handling.** Big-endian and little-endian PowerPC place vector lanes differently. The conversion macros choose `vec_mergeh(zero, value)` or `vec_mergeh(value, zero)` depending on `WORDS_BIGENDIAN` so that the high half of a byte vector maps consistently to the low-indexed half of a halfword vector. The same reasoning applies to 16-to-32-bit widening.

**Unaligned access.** On VSX builds, `vec_vsx_ld`/`vec_vsx_st` map directly to hardware unaligned instructions. On older AltiVec, they are emulated with two aligned loads and a `vec_perm` using `vec_lvsl`/`vec_lvsr`, followed by two aligned stores. This emulation is substantially more expensive and is why several kernels have separate fast paths.

**Transposition.** `VEC_TRANSPOSE_8` transposes an 8x8 matrix of 16-bit vectors using three stages of `vec_mergeh`/`vec_mergel`, analogous to a butterfly network. `VEC_TRANSPOSE_4` operates on a logical 4x4 matrix held in 8x4 registers, discarding low elements and requiring callers not to use the low destination elements.

**Difference macros.** `VEC_DIFF_H` loads 16 bytes from two pointers, widens them to signed 16-bit, subtracts, and advances both pointers. `VEC_DIFF_HL` produces both halves of a 16-byte load as two separate difference vectors. The 8-byte-aligned variant is faster when the caller knows alignment.

**Permutation helper.** `xxpermdi` selects 64-bit doublewords from two vectors. On compilers lacking the intrinsic, it is implemented as `vec_perm` with constant selector vectors; on little-endian code the selector may need an additional correction because early compilers exposed the endianness bias.

## 6. Dependencies

Included by every `common/ppc/*.c` file. Depends on `altivec.h` when `HAVE_ALTIVEC_H` is set, and on build macros `HAVE_VSX`, `WORDS_BIGENDIAN`, and compiler version macros.

## 7. Constants and tables

Selector constants for the `xxpermdi` fallbacks encode combinations of the four source doublewords. `VEC_STORE8` uses `vec_xst_len` on POWER9 and a permute-plus-store sequence otherwise.

## 8. Performance notes

These macros determine the performance of all PPC kernels. On pre-VSX hardware, emulated unaligned loads are a major cost; kernels therefore prefer aligned accesses where possible. The transposition macros are implemented as register-only shuffle sequences and are the standard way to achieve SIMD 2D transforms. When rebuilding, preserve the semantic identities of the conversion macros exactly; lane-order errors are a common source of subtle output mismatches on little-endian PowerPC.

## 9. References

- AltiVec Technology Programming Interface Manual.
- PowerPC VSX and POWER9 ISA documentation for `vec_xst_len`, `vec_xxpermdi`.
- GNU and Clang compiler documentation for AltiVec builtins.

## 10. Verification checklist

- Each conversion macro produces the expected lane ordering on both big- and little-endian targets.
- `LOAD_ZERO` must be declared before any zero-vector macro use.
- `VEC_STORE8` writes exactly eight bytes and does not touch neighbors.
- Transpose macros produce mathematically correct transposes for distinct input values.
- `vec_absd` fallback agrees with hardware absolute-difference on representative vectors.
- Emulated `vec_vsx_ld/st` round-trips data correctly at all byte offsets.

## 11. Open questions

- `UNKNOWN`: exact compiler versions for which the `xxpermdi` fallback selector correction is required.
- `UNKNOWN`: whether all listed macros are used by every PPC file in all build configurations.
