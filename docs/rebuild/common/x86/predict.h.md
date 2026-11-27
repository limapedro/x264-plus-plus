# `common/x86/predict.h` — x86 intra prediction declarations

**Dirty-side behavioural specification.** Source path: `common/x86/predict.h`.
Role: declares the x86 SIMD intra-prediction kernels and the five CPU-dispatch
initialisers implemented in `predict-a.asm` / `predict-c.c`.

---

## 1. Header

Declarations-only header. Every symbol is wrapped in
`#define x264_<name>_<suffix> x264_template(<name>_<suffix>)`. No data or code is
defined. It is included by `predict-c.c` and by the x86 dispatch code.

## 2. Purpose

Provides the authoritative list of prediction modes × block sizes × ISA
variants available on x86. It also documents, through its pointer types, the
two calling conventions used: simple modes take only the destination pointer
(edges are implicit in the plane), while 8x8 luma modes take an explicit
`edge[36]` buffer populated by a separate filter function.

## 3. Public interface

### 3.1 Dispatch initialisers
```
void x264_predict_16x16_init_mmx (uint32_t cpu, x264_predict_t pf[7]);
void x264_predict_8x16c_init_mmx (uint32_t cpu, x264_predict_t pf[7]);
void x264_predict_8x8c_init_mmx  (uint32_t cpu, x264_predict_t pf[7]);
void x264_predict_4x4_init_mmx   (uint32_t cpu, x264_predict_t pf[12]);
void x264_predict_8x8_init_mmx   (uint32_t cpu, x264_predict8x8_t pf[12],
                                  x264_predict_8x8_filter_t *filter);
```
Contract: fill the supplied arrays with the best available x86 implementations
for the detected CPU flags; leave entries unchanged when the required feature
is absent.

### 3.2 16x16 luma modes — `void (pixel *src)`
* V: `predict_16x16_v_{mmx2,sse,avx}` (AVX form takes `uint16_t*`).
* H: `predict_16x16_h_{mmx2,sse2,ssse3,avx2}` (sse2/avx2 take `uint16_t*`,
  ssse3 takes `uint8_t*`).
* DC / DC_TOP / DC_LEFT: `predict_16x16_dc_{sse2,avx2}`,
  `predict_16x16_dc_top_{sse2,avx2}`, `predict_16x16_dc_left_{sse2,avx2}`.
* P core: `predict_16x16_p_core_{mmx2,sse2,avx,avx2}` — signature
  `(pixel *src, int i00, int b, int c)` (mmx2 form takes `uint8_t*`).

### 3.3 8x16 chroma modes (4:2:2) — `void (pixel *src)`
* V: `predict_8x16c_v_{mmx,sse}` (sse takes `uint16_t*`).
* H: `predict_8x16c_h_{mmx2,sse2,ssse3,avx2}` (mixed byte/word signatures).
* DC / DC_TOP: `predict_8x16c_dc_{mmx2,sse2}`, `predict_8x16c_dc_top_{mmx2,sse2}`.
* P core: `predict_8x16c_p_core_{mmx2,sse2,avx,avx2}`.

### 3.4 8x8 chroma modes — `void (pixel *src)`
* V: `predict_8x8c_v_{mmx,sse}`.
* H: `predict_8x8c_h_{mmx2,sse2,ssse3,avx2}`.
* DC / DC_TOP: `predict_8x8c_dc_{mmx2,sse2}`, `predict_8x8c_dc_top_{mmx2,sse2}`.
* P core: `predict_8x8c_p_core_{mmx2,sse2,avx,avx2}`.

### 3.5 8x8 luma modes — `void (pixel *src, pixel edge[36])`
* V: `predict_8x8_v_{mmx2,sse}`.
* H: `predict_8x8_h_{mmx2,sse2}`.
* DC / DC_TOP / DC_LEFT: `predict_8x8_dc_{mmx2,sse2}`,
  `predict_8x8_dc_top_{mmx2,sse2}`, `predict_8x8_dc_left_{mmx2,sse2}`.
* DDL: `predict_8x8_ddl_{mmx2,sse2,ssse3,cache64_ssse3,avx}`.
* DDR: `predict_8x8_ddr_{mmx2,sse2,ssse3,cache64_ssse3,avx}`.
* VL: `predict_8x8_vl_{mmx2,sse2,ssse3,avx}`.
* VR: `predict_8x8_vr_{mmx2,sse2,ssse3,avx}`.
* HD: `predict_8x8_hd_{mmx2,sse2,ssse3,avx}`.
* HU: `predict_8x8_hu_{mmx2,sse2,ssse3,avx}`.
* Edge filter: `predict_8x8_filter_{mmx2,sse2,ssse3,avx}` — signature
  `(pixel *src, pixel edge[36], int i_neighbor, int i_filters)`.

