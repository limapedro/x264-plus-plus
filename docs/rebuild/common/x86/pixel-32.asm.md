# `common/x86/pixel-32.asm` — 32-bit x86 pixel metrics

**Dirty-side behavioural specification.** Source path:
`common/x86/pixel-32.asm`. Role: x86-32 (MMX) implementations of a small subset
of pixel metrics — the 8x8 SA8D, its intra-prediction x3 form, and the SSIM core
— that are handled by dedicated SSE/AVX code in `pixel-a.asm` on x86-64.

---

## 1. Header

Assembly source compiled only for 32-bit x86 with 8-bit support (the whole file
is guarded by `%if HIGH_BIT_DEPTH == 0`). It includes `x86inc.asm`/`x86util.asm`
and imports `pw_ppmmppmm` and `pw_pmpmpmpm` from the C layer. It begins with
`INIT_MMX mmx2` and defines three exported kernels plus local helper macros.

## 2. Purpose

On x86-32 the eight MMX registers are insufficient for the straightforward SSE2
 formulations of the 8x8 Hadamard and intra-SA8D kernels used on x86-64. This file
 provides specialised MMX schedules that stage transposed intermediate data in
 stack scratch areas, allowing 8x8 SA8D, `intra_sa8d_x3_8x8`, and the SSIM core
 to run on legacy 32-bit CPUs.

## 3. Public interface

### 3.1 `pixel_sa8d_8x8_internal` (local helper with global label)
* Loads two 8-row halves of a residual via `LOAD_DIFF_4x8P 0` and
  `LOAD_DIFF_4x8P 4`, applies `HADAMARD8_V` (Hadamard along the vertical
  dimension only), transposes each 4x4 half into a stack `trans` area, re-applies
  `HADAMARD8_V`, and accumulates absolute values with `SUM4x8_MM`.
* Final `pavgw m0,[trans]` averages the two half-results, giving the half-scale
  SA8D term. This label is called by the x86-64 file? No — it is local to the
  32-bit build and called by `pixel_sa8d_8x8`/`16x16` in `pixel-a.asm` through
  `cextern` on 32-bit.

### 3.2 `intra_sa8d_x3_8x8`
* `(void intra_sa8d_x3_8x8(uint8_t *fenc, uint8_t edge[36], int *res))`.
* Computes the 8x8 Hadamard of `fenc`, applies 1D Hadamards to the left/top edge
  samples (`HSUMSUB2` with `pw_ppmmppmm`, `pw_pmpmpmpm`), and evaluates the DC,
  horizontal, and vertical prediction modes.
* Writes the three costs to `res[0]=v`, `res[1]=h`, `res[2]=dc` (ordering per
  `SUM_MM_X3` + store: `movq [r2+0],m2` → v,h; `movd [r2+8],m0` → dc).
* The DC value is computed with scalar arithmetic: `dc = (left_top + top_left)*4
  + 32`, masked with `0xffc0` (round to multiple of 64).

### 3.3 `pixel_ssim_4x4x2_core`
* `(void pixel_ssim_4x4x2_core(const uint8_t *pix1, intptr_t stride1,
  const uint8_t *pix2, intptr_t stride2, int sums[2][4]))`.
* Loops over two adjacent 4x4 blocks; for each, accumulates four dword sums:
  `sum1`, `sum2`, `sum1^2`, `sum1*sum2` (and `sum2^2` folded into the third via
  `paddwd`) in MMX registers. Stores `sums[block][0..3]` using `movq`.
* The function calls `emms` before returning to restore x87 state.

## 4. State and data structures

* Stack layout for `intra_sa8d_x3_8x8`: `SUB esp,0x94` with named regions:
  `edge` at `esp+0x70` (32 bytes), `spill` at `esp+0x60` (16 bytes),
  `trans` at `esp+0` (96 bytes), `sum` overlapping `trans` (32 bytes).
