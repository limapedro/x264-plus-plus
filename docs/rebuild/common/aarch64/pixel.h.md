# `common/aarch64/pixel.h` — AArch64 pixel metric declarations

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/pixel.h`
- Role: declares all AArch64-accelerated pixel metric kernels, including NEON and SVE variants, and defines declaration macros that generate the full block-size matrix.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The generic pixel dispatcher (`common/pixel.c`) fills function-pointer tables for every metric and block size. This header is the contract: it enumerates every accelerated symbol and uses macros to avoid writing eight declarations per metric family.

## 3. Public interface

Declared families (each instantiated for block sizes 4×4, 4×8, 4×16, 8×4, 8×8, 8×16, 16×8, 16×16 unless noted):
- `int x264_pixel_sad_<block>_neon( pixel *, intptr_t, pixel *, intptr_t )`
- `void x264_pixel_sad_x3_<block>_neon( pixel *, pixel *, pixel *, pixel *, intptr_t, int * )`
- `void x264_pixel_sad_x4_<block>_neon( pixel *, pixel *, pixel *, pixel *, pixel *, intptr_t, int * )`
- `int x264_pixel_satd_<block>_neon( ... )`
- `int x264_pixel_ssd_<block>_neon( ... )`
- SVE SSD subset: `x264_pixel_ssd_{4x4,4x8,4x16,8x4,8x8}_sve( ... )`

Additional declarations:
- `void x264_pixel_ssd_nv12_core_neon( pixel*, intptr_t, pixel*, intptr_t, int, int, uint64_t*, uint64_t* )`
- `int x264_pixel_vsad_neon( pixel *, intptr_t, int )`
- `int x264_pixel_sa8d_8x8_neon( ... )`, `int x264_pixel_sa8d_16x16_neon( ... )`
- `uint64_t x264_pixel_sa8d_satd_16x16_neon( ... )`
- `int x264_pixel_sa8d_8x8_sve( ... )`
- `uint64_t x264_pixel_var_{8x8,8x16,16x16}_neon( pixel *, intptr_t )`
- `int x264_pixel_var2_{8x8,8x16}_neon( pixel *, pixel *, int * )`
- `uint64_t x264_pixel_var_{8x8,8x16}_sve( pixel *, intptr_t )`
- `uint64_t x264_pixel_hadamard_ac_{8x8,8x16,16x8,16x16}_{neon,sve}( pixel *, intptr_t )`
- `void x264_pixel_ssim_4x4x2_core_neon( const pixel *, intptr_t, const pixel *, intptr_t, int sums[2][4] )`
- `float x264_pixel_ssim_end4_neon( int sum0[5][4], int sum1[5][4], int width )`
- `int x264_pixel_asd8_neon( pixel *, intptr_t, pixel *, intptr_t, int )`

Declaration macros:
- `DECL_PIXELS(ret, name, suffix, args)`: expands to eight declarations for the standard block sizes.
- `DECL_PIXELS_SSD_SVE(ret, args)`: five declarations for the SVE SSD subset.
- `DECL_X1`, `DECL_X1_SSD_SVE`, `DECL_X4`: bind the above to the appropriate signatures.

## 4. State and data structures

- `pixel` is `uint8_t` for 8-bit builds (8-bit kernels); high-bit-depth variants use `uint16_t` and are declared elsewhere or via the same names in a different build configuration.
- `intptr_t` strides are signed pointer-sized integers.
- The SAD x3/x4 output is an `int*` of length 3 or 4.

## 5. Algorithm / behaviour

The header declares only; all metric semantics are documented in `pixel-a.S.md` and `pixel-a-sve.S.md`. Its structural contract:
- `_neon` names require `X264_CPU_NEON`; `_sve` names require `X264_CPU_SVE`.
- SVE overrides are deliberately partial: five SSD shapes, 8×8 SA8D, two variance shapes, and all four hadamard-ac shapes.
- Return conventions: metrics returning `int` are non-negative; variance and hadamard-ac return 64-bit values assembled from two 32-bit halves; SSIM end returns `float`.

## 6. Dependencies

- Included by `common/pixel.c`.
- Uses common typedefs `pixel`, `dctcoef`, and `x264_template` for symbol naming.
- Implementations in `pixel-a.S`, `pixel-a-sve.S`.

## 7. Constants and tables

No constants defined in the header (block-size enums `PIXEL_*` come from common headers).

## 8. Performance notes

Declaration-only. SVE/SVE2 note for a clean-room port: the partial override list matters for bitstream reproducibility — do not assume SVE implementations exist for SAD or SATD even if a port adds them. Public references: x264 pixel dispatcher design; ARM SVE feature detection.

## 9. References

- x264 `common/pixel.c` (generic metric dispatcher).
- `docs/rebuild/common/aarch64/pixel-a.S.md`, `pixel-a-sve.S.md`.
- ARM NEON/SVE programmer guides.

## 10. Verification checklist

- Every declared symbol has an implementation in one of the two assembly files.
- Signatures match the generic function-pointer types in `common/pixel.c`.
- The declaration macros expand to exactly the intended block-size matrix.
- For each feature mask, the selected function set matches the documented overrides.

## 11. Open questions

- UNKNOWN: whether the same header is used for 10-bit builds with different `pixel` typedef (likely yes, but verify against `common/pixel.c` guards).
