# `common/x86/dct-32.asm` — x86-32 transform and zigzag

**Dirty-side behavioural specification.** Source path: `common/x86/dct-32.asm`.
Role: the x86-32-specific transform kernels. Because x86-32 has only eight SIMD
registers (MMX) or eight XMM registers, these implementations rely heavily on
explicit spill/unspill of intermediate rows to the stack, in contrast to the
register-rich x86-64 versions.

---

## 1. Header

Assembly source included only for 32-bit x86 builds. Includes `x86inc.asm` and
`x86util.asm`; imports `pd_32`, `pw_pixel_max`, `pw_2`, `pw_m2`, `pw_32`,
`hsub_mul` via `cextern`. Defines three local helper macros:
`SPILL_SHUFFLE`, `UNSPILL_SHUFFLE`, `SPILL`, `UNSPILL`.

## 2. Purpose

Provide 8x8 DCT/IDCT on x86-32 where the register pressure of keeping eight rows
plus temporaries cannot be met. The file defines its own `DCT8_1D` and
`IDCT8_1D` macros (parameterised by element size and spill destinations) and
implements the 8-bit `sub8x8_dct`, `sub8x8_dct8`, `add8x8_idct`, `add8x8_idct8`
entry points with explicit memory round-trips. High-bit-depth forms use SSE2
with dword elements and spill some rows into the coefficient buffer.

## 3. Public interface

Exported symbols (declared in `dct.h.md`):

### 3.1 High-bit-depth 8x8 transform
* `sub8x8_dct8` — `(int16_t dct[64], uint16_t *pix1, uint16_t *pix2)` for
  SSE2/SSE4/AVX. Actually writes 64 dword coefficients into the 128-byte buffer.
  Uses the 8-point `DCT8_1D d` macro with output rows spilled to `dct` offsets.
* `add8x8_idct8` — `(uint8_t *dst, int16_t dct[64])` for SSE2/AVX; reads 64
  dwords from the buffer, uses `UNSPILL_SHUFFLE`/`SPILL_SHUFFLE` macros to stage
  rows through the coefficient buffer as scratch, then stores eight 16-bit rows
  with `STORE_DIFF`.

### 3.2 8-bit 8x8 transforms
* `sub8x8_dct` — `(int16_t dct[4][16], uint8_t*, uint8_t*)`; SSE2, SSSE3, AVX, XOP.
* `sub8x8_dct8` — `(int16_t dct[64], uint8_t*, uint8_t*)`; SSE2, SSSE3, AVX, XOP.
  Also a dedicated MMX label `sub8x8_dct8_mmx` with a `.skip_prologue` entry.
* `add8x8_idct` — `(uint8_t*, int16_t[4][16])`; SSE2, AVX.
* `add8x8_idct8` — `(uint8_t*, int16_t[64])`; SSE2, AVX, and an MMX label
  `add8x8_idct8_mmx` with `.skip_prologue`.

### 3.3 Internal helpers
* `load_diff_4x8_mmx`, `dct8_mmx`, `idct8_mmx` — MMX building blocks that compute
  one 4x8 residual load and one 8-point transform, saving/restoring the MMX
  register permutation via `SAVE_MM_PERMUTATION`/`RESET_MM_PERMUTATION`.
* `ADD_STORE_ROW` — loads one destination row, unpacks bytes to words, adds the
  transform result, `packuswb`, and stores back.
* `DCT8_1D`, `IDCT8_1D` — local 8-point butterflies with explicit spill slots.

## 4. State and data structures

* **Spill area:** The coefficient buffer itself (`r0` for forward, `r1` for
  inverse) doubles as scratch. `SPILL reg, mN` stores register `mN` at `[base + N*16]`
  (assumes offsets equal register numbers); `UNSPILL` reloads. `SPILL_SHUFFLE`
  allows explicit (register, offset) pairs when the natural mapping conflicts.
* **MMX permutation state:** Because MMX registers alias x87 stack registers,
  the code calls `SAVE_MM_PERMUTATION` before `ret` from helper routines and
  `RESET_MM_PERMUTATION` at call sites to keep the macro assembler's logical
  register numbering consistent.
* **Stack alignment:** The MMX 8x8 inverse reserves `0x74`/`0x94` bytes of stack
  for `args`, `spill`, and `trans` scratch areas, aligned via `and esp,~15`.

## 5. Algorithm / behaviour

