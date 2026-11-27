# `common/opencl/subpel.cl`

**Role:** Refines each integer lowres motion vector to quarter-pel precision using the packed half-pel reference planes, then re-scores the final vector with SATD and writes it into the per-frame motion-vector buffers.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `common/opencl/subpel.cl`
- Role: implement the half-pel and quarter-pel refinement stages of lookahead motion estimation and the final SATD-based cost used by mode selection.
- Marker: this document is a dirty-side behavioural specification derived from the reference implementation and public H.264 interpolation literature.

---

## 2. Purpose

Integer motion search cannot exploit the sub-pel interpolation that H.264 uses for inter prediction. This file takes the integer vectors produced by `hierarchical_motion`, evaluates the four neighbouring half-pel positions and then the four diamond quarter-pel positions around each, using the F/H/V/C planes built by `downscale.cl`. Four threads per macroblock evaluate the four offsets of each stage simultaneously. The winning vector is then re-measured with SATD (not SAD), because the reference's mode-selection stage compares SATD costs, and deposited in the large per-frame MV/cost buffers at the offset corresponding to the reference direction and frame distance.

---

## 3. Public interface

Device helpers:

- **`sad_8x8_ii_hpel(fenc, fencpos, fref_planes, qpos)`** — one thread measures 8×8 SAD at a half-pel position. `frefpos = qpos >> 2` selects the texel; `hpel_idx = ((qpos.x&2)>>1) + (qpos.y&2)` selects one of four bytes in the packed word; a `uint4` accumulator sums four channel-wise `abs_diff` results per two column groups over eight rows.
- **`sad_8x8_ii_qpel(fenc, fencpos, fref_planes, qpos)`** — one thread measures 8×8 SAD at an arbitrary quarter-pel position. The sample is the rounded average of two half-pel plane samples selected by the quarter-pel fractional bits; the scalar loop is used because the two plane positions can differ.
- **`satd_8x8_ii_qpel_coop4(fenc, fencpos, fref_planes, qpos, tmpp, idx)`** — four threads cooperate on an 8×8 SATD at a qpel position. Each thread handles two rows, packs two column differences into the low/high halves of a `sum2_t`, uses the transposed local exchange from `bidir.cl`'s pattern (Hadamard on rows, then on columns, with `abs2` accumulation), and returns the normalized SATD.
- **`hpoffs[4]`** — constant table of half-pel offsets in qpel units: `(0,-2)`, `(-2,0)`, `(2,0)`, `(0,2)` (up, left, right, down).

Kernel:

- **`subpel_refine( read_only image2d_t fenc, read_only image2d_t fref_planes, const global short2 *in_mvs, const global int16_t *in_sad_mv_costs, local int16_t *cost_local, local sum2_t *satd_local, local short2 *mvc_local, global short2 *fenc_lowres_mv, global int16_t *fenc_lowres_mv_costs, int mb_width, int lambda, int b, int ref, int b_islist1 )`** — global `[mb_width*4, mb_height]`, local dimensions from the host.

---

## 4. State and data structures

- **`fref_planes`:** `CL_R`/`CL_UNSIGNED_INT32` image in which each texel packs F/H/V/C bytes. Reading channel byte `8*hpel_idx` returns the desired half-pel sample; a qpel sample is `rhadd` of two such bytes.
- **`in_mvs` / `in_sad_mv_costs`:** the integer MVs and SAD costs from the last HME iteration (temporary ping-pong buffers).
- **`fenc_lowres_mv` / `fenc_lowres_mv_costs`:** final per-frame buffers, sized `mb_count * 2 * sizeof(int16_t) * (bframes+1)` for MVs and `mb_count * sizeof(int16_t) * (bframes+1)` for costs. The kernel offsets them by `(b_islist1 ? ref-b-1 : b-ref-1) * mb_count` before writing.
- **Local scratch:** `cost_local` (four `int16_t` per macroblock), `satd_local` (sixteen `sum2_t` per macroblock), `mvc_local` (four `short2` per macroblock), all laid out by the `mb_in_group` formula shared with the HME kernel.

---

## 5. Algorithm / behaviour

### 5.1 Half-pel and quarter-pel sampling

The packed image represents a 2×2 pattern of subsampled phases:

- F occupies the full-pel grid point; H is the horizontal midpoint; V the vertical midpoint; C the center of the four surrounding full-pel samples.
- For a qpel coordinate `q`, the full-pel texel is `q >> 2`. The half-pel plane index is obtained from the two low-order bits: bit 1 of x selects H/V blocks, bit 2 of y selects V/C rows, and their sum gives F=0, H=1, V=2, C=3.
- The qpel sample is formed by taking two half-pel samples: one at `A = q>>2` using plane `hpelA`, and one at `B = (q + 2*(q&1)) >> 2` using plane `hpelB`. The two are averaged with `rhadd` (round-up-half), matching the reference's bilinear qpel interpolation at this resolution.

