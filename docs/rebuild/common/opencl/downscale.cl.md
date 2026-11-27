# `common/opencl/downscale.cl`

**Role:** Builds the half-resolution luma pyramid and the packed half-pel (HPEL) reference planes used by every subsequent OpenCL lookahead kernel; also provides a device-side `memset` substitute.

> **Dirty-side behavioural specification.** Describes observable behaviour and structure only; no source text or near-verbatim pseudocode is reproduced.

---

## 1. Header

- Source path: `common/opencl/downscale.cl`
- Role: convert the full-resolution luma plane into (a) a four-channel, four-times-replicated lowres luma image, (b) a packed 32-bit word per lowres position holding the four half-pel phases F/H/V/C, and (c) the successively decimated pyramid used for hierarchical motion search.
- Marker: this document is a dirty-side behavioural specification derived from the reference implementation and public OpenCL literature.

---

## 2. Purpose

The lookahead operates on a half-resolution luma plane (8×8 samples per 16×16 macroblock) so that motion and intra analysis are cheap. This file performs that reduction on the GPU, avoiding a host-side downscale and a separate upload. It simultaneously constructs the interpolated half-pel planes that the sub-pixel refiners need, packing the four phases into one 32-bit word so a single texture fetch returns all of them. Finally, it generates the coarser pyramid levels by repeatedly averaging 2×2 blocks, and supplies a fill kernel to initialize rate-control buffers because the targeted OpenCL version predates `clEnqueueFillBuffer`/`clEnqueueFillImage`.

---

## 3. Public interface

Four kernels are exported to the host program:

- **`downscale_hpel( const global pixel *fenc, write_only image2d_t fenc_img, write_only image2d_t hpel_planes, int stride )`** — one work item per lowres position. Reads a 2×2 neighborhood (plus one extra column for the horizontal half-pel samples) from the full-resolution luma buffer, computes the four half-pel phase values, writes them packed into `hpel_planes`, then writes four consecutive lowres luma samples into the four channels of `fenc_img`.
- **`downscale1( read_only image2d_t higher_res, write_only image2d_t lower_res )`** and **`downscale2( ... )`** — functionally identical decimation kernels; the host alternates between them to avoid enqueueing the same kernel twice in a row (a workaround for a driver scheduling bug). Each work item produces one output texel by averaging four input texel-channels in a 2×2 footprint.
- **`memset_int16( global int16_t *buf, int16_t value )`** — one work item per element; writes `value` into `buf`.

All four are created from the program by name in `common/opencl.c` and dispatched exclusively from `x264_opencl_lowres_init()` in `encoder/slicetype-cl.c`.

---

## 4. State and data structures

### 4.1 `fenc_img` — lowres luma image

- Image format: `CL_RGBA` / `CL_UNSIGNED_INT8`; dimensions in texels are `mb_width*8*SIZEOF_PIXEL` by `mb_height*8*SIZEOF_PIXEL` (for 8-bit pixels this is 8 texels per macroblock axis).
- Each texel at `(x,y)` contains four successive lowres luma samples: channel 0 is `P(x,y)`, channel 1 is `P(x+1,y)`, channel 2 `P(x+2,y)`, channel 3 `P(x+3,y)`. Consecutive texels therefore overlap: a sample appears in up to four adjacent texels. The source explicitly calls this a "4× replication", trading memory size for fewer texture fetches.
- At the right edge the last three channels repeat the last valid sample because the work item advances `x` with a saturating `select`.
- Consumers read only channel `.s0` for a single pixel, or all four channels to fill four adjacent cache slots in one fetch.

### 4.2 `hpel_planes` — packed half-pel image

- Image format: `CL_R` / `CL_UNSIGNED_INT32`; same texel dimensions as `fenc_img`.
- Each 32-bit word holds four 8-bit samples in little-endian channel order: byte 0 = F (full-pel), byte 1 = H (horizontal half-pel), byte 2 = V (vertical half-pel), byte 3 = C (center / both-directions half-pel). Consumers extract byte `8*hpel_index` with a mask of `0xFF`, where `hpel_index = ((qpos.x&2)>>1) + (qpos.y&2)` maps to 0/1/2/3.

### 4.3 Full-resolution input

- `fenc` is a plain global pointer to the frame's luma plane; `stride` is the full-resolution row stride in bytes. Work item `(x,y)` bases its reads at row `2*y` of that buffer and uses row `2*y+1` plus, for the lowest row only, a repeated row `2*y+1` instead of a nonexistent row `2*y+2`.

### 4.4 Pyramid images

- `scaled_image2Ds[0]` is the output of `downscale_hpel`; levels 1..3 are produced by repeated `downscale1`/`downscale2`, each halving both texel dimensions.
- `inv_qscale_factor` is a per-macroblock `int16_t` buffer initialized to 256 ("no AQ") by `memset_int16` when AQ is disabled, or uploaded from the frame when AQ is enabled.

---

## 5. Algorithm / behaviour

### 5.1 `downscale_hpel`

