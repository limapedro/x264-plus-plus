# `common/opencl/motionsearch.cl`

**Role:** Performs the coarse-to-fine hierarchical (multi-resolution) integer motion search that supplies lowres motion vectors and SAD costs for every reference direction in the lookahead.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `common/opencl/motionsearch.cl`
- Role: implement one iteration of the hierarchical diamond motion search per kernel launch, with four cooperating threads per 8×8 macroblock, including MVP prediction from neighbouring blocks, boundary clamping, candidate refinement, and a fast-path skip when the MVP is unchanged.
- Marker: this document is a dirty-side behavioural specification derived from the reference implementation and public motion-estimation literature.

---

## 2. Purpose

Full-search integer motion estimation over a large range is too expensive for a lookahead that must analyse many frames ahead. The reference uses a hierarchical strategy: search a heavily decimated image first, then project the resulting vectors down to finer resolutions and refine locally. This file implements that strategy on the GPU. Its design lets all four diamond directions for a macroblock be evaluated simultaneously by four threads, while intermediate results are exchanged through local memory without global barriers.

---

## 3. Public interface

Device helpers:

- **`find_downscale_mb_xy(x, y, mb_width, mb_height)`** — maps a macroblock coordinate at the current scale to the nearest macroblock in the next coarser scale. Normal coordinates are halved; an odd last row/column is folded back by subtracting the parity bit before halving so edge blocks still find a valid parent.
- **`sad_8x8_ii_coop4(fenc, fencpos, fref, frefpos, idx, costs)`** — four threads cooperate on one 8×8 SAD. Thread `idx` handles rows `2*idx` and `2*idx+1`, accumulating the eight absolute differences per row pair and writing its partial to `costs[idx]`; the function returns the sum of all four partials. Includes a scalar slow path for the case where the reference x-coordinate is negative (see §5.1).
- **`sad_8x8_ii(fenc, fencpos, fref, frefpos)`** — a single-thread 8×8 SAD used for the four diamond probes; uses two `uint4` reads per row on the fast path.

Kernel:

- **`hierarchical_motion( read_only image2d_t fenc, read_only image2d_t fref, const global short2 *in_mvs, global short2 *out_mvs, global int16_t *out_mv_costs, global short2 *mvp_buffer, local int16_t *cost_local, local short2 *mvc_local, int mb_width, int lambda, int me_range, int scale, int b_shift_index, int b_first_iteration, int b_reverse_references )`** — global dimensions `[mb_width*4, mb_height]`, local dimensions chosen by the host.

---

## 4. State and data structures

- **MV ping-pong buffers:** `mv_buffers[0]` and `mv_buffers[1]`, each `mb_count * 2 * sizeof(int16_t)`, alternate as `in_mvs`/`out_mvs` between iterations so no kernel reads and writes the same buffer.
- **`out_mv_costs`:** shared `lowres_mv_costs` buffer, one 16-bit saturated cost per macroblock.
- **`mvp_buffer`:** retains the previous iteration's MVP per macroblock, enabling the early-exit copy when neither the MVP nor the scale changed.
- **`cost_local`:** four `int16_t` slots per macroblock in the work group; used to combine the four cooperating SAD partials and to pack `(cost<<2)|winning_index` during diamond search.
- **`mvc_local`:** four `short2` candidate vectors per macroblock; holds the neighbouring MVs used for median prediction and later re-testing.
- **Global MV layout:** for direction list0/list1 the eventual per-frame buffers index by `(b-ref-1)` or `(ref-b-1)` times `mb_count`; the HME kernel itself works on the temporary ping-pong buffers, and `subpel_refine` performs the scatter to the final layout.

---

## 5. Algorithm / behaviour

### 5.1 SAD helpers and the clamped-read hazard

Both SAD helpers measure the sum of absolute differences between an 8×8 block of the current lowres image and the same-sized block in the reference image at a candidate position.

