# bitstream.c — dirty-side behavioural specification

Source path: `common/bitstream.c`
Role: turns already-encoded NAL payloads into final byte streams (Annex-B start codes or length-prefixed MP4 form) and performs start-code-emulation prevention; also selects the CPU-specific helpers used for that work.

> Dirty-side behavioural specification. Describes observable behaviour and structure only; no source text is reproduced.

## 1. Header / identity

This file is the last stage of the H.264 output pipeline. It does **not** write syntax elements, Exp-Golomb codes, or CABAC bits — those are produced earlier into a temporary payload buffer. This file takes one completed `x264_nal_t` payload and copies it into the caller's output buffer, inserting the required wrapper (start code or 32-bit size) and applying the H.264 "emulation prevention" byte stuffing rule. A secondary responsibility is a one-time dispatch function that picks the fastest available implementation of the byte-stuffing routine and of the CABAC coefficient-scattering routines (the latter are implemented in architecture-specific files but selected here).

## 2. Purpose

Two container conventions must be supported without changing the encoder core:

* **Annex B** (elementary stream): every NAL unit is preceded by a three-byte `00 00 01` start code, or a four-byte `00 00 00 01` start code for selected NALs (the first NAL of an access unit / SPS / PPS / SEI, flagged by the caller through `b_long_startcode`).
* **Length-prefixed** (AVCC / MP4): every NAL unit is preceded by a big-endian 32-bit length that excludes the length field itself.

Beyond the wrapper, H.264 requires that the byte sequence inside a NAL never accidentally look like a start code. Therefore any occurrence of two consecutive zero bytes followed by a byte `<= 0x03` must have an extra `0x03` byte inserted after the two zeros. This file implements that rule (the "RBSP to EBSP conversion") and reports the resulting final size. It additionally supports AVC-Intra, where NALs are padded to a fixed class-dependent size.

## 3. Public interface

All names below are exported through `common.h`/`bitstream.h` with the usual `x264_template` renaming.

### `x264_nal_encode( x264_t *h, uint8_t *dst, x264_nal_t *nal )` → `void`

Contract:

* `dst` points to the destination buffer with enough room for the wrapper, NAL header, escaped payload, and possible AVC-Intra padding. The source and destination are allowed to be the same buffer region; the copy direction is forward-safe.
* `nal->p_payload` and `nal->i_payload` describe the **input** RBSP payload (no emulation prevention yet). On return they are overwritten with `orig_dst` and the **final** byte count, so the same descriptor becomes the output NAL.
* `nal->i_ref_idc` (0–3) and `nal->i_type` (1–31) form the one-byte NAL header; the forbidden-zero-bit is always zero.
* `nal->b_long_startcode` requests a four-byte start code in Annex-B mode; ignored in length-prefixed mode.
* `nal->i_padding` is both an input (padding carried over from a previous NAL, in AVC-Intra mode) and an output (unsatisfied padding to be applied to the next NAL). It is unused when AVC-Intra is off.
* The function leaves the FP/MMX state clean by calling `x264_emms()` before returning.

### `x264_bitstream_init( uint32_t cpu, x264_bitstream_function_t *pf )` → `void`

Contract:

* `cpu` is the capability bit mask; `pf` is an output dispatch table. The whole table is zeroed first, so unused pointers are `NULL`.
* Always sets `pf->nal_escape` to a scalar implementation, then upgrades it according to CPU features.
* On 64-bit x86 it also installs default SSE2 implementations of the three CABAC block-residual helpers and upgrades them with LZCNT / SSSE3 / AVX2 / AVX-512 where available. On other architectures those pointers stay `NULL`, meaning callers must use the generic C path.

### `nal_escape_c( dst, src, end )` → `uint8_t *` (internal, function pointer target)

Contract: copies bytes from `[src, end)` to `dst`, inserting `0x03` as required, and returns the new `dst` (one past the last byte written). The two bytes immediately before `dst` on entry are assumed to belong to the already-written NAL header / previous output; at least the NAL header byte is always present.

## 4. State and data structures