For work item `(x,y)` let the full-resolution rows be `R0` at `2*y` and `R1` at `2*y+1`, with `R2` equal to `R1` on the last lowres row and `R1` otherwise. All filters use `rhadd`, i.e. round-up-half averaging `(a+b+1)>>1`, matching the reference C downscale.

Half-pel phases:

- **F** at the even/even position: average horizontally the two vertically-averaged pairs `(R0[2x],R1[2x])` and `(R0[2x+1],R1[2x+1])`.
- **H** at the even/odd horizontal position: horizontal average of the vertically-averaged pairs at columns `2x+1` and `2x+2`.
- **V** at the odd/even vertical position: horizontal average of the vertically-averaged pairs formed from rows `R1`,`R2` at columns `2x` and `2x+1`.
- **C** at the odd/odd position: same as V but using columns `2x+1`,`2x+2`.

Each of the four subcubic results is masked to eight bits and shifted into its byte slot, then a single `write_imageui` stores the packed word at `(x,y)`.

Lowres luma replication: the sample already computed as F is placed in channel 0. The work item then advances its local `x` by one (clamped to the image width) three times, recomputing the F filter at each new column and storing those values into channels 1, 2 and 3. The vector is written to `fenc_img` at the original `(x,y)`. Hence a single texture fetch at any position returns four consecutive lowres pixels, and reads near the right edge see a replicated edge sample.

Work mapping: global dimensions are `[8*mb_width, 8*mb_height]`, one work item per lowres texel column/row; the host supplies no explicit local dimensions, so the implementation chooses them.

### 5.2 `downscale1` / `downscale2`

Each work item reads two texel rows from `higher_res`: row `2*y` and row `2*y+1`, at texel column `2*x` and, for the right half of the output, column `2*x+2`. Each texel contributes four channels, so a 2×2 footprint covers eight source channel values.

- Channel 0 of the output is the average of channels 0 and 1 of the upper-left texel and channels 0 and 1 of the lower-left texel, using nested `rhadd` so the rounding is applied at each stage (two round-up-half steps, not one exact quarter).
- Channels 1, 2 and 3 are computed analogously from the right half of the same texel pair, then from the next texel pair two columns over.
- Each channel is guarded by a `select` that falls back to the previously computed channel when the corresponding source column would lie past the image width. The source notes these guards appear redundant but are required to work around a driver bug; removing them changes output on affected drivers.
- The four results are written as one `uint4` to `lower_res` at `(x,y)`.

`downscale2` is a byte-for-byte equivalent second copy. The host alternates `downscale1`/`downscale2` per pyramid level so the same kernel is never enqueued consecutively, avoiding a dependency-calculation bug on AMD Southern Islands (the source notes the underlying driver bug was theoretically fixed in mid-2012, but the workaround is kept for all GPUs because it has no cost).

### 5.3 `memset_int16`

Linear fill: work item `i` writes `buf[i] = value`. Global size equals the number of elements. Used to set `inv_qscale_factor` to 256 (a neutral multiplier) for every macroblock when AQ is disabled.

### 5.4 Host dispatch contract

`x264_opencl_lowres_init()` in `encoder/slicetype-cl.c`:

1. Allocates shared images/buffers once: `weighted_luma_hpel` (`CL_R`/`CL_UNSIGNED_INT32`), `weighted_scaled_images[0..3]` (`CL_RGBA`/`CL_UNSIGNED_INT8`), the MV/cost/statistic buffers, and two `luma_16x16_image` buffers. Per-frame it allocates `luma_hpel` and four `scaled_image2Ds`.
2. Uploads the full-resolution luma plane into `luma_16x16_image[last_buf]` via a page-locked staging buffer.
3. Uploads `inv_qscale_factor` from the frame if AQ is enabled, otherwise launches `memset_int16` with value 256 and global size `mb_count`.
4. Launches `downscale_hpel` with global `[8*mb_width, 8*mb_height]`, arguments `(luma buffer, scaled_image2Ds[0], luma_hpel, stride)`.
5. Loops over pyramid levels, halving `gdim` each time and breaking when either dimension falls below 16; alternates `downscale_kernel2` and `downscale_kernel1`.
6. Later launches the intra kernel on `scaled_image2Ds[0]` and the row-sum kernels; result buffers are queued for asynchronous readback into `fenc->lowres_costs[0][0]`, `fenc->i_row_satds[0][0]` and `fenc->i_cost_est[0][0]`.

### 5.5 GPU versus CPU output; host selection

These kernels produce no user-visible coded samples; their outputs are analysis metadata. The GPU path is taken only when `h->param.b_opencl` is true and OpenCL initialization succeeded; otherwise the same metadata is produced by the CPU lookahead in `slicetype.c`. The downscale filters are deliberately matched to the reference C implementation (same `rhadd` rounding, same edge clamping), so the lowres pixels should agree with the CPU plane, but the overall lookahead decisions can still differ because later kernels approximate different searches. UNKNOWN: whether any driver-specific `select` workaround can produce values different from the CPU downscale at image borders.

---

## 6. Dependencies

