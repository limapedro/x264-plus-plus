# `common/common.h` - encoder-wide internal types

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\common.h` (mirrored at `common/common.h`). Role in one sentence: the central internal header that defines the bit-depth-templated type system, the encoder context `x264_t`, the slice header and lookahead structures, and the inline pixel clamps and MV-predictor helpers. Marker: **dirty-side behavioural specification**.

Reading note: `base.h` supplies depth-independent primitives (aligned malloc, clz/ctz, `x264_log2`); this header adds everything whose definition depends on `BIT_DEPTH`, `CHROMA_FORMAT` or `HAVE_INTERLACED`. It is included by every algorithm translation unit, so its type definitions are the shared vocabulary of the whole encoder.

Structure of this document: §3 fixes the type system and constants (including worked QP values); §4 describes the `x264_t` context and its synchronisation boundaries; §5 derives the behaviour of the inline helpers and the geometry macros; §7 documents the indexing offset macros. The remaining sections cover dependencies, performance, references and observable verification.

---

## 2. Purpose

Where `base.h` supplies depth-independent primitives, `common.h` establishes the types that vary with build configuration (8 vs 10 bits, chroma format, interlacing) and aggregates the entire per-encoder state into one object. It is the contract between the encoder core and the algorithm modules (prediction, motion compensation, DCT, quantisation, deblocking, bitstream, rate control). It also documents the multithreading memory model: which fields of `x264_t` are copied from thread to thread, and which are thread-local.

For an independent I-slice implementation, three things here are load-bearing:

1. **The QP constants.** `QP_MAX_SPEC`, `QP_BD_OFFSET` and `SPEC_QP` govern quantiser wrap-around, chroma QP mapping and the CAVLC `mb_qp_delta` wrapping in `cavlc.c.md`.
2. **The chroma geometry macros.** `CHROMA_V_SHIFT`, `CHROMA_H_SHIFT`, `CHROMA_SIZE` and `FRAME_SIZE` determine plane dimensions and are referenced directly by the frame and entropy code.
3. **The `x264_t` synchronisation split.** Fields above the first boundary are snapshot-copied into frame threads; fields below the second boundary in `mb` are private. Misplacing a mutable field here produces non-deterministic output.

A fourth concern is the *compile-time specialisation* strategy: `BIT_DEPTH`, `CHROMA_FORMAT` and `HAVE_INTERLACED` are fixed when the library is built, so expensive-looking operations such as chroma geometry and pixel clamping reduce to constants or simple bit ops. An independent implementation that makes them runtime variables can still be bit-exact, but should be aware that the reference's performance depends on the compiler folding them away.

---

## 3. Public interface

Templating macros: `x264_template(w)` concatenates `x264_<BIT_DEPTH>_<w>`; it is applied to all API entry points (`x264_encoder_open`, `_encode`, `_close`, `_headers`, `_reconfig`, `_parameters`, `_delayed_frames`, `_maximum_delayed_frames`, `_intra_refresh`, `_invalidate_reference`, `_nal_encode`, plus internal functions such as `x264_log`, `x264_cqm_init`, `x264_frame_new`). `x264_encoder_open` is deliberately undefined before being redefined so that linking an incompatible bit-depth library fails.

Depth-dependent types and constants:

| 8-bit build | 10-bit build |
| --- | --- |
| `pixel = uint8_t`, `pixel4 = uint32_t`, `dctcoef = int16_t`, `udctcoef = uint16_t` | `pixel = uint16_t`, `pixel4 = uint64_t`, `dctcoef = int32_t`, `udctcoef = uint32_t` |
| `PIXEL_SPLAT_X4(x) = x*0x01010101`, `MPIXEL_X4 = M32` | `x*0x0001000100010001ULL`, `M64` |

`SIZEOF_PIXEL`, `CPPIXEL_X4`, `QP_BD_OFFSET = 6*(BIT_DEPTH−8)`, `QP_MAX_SPEC = 51+offset`, `QP_MAX = QP_MAX_SPEC+18`, `PIXEL_MAX = (1<<BIT_DEPTH)−1`, `X264_LOOKAHEAD_QP = 12+offset`, `SPEC_QP` (clamp to spec maximum), `X264_PCM_COST`. Chroma helpers: `CHROMA_H_SHIFT` (true for 420/422), `CHROMA_V_SHIFT` (true for 420), `CHROMA_SIZE`, `FRAME_SIZE(s) = s + 2*CHROMA_SIZE(s)`, `CHROMA444`. NAL overhead constants: 5 bytes per NAL, 6 for filler, and an SEI overhead that is reduced when Annex B has an existing NAL in this access unit. With `HAVE_INTERLACED` off, `MB_INTERLACED`, `SLICE_MBAFF` and `PARAM_INTERLACED` collapse to constant 0; with `CHROMA_FORMAT` defined at compile time, the chroma shifts become compile-time constants.

Declared functions: `void x264_log(x264_t *, int level, const char *fmt, ...)`, `void x264_cavlc_init(x264_t *)`, `void x264_cabac_init(x264_t *)`. Inline helpers: `x264_clip_pixel(int)` (clamp with branch-free bit trick), `x264_predictor_roundclip` (convert quarter-pel candidates to full pel with rounding +2>>2, drop zero/duplicate, clip to MV limits), `x264_predictor_clip` (clip quarter-pel candidates directly to limits shifted by 2), plus `mvsad_t { int sad; int16_t mv[2]; }`.

### 3.1 QP constant worked examples

With `BIT_DEPTH = 8`: `QP_BD_OFFSET = 0`, `QP_MAX_SPEC = 51`, `QP_MAX = 69`, `PIXEL_MAX = 255`, `X264_LOOKAHEAD_QP = 12`. With `BIT_DEPTH = 10`: `QP_BD_OFFSET = 12`, `QP_MAX_SPEC = 63`, `QP_MAX = 81`, `PIXEL_MAX = 1023`, `X264_LOOKAHEAD_QP = 24`. The extra 18 QPs above `QP_MAX_SPEC` form the "emergency" range used when rate control cannot meet a target; `nr_offset_emergency` is indexed by QP above the spec maximum.

`SPEC_QP(x) = min(x, QP_MAX_SPEC)`: for 8-bit `SPEC_QP(57) = 51`, for 10-bit `SPEC_QP(57) = 57`. A clean implementation must apply this clamp before deriving quantisation factors, otherwise the tables indexed by QP would be overrun. The CAVLC wrap uses `QP_MAX_SPEC+1` (52 for 8-bit, 64 for 10-bit) as the modulus; see `cavlc.c.md` §5.8.

---

## 4. State and data structures

`x264_slice_header_t` carries the H.264 slice header as a plain struct: SPS/PPS pointers, type `i_type`, first/last MB, PPS id, frame number, MBAFF/field flags, IDR id, POC and delta-POC fields, redundant pic count, direct-spatial flag, reference-index overrides, per-list reordering arrays (`idc`,`arg` per reference), prediction weights (`x264_weight_t` per ref/plane), MMCO commands, CABAC init idc, QP/QS deltas, deblocking control. It is created per picture and may be backed up (`sh_backup`) for the dec-ref-pic-marking SEI.

`x264_lookahead_t` holds the threaded lookahead: exit and active flags, keyframe analysis flag, `i_last_keyframe`, slicetype length, `last_nonb`, a thread handle and three synchronised frame lists `ifbuf` (input), `next` (pending), `ofbuf` (output).

`x264_frame_stat_t` accumulates per-frame coding statistics: MV/texture/misc bit counts, 19 macroblock-type counters, MB counts by type/ref/partition/CBP/prediction mode, direct scores, SSD for three planes and an SSIM accumulator with a sample count.

`struct x264_t` is the main context. Its fields, in order of logical grouping:

- Configuration and API: `param`, opaque `api` pointer, arrays of up to 128 frame-thread contexts and 16 lookahead-thread contexts, thread indices/phase, slice-row bounds and pass, `threadpool` and `lookaheadpool`, a mutex and condition variable for slice-thread barriers.
- Bitstream output: NAL array and capacity, flat bitstream buffer and size, a `bs_t` writer, plus `nal_buffer` for reallocation.
- Reconfiguration: `reconfig_h`, `reconfig`.
- **Thread-synchronisation boundary:** everything after this comment is copied between frame threads; everything after the second boundary in `mb` is thread-local or constant. The synchronised portion includes frame/POC counters, HRD/VBV clocks, quantisation-matrix pointers, cost tables, chroma-QP table, slice header, SPS/PPS, CABAC context, frame queues and reference lists, current `fenc`/`fdec`, and the `mb` fields up to `field_decoding_flag`.
- Macroblock state: dimensions/strides, search parameters, MV limits at quarter/full-pel granularity, neighbour availability masks and neighbour block types/coordinates, a pointer to the left-index table, then large per-MB arrays (`base`, `type`, `partition`, `qp`, `cbp`, intra prediction modes, non-zero counts, chroma modes, MVs/MVDs/refs/MVRs, skip flags, transform-size flags, slice table, field map, weighted reference buffers), current-block values, skip/reencode flags, QP/lambda state, direct-mode scaling factors and reference-map tables.
- Picture scratch: `fenc_buf[48*16]`, `fdec_buf[54*32]` (both 64-byte aligned), 4x4/8x8 backup and DCT buffers, psy-RD caches, pointers into source/reconstructed/reference planes (including half-pel variants), stride list.
- Neighbour cache: Scan8-indexed arrays for intra modes, non-zero counts (deliberately misaligned by 8 for assembly convenience), refs, MVs, MVDs, skip flags, direct MVs/refs, transform-size and skip neighbour counts, CBP neighbours, MBAFF extras and deblock strengths.
- Rate control pointer, cumulative statistics, noise-reduction state, subsampled-pixel table, per-thread scratch buffers and intra border backups, and CPU-dispatched function tables (`predict_*`, `pixf`, `mc`, `dctf`, `zigzagf`, `quantf`, `loopf`, `bsf`), the lookahead pointer and optional OpenCL handle.

Ownership: `mb.base` is the single allocation backing all per-MB arrays; `frame->base` plays the same role for a frame. Function-table pointers are borrowed from static tables. `fenc`/`fdec`/reference pointers are owned by frame queues and reference lists.

### 4.1 Synchronisation-boundary decision table

| Region | Copied to workers? | Typical contents | Rule for new fields |
|--------|--------------------|------------------|---------------------|
| Before first boundary | no | per-encoder config, thread contexts, output buffers | constant or master-only; never written by a worker |
| Between boundaries | yes (snapshot at frame start) | frame/POC counters, SPS/PPS, slice header, SPS-derived tables, `mb` up to `field_decoding_flag` | write before workers start; treat as read-only during the frame |
| After second boundary in `mb` | no | per-MB arrays, current-block values, neighbour caches, lambda state | private per worker; safe to mutate freely |

The snapshot copy is performed once per frame, before slice threads begin. It is cheaper than contended shared writes and gives each worker a consistent view. Any mutable field mistakenly placed in the copied region will be overwritten by the next snapshot, losing worker updates; any field that must be shared but placed in the private region will diverge across workers.

### 4.2 Function-table dispatch points

| Table | Selected by | Used by |
|-------|-------------|---------|
| `predict_*` | CPU + chroma format | intra prediction (`common/predict.c`) |
| `pixf` | CPU + bit depth | pixel SSD/SATD, deblock strength inputs |
| `mc` | CPU + bit depth | motion compensation, plane copy |
| `dctf` | CPU | forward/inverse DCT |
| `zigzagf` | CPU | zigzag scan and 8x8→4x4 CAVLC interleave |
| `quantf` | CPU + bit depth | quant/dequant, `coeff_level_run` for CAVLC |
| `loopf` | CPU + bit depth | deblocking filter |
| `bsf` | CPU | CABAC/CAVLC bitstream primitives |

All are chosen once at encoder open from `param.cpu` and stored directly in the context so the hot inner loops load a pointer rather than performing a global lookup.

---

## 5. Algorithm / behaviour

`x264_clip_pixel` clamps to `[0, PIXEL_MAX]` by testing `x & ~PIXEL_MAX` and, when negative, returning `(-x)>>31 & PIXEL_MAX`. Worked example (8-bit): `x = −1` gives `x & ~255 = −1` (negative) → `(1)>>31 & 255 = 255`? No: `−x = 1`, `1>>31 = 0`, so result 0 — the correct clamp for −1 is 0. `x = 256` gives `256 & ~255 = 256` (positive, non-zero) so the branch is taken and it returns... the formula's guard is `if (x & ~PIXEL_MAX) return (x >> 31) ? 0 : PIXEL_MAX` in the general form; the branch-free version returns `PIXEL_MAX` for positive overflow. The key contract is simply: result ∈ [0, PIXEL_MAX], with saturation for both signs.

`x264_predictor_roundclip` is used for full-pel candidate lists: it rounds each quarter-pel vector with `(v+2)>>2` (arithmetic shift for negatives), packs `(mx,my)` into a 32-bit word to reject the zero vector and duplicates of the preferred vector, then clips componentwise. `x264_predictor_clip` keeps quarter-pel precision and scales the bounds by four. Both return the number of accepted candidates, which is the loop bound for the motion-search predictor stage.

Worked example: candidate (mx,my) = (5,−3) quarter-pel → round `(5+2)>>2 = 1`, `(−3+2)>>2 = −1` → full-pel (1,−1). Candidate (1,1) quarter-pel → `(3>>2)=0`, `(3>>2)=0` → zero vector, dropped. Candidate identical to the preferred vector is dropped to avoid searching the same point twice.

The thread-synchronisation split is a central invariant: fields above the first boundary are snapshot-copied from the master context into each worker at the start of a frame so workers see a consistent view; fields below the second boundary are private and never copied. Consequently, any new mutable field placed in the copied region must be written before workers start, and any field updated by a worker must live in the private region or be protected by a mutex/atomic.

Quantisation-matrix pointers are organised as `[cqm][qp_mod_6][coef]` for decoding and `[cqm][qp][coef]` for trellis/deadzone; `nr_offset_emergency` is indexed by QP above the spec maximum. Cost tables hold QP-indexed lambda-derived MV/reference/mode costs, with separate full-pel tables. The CPU-dispatched tables are selected once at encoder open from `param.cpu`; calls go through these pointers in the hot inner loops, which is why they are stored directly in the context rather than looked up globally.

### 5.1 Chroma geometry equations

For a luma region of `s` samples, the chroma sample count per chroma plane is:

```
CHROMA_SIZE(s) = s >> (CHROMA_H_SHIFT + CHROMA_V_SHIFT)
FRAME_SIZE(s)  = s + 2*CHROMA_SIZE(s)
```

So for 4:2:0 (`H=V=1`): `CHROMA_SIZE(s) = s>>2` and `FRAME_SIZE(s) = 1.5s`; for 4:2:2 (`H=1,V=0`): `CHROMA_SIZE(s) = s>>1`, `FRAME_SIZE(s) = 2s`; for 4:4:4 and 4:0:0: `CHROMA_SIZE(s) = s`, `FRAME_SIZE(s) = 3s` or `s` respectively. The CAVLC writer uses `CHROMA_V_SHIFT` to choose the chroma-DC token category (`5 − V_SHIFT`) and the chroma-AC traversal step (`8 << V_SHIFT`), so a wrong shift here changes the bitstream, not just performance.

`CHROMA444` is true only for 4:4:4 (and is a compile-time constant in a fixed-format build). It selects `plane_count = 3` in the entropy writers: with `CHROMA444`, luma and both chroma planes are coded as equal-sized 4x4 blocks with *no separate chroma pass*; without it, `plane_count = 1` and chroma is handled by the dedicated DC/AC traversal. An implementation that sets this from the wrong field will write the chroma residuals in the wrong order.

`QP_BD_OFFSET` also enters the chroma QP mapping: the actual chroma QP is derived from the luma QP plus `i_chroma_qp_index_offset` (from the PPS, see `set.h.md`) and is clamped into the bit-depth-dependent range before indexing the chroma QP table. With `BIT_DEPTH = 10`, a PPS offset of −3 applied at luma QP 3 yields a chroma QP of 0 after clamping, not −6 — the clamp is essential because the tables have no negative-index entries.

### 5.7 Per-macroblock array layout

The `mb` sub-struct's large arrays are all indexed by macroblock address in raster order, with total length equal to the number of macroblocks in the padded picture. `base` is the single backing allocation. The arrays include: `type` and `partition` (decisions), `qp` and `cbp` (coding state), `intra4x4_pred_mode` and `intra_chroma_pred_mode` (prediction), `non_zero_count` (entropy context), `mv[2]`/`mvd[2]`/`ref[2]`/`mvr[2]` (motion), `skip`/`b_transform_8x8` (flags), `slice_id`/`field` (slice and MBAFF mapping), and weighted-reference buffers. Because they share one allocation, a clean implementation should compute each array's base as an offset into `mb.base` and keep them aligned to the width of their elements; the reference deliberately misaligns `non_zero_count` by 8 bytes for an assembly store trick, but a C implementation may align it normally.

### 5.8 Interlacing compile-time collapse

When `HAVE_INTERLACED` is disabled, `MB_INTERLACED`, `SLICE_MBAFF` and `PARAM_INTERLACED` are all defined as constant 0. This lets the compiler eliminate every MBAFF branch in the CAVLC writer and in motion compensation, including the `mb_field_decoding_flag` write and the separate field-plane border expansion. A clean implementation that keeps these as runtime variables must still produce bit-exact output with interlacing disabled, but will be slower.

### 5.2 Bits-per-symbol cost helpers

`bs_size_ue(codeNum)` and `bs_size_se(value)` are the sizing counterparts of the writers, returning the bit count without emitting. The RD helpers use them (e.g. `chroma_size_cavlc` starts from `bs_size_ue(chroma_mode)`). For codeNum `k`, `bs_size_ue(k) = 2*floor(log2(k+1))+1`; for `value = 0`, `bs_size_se = 1`, and otherwise it equals `bs_size_ue(2|value| − (value>0?1:0))`. A clean implementation should verify these against the writer on random values.

### 5.3 Slice-header field contracts

`x264_slice_header_t` is rebuilt for each picture and then copied into `h->sh`. Its fields fall into four groups:

| Group | Fields | Contract |
|-------|--------|----------|
| Identity | `sps`, `pps`, `i_type`, `i_first_mb`, `i_last_mb`, `i_pps_id` | `i_first_mb/i_last_mb` bound the slice in MB address order; PPS id selects the active PPS |
| Frame numbering | `i_frame_num`, `i_idr_pic_id`, `i_poc`, `i_delta_poc[2]`, `i_num_ref_idx_active[2]` | derived once per picture from the frame/POC counters; delta POC indexed by list |
| Reference/weighting | `i_ref_pic_list_reordering[2]`, `ref_pic_list_reordering[2][]` (`idc`,`arg`), `weight[2][][3]`, `weight_buf` | reordering commands and per-ref/per-plane prediction weights; count bounded by `X264_REF_MAX` |
| Coding controls | `b_cabac`, `cabac_init_idc`, `i_qp`, `i_sp_for_idr`, `sh->deblock[]`, `i_slice_qp_delta` | CABAC enable/init, initial QP and deblocking overrides |

`sh_backup` is a saved copy of the fields needed to emit an `dec_ref_pic_marking` SEI after the slice has moved on; it exists because the SEI is written in a different NAL than the slice header but must describe the same picture.

### 5.4 Lookahead structure contract

`x264_lookahead_t` coordinates slicetype decision, mbtree propagation and AQ offsets. It owns three synchronised frame lists: `ifbuf` (frames entering from the encoder), `next` (frames whose lookahead analysis is in progress), and `ofbuf` (frames ready to be encoded). Each frame carries its own lookahead state (`i_cost_est`, `i_cost_est_aq`, `i_intra_mbs`, `f_qp_offset`, `f_qp_offset_aq`, `b_scenecut`) in the `x264_frame_t` described in `frame.h.md`; the lookahead object itself holds only the clocks, flags and queue handles. The three lists transfer reference ownership — a frame is returned to the unused pool only after its consumer finishes with it.

### 5.5 NAL overhead constants

The header defines per-NAL overhead used by rate control to reserve bits: 5 bytes for a generic NAL (start code plus header, in the Annex-B accounting used here), 6 bytes for a filler NAL, and an SEI overhead that is reduced when an Annex-B NAL is already present in the access unit (because a start code is shared). These are *accounting* constants, not emitted bytes: the actual NAL emission is done in `encoder/encoder.c`. An implementation that ignores them will produce slightly different bit allocation at very low bitrates.

### 5.6 The `mvsad_t` helper type

`mvsad_t { int sad; int16_t mv[2]; }` pairs a motion-vector SAD (sum of absolute differences) with the vector that produced it. It is used by the motion-search predictor stage to return the best of a small candidate set. The `sad` is in pixel units at the search block size; the `mv` is in quarter-pel units. Because it is a plain aggregate returned by value, a clean implementation can reproduce it exactly without any layout concern.

---

## 6. Dependencies

Includes `base.h` first, then conditionally `opencl.h`, followed by `cabac.h`, `bitstream.h`, `set.h`, `predict.h`, `pixel.h`, `mc.h`, `frame.h`, `dct.h`, `quant.h`, `threadpool.h`; at the end includes `macroblock.h` (needs `x264_t`), optionally `x86/util.h`, and `rectangle.h`. Included by all encoder and algorithm translation units. It is the single largest internal interface surface in the project.

---

## 7. Constants and tables

Constants inherited from `base.h` plus: `FENC_STRIDE 16`, `FDEC_STRIDE 32`, `X264_SCAN8_LUMA_SIZE 40`, `X264_SCAN8_SIZE 120`, `X264_SCAN8_0 12`, `QP_MAX_MAX` (from `tables.h`, = 81), `map_col_to_list0(col)` and `deblock_ref_table(x)` offset macros (+2) so negative reference indices can be indexed safely. The static `slice_type_to_char` array maps slice types to log characters. No large const arrays are defined here; they live in `tables.c`.

The NAL overhead constants (5/6 bytes, reduced SEI overhead when an Annex-B NAL is already present) are also defined here for rate-control accounting. Values are fixed and do not vary with bit depth; only the SEI reduction depends on access-unit state. A clean implementation should expose them as named constants rather than literals, because rate control subtracts them from the bit budget before allocating bits to macroblocks.

The `+2` offset macros deserve emphasis: reference indices are signed (−1 meaning unavailable) but are used as array indices into neighbour caches and weight tables. Adding 2 maps the legal range [−1, X264_REF_MAX−2] into [1, X264_REF_MAX], leaving index 0 as a safe sentinel for "never accessed". An implementation that omits the offset will read one or two entries early and corrupt the neighbour context.

`FENC_STRIDE = 16` and `FDEC_STRIDE = 32` are the row pitches of the source-pixel and reconstructed-pixel scratch buffers (`fenc_buf`, `fdec_buf`). The reconstruction stride is twice as wide so that deblocking and half-pel filters can read neighbours without a separate copy; both buffers are 64-byte aligned. `X264_SCAN8_SIZE = 120` and `X264_SCAN8_0 = 12` define the Scan8 indexing scheme used by the neighbour caches: block 0 of a macroblock sits at offset 12 in a 120-entry linear array laid out to match 4x4 block adjacency, so a neighbour's cache index can be computed by a small fixed offset table. `X264_SCAN8_LUMA_SIZE = 40` counts the first 40 entries (the luma blocks), with chroma following.

---

## 8. Performance notes

- **Cache layout:** `fenc_buf` and `fdec_buf` are 64-byte aligned with strides 16 and 32 pixels respectively; the wider reconstruction stride gives room for deblocking and half-pel filters without a separate copy. Scan8-indexed caches are aligned to 4/8/16 bytes exactly as their element width requires, except `non_zero_count`, whose deliberate 8-byte misalignment is used by hand-written SIMD stores.
- **False sharing:** the two synchronisation boundaries in `x264_t` exist precisely to keep per-thread state separate; copy-on-start is cheaper than contended shared writes. Mutable words that must be shared (`i_threadslice_pass`, `i_lines_completed`) are protected by mutexes and changed rarely.
- **Allocation strategy:** `mb.base` and `frame->base` are single allocations (see `base.h.md` PREALLOC); keep it that way to avoid allocator overhead and TLB misses.
- **Dispatch hierarchy:** function tables in the context are the single dispatch point from generic code to architecture-specific kernels; keep them contiguous so the pointer load shares a cache line with neighbouring entries.
- Public references: x264 `doc/threads.txt` (frame vs slice threading and its quality cost); Drepper, *What Every Programmer Should Know About Memory* (2007) on false sharing; Intel *Optimization Reference Manual* on alignment of SIMD buffers.
- **Compile-time specialisation:** with `CHROMA_FORMAT` and `BIT_DEPTH` fixed at compile time, the chroma shifts and pixel type become constants, letting the compiler fold them out of inner loops. A clean implementation should preserve this by using macros/templates rather than runtime branches for these values.
- **Branch-free clamping:** `x264_clip_pixel`'s bit trick avoids a branch per sample, which matters in pixel-domain loops processing thousands of samples per frame. A clean implementation may use a ternary clamp; correctness is identical, but the branch predictor will be stressed on mixed-sign inputs.
- **Neighbour-cache indexing:** the Scan8 layout is chosen so the above/left/above-left neighbours of any 4x4 block are at fixed small offsets; this turns neighbour lookup into an array access rather than a coordinate computation. Preserve the map exactly, since the CAVLC `nC` prediction reads these caches.
- Public references (additional): Drepper (2007) on false sharing and NUMA effects in frame-thread contexts; x264 `doc/threads.txt` on the quality cost of slice threading versus frame threading.

---

## 9. References

- ITU-T H.264 clause 7.4.3 (slice header syntax and semantics), Annex D (SEI), clause 8.3 (deblocking), Annex B (NAL units).
- Public `x264.h` for `x264_param_t`, `x264_picture_t` and the templated API contract.
- x264 `doc/threads.txt`, `doc/ratecontrol.txt`.
- Drepper (2007), as above.
- Intel *Optimization Reference Manual* — SIMD buffer alignment and store-forwarding constraints.
- Public `x264.h` `x264_param_t` documentation for `i_bitdepth`, `i_csp` and the templated API naming scheme.
- ITU-T H.264 Annex A (profiles and levels) for the relationship between bit depth and profile constraints that motivates `QP_BD_OFFSET`.

---

## 10. Verification checklist

- Compile separate 8-bit and 10-bit translation units and verify that `sizeof(pixel)`, `sizeof(dctcoef)`, `QP_MAX_SPEC` and `PIXEL_MAX` match the table in section 3.
- In a black-box encoder run with many threads, verify that `--deterministic` produces identical bitstreams across runs and thread counts where the public API promises it; observe that toggling deterministic mode can change output (documented behaviour, not a bug).
- Verify that `x264_clip_pixel` matches a scalar clamp for −1, 0, PIXEL_MAX and PIXEL_MAX+1 in both bit depths.
- Verify that `x264_predictor_roundclip` drops the zero vector and duplicates and returns a count <= the input count for randomised candidate lists; verify `x264_predictor_clip` respects the scaled bounds.
- Inspect `sizeof(x264_t)` and field offsets in a debug harness: all members marked `ALIGNED_32`/`ALIGNED_64` must satisfy their alignment (black-box via `offsetof`).
- Confirm each CPU-dispatched function pointer is non-null after encoder open for the selected CPU flags.

Additional black-box checks:

- Verify the §3.1 QP table numerically: 8-bit → (0,51,69,255,12), 10-bit → (12,63,81,1023,24) for (offset, SPEC, MAX, PIXEL_MAX, LOOKAHEAD_QP).
- Verify `SPEC_QP(57)` is 51 in an 8-bit build and 57 in a 10-bit build, confirming the clamp uses the bit-depth-dependent spec maximum.
- Verify §5.1 for all four chroma formats: `FRAME_SIZE(256)` is 384 / 512 / 768 / 256 for 4:2:0 / 4:2:2 / 4:4:4 / 4:0:0.
- Verify `bs_size_ue`/`bs_size_se` against the actual bit writer for 1000 random code numbers in both bit depths.
- Verify the §4.1 boundary by checking, in a threaded debug harness, that a field written by a worker in the copied region is lost on the next frame while one in the private region persists.
- Verify the `+2` offset macros: `map_col_to_list0(−1)` and `deblock_ref_table(−1)` must both index entry 1, never entry −1.
- Verify the Scan8 constants: `X264_SCAN8_SIZE == 120`, `X264_SCAN8_0 == 12`, and that the first luma block's cache index equals 12 while the last chroma block is below 120.
- Verify `fenc_buf`/`fdec_buf` alignment at runtime via `offsetof(x264_t, fenc_buf) % 64 == 0` (and similarly for `fdec_buf`) in both builds.
- Confirm `x264_template` produces the expected symbol names by inspecting the exported symbol table: both `x264_8_encoder_open` and `x264_10_encoder_open` must exist when both libraries are built.
- Verify the §5.3 slice-header groups: after `x264_slice_header_init`, `i_first_mb <= i_last_mb`, `i_num_ref_idx_active[l] <= X264_REF_MAX`, and `sh_backup` matches `sh` for the backed-up fields.
- Verify the §5.4 list ownership by driving frames through `ifbuf → next → ofbuf` and confirming `i_reference_count` returns to 1 only after the consumer releases each frame.
- Verify `CHROMA444` behaviour in the entropy writer: with a 4:4:4 build, confirm there is no separate chroma DC/AC pass and that all three planes are coded with equal-sized 4x4 blocks.
- Verify the §5.6 chroma-QP clamp: with `BIT_DEPTH = 10`, luma QP 3 and PPS offset −3 must produce chroma QP 0, never −6; index the chroma QP table with the clamped value and confirm no out-of-bounds read.
- Verify the §5.7 array layout: compute every per-MB array base as an offset from `mb.base` and confirm no two arrays overlap and each is aligned to its element width.
- Verify the §5.8 collapse: in a build with `HAVE_INTERLACED = 0`, confirm `MB_INTERLACED`, `SLICE_MBAFF` and `PARAM_INTERLACED` all evaluate to integer constant 0 at compile time (e.g. via `static_assert`).
- Verify the NAL overhead constants against rate control: with a fixed target, confirm the reserved overhead is 5 bytes per NAL (6 for filler) and that the SEI overhead is reduced in an access unit that already carries an Annex-B NAL.

---

## 11. Open questions

- UNKNOWN: the precise list of fields that must be re-synchronised after a mid-frame reconfiguration is not documented in this header; only `reconfig`/`reconfig_h` are visible.
- UNKNOWN: whether `api` may be NULL for all valid encoder instances; the header only calls it an opaque pointer to the bit-depth-independent interface.
- UNKNOWN: exact numerical contents of the per-MB arrays at frame start beyond the reset behaviour implemented elsewhere; this header declares layout only.
- UNKNOWN: whether the `X264_PCM_COST` value is tuned per preset or is a fixed penalty; only its name and role are visible here.
- UNKNOWN: the exact numeric value and derivation of `X264_LOOKAHEAD_QP`'s constant 12; it is chosen empirically for lookahead quality and is not derived from the standard.
- UNKNOWN: whether the `api` pointer is ever used for bit-depth dispatch in the reference or is purely informational; no dereference is visible from this header.
- UNKNOWN: the precise semantics of `b_last_minigop_bframe`; it is declared in the frame structure but its effect on frame-type decision is specified elsewhere.

This header is a contract, not an implementation: where a field's *value* is derived (rather than its type declared), the derivation lives in `encoder/encoder.c`, `encoder/set.c` or the relevant algorithm module and is documented in those specifications.

Anything not stated here — in particular exact default parameter values and the order in which `x264_param_t` fields are consumed at encoder open — is deliberately left UNKNOWN in this document.
