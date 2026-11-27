# `common/ppc/pixel.h` — PowerPC pixel-metric declaration

## 1. Header

- **Source path:** `common/ppc/pixel.h`.
- **Role in one sentence:** declares the single entry point that installs AltiVec pixel-metric overrides.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

The PowerPC pixel back end is large but exposes only one symbol. This header declares that initializer so the generic pixel-initialization code can install all Altivec metric routines in one call.

## 3. Public interface

- `void x264_pixel_init_altivec( x264_pixel_function_t *pixf )`

The function takes the caller-owned pixel function table and mutates it in place. The caller must have already verified AltiVec support. The function returns nothing.

## 4. State and data structures

`x264_pixel_function_t` contains arrays of function pointers indexed by the pixel-size enum (`PIXEL_16x16`, `PIXEL_16x8`, `PIXEL_8x16`, `PIXEL_8x8`, `PIXEL_8x4`, `PIXEL_4x8`, `PIXEL_4x4`). Additional scalar fields hold intra-metric and SSIM helpers.

## 5. Algorithm / behaviour

The initializer installs Altivec implementations for SAD (single, x3, x4), SATD (single, x3, x4), SSD, SA8D, variance, Hadamard-AC, intra mode-comparison helpers, and the SSIM core. Only 8-bit builds install these routines; high-bit-depth builds leave the table untouched. The underlying metric definitions match the generic C implementations: absolute differences, Hadamard-domain absolute sums, squared differences, sample and square sums, and SSIM accumulators.

## 6. Dependencies

- Implemented in `common/ppc/pixel.c`.
- Called from the generic pixel-initialization path in `common/pixel.c`.
- Uses `x264_pixel_function_t` and the pixel-size enum from `common/pixel.h`.

## 7. Constants and tables

Pixel-size enum values and the standard block dimensions listed in the state section. No numeric algorithm constants are defined by the header itself.

## 8. Performance notes

The installed functions are the innermost loops of motion estimation and mode decision. AltiVec's 16-byte vectors align well with 8x8 and 16x8 blocks; smaller blocks pack two rows per vector. The initializer itself is not performance-critical.

## 9. References

- `common/ppc/pixel.c` for the concrete routines.
- `common/pixel.h` for the function-table definition.
- `common/pixel.c` for canonical metric semantics.

## 10. Verification checklist

- All documented function-pointer fields are set in 8-bit builds.
- No fields are changed in high-bit-depth builds.
- Repeated calls produce identical tables.
- Installed functions match generic results on representative blocks.

## 11. Open questions

- `UNKNOWN`: exact call site and CPU feature test used before this initializer is invoked.