- **Calls:** OpenCL built-ins `read_imageui`, `write_imageui`, `rhadd`, `select`, `abs_diff` (via header helpers in other files, not directly here).
- **Uses from `x264-cl.h`:** `pixel`, `int16_t`, sampler definition.
- **Called by:** `x264_opencl_lowres_init()` in `encoder/slicetype-cl.c`; kernels are created by name in `common/opencl.c`.
- **Consumers of outputs:** `intra.cl` (`fenc_img`), `motionsearch.cl` (pyramid levels), `subpel.cl` and `bidir.cl` (`hpel_planes`), `sum_intra_cost`/`sum_inter_cost` (`inv_qscale_factor`).

---

## 7. Constants and tables

- `NUM_IMAGE_SCALES` = 4 (from `opencl.h`): the number of pyramid levels allocated and the number of hierarchical search resolutions.
- Minimum pyramid dimension 16: the host stops decimating when either texel dimension drops below 16, so very small frames skip coarse levels.
- `SIZEOF_PIXEL` = 1 for the 8-bit build: sets the texel dimensions to `8*mb_width` by `8*mb_height`.
- Half-pel byte order F/H/V/C with shift `8*hpel_index`, mask `0xFF`; `hpel_index` is the sum of bit 1 of x and bit 2 of y, matching the 2×2 phase layout.
- `PAGE_LOCKED_BUF_SIZE` = 32 MiB and `MAX_FINISH_COPIES` = 1024 (from `opencl.h`) govern host staging, not kernel math.

---

## 8. Performance notes

- Texture cache: the 4× replicated luma layout lets a work item read one `uint4` and obtain four consecutive samples; kernels that need four adjacent values (the intra cache loader) use one fetch instead of four. The cost is 4× the image storage.
- Coalescing: consecutive work items write consecutive texels in both output images, giving fully coalesced 16-byte `write_imageui` stores.
- Redundant reads: `downscale_hpel` recomputes the F filter four times per work item (with shifting column) to fill the replicated channels; this is intentional because it avoids a second pass and keeps all writes in one vector store.
- Local memory: none used in this file; all filters operate in registers.
- Occupancy: no explicit local size, so the driver selects the work-group size; the source notes a historical AMD Tahiti scheduling bug handled by alternating two kernel instances rather than by tuning occupancy.
- Platform limitations: OpenCL 1.1 lacks a buffer/image fill command, hence `memset_int16`; the alternating-kernel workaround is retained for all vendors because it is free.
- The `select` guards in `downscale1/2` are a driver-bug workaround and must be preserved even though logically redundant.

---

## 9. References

- Khronos Group, *The OpenCL Specification* 1.1/1.2: image objects, `CL_RGBA`/`CL_UNSIGNED_INT8` formats, `write_imageui`, `rhadd`, `select`, `CLK_ADDRESS_CLAMP_TO_EDGE`.
- ITU-T Rec. H.264 / ISO/IEC 14496-10, clause on integer and half-pel interpolation: motivation for F/H/V/C half-pel phases.
- AMD, *AMD OpenCL User Guide* / Southern Islands errata notes: driver scheduling dependency bug and the kernel-alternation workaround described in the source comments.
- General GPU image-pyramid literature: successive 2×2 box reduction for hierarchical motion estimation.

---

## 10. Verification checklist

- For a uniform input value `V`, confirm every F/H/V/C byte in `hpel_planes` equals `V` and every channel of `fenc_img` equals `V`.
- For a known small pattern, verify the F phase equals `rhadd(rhadd(a,b),rhadd(c,d))` for the 2×2 block, i.e. round-up-half at both stages, not a single rounded quarter.
- Verify the H, V and C phases use the correct column/row offsets described above and that the bottom row repeats row `2*y+1` instead of reading out of bounds.
- Verify channel replication on the last valid column: channels past the right edge must repeat the last sample.
- Verify the packed HPEL word byte order by reading it back as four `uint8` values and comparing with the four computed phases.
- Verify `downscale1` and `downscale2` produce identical images for the same input, including at the right edge where the `select` guards activate.
- Verify that repeated decimation produces the expected pyramid sizes and that the host stops at the 16-texel threshold.
- Verify `memset_int16` fills exactly `mb_count` elements and that the value 256 is interpreted as a neutral AQ multiplier by `sum_intra_cost`.
- Verify GPU-generated lowres pixels match the CPU lowres luma plane bit-for-bit for a representative frame (if the clean implementation provides a CPU path for comparison).

---

## 11. Open questions

- **UNKNOWN:** precise image-width accounting — channels hold four successive lowres samples while the declared texel width is `8*mb_width`; consumers address texels at lowres-pixel coordinates and rely on overlap. Confirm with a live device that no row is silently truncated.
- **UNKNOWN:** whether the source's driver-bug workaround (`select` guards) is still necessary on modern drivers; it is harmless but costs a few instructions.
- **UNKNOWN:** whether `downscale2` was ever intended to diverge from `downscale1`; the source states they are identical.
- **UNKNOWN:** behaviour for 10-bit input, given `CL_UNSIGNED_INT8` formats are hard-coded.