### 5.1 Spill macros
`SPILL_SHUFFLE base, regs..., offsets...` walks a register list and an offset
list in lockstep, emitting `mova [base+offset*16], m reg`. `UNSPILL_SHUFFLE`
does the reverse. `SPILL`/`UNSPILL` are convenience forms where offsets equal
register indices. This lets the 8-point butterfly keep more than eight logical
values alive by cycling some through memory.

### 5.2 8-point DCT (`DCT8_1D`, 32-bit variant)
Mathematically identical to `dct-64.asm`'s butterfly (same shift-and-add factors
and same natural ordering) but with a different register schedule:
1. Form `s07/d07`, `s16/d16`, `s25/d25`, `s34/d34` via `SUMSUB_BA`.
2. Form `a0=s34+s07`, `a2=s34-s07`, `a1=s25+s16`, `a3=s25-s16`.
3. Store three of the intermediate results (`dst0`, `dst4`, one more) directly
   to the spill area at `%10`,`%11`,`%12`, then compute the odd coefficients
   `a4..a7` with the same `d+(d>>1)` factors.
4. Second stage produces `b0..b7`; a final `SWAP` reorders. Some outputs are
   left in memory for the caller to reload after the transpose.

### 5.3 8-point inverse (`IDCT8_1D`, 32-bit variant)
Inverse of the above. Takes six data registers plus `m0`,`m4` loaded from the
spill area (`%10`,`%11`); reconstructs the eight samples with cascaded
sum/difference stages (`SUMSUB_BA`) and a final `SWAP` sequence. No rounding is
done here; the caller adds +32 and shifts right 6.

### 5.4 High-bit-depth `sub8x8_dct8`
1. `LOAD_DIFF8x4` twice loads eight rows of word differences.
2. `DCT8_1D w` with spill destinations `[r0]`, `[r0+0x10]`, `[r0+0x50]`.
3. Reload `m0`, store two rows, `TRANSPOSE4x4W` on the low four, `WIDEN_SXWD`
   to dwords, `DCT8_1D d` with new spill slots, then store eight dword rows.
4. Repeat for the high four rows with different spill offsets.
Because the 32-bit build cannot hold all eight widened rows, intermediate
results go back into `dct`.

### 5.5 High-bit-depth `add8x8_idct8`
1. `add r1,128`; `UNSPILL_SHUFFLE` loads six rows from scratch offsets
   `-6,-4,-2,2,4,6` relative to `r1`.
2. `IDCT8_1D d` with spill slots `[r1-128]`, `[r1+0]`; `TRANSPOSE4x4D` on the
   low four; add `pd_32` to DC; spill the four transposed rows.
3. Transpose the high four; reload from `-5,-3,-1,3,5,7`; `IDCT8_1D d` again.
4. More transpose/spill cycles, then eight `STORE_DIFF` operations write the
   final rows, shifting right 6, adding destination, clamping, storing.
The heavy use of the coefficient buffer as scratch is the defining difference
from `dct-64.asm`.

### 5.6 8-bit MMX building blocks
* `load_diff_4x8_mmx` loads eight rows of `pix1-pix2` with `LOAD_DIFF`,
  temporarily spills `m5` to `[r0]`, and returns.
* `dct8_mmx` calls `DCT8_1D w` with spill slots `[r0]`, `[r0+0x40]`, `[r0+0x60]`,
  then `SAVE_MM_PERMUTATION; ret`.
* `idct8_mmx` calls `IDCT8_1D w` with slots `[r1+0]`, `[r1+64]`.

### 5.7 8-bit `sub8x8_dct8_mmx`
1. `call load_diff_4x8_mmx`, `call dct8_mmx`; unspill and transpose the low
   4x4; spill.
2. Repeat with pointer offsets `+4` to transform the right half; combine the
   two halves with careful `punpck`/`psllq`/`PALIGNR` sequences (the comment
   notes a dependency on the MMX permutation to avoid register conflicts).
3. Transform the remaining two halves and store the 64-word result.
The ordering is chosen to minimise the number of `RESET_MM_PERMUTATION` calls.

### 5.8 8-bit `add8x8_idct8_mmx`
1. `add word [r1],32` (rounding bias injected into the DC coefficient).
2. Unspill, `idct8_mmx`, transpose, spill — repeated four times with pointer
   offsets `+8` and a mid-stage swap `[r1+0x08]`↔`[r1+0x40]`.
