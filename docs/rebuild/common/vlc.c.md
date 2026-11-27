# `common/vlc.c` - runtime-generated CAVLC lookup tables

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\vlc.c` (mirrored at `common/vlc.c`). Role in one sentence: builds, once per encoder instance, the two runtime CAVLC lookup tables - the level/suffix code table and the packed zero-run code table - from H.264's fixed prefix/suffix code rules. Marker: **dirty-side behavioural specification**.

## 2. Purpose

H.264 CAVLC codes level and run values using prefix/suffix structures that can be encoded compactly as tables. Static tables would be large for the full level range; instead the encoder derives two arrays at initialisation: a per-suffix table indexed by a clipped level, carrying code bits, code length and the next suffix to use; and a 64K-entry table indexed by a 16-bit nonzero bitmap, carrying a complete zero-run bit sequence plus its length in a single 32-bit word.

## 3. Public interface

- `void x264_cavlc_init(x264_t *h)`: fills both global tables. Must be called before CAVLC bitstream writing/analysis; the tables are shared by all encoder instances and are safe to rebuild identically.
- Exported globals declared in `bitstream.h`: `vlc_large_t x264_level_token[7][LEVEL_TABLE_SIZE]` and `uint32_t x264_run_before[1<<16]`. Names are bit-depth templated. `LEVEL_TABLE_SIZE` is 128, so the level table covers signed values -64..63.
- `vlc_large_t` has `uint16_t i_bits`, `uint8_t i_size`, `uint8_t i_next`: the code value, its bit length, and the suffix-table index to use for the next coefficient of the same block.

## 4. State and data structures

The level table is indexed as `[suffix][level + 64]`. The run table is indexed by a 16-bit bitmap whose set bits mark nonzero coefficients of a 4x4 block in zigzag order; entry value packs `(bits << 5) + size`, relying on the fact that the longest zero-run code sequence is 25 bits and therefore fits with a 5-bit length field.

Temporary state inside the builder: a 16-element `dctcoef` array (16-byte aligned) used to feed the CPU-dispatched `coeff_level_run[DCT_LUMA_4x4]` function, an `x264_run_level_t` receiving the last nonzero index and the coefficient list, and a rotating 32-bit mask used to find zero runs.

## 5. Algorithm / behaviour

**Level table generation.** For suffix index s = 0..6 and level = -64..63:
- Derive the sign mask from the arithmetic right shift of `level >> 15`; compute `abs_level = (level ^ mask) - mask`, i.e. magnitude with sign removed without a branch.
- Convert magnitude to the H.264 level code: `abs_level ? 2*abs_level - mask - 2 : 0`, where `mask` is 0 for positive and -1 for negative; this maps +1/-1 to code 0, +2/-2 to code 2/1, and so on.
- Let `q = level_code >> s` (the number of suffix-shifted prefix steps).
- If `q < 14`: a normal prefix/suffix code of length `q + 1 + s`, whose bit pattern is `(1<<s) + (level_code & ((1<<s)-1))` - a run of q ones, a zero, then the s suffix bits.
- Else if `s == 0` and `level_code < 30`: the special 13-prefix escape of length 19, bits `(1<<4) + (level_code-14)`.
- Else if `s > 0` and `q == 14`: length `15 + s`, same suffix-bit construction (the 14-prefix case).
- Otherwise: the long escape, `level_code -= 15<<s` (and another 15 when s == 0), length 28, bits `(1<<12) + level_code`.
- Determine the next suffix index: start at s, force at least 1 for s == 0, then increment while `abs_level > (3 << (next-1))` and `next < 6`. Store all three fields.

This implements H.264's rule that the suffix table grows with coefficient magnitude up to a cap, so large levels use longer suffixes to keep prefix runs short.

**Run-before table generation.** Entry 0 and 1 are zero (no zero runs possible for those bitmaps). For each 16-bit bitmap i = 2..65535:
- Build a 16-element coefficient array whose j-th entry is 1 if bit j of i is set, else 0.
- Call `h->quantf.coeff_level_run[DCT_LUMA_4x4]` to obtain the number of nonzero coefficients and the index of the last one (`runlevel.last`).
- Compute `zeros = last + 1 - total`, the number of leading zero coefficients (run before the first nonzero).
- Build a rotating mask `i << (clz(i)+1)` so that successive leading-zero runs can be counted with `clz`.
- For each gap until zeros are exhausted, choose the run-code table index `min(zeros,7)-1`, read the run length from `clz(mask)`, append the corresponding `x264_run_before_init[idx][run]` bits and length, subtract the run from the remaining zeros, and shift the mask past the run and its terminating one.
- Store `(bits << 5) + size`.

The auxiliary `x264_run_before_init[7][16]` is a static H.264 table (see `tables.c.md`) of prefix/suffix codes for zero runs of length 0..15 with a total-zero context of 1..7. The generated table collapses the entire zero-run prefix sequence for a 4x4 block into one lookup, which is the speed point.

### Additional behavioural detail

**Level-code mapping examples.** The sign mask is minus one for negative levels and zero for nonnegative ones. Magnitude is obtained without a branch by XOR with the mask and subtraction of the mask. The level code is even for positive levels and odd for negative levels, starting at zero for magnitude one: +1 -> 0, -1 -> 1, +2 -> 2, -2 -> 3. This is the standard's `levelCode` mapping; the tables here store the complete bit pattern rather than the code number.

**Suffix progression.** A suffix of zero means no low bits are appended; each successive suffix adds one suffix bit and shifts the prefix threshold. The next-suffix rule uses the threshold `3 << (next-1)`: a magnitude above that value advances to the next table, up to a maximum index of five. This makes small coefficients use compact codes while large coefficients use longer suffixes, which is exactly the trade-off in H.264's `coeff_token`/level coding.

**Escape branches.** Three branches beyond the normal prefix/suffix code exist: a special 19-bit code for suffix zero and level code below 30, a 15+s-bit code for the prefix-14 case with a nonzero suffix, and a 28-bit escape for everything else. The 28-bit escape subtracts `15<<s` (and an extra 15 for suffix zero) before storing the residual in the low 12 bits. These boundaries are where a reimplementation is most likely to diverge; verify each boundary with reference vectors.

**Run-before generation walkthrough.** For a 16-bit bitmap, the builder obtains the number of nonzero coefficients and the index of the last one from the CPU-dispatched coefficient scanner. The difference `last + 1 - total` is the leading zero run. A mask is formed by shifting the bitmap left past its leading one; successive `clz` calls then count the zero gaps between subsequent nonzero coefficients. For each gap, the code table is selected by `min(remaining_zeros,7)-1` and the run value by `clz(mask)`. Bits are accumulated by shifting left and OR-ing, and the total length is accumulated separately. The final packed word reserves five bits for the length, justified by the comment that the longest possible run sequence is 25 bits.

**Why the 128-entry level table is enough.** Levels beyond the table range are clipped by the caller before indexing. The comment in `bitstream.h` records that larger tables help only at extremely high bitrates and that the chosen size was empirically optimal at QP18 on a Nehalem CPU. The table is therefore a cache-size decision, not a standard requirement.

**Global-table implications.** Both tables are globals written by `x264_cavlc_init` with no synchronisation. In a single-process multi-encoder scenario the writes are idempotent, so results are correct in practice, but a strict clean-room implementation should either make them thread-local or initialise them once with a guarded flag. Any change must preserve the exact numeric contents because they are consumed by both C and SIMD CAVLC writers.

## 6. Dependencies

Includes `common.h`. Uses `x264_clz` from `osdep.h`, `ALIGNED_ARRAY_16`, `h->quantf.coeff_level_run[DCT_LUMA_4x4]` (CPU-dispatched in `quant.c`/SIMD), `x264_run_before_init` from `tables.c`, and `h->quantf` function selection which must already be initialised. Called from encoder open before CAVLC writing/analysis; shared mutable globals are written by every encoder instance with identical results.

## 7. Constants and tables

`LEVEL_TABLE_SIZE = 128` (chosen as an empirical cache-size/quality trade-off at QP18 on Nehalem; larger tables help only at extreme bitrates but cost cache). Prefix thresholds 14 and 30; escape lengths 19 and 28; suffix cap 5 (7 tables indexed 0..6). Zero-run length field is 5 bits inside the 32-bit packed word. `x264_run_before_init` has 7 rows by 16 columns.

## 8. Performance notes

- **Cache layout:** the level table is 7*128 entries of 4 bytes = 3.5 KiB, fitting in L1; the run table is 64K * 4 bytes = 256 KiB, intentionally consulted only in the slower CAVLC paths or cached per block. Keep the level table's compact layout.
- **Allocation:** both tables are static globals; no per-frame allocation. The builder uses stack arrays only.
- **Dispatch hierarchy:** generation calls the CPU-dispatched `coeff_level_run`; therefore `x264_cavlc_init` must run after CPU function tables are selected, otherwise results could differ between builds (they should not, but the dependency is real).
- **False sharing:** tables are written at init and read-only thereafter; safe to share across threads.
- **Determinism:** because the derivation is purely integer and based on fixed tables, results are identical across CPU paths provided `coeff_level_run` returns the same last index.
- Public references: ITU-T H.264 clause 9.2.2 (CAVLC), Tables 9-1 (coeff_token), 9-5 (total_zeros), 9-6 (run_before); Intel Optimization Reference Manual on table locality.

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10, clause 9.2.2 and its VLC tables (`coeff_token`, `level_code`, `total_zeros`, `run_before`).
- `bitstream.h` declarations and comments explaining the 25-bit/5-bit packing and the 128-entry choice.
- `tables.c.md` in this directory for `x264_run_before_init`.
- Drepper, *What Every Programmer Should Know About Memory* (2007), on lookup-table cache footprint.

## 10. Verification checklist

- For small positive and negative levels, verify the generated code length and bits against the H.264 level-code table: +1/-1 -> code 0, +2 -> 2, -2 -> 1, and so on, for suffix indices 0..6.
- Verify boundary transitions: `q = 13`, `q = 14`, `level_code` just below/above 30 for suffix 0, and the long-escape branch; lengths must be 14+s, 15+s, 19 or 28 as appropriate.
- Verify `i_next` is monotone in magnitude, never exceeds 6, and is at least 1.
- For random 16-bit bitmaps, verify the generated run-before word decodes to a sequence of zero-run codes whose total consumed zeros equals the number of leading zeros in the bitmap (using an independent decoder).
- Verify entries 0 and 1 of `x264_run_before` are zero.
- Run `x264_cavlc_init` twice on the same encoder and verify tables are byte-identical (idempotence).
- Encode a short CAVLC stream and compare with a reference encoder's bitstream for the same parameters (black-box end-to-end check).

Additional black-box checks:

- Verify every level-table entry's `i_size` is at least 1 and at most 28, and that `i_next` is between 1 and 6.
- For each suffix table, verify that increasing level magnitude never decreases `i_size` (monotonicity within a table).
- Verify that the special 19-bit branch is used only for suffix zero and level code between 14 and 29 inclusive.
- Verify that for suffix six and large magnitudes the long-escape branch produces a 28-bit code and that the residual fits in 16 bits.
- Pick ten random 4x4 coefficient bitmaps, decode the generated `x264_run_before` word with an independent CAVLC decoder, and verify the reconstructed leading zeros match the original bitmap.
- Verify that callers can use the generated table for chroma DC blocks with the appropriate `total_zeros` context without out-of-range indexing.
- Verify a second encoder instance rebuilt with a different CPU feature mask produces byte-identical tables (proves the generation is CPU-independent).
- Verify that no table entry changes after an encode, by snapshotting the globals before and after.

## 11. Open questions

- UNKNOWN: whether the level table is safe to generate concurrently by two encoder instances; it is a global written without locking, so concurrent initialisation could race (identical values, but technically a data race).
- UNKNOWN: exact derivation of the `x264_run_before_init` constants is standard-defined but not reproduced here; see `tables.c.md`.
- UNKNOWN: behaviour for levels outside -64..63 is not covered by the 128-entry table; callers must clip before indexing.
