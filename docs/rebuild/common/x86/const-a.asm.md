# common/x86/const-a.asm — behavioural specification

**Role:** defines the shared SIMD constant vectors (broadcast scalars, shuffle masks, sign patterns and a population-count table) referenced by nearly every x86 kernel.
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/const-a.asm`
- Role: read-only data provider; emits global hidden objects into a read-only section using the `x86inc` `const` macro. No executable functions.
- Built once per bit-depth configuration (the `pw_pixel_max` value depends on `BIT_DEPTH`).

---

## 2. Purpose

SIMD kernels repeatedly need the same constants: all-ones, powers of two, sign masks, byte/word masks, shuffle indices and lookup tables. Rather than materialising them locally (costing registers and instructions), x264 exposes a single table of named, aligned vectors that kernels declare with `cextern` and load with `mova`. This file is that table.

---

## 3. Public interface

Each `const name, <data>` emits a symbol `x264_name` (subject to `mangle`) with hidden/private visibility and the given bytes. Kernels reference them as `[name]` after `cextern name`.

Named constants (grouped by element width):

- **Byte vectors:** `pb_1` (32 bytes of 1), `hsub_mul` (16 bytes alternating +1/−1), `pb_unpackbd1`, `pb_unpackbd2` (byte indices for unpacking), `pb_01` (8×{0,1}), `pb_0` (16 zeros), `pb_a1` (16×0xA1), `pb_3` (16×3), `pb_shuf8x8c` (shuffle mask for chroma intra prediction).
- **Word vectors:** `pw_1`, `pw_2`, `pw_4`, `pw_8`, `pw_16`, `pw_32`, `pw_64`, `pw_256`, `pw_512`, `pw_00ff`, `pw_pixel_max`, `pw_0to15`, `pw_m2`, `pw_32_0` (four 32s then four 0s), `pw_8000`, `pw_3fff`, `pw_ff00`, and sign patterns `pw_ppppmmmm`, `pw_ppmmppmm`, `pw_pmpmpmpm`, `pw_pmmpzzzz`.
- **Dword vectors:** `pd_1`, `pd_8`, `pd_32`, `pd_1024`, `pd_ffff`, `pd_0123`, `pd_4567`, `deinterleave_shufd` (dword indices 0,4,1,5,2,6,3,7).
- **Tables:** `popcnt_table` (256 bytes, each = number of set bits in its index), `sw_64` (one dword 64).

All data is emitted with `SECTION_RODATA 32`, i.e. 32-byte aligned (falling back to the `.rdata` section name on Windows targets).

---

## 4. State and data structures

- No mutable state; the section is non-executable and read-only.
- Ownership: global to the compiled library, one instance per build (per bit depth / symbol prefix). Lifetime = process lifetime.
- Invariants exploited by callers: vectors are aligned to 32 bytes (so `mova` is legal even for YMM reads), repeated-element vectors are exactly `mmsize` or a multiple thereof in length, sign patterns have fixed lane ordering.

---

## 5. Algorithm / behaviour

### 5.1 Layout rules
The file uses NASM `times` repetition to materialise each vector. Since the section is 32-byte aligned and each object is laid out consecutively, individual symbols may not themselves all start on a 32-byte boundary; callers that require alignment use `mova` only when the specific offset guarantees it, otherwise `movu`. UNKNOWN whether the reference relies on every symbol being naturally aligned (the `const` macro does not insert per-symbol alignment).

### 5.2 Derivations
- `pw_pixel_max` is computed as `(1 << BIT_DEPTH)-1`, so it is 255 in 8-bit builds, 1023 in 10-bit, 4095 in 12-bit. This is the sole bit-depth-dependent object.
- `popcnt_table[x]` is computed at assembly time by summing the eight bit tests of `x`, yielding the population count of each byte value (0..8).
- `hsub_mul` alternates +1 and −1 per byte, used with multiply-add to compute horizontal sums/differences.
- The `pw_` sign-pattern vectors encode repeating sequences of +1/−1 with different phases (four-plus/four-minus, two-plus/two-minus, alternating, and a 1,−1,−1,1 followed by zeros), used by Hadamard and DCT sign flips.
- `deinterleave_shufd` provides the dword index permutation used to regroup even/odd dwords after deinterleaving.
- `pb_shuf8x8c` maps source byte positions to repeated values for intra-prediction construction.
- `sw_64` is a 64-bit scalar used by the cacheline-split SAD paths as a shift amount.

### 5.3 Usage pattern
A kernel declares `cextern <name>` near its top; the `x86inc` `cextern` macro mangles the name so that high-depth and 8-bit builds can reference the same constant names with different contents. Constants are loaded with `mova` when aligned and `movu` otherwise; mask constants are typically held in a register across a loop.

---

## 6. Dependencies

- Includes `x86inc.asm` for `const` and `SECTION_RODATA`.
- Consumed by `sad-a.asm`, `sad16-a.asm`, `mc-a.asm`, `mc-a2.asm`, `dct-a.asm`, `predict-a.asm`, `quant-a.asm`, `deblock-a.asm` etc.
- `BIT_DEPTH` must be defined by the build before assembling for `pw_pixel_max` to have the correct value.

---

## 7. Constants and tables

See the interface list in section 3. Values are exact powers/sequences, not tunable. Two are derived rather than literal: `pw_pixel_max` and `popcnt_table`. `pb_a1` (0xA1) is a known harmless filler byte used where a nonzero pattern is needed to test masks.

---

## 8. Performance notes

- Centralising constants avoids per-function literal pools and lets hot loops keep masks in registers.
- Alignment to 32 bytes permits aligned YMM loads; a rebuild should preserve the section alignment.
- `popcnt_table` is a software fallback for CPUs without POPCNT; a modern rebuild can use the hardware `popcnt` instruction when `cpuflag(sse42)` (see `GPR_INSTR popcnt` in `x86inc`), but must keep the table for compatibility.

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- Broadcast scalars such as `pw_512`, `pw_32` can be embedded directly into an AVX-512 arithmetic instruction via `{1toN}` (e.g. `vpbroadcastw m3, t7d` in `mc-a.asm`), eliminating a load and a register. A rebuild targeting AVX-512 should prefer embedded broadcasts over loading these vectors.
- At 512 bits, 16-byte vectors like `pw_16` must be broadcast (`vbroadcasti128`/`vpbroadcastw`) rather than loaded as a 16-byte constant to fill a ZMM register; `mc-a.asm` does this for `pw_512` and `pw_8`.
- Bottleneck: constants such as `pw_0to15` are lane-position dependent; widening to 512 bits needs a different sequence (`vpbroadcasti32x8` of a 16-byte pattern) — not provided by the reference table, mark UNKNOWN for direct AVX-512 equivalents.
- Refer to Intel Intrinsics Guide for broadcast forms and Agner Fog's instruction tables for load-vs-embedded-broadcast costs.

---

## 9. References

- Intel SDM Vol. 2 (immediate broadcast forms, `pshufb`, `pmovmskb`).
- Intel Intrinsics Guide (`_mm512_set1_epi16`, embedded broadcast).
- x264 source public documentation on x264asm constant conventions.

---

## 10. Verification checklist

1. Assemble the file and inspect the emitted symbol table: every named constant must exist with the `x264_` prefix and hidden visibility.
2. Dump `.rodata`/`.rdata` and verify each vector's bytes against the definitions above (especially `pw_pixel_max` in 8/10/12-bit builds).
3. Verify `popcnt_table[i]` equals the Hamming weight of `i` for all 256 values.
4. Verify section alignment is 32 bytes with `objdump -h` / `dumpbin /headers`.
5. Compile all kernels against the rebuilt constants and run `checkasm` — any wrong mask produces immediate bit-exactness failures.

---

## 11. Open questions

- Individual symbol alignment beyond the 32-byte section start is UNKNOWN; the `const` macro does not emit per-symbol align directives, so callers must rely on object sizes being multiples of 16/32.
- Whether any kernel relies on `pb_a1`'s exact value beyond "nonzero" is UNKNOWN.
- Behaviour in builds with `BIT_DEPTH` undefined (defaults to 0, making `pw_pixel_max = 0`) is UNKNOWN/irrelevant to normal builds.