- Fast path: when the reference x-coordinate is non-negative, each pair of adjacent pixels is fetched as a four-channel `uint4`, so two fetches cover one row; `abs_diff` is applied per channel and the four lanes summed.
- Slow path: when the reference x-coordinate is negative, the sampler's clamp-to-edge behaviour would turn a hypothetical window `[0,0,1,2]` into `[0,1,2,3]`, i.e. it would duplicate the wrong pixel. The helper therefore falls back to a per-pixel scalar loop over the whole 8×8 block so that negative coordinates are handled by explicit clamping of each coordinate, not by the sampler.
- The cooperative variant splits the 8 rows into two-row slices, one per thread, and returns the total by summing the four local slots. This is the form used for the initial candidate and the "no pad" refinement; the single-thread form is used for the four diamond probes because each probe is naturally assigned to one thread.

### 5.2 MVP construction

When this is not the first iteration of the hierarchy:

- Candidate neighbours are collected from `in_mvs`. If `b_shift_index` is set (coarse levels), the neighbour coordinate is first mapped through `find_downscale_mb_xy`; otherwise the direct coordinate is used. Each candidate is arithmetically shifted right by `scale`, converting a finer-resolution MV into the current resolution.
- The neighbour set depends on `b_reverse_references`: even iterations collect left, up, up-right and up-left; odd iterations collect right, down, down-right and down-left, each guarded by frame bounds. This alternation smooths the motion field faster at coarse scales.
- The MVP is the component-wise median of up to three candidates (or the sole candidate if fewer than two were available).

Early exit: when `b_shift_index` is zero and the computed MVP exactly equals the value stored in `mvp_buffer` for this macroblock, the kernel copies `in_mvs[mb_xy]` to `out_mvs[mb_xy]` and returns without measuring any candidates. This skips redundant work when a finer level's predictor is unchanged from the previous level.

### 5.3 Search bounds and initial cost

- Allowed MV components are clamped to a rectangle that keeps the 8×8 block inside the frame with four pixels of padding: minimum `-8*(mb_x, mb_y) - 4`, maximum `8*(mb_width,mb_height - mb_x,mb_y - 1) + 4` (the two components are computed analogously).
- `bestmv` starts as the clamped MVP; `refcrd = coord + bestmv`.
- Initial cost is the cooperative SAD at `bestmv` plus `lambda * mv_cost(|bestmv - mvp| << (2+scale))`. The shift by `2+scale` converts the current-resolution difference back into qpel units for the rate model, and the extra power of two accounts for the current scale.

### 5.4 Diamond descent

In a loop bounded by `me_range`:

- Thread `mb_i` probes `bestmv + dia_offs[mb_i]`, where the four offsets are up, left, right and down by one current-resolution pixel. Cost is single-thread SAD plus the same `lambda * mv_cost(...)` penalty, with the difference shifted by `2+scale`.
- The cost and the winner index are packed into one 32-bit local word as `(cost<<2) | mb_i`, and the minimum across the four threads is taken. Packing the index into the low bits lets the winning direction be recovered with a simple mask.
- If the best packed cost is not strictly smaller than the current `bcost`, the loop breaks. Otherwise `bestmv` advances by the winning offset, `bcost` becomes that cost, and the loop breaks early if `bestmv` reaches the padded boundary.
- `me_range` is decremented each pass; the host supplies `scaled_me_range = i_me_range >> scale`.

### 5.5 "No pad" refinement

After the diamond loop, additional candidates are tested with the cooperative SAD (which handles negative x correctly) and with a rate penalty that uses the unshifted difference `|trymv - mvp|` converted to qpel by `<< (2+scale)`:

- The zero vector, with penalty weight 0 (free check).
- If this is not the first iteration, the previous iteration's MV, read from `in_mvs` (through `find_downscale_mb_xy` when `b_shift_index` is set) and shifted right by `scale`, with penalty weight `lambda`.
- Each collected MVC candidate, with penalty weight `lambda`.

