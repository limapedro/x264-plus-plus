# common/x86/x86util.asm — behavioural specification

**Role:** library of SIMD utility macros (transposes, Hadamard butterfly steps, broadcasts, clipping, shifts, load/store helpers, sign/absolute-value helpers) shared by all x86 DSP kernels on top of `x86inc.asm`.
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/x86util.asm`
- Role: pure preprocessor include that adds bit-depth-aware constants and a reusable vocabulary of vector operations to the `x264asm` framework.
- No runtime state or exported functions; everything is a macro expansion into the including kernel.

---

## 2. Purpose

While `x86inc.asm` solves ABI/dispatch, this file solves *algorithm plumbing*: it provides bit-depth-parameterised pixel/DCT sizes and a set of macros that implement common SIMD idioms (interleaving/uninterleaving, sums and differences, horizontal reduction, absolute value, byte-aligned shifts, broadcast of scalars, load-a-pixel-row-and-subtract, store-a-DCT-row). Each macro selects an instruction sequence appropriate to the currently active feature level, so a kernel written once compiles to MMX, SSE2, SSSE3, AVX/AVX2 or AVX-512 variants.

---

## 3. Public interface

### 3.1 Bit-depth and stride configuration
- If `BIT_DEPTH` is undefined it is set to 0; `HIGH_BIT_DEPTH` is 1 iff `BIT_DEPTH > 8`.
- `FENC_STRIDE = 16`, `FDEC_STRIDE = 32` (in pixels).
- `SIZEOF_PIXEL` = 1 (8-bit) or 2 (high depth); `SIZEOF_DCTCOEF` = 2 or 4; `pixel` = `byte` or `word`.
- `vpbroadcastdct` = `vpbroadcastw` (8-bit) or `vpbroadcastd` (high); `vpbroadcastpix` = `vpbroadcastb` or `vpbroadcastw`.
- `FENC_STRIDEB = SIZEOF_PIXEL*FENC_STRIDE`, `FDEC_STRIDEB = SIZEOF_PIXEL*FDEC_STRIDE`; `PIXEL_MAX = (1<<BIT_DEPTH)-1`.
- `cextern_common name` — declare an external symbol with the plain `x264_` prefix rather than the bit-depth-specific one.
- `FIX_STRIDES regs...` — double each listed register at assemble time when `HIGH_BIT_DEPTH` (converts pixel strides to byte strides).

### 3.2 Interleave / transpose / butterfly macros
- `SBUTTERFLY type, a, b, tmp`, `SBUTTERFLY2` — interleave halves of two registers (word/dword/qword interleaves). The AVX2 `dqqq` specialisation uses `vperm2i128`+`vinserti128`; AVX-enabled builds use 3-operand `punpckh/punpckl`.
- `TRANSPOSE4x4W`, `TRANSPOSE2x4x4W`, `TRANSPOSE4x4D`, `TRANSPOSE8x8W` — register-level matrix transposes; the 8x8 word version has a 32-bit fallback that spills through two temporaries.
- `SWAP` (from x86inc) is used throughout to track permutations.

### 3.3 Arithmetic / sign macros
- `WIDEN_SXWD dst, src` — sign-extend words to dwords (SSSE3 `pmovsxwd` path vs unpack+arithmetic-shift).
- `ABSW`, `ABSW2`, `ABSB`, `ABSD` — absolute values for word/byte/dword with SSSE3-specific shortcuts and in-place variants.
- `PSIGN`, `PSIGNW`, `PSIGND` — multiply by sign, using SSSE3 `psign` or XOR/subtract emulation.
- `CLIPW dst, min, max` — signed clamp words (`pmaxsw`+`pminsw`).
- `SUMSUB_BA`, `SUMSUB_BADC`, `SUMSUB2_AB`, `SUMSUBD2_AB`, `HADAMARD*`, `HADAMARD*_2D` — butterfly sum/difference networks for Hadamard transforms, with optional max/amax variants and XOP fused shortcuts.
- `ACCUM instr, accum, input, iteration` — add on later iterations, swap on the first (avoids initialising an accumulator).

### 3.4 Reductions and broadcasts
- `HADDD`, `HADDW`, `HADDUWD`, `HADDUW` — horizontal sums of dwords/words (including unsigned-word variants and XOP `vhadd` shortcuts).
- `SPLATW`, `SPLATD`, `SPLATB_LOAD` — broadcast a scalar or a loaded byte to all lanes (AVX2 `vpbroadcast*`, otherwise shuffle chains).
- `PALIGNR` — byte-align two registers, with an AVX2 form that uses a precomputed partner register and a non-SSSE3 shift/or fallback.
- `PSHUFLW` — width-agnostic word shuffle (pshufw on MMX, pshuflw otherwise).
- `PSLLPIX`, `PSRLPIX` — shift a vector by a number of pixels (MMX qword vs XMM dword shift).
- `MOVHL` — move high half to low half with encoding-size-aware instruction choice.

### 3.5 Load/store helpers
- `LOAD_DIFF dst, tmp, zero/mask, fenc_addr, fdec_addr[, aligned]` — load a row of pixels from a fenc and fdec pointer and compute the signed difference; high-depth uses word subtract, 8-bit uses byte unpacks (or `pmaddubsw` in `LOAD_DIFF8x4` for SSSE3).
- `LOAD_DIFF8x4 dst1..4, tmp, mul, fenc_ptr, fdec_ptr` and `LOAD_DIFF_8x4P`, `LOAD_DIFF16x2_AVX2` — pipelined multi-row variants.
- `STORE_DCT`, `STORE_IDCT` — write 8 DCT/IDCT rows from four registers into a stride.
- `DIFFx2`, `STORE_DIFF` — add a scaled residual to pixels and pack/clamp (8-bit) or clip to pixel max (high depth).
- `SHUFFLE_MASK_W` — generate a `pshufb` mask from eight word indices.

---

## 4. State and data structures

- The only state is the preprocessor configuration established at include time (`BIT_DEPTH`, `HIGH_BIT_DEPTH`, size constants) plus the `x86inc` state (`mmsize`, `cpuflags`, `avx_enabled`, symbolic registers). Macros are stateless expansions; `SWAP`-driven permutations are tracked by `x86inc`'s `nn` mapping.
- Data objects referenced by macros (`pw_1`, `pw_16`, `pw_32`, `pw_512`, `pw_00ff`, `pw_8`, `pb_0`, `pb_3`, `pw_pixel_max`, masks) are defined in `const-a.asm` and declared with `cextern` by the including kernel.

---

## 5. Algorithm / behaviour

### 5.1 Butterflies and transposes
`SBUTTERFLY wd, a, b, t` combines the low words of `a` and `b` into `a` and the high words into `t`, then swaps the symbolic names of `b` and `t`. With four registers and two `wd` then two `dq` steps, `TRANSPOSE4x4W` transposes a 4x4 word block with no memory traffic; `TRANSPOSE8x8W` extends this with a `qdq` stage and two final swaps. The 32-bit variant must reuse one input register as a spill slot because only eight MMX registers exist.

### 5.2 Absolute value and sign
`ABSW` uses `pabsw` when SSSE3 is available. Otherwise, for a distinct destination it computes `max(src, -src)`; for an in-place operation it uses a temporary zero register, and the `sign` variant (for pairing with `PSIGNW`) modifies its source. `ABSW2` does two lanes at once. `ABSD` handles the full 32-bit range via compare/XOR/subtract with an optional copy.

### 5.3 Reductions
`HADDD` progressively halves the vector using `vextracti32x8`/`vextracti128`/`MOVHL` and `paddd`, finishing with a word shuffle and add (or XOP `vphadddq`). `HADDW` first converts words to dwords with `pmaddwd [pw_1]`, then calls `HADDD`. The unsigned variants mask the high halves (`HADDUWD`) before adding, used where partial sums cannot be signed.

### 5.4 Broadcasts
`SPLATW`/`SPLATD` use `vpbroadcastw/d` on AVX2; otherwise they load into the low word/dword, shuffle within the low 64 bits (`pshuflw`/`pshufd`), and, for YMM, replicate across 128-bit halves with `vpbroadcastq`. `SPLATB_LOAD` loads four bytes starting three before the pointer (to avoid a cacheline crossing), then uses `pshufb` or unpack+word-splat.

### 5.5 Load/difference and store paths
8-bit `LOAD_DIFF` loads 8 bytes from each plane with `movh`, unpacks to words against either a zero register or a mask register, and subtracts. High-depth loads full vectors with `movu`/`mova` and subtracts words directly; AVX permits a destructive 2-operand form with a memory source. `STORE_DIFF` shifts the residual right by 6 (the residual scale), adds the original pixel row, packs to bytes with unsigned saturation (8-bit) or clips to `[0, PIXEL_MAX]` with `CLIPW` (high depth), then writes back.

### 5.6 Hadamard networks
`HADAMARD distance, mode, a, b, tmp[, tmp2]` optionally reorders operands for a horizontal pass (`sumsub` requires order because subtraction is not commutative; `max`/`amax` do not), then applies `SUMSUB_BA` or `pmaxsw` (with `ABSW` for `amax`). Composed forms build 2D 2x2/4x4/8x8 Hadamards; the SSE-specific 4x4 form reduces register pressure by interleaving vertical and horizontal stages.

---

## 6. Dependencies

- Requires `x86inc.asm` (included by the caller before this file); uses its `SWAP`, `mova/movu/movh`, `mmsize`, `cpuflag()` and register aliases.
- `cextern_common` is used by kernels that need symbols defined in the 8-bit build from the high-depth build and vice versa.
- Constants in `const-a.asm` must be declared with `cextern` in each kernel before use.

---

## 7. Constants and tables

- Derived constants: `FENC_STRIDE=16`, `FDEC_STRIDE=32`, `SIZEOF_PIXEL∈{1,2}`, `SIZEOF_DCTCOEF∈{2,4}`, `PIXEL_MAX=2^BIT_DEPTH-1`.
- Macros reference the shared constant vectors: `pw_1` (all 1), `pw_8`, `pw_16`, `pw_32`, `pw_512`, `pw_00ff` (byte low masks), `pw_pixel_max`, `pb_0`, `pb_3`, `pd_8`, `pd_32`.
- `SHUFFLE_MASK_W` produces byte-pair masks for word-level `pshufb` operations; negative indices produce the `0x80` "set zero" byte.

---

## 8. Performance notes

- `ABSW`/`SPLAT*`/`PALIGNR`/`HADD*` all have SSSE3/AVX/AVX2 fast paths; a rebuild that omits them would be correct but materially slower on modern CPUs.
- The 32-bit transpose spill path exists because of the eight-register MMX limit; on x86-64 the register-rich path avoids all spills.
- `SBUTTERFLY` with the AVX2 `dqqq` variant uses `vperm2i128`+`vinserti128` instead of multiple `punpck*`, reducing uops in 8x8 word transposes.
- `LOAD_DIFF8x4`'s SSSE3 `pmaddubsw` path computes four rows' signed differences with half the instructions of the generic unpack path.

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- On AVX2, `SPLAT*` maps to a single `vpbroadcast*`; on AVX-512 the same macros can use embedded broadcast (`{1toN}`) directly in an arithmetic instruction, removing a register and an instruction — a concrete opportunity for kernels such as weighted prediction (see `mc-a.asm` spec).
- 512-bit `psadbw`/`pmaddubsw` double throughput per instruction but require combining eight 64-bit lanes at reduction time; `HADDD`'s staged `vextracti32x8`/`vextracti128` sequence is the current pattern (Intel Intrinsics Guide).
- Bottleneck: `PALIGNR` at 512-bit needs a 64-byte partner register and `vpermt2b`-style shuffles; the macro's AVX2-only precomputed-partner strategy must be extended for a true AVX-512 kernel. UNKNOWN whether the reference supplies such an extension (none appears in this file).
- Bottleneck: frequent `SWAP` permutations are free at runtime but can create register-pressure peaks in x4 SAD; AVX-512's 32 registers alleviate this (Agner Fog, *Optimizing subroutines in assembly language*).
- Frequency throttling on wide vectors as noted in the `x86inc` spec (Intel Optimization Reference Manual).

---

## 9. References

- Intel Intrinsics Guide (`pshufb`, `pmaddubsw`, `psadbw`, `vpbroadcast*`, `vpcompress*`).
- Intel SDM Vol. 2 (instruction semantics and encoding).
- Agner Fog, *Optimizing subroutines in assembly language* (instruction tables, dependency chains).
- x264 project x264asm documentation / source header commentary (macro intent).

---

## 10. Verification checklist

1. For each `INIT_*` level, assemble a small kernel that invokes every utility macro and disassemble to confirm the expected instruction form (e.g. SSSE3 `pabsw`, AVX2 `vpbroadcastw`, no legacy `pshufw` on XMM).
2. Check `FIX_STRIDES` doubles registers only in the high-depth build.
3. Unit-test `TRANSPOSE4x4W/8x8W` by feeding a known matrix and checking the output ordering, in both the x86-64 and 32-bit paths.
4. Check `ABSW` in all three forms (SSSE3, distinct dst, in-place, `sign`) against a scalar absolute value for the full int16 range including `-32768`.
5. Check `HADDW`/`HADDUW` reductions against scalar sums for random vectors, including 8/16/32/64-byte widths.
6. Differential test of every kernel that uses these macros against the C reference through `checkasm`, for each ISA variant.

---

## 11. Open questions

- Whether a 512-bit-specific `PALIGNR` fast path is required for a faithful rebuild: not present in the reference file; UNKNOWN if any calling kernel needs it.
- `LOAD_DIFF_8x4P` default parameters (`r0,r2,0,1`) are used by only a few kernels; exact edge behaviour when `SIZEOF_PIXEL=2` is UNKNOWN beyond the generic macro expansion.
- The precise performance claims in the file's comments (e.g. relative cost of unaligned loads on Bulldozer) are historical; UNKNOWN how they map to current hardware, but they explain the chosen instruction sequences.