* `x264_t::param.b_annexb` — selects Annex-B (`1`) or length-prefixed (`0`) output.
* `x264_t::param.i_avcintra_class` — nonzero selects AVC-Intra padding mode (class 50/100/200/300/400).
* `x264_t::bsf.nal_escape` — function pointer installed by `x264_bitstream_init`; signature `(uint8_t *dst, uint8_t *src, uint8_t *end) -> uint8_t *`.
* `x264_t::bsf.cabac_block_residual_internal`, `..._rd_internal`, `..._8x8_rd_internal` — three function pointers for coefficient-to-CABAC scattering, selected here but used by the CABAC slice writer.
* `x264_nal_t` fields consumed/produced: `p_payload`, `i_payload`, `i_padding`, `i_ref_idc`, `i_type`, `b_long_startcode`.
* `NALU_OVERHEAD` equals **5** (assumed cost of start code + NAL type per NAL) and is used in the AVC-Intra padding equation. It is defined in `common.h`; its value is a fact, not a copied table.
* Local pointer arithmetic: `orig_dst` remembers the start of this NAL's output so that both the length field and the returned descriptor can be patched afterwards.

Lifetime: all state is transient within one call. The dispatch table is initialized once per encoder instance; its function pointers remain valid for the encoder lifetime.

## 5. Algorithm / behaviour

### 5.1 Byte-stuffing filter (`nal_escape_c`)

The filter works on a byte stream, looking only at the last two bytes already written to the destination. Denote the destination cursor `D` and the source cursor `S`, with end pointer `E`.

1. **Warm-up:** the first two source bytes, if present, are copied verbatim. This is safe because when the first source byte is written the only preceding output byte is the NAL header, and when the second source byte is written the two preceding output bytes are the NAL header and the first source byte. A valid NAL header has its top (forbidden) bit zero and a nonzero type, hence is nonzero, so the "two preceding zeros" condition can never be satisfied inside the warm-up for any legal NAL. (This explains why a special case is possible; the code does not inspect the header.)
2. **Main loop:** while `S < E`:
   * Let `p = S[0]` be the next source byte.
   * Let `z2 = (D[-2] == 0)` and `z1 = (D[-1] == 0)` be the last two emitted bytes.
   * If `p <= 0x03` **and** `z2` **and** `z1`, emit one extra byte `0x03` at `D`, advancing `D`.
   * Emit `p` at `D`, advancing `D`; advance `S`.
3. Return `D`.

Decision table for the byte following a `00 00` pair (the only case where insertion happens):

| Next source byte | Emitted bytes | Reason |
|---|---|---|
| `00` | `03 00` | prevent `00 00 00` start-code-like sequence |
| `01` | `03 01` | prevent `00 00 01` start code |
| `02` | `03 02` | prevent `00 00 02` |
| `03` | `03 03` | required by the standard so the escape byte itself cannot be confused |
| `04`–`ff` | unchanged | no start-code-like pattern possible |

Worked example: input `00 00 00 01` after a nonzero NAL header produces output `00 00 03 00 01`. Input `00 00 01 02 03` produces `00 00 03 01 02 03`. Input `00 00 00 00` produces `00 00 03 00 00` (after the insertion, the previously emitted byte is `03`, so the fourth zero is not escaped again).

Important consequences:

* The check looks at the **destination**, not the source. Once a `03` has been inserted it breaks the zero run, so consecutive `00` sources never produce consecutive `03` insertions.
* The threshold is inclusive (`<= 3`), not exclusive; escaping `03` itself is mandatory in H.264.
* The filter is linear in the payload length and touches each output byte once; no lookahead beyond two bytes is required.

### 5.2 NAL encapsulation (`x264_nal_encode`)

Let `O` be the original destination pointer.

1. **Wrapper selection.**
   * Annex B: emit three bytes `00 00 01`; if `b_long_startcode` is set, emit one additional leading `00` so the sequence is `00 00 00 01`.
   * Length-prefixed: advance the cursor by 4 bytes, reserving space for a big-endian 32-bit size field that is filled in later. No zero bytes are written here.
2. **NAL header:** emit one byte formed as `(0 << 7) | (i_ref_idc << 5) | i_type`. The forbidden-zero bit is always 0; bits 6–5 carry `nal_ref_idc`; bits 4–0 carry `nal_unit_type`. The caller is responsible for `i_type` fitting in five bits.
3. **Payload:** call the selected `nal_escape` worker with the cursor as destination and `[nal->p_payload, nal->p_payload + nal->i_payload)` as source. The worker performs §5.1.
4. **Size computation:** `S = cursor - O`. This includes the wrapper, the NAL header, and the escaped payload.
5. **AVC-Intra padding (only if `i_avcintra_class != 0`):** compute
   `pad = nal->i_payload + nal->i_padding + NALU_OVERHEAD - S`.
   If `pad > 0`, zero-fill `pad` bytes at the cursor and increase `S` by `pad`. Then set `nal->i_padding = max(pad, 0)`, i.e. the amount written now is consumed and any deficit is carried to the next NAL. The `i_payload` used in the equation is the **original input** length, because `nal->i_payload` has not yet been overwritten. The effect is that the sum of payload bytes plus overhead across a group of NALs is rounded up to a fixed multiple implicitly chosen by the caller's accounting.