Each candidate is only evaluated if its absolute difference from the MVP exceeds one pixel in either component (`diff.x > 1 || diff.y > 1`); small differences are assumed already covered by the diamond search. Any candidate that beats `bcost` replaces `bestmv`.

Finally, thread `mb_i==0` writes `bestmv << scale` (back to the finer resolution's units) to `out_mvs[mb_xy]` and `min(bcost, LOWRES_COST_MASK)` to `out_mv_costs[mb_xy]`.

### 5.6 Host dispatch contract

`x264_opencl_motionsearch()` in `encoder/slicetype-cl.c`:

- Optionally applies weighted prediction to the reference planes first (see `weightp.cl`): each pyramid level and the HPEL plane are filtered into shared `weighted_*` buffers, which then replace `fref->opencl.*` for this search.
- Iterates scale from `NUM_IMAGE_SCALES-1` down to 0. `mb_width` and grid height are `h->mb.i_mb_width >> scale` and `mb_height >> scale`; levels with fewer than two macroblocks per axis are skipped.
- `gdims[0]` is multiplied by four (four threads per macroblock) and passed through `optimal_launch_dims()`, which derives `ldims` from the device's preferred work-group multiple (typically 64) and the compute-unit count. `mb_per_group = (ldims[0]>>2) * ldims[1]`; local allocation sizes are `4*mb_per_group*sizeof(int16_t)` for `cost_local` and `4*mb_per_group*sizeof(int16_t)*2` for `mvc_local`.
- Number of iterations per scale is `{1, 1, 2, 3}` for scales 0..3; `b_shift_index` and `b_first_iteration` start at 1 and become 0 after the first launch; `b_reverse_references` toggles for scales above 2 and is forced to 0 at finer scales. The MV buffers ping-pong (`A = !A`) after every iteration.
- After the HME loop, the same launch dimensions and local sizes are reused for `subpel_refine`, which scatters results into the final per-frame MV buffers.
- Results are read back through the page-locked staging buffer into `fenc->lowres_mvs[0|1][ref-b-1]` or `[b-ref-1]`.

### 5.7 GPU versus CPU output; host selection

The integer MVs and SAD costs are lookahead estimates used for frame-type decisions; they are never used to reconstruct coded samples. The host takes the GPU path when `h->param.b_opencl` is true, calling `x264_opencl_motionsearch()` for list0 and list1 separately (with weighted prediction applied only to list0 when `b == p1`). Otherwise the CPU motion search in `slicetype.c` produces the same metadata. Because the GPU uses a coarse diamond search with `rhadd`-based HPEL sampling and a logarithmic rate approximation, its vectors may differ from the CPU's; the reference treats the two as interchangeable estimators. UNKNOWN: whether the project expects bit-identical MVs in any configuration.

---

## 6. Dependencies

- **Uses from `x264-cl.h`:** `int16_t`, `short2`, `LOWRES_COST_MASK`, `dia_offs`, `mv_cost`, `x264_median_mv`, the sampler, `convert_*_sat`, `abs_diff`, `min/max`, `clamp`.
- **Input images:** pyramid levels `scaled_image2Ds[scale]` produced by `downscale.cl` (or weighted variants from `weightp.cl`).
- **Input/output buffers:** `mv_buffers[2]`, `lowres_mv_costs`, `mvp_buffer` from `opencl.h`.
- **Next stage:** `subpel.cl::subpel_refine` consumes `out_mvs` and `out_mv_costs` from the final HME iteration.
- **Host:** kernels created in `common/opencl.c`; dispatched in `encoder/slicetype-cl.c`; launch geometry from `optimal_launch_dims()`.

---

## 7. Constants and tables

- `mb_size = 8`: lowres macroblock edge in pixels; all coordinate and bound arithmetic uses it.
- Boundary padding = 4 pixels around the frame in MV clamping.
- `dia_offs[4]`: up, left, right, down unit steps; index encoded in the low two bits of the packed candidate cost.
- Iteration counts `{1,1,2,3}` per scale from finest (0) to coarsest (3), i.e. the coarsest level is refined three times.
- `num_iterations` total eight HME launches for a full search (sum of counts), each with four threads per macroblock.
- `mv_cost` shift `2+scale`: qpel conversion plus scale compensation.
- `LOWRES_COST_MASK` (16383): saturation of written costs.

---

## 8. Performance notes

- **Cooperation:** four threads per macroblock give one SAD per diamond direction in parallel; local-memory exchange replaces a reduction over global memory.
- **Coalescing:** image reads use `uint4` fetches covering four consecutive pixels; neighbouring work items read consecutive blocks, so texture requests are coalesced.
- **Branch behaviour:** the negative-x slow path is divergent only when a candidate crosses the left border; the early-exit skip is uniform per macroblock (all four threads share the same MVP and skip together).
- **Register/local pressure:** `cost_local` and `mvc_local` scale with `mb_per_group`; the host shrinks `ldims` until enough work groups exist to cover the compute units, balancing occupancy against reuse of the local structures.
- **Ping-pong buffers:** alternate input/output buffers avoid read-after-write hazards between successive launches without extra copies.
- **Platform limitation:** the AMD Southern Islands scheduling-bug workaround inserts a meaningless 20-byte `clEnqueueCopyBuffer` before `subpel_refine` to create a data dependency.
- **Hotness:** HME is the dominant cost of the GPU lookahead; the number of iterations scales with resolution and preset me_range.

---

## 9. References

- ITU-T Rec. H.264 / ISO/IEC 14496-10: motion-vector prediction, median prediction from neighbouring blocks, and quarter-pel MV units.
- Hierarchical / multi-resolution block matching literature: coarse-to-fine search with projection of coarser vectors, e.g. Bierling, "Displacement estimation by hierarchical blockmatching" (SPIE 1988).
- Khronos Group, *OpenCL C Specification*: image samplers with clamp-to-edge addressing, `uint4` reads, local-memory barriers and `convert_*_sat`.
- AMD, *OpenCL Optimization Guide*: work-group sizing from `CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE` and `CL_DEVICE_MAX_COMPUTE_UNITS`.
- General rate-distortion motion estimation literature for the `lambda * mv_cost` coupling.

---

## 10. Verification checklist

- For a static frame (fenc identical to fref), confirm all MVs converge to zero and costs equal the SAD of zero displacement plus the MV penalty (which is zero for a zero MV).
- For a frame translated by an integer offset within the search range, confirm the final MV equals that offset (after scaling back) for interior macroblocks.
- Verify boundary clamping: MVs never push the 8×8 block outside the padded bounds, and blocks at the left edge produce sensible vectors despite the negative-x slow path.
- Verify the early-exit path by feeding an `in_mvs` field whose median equals the stored `mvp_buffer` value; confirm output equals input and no candidate measurement is observable.
- Verify `b_reverse_references` alternation changes which neighbours are used for MVP at coarse levels; confirm median-of-three correctness for each case.
- Verify the packed `(cost<<2)|index` encoding: the recovered winning direction must match the smallest cost and the updated `bestmv` must move in that direction.
- Verify `out_mv_costs` is saturated to 16383 for very high distortion inputs.
- Confirm the number of launches and ping-pong alternation for a small frame matches the documented iteration counts.
- Compare GPU MVs against the CPU lookahead for a synthetic sequence and confirm they are close (bit-exact equality is not expected).

---

## 11. Open questions

- **UNKNOWN:** exact numerical equivalence of the GPU diamond search with the CPU's ME algorithm; the reference offers no guarantee.
- **UNKNOWN:** whether the four-pixel padding in MV bounds is sufficient for all block sizes and bit depths.
- **UNKNOWN:** behaviour when a pyramid level has fewer than two macroblocks — the host skips it, but the effect on the next finer level's `in_mvs` is not documented here.
- **UNKNOWN:** whether `mvp_buffer` is guaranteed initialized before the first launch (the first iteration sets `b_first_iteration=1` and does not read it, but `b_shift_index=1` also suppresses the early-exit condition).