### 3.6 4x4 luma modes — `void (pixel *src)`
* H: `predict_4x4_h_avx2`.
* DDL: `predict_4x4_ddl_{mmx2,sse2,avx}`.
* DDR: `predict_4x4_ddr_{mmx2,sse2,ssse3,avx}`.
* VL: `predict_4x4_vl_{mmx2,sse2,avx}`.
* VR: `predict_4x4_vr_{mmx2,sse2,ssse3,cache64_ssse3,avx}`.
* HD: `predict_4x4_hd_{mmx2,sse2,ssse3,avx}`.
* HU: `predict_4x4_hu_mmx2`.
* DC: `predict_4x4_dc_mmx2`.

## 4. State and data structures

The header defines no state. Caller-side structures:
* `x264_predict_t pf[7|12]`: array of function pointers indexed by prediction
  mode enum; filled by the init functions.
* `edge[36]`: 36-element pixel array holding left/top/corner samples in a
  SIMD-friendly layout, produced by `predict_8x8_filter`.
* `i00`, `b`, `c`: scalar plane-mode parameters computed in `predict-c.c`.
* `i_neighbor`, `i_filters`: bitmasks controlling which edge neighbours and
  which filtered variants the 8x8 filter computes.

## 5. Algorithm / behaviour

The header defines no algorithms. It encodes the following contracts:
* Prediction functions write the predicted block into `src` (in-place
  overwrite); the surrounding plane already contains the reference samples at
  the prescribed negative offsets.
* For 8x8 luma modes, the reference samples are provided in `edge[]` rather
  than read from the plane, allowing out-of-frame edge replication to be done
  once.
* Plane-P cores receive precomputed parameters; they do not inspect edges.
* `*_p_core` signatures use `int` for `i00/b/c` regardless of pixel type; the
  assembly interprets them as signed 16-bit or 32-bit depending on bit depth.

## 6. Dependencies

* Included by `predict-c.c` and by the x86 dispatch initialisation file.
* Uses `pixel`, `uint8_t`, `uint16_t`, `uint32_t`, `intptr_t` typedefs and the
  `x264_template` renaming macro.
* Declared kernels are implemented in `predict-a.asm`; dispatch functions in
  `predict-c.c`.

## 7. Constants and tables

No constants. The mode-enum indices (`I_PRED_*`) and CPU flags (`X264_CPU_*`)
are defined in `common/predict.h` / `common/common.h`.

## 8. Performance notes

* The header exposes fine-grained ISA variants per mode, letting the dispatcher
  choose e.g. SSSE3 for H but AVX2 for plane-P. A clean implementation should
  preserve this granularity.
* **Wider vectors:** AVX/AVX2 variants exist for plane-P and H modes; prefer
  them on modern CPUs.
* **Transpose strategy:** not applicable; prediction is store-heavy, and the
  macros `STORE8`/`STORE16` minimise store instructions.
* **Avoiding spills:** the `*_p_core` interface passes parameters in registers
  so the kernel needs no stack access for setup.
* **Preprocessing constants:** `edge[36]` and `i00/b/c` are precomputed once per
  macroblock; the SIMD kernels are pure consumers.
* **Cache variants:** `cache64_ssse3` variants exist for 4x4 VR and 8x8
  DDL/DDR; dispatch should respect `X264_CPU_CACHELINE_64`.

## 9. References

* ITU-T H.264 intra prediction specification.
* Public x264 source history for the mode/ISA matrix.
* Intel/AMD SIMD reference manuals for the listed suffixes.

## 10. Verification checklist

* Confirm every declared symbol is emitted in a full build and that each
  dispatch function installs the correct pointer for a given CPU flag set.
* Verify signature types match the assembly `cglobal` argument counts (notably
  the 3-argument `*_p_core` and 4-argument `*_filter`).
* For each mode/ISA, compare predicted samples to the portable C implementation.
* Confirm `edge[36]` is large enough for all SIMD loads (some variants read up
  to 32/36 elements with unaligned loads).
* Verify 8-bit vs high-bit-depth signatures are dispatched to the correct build.

## 11. Open questions

* `UNKNOWN`: exact enum values and ordering of `I_PRED_*` constants; consult
  `common/predict.h` in the reference tree.
* `UNKNOWN`: whether `predict_8x8_v_sse`/`predict_8x16c_v_sse` use `movups`
  (unaligned) requiring a 16-byte-aligned `src` in practice.
* `UNKNOWN`: precise conditions under which `predict_4x4_h_avx2` is installed
  (appears only in high-bit-depth 4x4 dispatch).
