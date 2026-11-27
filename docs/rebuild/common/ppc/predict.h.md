# `common/ppc/predict.h` — PowerPC intra-prediction declarations

## 1. Header

- **Source path:** `common/ppc/predict.h`.
- **Role in one sentence:** declares the two PowerPC intra-prediction initializers.
- **Marker:** Dirty-side behavioural specification; all descriptions are behavioural and written independently of the reference source.


## 2. Purpose

Only two intra-prediction families have AltiVec implementations on PowerPC: 16x16 luma and 8x8 chroma, and only the plane mode in the chroma case. This header exposes their initializers so the generic prediction dispatchers can install them.

## 3. Public interface

- `void x264_predict_16x16_init_altivec( x264_predict_t pf[7] )`
- `void x264_predict_8x8c_init_altivec( x264_predict_t pf[7] )`

Each takes the caller-owned prediction function table and replaces selected entries. Neither returns a value, and neither takes a CPU feature mask; the caller is responsible for checking AltiVec support.

## 4. State and data structures

`x264_predict_t` is a pointer to a function taking a single destination pointer. The table is indexed by the intra-mode enumerations from `common/predict.h`: `I_PRED_16x16_*` and `I_PRED_CHROMA_*`.

## 5. Algorithm / behaviour

- `x264_predict_16x16_init_altivec` installs all seven 16x16 luma modes: vertical, horizontal, DC, plane, DC-left, DC-top, and DC-128.
- `x264_predict_8x8c_init_altivec` installs only the plane mode (`I_PRED_CHROMA_P`); the remaining chroma modes stay generic.

The installed routines implement the standard H.264 prediction formulas: neighbor replication for vertical/horizontal, rounded averaging for DC, and the fixed-point plane equation using gradients from the top and left neighbors. All assignments are compile-time guarded for 8-bit builds.

## 6. Dependencies

- Implemented in `common/ppc/predict.c`.
- Called from `common/predict.c` dispatchers.
- Uses the mode enumerations from `common/predict.h`.

## 7. Constants and tables

- `FDEC_STRIDE = 32`.
- Plane parameters use `(5H+32)>>6`, `(5V+32)>>6` for 16x16 and `(17H+16)>>5`, `(17V+16)>>5` for 8x8 chroma.

## 8. Performance notes

Dispatch is one-time. The 16x16 AltiVec routines fill a full row with one vector store, which is the main gain over scalar code. The 8x8 chroma plane mode is the only chroma kernel that benefited from vectorization on the reference hardware.

## 9. References

- `common/ppc/predict.c` for the implementations.
- `common/predict.c` for generic formulas.
- ITU-T H.264 intra prediction specification.

## 10. Verification checklist

- Correct entries are installed; others are untouched.
- 8-bit and high-bit-depth behaviour matches the compile-time guards.
- Installed routines match generic results for all modes they override.

## 11. Open questions

- `UNKNOWN`: whether newer PowerPC cores would benefit from additional chroma kernels that the reference left generic.