The half-pel SAD helper exploits the packed channels: each `read_imageui` returns four consecutive samples in `.s0..s3`, so eight rows × two column groups = sixteen fetches cover the full 8×8 block; four `uint4` channel accumulators are summed at the end.

### 5.2 Work mapping and MVP

- `mb_x = global_id(0) >> 2`, `mb_i = global_id(0) & 3`, `mb_y = global_id(1)`, `mb_xy = mb_y*mb_width + mb_x`. Work items beyond `mb_width` return.
- `mb_in_group = local_id(1)*(local_size(0)>>2) + (local_id(0)>>2)`; each pointer into `cost_local`, `satd_local`, `mvc_local` is advanced by its per-macroblock allocation.
- Neighbouring MVs are collected from `in_mvs` using the same pattern as the HME even-iteration case: left, up, up-right and up-left (bounds permitting). The median of up to three is the MVP; all four `mvc_local` slots are cleared first.
- The current best vector `bmv` is the integer MV from `in_mvs[mb_xy]`. Both `bmv` and `mvp` are scaled to qpel units by shifting left two.

### 5.3 Half-pel refinement

For each thread `mb_i`:

- Candidate `trymv = bmv + hpoffs[mb_i]` — one of the four half-pel neighbours.
- `qpos = (coord << 2) + trymv`, where `coord = (mb_x,mb_y)<<3` is the block position in lowres pixels.
- Cost is `sad_8x8_ii_hpel(...)` plus `lambda * mv_cost(|trymv - mvp|)` (already in qpel units, so no extra shift).
- The cost and thread index are packed into `cost_local[mb_i]` as `(cost<<2) + mb_i`; the minimum across four threads is taken. If the best cost is strictly less than `bcost`, `bmv` advances by the winning `hpoffs` entry and `bcost` is updated.

### 5.4 Quarter-pel refinement

The same macro executes again with `dia_offs` from `x264-cl.h` and `sad_8x8_ii_qpel`: four unit qpel steps around the now half-pel-aligned `bmv`. Costs are again packed and reduced across the four threads; improvement updates `bmv` and `bcost`.

### 5.5 SATD re-score and writeback

- `qpos = (coord<<2) + bmv`; all four threads call `satd_8x8_ii_qpel_coop4` with their `satd_local` scratch and sum the four returned partial SATDs into `bcost`.
- The MV rate penalty is added once: `bcost += lambda * mv_cost(|bmv - mvp|)`.
- `fenc_lowres_mv[mb_xy]` stores `bmv` narrowed to `short2`; `fenc_lowres_mv_costs[mb_xy]` stores `min(bcost, LOWRES_COST_MASK)`.

Note the deliberate asymmetry: SAD is used to *find* the sub-pel vector (cheap, four-way parallel), but SATD is used for the *final cost* so that it is comparable with the intra and bidir costs later consumed by `mode_selection`.

### 5.6 Host dispatch contract

`x264_opencl_motionsearch()` in `encoder/slicetype-cl.c` after the HME loop:

- Reuses the final `gdims`/`ldims` from scale 0 and the same `cost_local_size`/`mvc_local_size`; adds `satd_local_size = mb_per_group * sizeof(uint32_t) * 16`.
- Arguments in order: `fenc->scaled_image2Ds[0]`, the (possibly weighted) reference HPEL plane, `mv_buffers[A]`, `lowres_mv_costs`, three local buffers, the selected final MV/cost buffers (`lowres_mvs1`/`lowres_mv_costs1` when `b_islist1`, otherwise `lowres_mvs0`/`lowres_mv_costs0`), then `mb_width`, `lambda`, `b`, `ref`, `b_islist1`.
- On AMD Southern Islands a meaningless 20-byte buffer copy is enqueued before the kernel to work around a driver scheduling bug.
- After the kernel, the relevant slice of the final MV buffer is read back into `fenc->lowres_mvs[list][distance]` at offset `mvlen * (ref-b-1)` or `mvlen * (b-ref-1)`.

### 5.7 GPU versus CPU output; host selection

The refined MVs and SATD costs are lookahead metadata. The GPU path is selected by `b_opencl`; the CPU path performs the equivalent sub-pel search in `slicetype.c`. Because the GPU interpolates from precomputed packed HPEL planes with `rhadd` rounding and re-scores with a packed-lane SATD, its vectors/costs can differ from the CPU's; the reference does not require equality. UNKNOWN: whether the quarter-pel refinement is guaranteed to find the same optimum as the CPU subpel search.

---

## 6. Dependencies