6. **Length field (length-prefixed only):** let `chunk = S - 4` (the size excludes the four size bytes themselves). Write `chunk` big-endian into `O[0..3]`: most significant byte first, least significant byte last.
7. **Descriptor update:** set `nal->i_payload = S` and `nal->p_payload = O`. Thus after the call the descriptor points at the complete NAL (including wrapper) and its total length.
8. **State hygiene:** call `x264_emms()` to clear any pending MMX/SSE state before returning to ordinary C code.

Edge cases:

* Empty payload (`i_payload == 0`): the warm-up copies nothing; the output is just the wrapper plus NAL header.
* Payload shorter than two bytes: only the first byte (or none) is copied verbatim; the main loop never runs.
* In Annex-B mode the size field is not written; the `i_payload` reported is still the total byte count of the emitted NAL including start code.
* In length-prefixed mode the reserved four bytes are counted in `S` but not in `chunk`; this is why `chunk = S - 4`.

### 5.3 CPU dispatch (`x264_bitstream_init`)

The dispatch is a strictly ordered series of overrides: a more capable feature overrides a less capable one, and later checks override earlier ones. The ordering is:

1. Zero the whole table; set the scalar `nal_escape_c`.
2. `HAVE_MMX` builds:
   * On 64-bit x86, set the three CABAC residual pointers to their baseline SSE2 versions **before** any feature tests.
   * `MMX2` → `nal_escape_mmx2`.
   * `SSE2` combined with the "SSE2 is fast" hint → `nal_escape_sse2`. (A slow-SSE2 CPU, e.g. some early Pentium 4, keeps MMX2.)
   * `LZCNT` → upgrade all three CABAC residual pointers to LZCNT variants.
   * `SSSE3` → upgrade the two RD residual pointers; if `LZCNT` is also present, upgrade them again to combined SSSE3+LZCNT variants.
   * `AVX2` → upgrade `nal_escape` to AVX2 and the non-RD residual pointer to AVX2.
   * `AVX512` → upgrade all three residual pointers to AVX-512 variants. `nal_escape` is not upgraded by AVX-512 (the AVX2 version remains).
3. ARM builds: `HAVE_ARMV6` with `NEON` and `HAVE_AARCH64` with `NEON` both select `nal_escape_neon`.

The dispatch is deterministic for a given capability mask; there is no runtime measurement or fallback probing.

## 6. Dependencies

* **Called by:** the encoder's NAL assembly / output routine (in `encoder.c`), for every NAL unit written to the stream — SPS, PPS, SEI, slice data, filler, AUD, end-of-sequence.
* **Calls:** `h->bsf.nal_escape` (function pointer set by this file), `x264_emms()` (FP-state cleanup), plus the selected CABAC residual helpers from `common/x86/*.asm` or `common/arm/*.asm`, which are only invoked later by the slice writer.
* **Reads:** `x264_t::param` (Annex-B flag, AVC-Intra class), `x264_nal_t` fields, CPU capability mask.
* **Shared constants:** `NALU_OVERHEAD` from `common.h`; architecture feature flags (`X264_CPU_*`) from `cpu.h`.
* **Relationship to `bitstream.h`:** the inline bit writer (`bs_t`) and Exp-Golomb size helpers live in the header and are used *before* this file; this file never sees `bs_t`.

## 7. Constants and tables

| Symbol | Value | Meaning / why |
|---|---|---|
| Annex-B short start code | `00 00 01` (3 bytes) | Standard H.264 delimiter; minimal legal form. |
| Annex-B long start code | `00 00 00 01` (4 bytes) | Used for the first NAL of an access unit and parameter sets; improves decoder resynchronisation. |
| Size prefix | 4 bytes, big-endian | MP4/AVCC convention; excludes the prefix itself. |
| NAL header | `(ref_idc << 5) \| type` | forbidden-zero bit fixed at 0. |
| Emulation-prevention byte | `0x03` | Inserted after two zeros when the next byte is `<= 0x03`. |
| Escape threshold | `0x03` inclusive | Must also escape literal `03` bytes. |
| Warm-up bytes copied verbatim | 2 | See §5.1 rationale concerning the NAL header. |
| `NALU_OVERHEAD` | 5 | Assumed fixed wrapper cost in AVC-Intra padding accounting. |
| `LEVEL_TABLE_SIZE` | 128 | Declared in `bitstream.h`; size of the CAVLC level-table family (not used here). |
| Dispatch order | scalar → MMX2 → SSE2(fast) → AVX2 → NEON | Monotonic capability ordering; AVX-512 affects only CABAC residual helpers. |