* Stack layout for `pixel_sa8d_8x8_internal`: `push r0; push r2;
  sub esp,0x74`, with `args=esp+0x74`, `spill=esp+0x60`, `trans=esp+0`.
* All scratch is 16-byte aligned or accessed with `movq` (8-byte) to avoid
  alignment faults on old MMX-only CPUs.
* MMX register permutation is tracked by the assembler; helper routines restore
  it implicitly by avoiding `RESET_MM_PERMUTATION` in the middle.

## 5. Algorithm / behaviour

### 5.1 `LOAD_DIFF_4x8P`
Loads eight rows of `pix1-pix2` as 8-byte MMX words using `LOAD_DIFF` (unsigned
subtract in both directions + OR). Uses `r1`/`r3` (strides) and `r4`/`r5`
(3×strides, precomputed by callers). Spills `m5` once to `[spill]` to break the
register cycle when loading the last row.

### 5.2 `SUM4x8_MM`
Takes eight word rows in `m0..m7`, computes absolute values pairwise with
`ABSW2`, adds four pairs twice (`m0+=m2`, `m1+=m3`, `m4+=m6`, `m5+=m7`), then
`m0+=m4`, `m1+=m5`, `m0+=m1`. Uses `[spill]`/`[spill+8]` to free `m6`/`m7` as
temporaries for the `ABSW2` calls.

### 5.3 `pixel_sa8d_8x8_internal`
1. First 4x8 half: `LOAD_DIFF_4x8P 0`, `HADAMARD8_V` (eight 4-point vertical
   butterflies), transpose the two 4x4 halves into `[trans+0x00..0x38]`.
2. Second half (pointer offset +4): same, transposed into `[trans+0x40..0x78]`.
3. Re-load the first half's transposed data and apply `HADAMARD8_V` again,
   `SUM4x8_MM`, save result to `[trans]`.
4. Same for the second half; `pavgw m0,[trans]` averages the two sums,
   implementing the `(sum+1)>>1` SA8D rounding.
The two-stage vertical-then-transpose-then-vertical structure is the 2D 8x8
Hadamard, split to fit eight registers.

### 5.4 `intra_sa8d_x3_8x8`
1. Unpack left edge (`r1+7`) and top edge (`r1+16`) bytes to words in
   `m0..m3` and apply two rounds of `HSUMSUB2` (shuffles `q1032`, `q2301`
   combined with `pmullw` by `pw_ppmmppmm`/`pw_pmpmpmpm`) to compute the 1D
   Hadamard of each edge.
2. Store the four resulting edge-Hadamard vectors at `[edge+0..24]`.
3. `LOAD_4x8P 0` / `LOAD_4x8P 4` load the two column halves of `fenc`; apply
   `HADAMARD8_V` and transpose to stack, exactly as in the SA8D kernel.
4. Compute the 7x4 sum of absolute Hadamard coefficients (`ABSW2`, `paddw`).
5. DC mode: subtract `left_edge<<3` and the scalar DC prediction from the
   Hadamard DC coefficient, take absolute values, add the 7x4 sum.
6. Horizontal mode: repeat for the transposed half and edges; vertical mode
   uses the other edge buffer.
7. `SUM_MM_X3` with `pavgw` reduces each mode's four word lanes to one value;
   stores v/h to `[r2+0]` (16 bytes) and dc to `[r2+8]`.

### 5.5 `pixel_ssim_4x4x2_core`
1. `r4=4` is the block offset loop counter; each iteration processes two
   4x4 blocks starting at `pix+r4` and `pix2+r4`.
2. For each of four rows, zero-extend one byte from each image to words
   (`movd` + `punpcklbw` with zero), accumulate `m1 += a`, `m2 += b`,
   `m3 += a*a`, `m4 += a*b` (with `m3 += b*b` via reusing registers).
