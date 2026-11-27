# `common/common.h` - encoder-wide internal types

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\common.h` (mirrored at `common/common.h`). Role in one sentence: the central internal header that defines the bit-depth-templated type system, the encoder context `x264_t`, the slice header and lookahead structures, and the inline pixel clamps and MV-predictor helpers. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Where `base.h` supplies depth-independent primitives, `common.h` establishes the types that vary with build configuration (8 vs 10 bits, chroma format, interlacing) and aggregates the entire per-encoder state into one object. It is the contract between the encoder core and the algorithm modules (prediction, motion compensation, DCT, quantisation, deblocking, bitstream, rate control). It also documents the multithreading memory model: which fields of `x264_t` are copied from thread to thread, and which are thread-local.

## 3. Public interface

Templating macros: `x264_template(w)` concatenates `x264_<BIT_DEPTH>_<w>`; it is applied to all API entry points (`x264_encoder_open`, `_encode`, `_close`, `_headers`, `_reconfig`, `_parameters`, `_delayed_frames`, `_maximum_delayed_frames`, `_intra_refresh`, `_invalidate_reference`, `_nal_encode`, plus internal functions such as `x264_log`, `x264_cqm_init`, `x264_frame_new`). `x264_encoder_open` is deliberately undefined before being redefined so that linking an incompatible bit-depth library fails.

Depth-dependent types and constants:

| 8-bit build | 10-bit build |
| --- | --- |
| `pixel = uint8_t`, `pixel4 = uint32_t`, `dctcoef = int16_t`, `udctcoef = uint16_t` | `pixel = uint16_t`, `pixel4 = uint64_t`, `dctcoef = int32_t`, `udctcoef = uint32_t` |
| `PIXEL_SPLAT_X4(x) = x*0x01010101`, `MPIXEL_X4 = M32` | `x*0x0001000100010001ULL`, `M64` |

`SIZEOF_PIXEL`, `CPPIXEL_X4`, `QP_BD_OFFSET = 6*(BIT_DEPTH-8)`, `QP_MAX_SPEC = 51+offset`, `QP_MAX = QP_MAX_SPEC+18`, `PIXEL_MAX = (1<<BIT_DEPTH)-1`, `X264_LOOKAHEAD_QP = 12+offset`, `SPEC_QP` (clamp to spec maximum), `X264_PCM_COST`. Chroma helpers: `CHROMA_H_SHIFT` (true for 420/422), `CHROMA_V_SHIFT` (true for 420), `CHROMA_SIZE`, `FRAME_SIZE(s) = s + 2*CHROMA_SIZE(s)`, `CHROMA444`. NAL overhead constants: 5 bytes per NAL, 6 for filler, and an SEI overhead that is reduced when Annex B has an existing NAL in this access unit. With `HAVE_INTERLACED` off, `MB_INTERLACED`, `SLICE_MBAFF` and `PARAM_INTERLACED` collapse to constant 0; with `CHROMA_FORMAT` defined at compile time, the chroma shifts become compile-time constants.

Declared functions: `void x264_log(x264_t *, int level, const char *fmt, ...)`, `void x264_cavlc_init(x264_t *)`, `void x264_cabac_init(x264_t *)`. Inline helpers: `x264_clip_pixel(int)` (clamp with branch-free bit trick), `x264_predictor_roundclip` (convert quarter-pel candidates to full pel with rounding +2>>2, drop zero/duplicate, clip to MV limits), `x264_predictor_clip` (clip quarter-pel candidates directly to limits shifted by 2), plus `mvsad_t { int sad; int16_t mv[2]; }`.

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

## 5. Algorithm / behaviour

`x264_clip_pixel` clamps to `[0, PIXEL_MAX]` by testing `x & ~PIXEL_MAX` and, when negative, returning `(-x)>>31 & PIXEL_MAX`. `x264_predictor_roundclip` is used for full-pel candidate lists: it rounds each quarter-pel vector with `(v+2)>>2` (arithmetic shift for negatives), packs `(mx,my)` into a 32-bit word to reject the zero vector and duplicates of the preferred vector, then clips componentwise. `x264_predictor_clip` keeps quarter-pel precision and scales the bounds by four. Both return the number of accepted candidates, which is the loop bound for the motion-search predictor stage.

The thread-synchronisation split is a central invariant: fields above the first boundary are snapshot-copied from the master context into each worker at the start of a frame so workers see a consistent view; fields below the second boundary are private and never copied. Consequently, any new mutable field placed in the copied region must be written before workers start, and any field updated by a worker must live in the private region or be protected by a mutex/atomic.

Quantisation-matrix pointers are organised as `[cqm][qp_mod_6][coef]` for decoding and `[cqm][qp][coef]` for trellis/deadzone; `nr_offset_emergency` is indexed by QP above the spec maximum. Cost tables hold QP-indexed lambda-derived MV/reference/mode costs, with separate full-pel tables.

The CPU-dispatched tables are selected once at encoder open from `param.cpu`; calls go through these pointers in the hot inner loops, which is why they are stored directly in the context rather than looked up globally.

## 6. Dependencies

Includes `base.h` first, then conditionally `opencl.h`, followed by `cabac.h`, `bitstream.h`, `set.h`, `predict.h`, `pixel.h`, `mc.h`, `frame.h`, `dct.h`, `quant.h`, `threadpool.h`; at the end includes `macroblock.h` (needs `x264_t`), optionally `x86/util.h`, and `rectangle.h`. Included by all encoder and algorithm translation units. It is the single largest internal interface surface in the project.

## 7. Constants and tables

Constants inherited from `base.h` plus: `FENC_STRIDE` 16, `FDEC_STRIDE` 32, `X264_SCAN8_LUMA_SIZE` 40, `X264_SCAN8_SIZE` 120, `X264_SCAN8_0` 12, `QP_MAX_MAX` (from `tables.h`), `map_col_to_list0(col)` and `deblock_ref_table(x)` offset macros (+2) so negative reference indices can be indexed safely. The static `slice_type_to_char` array maps slice types to log characters. No large const arrays are defined here; they live in `tables.c`.

## 8. Performance notes

- **Cache layout:** `fenc_buf` and `fdec_buf` are 64-byte aligned with strides 16 and 32 pixels respectively; the wider reconstruction stride gives room for deblocking and half-pel filters without a separate copy. Scan8-indexed caches are aligned to 4/8/16 bytes exactly as their element width requires, except `non_zero_count`, whose deliberate 8-byte misalignment is used by hand-written SIMD stores.
- **False sharing:** the two synchronisation boundaries in `x264_t` exist precisely to keep per-thread state separate; copy-on-start is cheaper than contended shared writes. Mutable words that must be shared (`i_threadslice_pass`, `i_lines_completed`) are protected by mutexes and changed rarely.
- **Allocation strategy:** `mb.base` and `frame->base` are single allocations (see `base.h.md` PREALLOC); keep it that way to avoid allocator overhead and TLB misses.
- **Dispatch hierarchy:** function tables in the context are the single dispatch point from generic code to architecture-specific kernels; keep them contiguous so the pointer load shares a cache line with neighbouring entries.
- Public references: x264 `doc/threads.txt` (frame vs slice threading and its quality cost); Drepper, *What Every Programmer Should Know About Memory* (2007) on false sharing; Intel *Optimization Reference Manual* on alignment of SIMD buffers.

## 9. References

- ITU-T H.264 clause 7.4.3 (slice header syntax and semantics), Annex D (SEI), clause 8.3 (deblocking), Annex B (NAL units).
- Public `x264.h` for `x264_param_t`, `x264_picture_t` and the templated API contract.
- x264 `doc/threads.txt`, `doc/ratecontrol.txt`.
- Drepper (2007), as above.

## 10. Verification checklist

- Compile separate 8-bit and 10-bit translation units and verify that `sizeof(pixel)`, `sizeof(dctcoef)`, `QP_MAX_SPEC` and `PIXEL_MAX` match the table in section 3.
- In a black-box encoder run with many threads, verify that `--deterministic` produces identical bitstreams across runs and thread counts where the public API promises it; observe that toggling deterministic mode can change output (documented behaviour, not a bug).
- Verify that `x264_clip_pixel` matches a scalar clamp for -1, 0, PIXEL_MAX and PIXEL_MAX+1 in both bit depths.
- Verify that `x264_predictor_roundclip` drops the zero vector and duplicates and returns a count <= the input count for randomised candidate lists; verify `x264_predictor_clip` respects the scaled bounds.
- Inspect `sizeof(x264_t)` and field offsets in a debug harness: all members marked `ALIGNED_32`/`ALIGNED_64` must satisfy their alignment (black-box via `offsetof`).
- Confirm each CPU-dispatched function pointer is non-null after encoder open for the selected CPU flags.

## 11. Open questions

- UNKNOWN: the precise list of fields that must be re-synchronised after a mid-frame reconfiguration is not documented in this header; only `reconfig`/`reconfig_h` are visible.
- UNKNOWN: whether `api` may be NULL for all valid encoder instances; the header only calls it an opaque pointer to the bit-depth-independent interface.
- UNKNOWN: exact numerical contents of the per-MB arrays at frame start beyond the reset behaviour implemented elsewhere; this header declares layout only.