No data tables are defined in this file; all behaviour is driven by the byte-level rule and the dispatch sequence.

## 8. Performance notes

Hotness: `x264_nal_encode` runs once per NAL — a handful of times per frame for typical content, plus possibly one slice per row in sliced-threads mode. The escaped byte copy is memory-bound but touches only the final output, so it is secondary to transform/quantisation for normal presets. It becomes relevant at very high bitrates, with many slices, or with large SEI/filler NALs (AVC-Intra padding can inflate NALs substantially).

Concrete optimisation opportunities (each paired with a public reference):

1. **SIMD byte-stream scattering / stuffing.** The scalar loop has a data-dependent branch per byte. SIMD versions compare a vector of source bytes against `<= 3`, detect zero pairs using shift-and-mask operations, build a mask of insertion positions, and then expand the vector by scattering bytes into two or three output vectors. On x86 this maps naturally to `_mm_cmpeq_epi8`, `_mm_cmplt_epi8` / `_mm_cmpgt_epi8`, `_mm_movemask_epi8`, `palignr`, and on SSSE3+ `_mm_shuffle_epi8` for the compaction/expansion step; AVX2 uses the 256-bit analogues. On ARM NEON the analogous primitives are `vceqq_u8`, `vcltq_u8`, `vbslq_u8`, `vqtbl1q_u8`. See the Intel Intrinsics Guide (https://www.intel.com/content/www/us/en/docs/intrinsics-guide/) and the ARM NEON Intrinsics Reference (https://developer.arm.com/architectures/instruction-sets/intrinsics/). Public H.264 descriptions of the rule: ITU-T Recommendation H.264 §7.4.1 and §B.1.1 (start-code emulation prevention).
2. **Branch elimination via bit tricks.** Instead of a conditional insert per byte, compute a mask of "insert here" positions once per vector and derive output indices by prefix sums of that mask. This removes the data-dependent branch entirely and makes throughput independent of payload content — important because encoder output otherwise stalls on all-zero or all-`ff` payloads. General material: Intel® 64 and IA-32 Architectures Optimization Reference Manual, "Branch Prediction Optimization" chapter (https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html), and the classic branchless bit-hack catalogue at https://graphics.stanford.edu/~seander/bithacks.html.
3. **Batching NAL writes.** Because each NAL needs a wrapper and a size/count patch, writing NALs one at a time causes short, dependent stores. Accumulating several NALs into a contiguous output buffer and patching all length fields in a second pass improves store locality and reduces call overhead. This is especially effective in sliced-threads mode. Public guidance on store forwarding and write-combining: Intel Optimization Reference Manual, "Memory Optimization" chapter.
4. **Decoupling size accounting from emission.** The AVC-Intra padding formula needs only lengths, not bytes; a fast path can compute escaped lengths by counting zero-runs first and emit in a second pass, allowing the padding decision to be made before copying. This avoids a partial copy when padding is needed.
5. **CABAC residual scattering (selected here).** The `cabac_block_residual_*` pointers scatter coefficient levels into the arithmetic coder; SIMD implementations process several coefficients per iteration and use `lzcnt`/`bsr` for leading-zero counts and SSSE3 `pshufb` for nibble layout. Public reference: Intel Intrinsics Guide entries for `_lzcnt_u32` / `_mm_shuffle_epi8`, and the H.264 CABAC coefficient-coding specification (ITU-T H.264 Annex G / ISO-IEC 14496-10 Annex G).
6. **State hygiene cost.** `x264_emms()` is a single `emms` on x86; it must remain at the end of the function, not per NAL in a loop. Batching NALs gives one `emms` for many NALs.

Cache behaviour: the payload is read sequentially and written sequentially to a different region, so hardware prefetchers handle it well; the destination is usually in the final output bitstream buffer and may be reused by the muxer shortly after, keeping it in last-level cache.

## 9. References

* ITU-T Recommendation H.264 (ISO/IEC 14496-10), §7.4.1 — syntax and start-code emulation prevention; Annex B — byte-stream format. https://www.itu.int/rec/T-REC-H.264
* ISO/IEC 14496-12 / ISO BMFF — length-prefixed sample entry conventions used by MP4/AVCC. https://www.iso.org/standard/74428.html
* Intel Intrinsics Guide — vector compare, mask, and shuffle intrinsics used by SIMD emulators. https://www.intel.com/content/www/us/en/docs/intrinsics-guide/
* ARM NEON Intrinsics Reference. https://developer.arm.com/architectures/instruction-sets/intrinsics/
* Intel® 64 and IA-32 Architectures Optimization Reference Manual. https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
* Marpe, Schwarz, Wiegand, "Context-Based Adaptive Binary Arithmetic Coding in the H.264/AVC Video Compression Standard", IEEE TCSVT 13(7), 2003 — background for the CABAC helpers selected in this file.

## 10. Verification checklist (observable conformance checks)

These checks require only generated bytes, not internal state:

1. **Start code check (Annex B):** for every NAL, verify the output begins with `00 00 01`, and that `b_long_startcode` NALs begin with `00 00 00 01`. Verify no NAL reports a long start code when the flag was clear.
2. **Emulation-prevention completeness:** scan each emitted NAL (after the start code / size prefix and NAL header). Assert that no three-byte window equals `00 00 00`, `00 00 01`, `00 00 02`, or `00 00 03`. This is the single strongest black-box check.
3. **Minimality:** assert that whenever `03` appears immediately after `00 00`, the following byte is `<= 0x03`; an unnecessary `03` before a byte `>= 0x04` would indicate an over-aggressive filter.
4. **Round-trip decode:** feed the produced stream to an independent H.264 parser (e.g. `ffmpeg -i` or the JM reference decoder). Confirm that the recovered RBSP payload (after removing emulation-prevention bytes) is byte-identical to the original `nal->p_payload` input.
5. **Deterministic test vectors:** with a nonzero NAL header, check these source→emitted payload mappings:
   * `00 00 00` → `00 00 03 00`
   * `00 00 01` → `00 00 03 01`
   * `00 00 02 03` → `00 00 03 02 03`
   * `00 00 03 04` → `00 00 03 03 04`
   * `00 00 00 00` → `00 00 03 00 00`
   * `ff ff 00 01` → `ff ff 00 01` (no insertion; zero-run length 1)
6. **Length-prefixed mode:** read the first four bytes as a big-endian unsigned integer; assert it equals `nal->i_payload - 4` after the call, and that the NAL data following those four bytes starts with the NAL header byte `(ref_idc<<5)|type`.
7. **Descriptor contract:** after the call, assert `nal->p_payload == dst` and `nal->i_payload` equals the total number of bytes written for that NAL (including wrapper).
8. **AVC-Intra padding:** with `i_avcintra_class != 0`, assert the emitted NAL ends with a run of zero bytes of length `max(0, original_payload + carried_padding + 5 - unpadded_size)`, and that the returned `i_padding` equals `max(pad, 0)`. Across a sequence of NALs, assert no padding is silently dropped.
9. **Dispatch sanity:** on an x86-64 CPU with AVX2, verify that the installed `nal_escape` is the AVX2 variant and that its output is byte-identical to the scalar variant for the vectors in item 5; repeat for MMX2/SSE2/NEON where available.
10. **Empty payload:** verify output length is exactly 4 (long Annex-B) or 3 (short Annex-B), or 5 in length-prefixed mode (4 size bytes + header), and that the size field reads 1.

## 11. Open questions

* The exact reason for unconditionally copying the first two payload bytes is inferred from the NAL-header argument in §5.1; no explicit justification is present in the analysed file. For a *zero* NAL header byte (illegal/Unspecified type 0) behaviour would differ from a general per-byte filter — UNKNOWN whether any caller can produce that case.
* `NALU_OVERHEAD = 5` is taken from `common.h`; the precise AVC-Intra class-to-target-size mapping is implemented in the caller, not here — UNKNOWN from this file alone.
* Exact SIMD implementations of `nal_escape_mmx2/sse2/avx2/neon` and of the CABAC residual helpers are architecture-specific and were not part of the assigned files; equivalence with the scalar filter is assumed but not verified here — UNKNOWN.
* Whether `i_padding` can be negative on entry, and how the caller initialises it for the first NAL of a frame, is UNKNOWN from this file.
* The "SSE2 is fast" capability hint is set elsewhere; its heuristic is UNKNOWN.