3. Horizontally reduce `m1`,`m2` with `paddusw` + `pshufw q0032`, and pack with
   `punpcklwd`; reduce `m3`,`m4` similarly with `paddd`; store four dwords to
   `sums[block]`.
The 32-bit core differs from the SSE2/AVX version only in using MMX and 8-byte
stores; the numeric results are identical.

## 6. Dependencies

* Includes `x86inc.asm`, `x86util.asm`.
* Imports `pw_ppmmppmm`, `pw_pmpmpmpm` from the C layer.
* Called by the x86-32 dispatch layer; `pixel_a_32`/dispatcher maps the listed
  symbols into the generic function table.
* `pixel_sa8d_8x8_internal` is referenced from `pixel-a.asm` on x86-32 via
  `cextern`.

## 7. Constants and tables

* `pw_ppmmppmm`: word sign pattern `[1,1,-1,-1,1,1,-1,-1]` for the edge
  Hadamard's second stage.
* `pw_pmpmpmpm`: word sign pattern `[1,-1,1,-1,...]` for the first stage.
* The scalar DC mask `0xffc0` rounds `(left+top)*4+32` down to a multiple of 64.
* No local `.rodata`; all other constants come from `x86util` or the C layer.

## 8. Performance notes

* **Wider vectors:** MMX is 64-bit; no wider option exists on the target CPUs.
  The primary optimisation is minimizing `movq` spill traffic to `[trans]`.
* **Transpose strategy:** Each 8x8 block is split into two 4x4 halves; only
  `TRANSPOSE4x4W` is available/cheap in MMX, so the code transposes each 4x4
  separately into a stack area rather than attempting an 8x8 transpose.
* **Avoiding spills:** `LOAD_DIFF_4x8P` spills exactly one register (`m5`) per
  half; `SUM4x8_MM` frees two (`m6`,`m7`) via stack to make room for ABSW2
  temporaries. These are the minimum necessary for eight-row liveness.
* **Preprocessing constants:** the edge sign masks are precomputed `pw_*`
  constants; the DC bias `0xffc0` mask is an immediate.
* **x87 state:** `emms` in the SSIM core is mandatory after MMX use; forgetting
  it would corrupt floating-point callers.
* **Hotness:** these kernels only run on legacy 32-bit CPUs; on modern x86-64
  the SSE/AVX versions in `pixel-a.asm` are used exclusively.
* **References:** `pixel-a.asm.md` section 8 describes the modern equivalents.

## 9. References

* ITU-T H.264 intra prediction and Hadamard transform definitions.
* Intel MMX/SSE programming manuals for `ABSW2`, `LOAD_DIFF`, `TRANSPOSE4x4W`.
* SSIM paper (Wang et al.) for the accumulation formula.

## 10. Verification checklist

* Compare `pixel_sa8d_8x8_internal` output to the SSE2 `pixel_sa8d_8x8` for the
  same inputs; must match bit-exactly.
* Compare `intra_sa8d_x3_8x8`'s three costs to the portable C implementation for
  random fenc/edge data, including edge cases with corner replication.
* Verify `pixel_ssim_4x4x2_core` outputs match the SSE2/AVX core and that `emms`
  leaves x87 usable.
* Check stack scratch areas do not overflow: `trans` is 96 bytes,
  `esp+0x74`/`0x94` allocations must be restored exactly on return.

## 11. Open questions

* `UNKNOWN`: whether `pixel_sa8d_8x8_internal` is actually referenced from
  `pixel-a.asm` on a 32-bit build, or is only used locally (the label is global
  with `.skip_prologue`-style naming).
* `UNKNOWN`: the exact mapping of `res[0..2]` to v/h/dc in `intra_sa8d_x3_8x8`
  should be confirmed against the C caller; the order is inferred from the
  store sequence.
* `UNKNOWN`: behaviour for `height` values other than 8/16 in the SSIM core
  (the loop counter `r4=4` assumes exactly two 4x4 blocks).