3. A comment documents the memory layout at the midpoint:
   ```
   A0------ A1------   B0------ F0------   C0------ G0------   D0------ H0------
   E0------ E1------   B1------ F1------   C1------ G1------   D1------ H1------
   ```
   (these are descriptive labels, not copied source). The code then unspills,
   calls `idct8_mmx`, shifts all eight results right 6, swaps rows into their
   final positions, and uses `ADD_STORE_ROW` eight times to add/clip/store.

### 5.9 8-bit `DCT_SUB8` / `ADD8x8` / `ADD8x8_IDCT8`
SSE2/SSSE3/AVX/XOP variants use `LOAD_DIFF8x4` and `SPILL`/`UNSPILL` to hold
four rows at a time; `DCT4_1D`/`TRANSPOSE2x4x4W`/`STORE_DCT` for forward, and
`IDCT4_1D`/`TRANSPOSE2x4x4W`/`DIFFx2`/`STORE_IDCT` for inverse. Structurally the
same as `dct-64.asm` but with spills because only eight XMM registers exist.

## 6. Dependencies

* Includes `x86inc.asm`, `x86util.asm`.
* Shares `.skip_prologue` labels with `dct-a.asm` via `cextern`.
* Called only in 32-bit builds; the dispatcher selects these when the ISA is
  MMX2/SSE2/SSSE3/AVX/XOP on x86-32.
* Uses C constants imported via `cextern`.

## 7. Constants and tables

No local tables. All masks come from `cextern`:
* `pd_32`, `pw_32` — inverse rounding bias (dword / word).
* `pw_pixel_max` — clamp ceiling.
* `pw_2`, `pw_m2` — butterfly helpers.
* `hsub_mul` — `[1,-1,...]` byte multiplier for `pmaddubsw` on SSSE3.
The spill offsets `0x00,0x10,0x40,0x50,0x60,0x80,0x90,0xC0,0xE0` are chosen to
match the `dct` array layout and the number of registers available.

## 8. Performance notes

* **Spill avoidance is the main goal:** the 32-bit code is inherently slower
  than the 64-bit code because every 8-point transform must cycle intermediate
  rows through memory. The `SPILL`/`UNSPILL` macros generate aligned 16-byte
  moves to keep this cheap.
* **Register scheduling:** `DECLARE_REG_TMP 6` on WIN64 and `2` elsewhere reduces
  REX prefixes in the AVX2 `dct2x4dc` path; analogous tricks appear throughout.
* **Transpose strategy:** Mixed `TRANSPOSE4x4W` (4x4) and manual `punpck`/`PALIGNR`
  sequences are used because a full 8x8 transpose would need more registers than
  are available.
* **MMX permutation overhead:** `SAVE_MM_PERMUTATION`/`RESET_MM_PERMUTATION`
  calls cost a few instructions each; the code groups work to minimise them.
  A clean implementation should avoid MMX entirely on modern x86-32 CPUs.
* **References:** `dct-64.asm.md` section 8 describes the register-rich approach
  that these kernels approximate; `dct-a.asm.md` section 8 covers general
  transform speedups.
* **UNKNOWN:** relative performance of the MMX vs SSE2 8-bit kernels on various
  x86-32 CPUs; dispatch heuristics in the source choose SSE2 where available.

## 9. References

* ITU-T H.264 integer transform definitions.
* Public x264 source history for the 32-bit transform scheduling decisions.
* Intel/AMD SIMD manuals for MMX/x87 aliasing and `SAVE_MM_PERMUTATION`.

## 10. Verification checklist

* All checks listed in `dct-64.asm.md` section 10 apply, with emphasis on the
  spill/reload paths.
* Verify that `sub8x8_dct8_mmx` produces the same 64 words as the SSE2 variant
  for identical inputs (tests the MMX permutation management).
* Verify `add8x8_idct8_mmx`'s mid-stage row swaps produce correct row ordering;
  a mis-ordered swap yields a block that is transposed or mirrored.
* Confirm the spill offsets do not overlap with caller data beyond the documented
  64/128-byte coefficient buffer.
* On x86-32, confirm no x87 state corruption after MMX kernels (call `emms`
  behaviour is implicit in `RESET_MM_PERMUTATION`).

## 11. Open questions

* `UNKNOWN`: whether the `sse4` instantiation of `sub8x8_dct8` differs from
  `sse2` in any observable way (it appears identical).
* `UNKNOWN`: exact behaviour of the `DCT_SUB8` macro's `SWAP 7,9`/`SWAP 7,10`
  register aliases on CPUs where MMX and XMM register numbering interacts.