- **Uses from `x264-cl.h`:** `pixel`, `sum2_t`, `LOWRES_COST_MASK`, `dia_offs`, `mv_cost`, `x264_median_mv`, `HADAMARD4`, `abs2`, `BITS_PER_SUM`, `rhadd`, the sampler.
- **Input:** `scaled_image2Ds[0]` from `downscale.cl`; `luma_hpel` (or `weighted_luma_hpel`) from `downscale.cl`/`weightp.cl`; HME results from `motionsearch.cl`.
- **Output consumers:** `bidir.cl::mode_selection` reads `fenc_lowres_mvs0/1` and `fenc_lowres_mv_costs0/1`; `fref1_lowres_mvs0` supplies temporal B prediction.
- **Host:** kernel created in `common/opencl.c`; dispatched in `encoder/slicetype-cl.c`.

---

## 7. Constants and tables

- `hpoffs[4]`: `(0,-2)`, `(-2,0)`, `(2,0)`, `(0,2)` — half-pel offsets in qpel units; the order matches the four cooperating threads.
- `dia_offs[4]` (from `x264-cl.h`): `(0,-1)`, `(-1,0)`, `(1,0)`, `(0,1)` — quarter-pel steps.
- Qpel scale factor 4 (`<<2`) converts integer lowres MVs and block coordinates to qpel units.
- HPEL plane index `((x&2)>>1) + (y&2)`; byte shift `8*index`, mask `0xFF`.
- SATD normalization `(low + high) >> 1` from the packed-lane representation.
- `LOWRES_COST_MASK` (16383) saturates the final cost.

---

## 8. Performance notes

- **Parallelism:** four threads per macroblock evaluate all four offsets of each refinement stage; the packing of `(cost<<2)|index` makes the reduction and winner selection branch-free.
- **Texture efficiency:** the half-pel SAD reads four consecutive samples per `uint4`, giving 4× fewer fetches than a scalar loop; the qpel SAD cannot do this because its two plane positions differ, so it uses a scalar loop.
- **Local memory:** `satd_local` is the largest scratch allocation (16 `sum2_t` per macroblock) and is the main occupancy limiter for this kernel.
- **Coalescing:** consecutive work items access consecutive lowres blocks, so image reads are coalesced; final MV writes are 4-byte `short2` stores to consecutive macroblock indices from thread 0 only, so writes are sparse but uncontended.
- **Platform limitation:** the AMD Southern Islands dummy copy is a driver workaround, not an algorithmic step.
- **Hotness:** this kernel runs once per reference direction per frame, so it is roughly twice as expensive as a single-direction HME tail.

---

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10, clause on quarter-pel interpolation for luma: half-pel filters and bilinear averaging motivating the F/H/V/C planes and qpel `rhadd`.
- Khronos Group, *OpenCL C Specification*: image reads, vector channel selection, local memory, `abs_diff`, `rhadd`.
- Hadamard SATD literature: packed-lane SATD with final `>>1` normalization.
- AMD Southern Islands driver notes referenced by the host implementation for the dummy-copy workaround.

---

## 10. Verification checklist

- For a static frame, refined MVs should be zero (or one qpel step around zero if noise) and the SATD cost should match the SAD cost at zero displacement.
- For a frame shifted by an exact half pixel in x, confirm the half-pel stage selects the H-plane candidate and the final MV has x-qpel component 2.
- For a frame shifted by an exact quarter pixel, confirm the qpel diamond stage can reach the correct one-pel qpel offset.
- Verify HPEL plane selection: for a qpos whose x low bits are `10` and y low bits are `00`, the sampled byte must be channel 1 (H); for `00/10`, channel 2 (V); for `10/10`, channel 3 (C).
- Verify the qpel sample uses `rhadd` of the A and B plane samples, not arithmetic truncation.
- Verify the packed cost index encoding allows recovery of the winning offset and that `bmv` moves in that direction.
- Verify final costs are saturated to 16383 and are written at the correct frame-distance offset for list0 versus list1.
- Verify SATD re-scoring equals a scalar reference SATD for the final vector.
- Confirm the AMD SI dummy copy does not change numerical results.

---

## 11. Open questions

- **UNKNOWN:** whether the two-stage half-pel-then-qpel search can be trapped in a local minimum that the CPU search avoids; no equivalence guarantee is given.
- **UNKNOWN:** exact equivalence of the packed-lane SATD helper with the scalar SATD used in the C lookahead at all residual magnitudes (especially signed extremes).
- **UNKNOWN:** whether the final MV write from thread 0 only creates a race with other threads still finishing their SATD calls; a local barrier appears absent in the source, so this relies on the `mb_i==0` thread's own SATD finishing before the write (UNKNOWN driver memory-ordering assumption).
- **UNKNOWN:** behaviour of the HPEL indexing when `qpos` is negative (clamped reads may alias the wrong plane); bounds are inherited from HME, which clamps MVs, so this may not be reachable.
