# `encoder/me.h` — motion-estimation interface

**Role (one sentence):** This small header defines the per-block motion-search descriptor and the public entry points for full-pel search, sub-pel/RD refinement and bidirectional refinement.

> **Dirty-side behavioural specification.** Written from the reference source and public literature for the clean-room reimplementation. No source code or near-verbatim pseudocode is reproduced; only behaviour, structure, constants and contracts are described.

---

## 1. Header

- Source path: `encoder/me.h` (about 111 lines).
- Role: shared data type and function declarations used by `analyse.c`, `me.c` and `rdo.c`.
- Every exported symbol is wrapped in `x264_template(...)`, a build-time renaming macro; the logical names below are what matters.

---

## 2. Purpose

Motion estimation needs a single, reusable "search job" object because the analyser searches the same block against many reference frames and partitions. The header fixes that object's layout and the small set of operations that can be performed on it, so that the analyser can prepare a job once, mutate only the reference pointers and MVP, and call the same search routine repeatedly. It also centralises the comparison-and-copy helper macros and the saturation constants used throughout the search code.

---

## 3. Public interface

### Constants

- `COST_MAX = 1<<28` — maximum ordinary search cost (about 268 million). Costs are accumulated in signed 32-bit integers and compared with this sentinel; `INT_MAX` is also used as an initial "not yet found" value by the analyser.
- `COST_MAX64 = 1ULL<<60` — maximum RD cost, accumulated in 64-bit integers because residual entropy cost times lambda can exceed 32 bits at low QP.

### Type `x264_me_t`

A 64-byte-aligned structure whose first member is aligned explicitly (a compiler hack to force both the base address and `sizeof` to be multiples of the alignment). Fields, grouped by role:

**Inputs set by the caller**

- `int i_pixel` — pixel/block class (`PIXEL_16x16`, `16x8`, `8x16`, `8x8`, `8x4`, `4x8`, `4x4`), used to select SIMD comparators and width/height.
- `uint16_t *p_cost_mv` — table of `lambda * bits` for MV components, indexed *relative to the MVP* (negative indices valid).
- `int i_ref_cost` — bit cost of the chosen reference index, added by the analyser rather than by the search itself.
- `int i_ref` — reference index in the chosen list.
- `const x264_weight_t *weight` — weighted-prediction descriptor; `x264_weight_none` when inactive.
- `pixel *p_fref[12]` — reference plane pointers: index 0 full-pel luma, 1–3 the three other half-pel luma phases (only when sub-pel search enabled), 4–7 and 8–11 the corresponding chroma phases in 4:4:4, or index 4 a single chroma pointer in subsampled formats.
- `pixel *p_fref_w` — luma reference plane with weighting already applied, used by the full-pel fast path.
- `pixel *p_fenc[3]` — source planes at the block offset.
- `uint16_t *integral` — integral image for the ESA/TESA successive-elimination search.
- `int i_stride[3]` — strides of the three reference planes.
- `int16_t mvp[2]` (4-byte aligned) — predicted MV in quarter-pel units; the MV bit table is based on this.

**Outputs written by the search/refinement routines**

- `int cost_mv` — `lambda * bits` for the finally selected MV.
- `int cost` — distortion plus MV bits in the full-pel/SATD phase; reference cost is added by the caller. In RD refinement this field seeds the 64-bit RD comparison.
- `int16_t mv[2]` (8-byte aligned) — selected MV in quarter-pel units.

**Lifetime:** the analyser owns this object (as fields of `x264_mb_analysis_t`); searches mutate only the documented outputs plus MV-related internals. Scratch planes pointed to by `p_fref[]` belong to the frame manager and remain valid for the whole frame.

### Functions

| Declaration | Contract |
|---|---|
| `void x264_me_search_ref(x264_t*, x264_me_t*, int16_t (*mvc)[2], int i_mvc, int *p_fullpel_thresh)` | Full search with optional extra predictors and optional shared reference-pruning threshold. The `x264_me_search(h,m,mvc,i_mvc)` macro is the same call with a null threshold pointer. |
| `void x264_me_refine_qpel(x264_t*, x264_me_t*)` | Refine the winner using the "post-selection" half/quarter-pel iteration counts for the current `subme`. |
| `void x264_me_refine_qpel_refdupe(x264_t*, x264_me_t*, int *p_halfpel_thresh)` | Duplicate-reference shortcut: no half-pel, at most two quarter-pel iterations. |
| `void x264_me_refine_qpel_rd(x264_t*, x264_me_t*, int i_lambda2, int i4, int i_list)` | RD-driven quarter-pel refinement of partition `i4` in list `i_list`; writes MV/MVD caches. |
| `void x264_me_refine_bidir_rd(x264_t*, x264_me_t *m0, x264_me_t *m1, int i_weight, int i8, int i_lambda2)` | Joint RD refinement of both Bi MVs for 8x8 index `i8`. |
| `void x264_me_refine_bidir_satd(x264_t*, x264_me_t *m0, x264_me_t *m1, int i_weight)` | Same but SATD-only. |
| `uint64_t x264_rd_cost_part(x264_t*, int i_lambda2, int i8, int i_pixel)` | Implemented in `rdo.c`; computes distortion plus entropy cost for a trial partition. |

### Helper macros

A family of `COPYn_IF_LT` / `COPY2_IF_GT` macros atomically update a cost and one to four associated values when a strictly smaller (or greater) cost is found. They exist so that a single comparison selects both a cost and its accompanying MV/direction tuple. The `x264_me_search` and `x264_template` macros are compile-time aliases.

---

## 4. State and data structures

Only one data structure is defined here. Important invariants a clean implementation must preserve:

1. Alignment: 64-byte base and size, 4-byte `mvp`, 8-byte `mv`. SIMD code loads these fields with aligned instructions.
2. Quarter-pel MVs: all motion vectors are stored multiplied by four; full-pel coordinates are obtained by `(v+2)>>2`.
3. Table indexing: `p_cost_mv` points at the *MVP* entry, so `p_cost_mv[mv - mvp]` is the bit cost of a displacement relative to the predictor. This is why negative indices are legitimate and why the table is allocated with a large prefix/suffix.
4. `i_pixel` must be one of the seven supported block classes; SIMD function tables are indexed by it.
5. `p_fref_w` may differ from `p_fref[0]` only for weighted prediction; the analyzer sets both.

---

## 5. Algorithm / behaviour

The header itself performs no computation. Its behavioural significance is the *contract* it imposes on the implementation in `me.c`:

- The search reads all input fields and must not modify them except `mvp` in the RD path (where the analyser recomputes it for a partition). Outputs are `cost`, `cost_mv`, `mv`.
- The cost returned by a full search is `SATD/SAD(distortion) + lambda * mv_bits`; `i_ref_cost` is added by the caller *after* the call, so a caller comparing candidates across reference frames must add it itself and must remember to subtract it again before a winner-only refinement of blocks `<= 8x8`.
- Sub-pel refinement may replace the SAD-based cost with a SATD-based one and may add chroma distortion; the two costs are not numerically comparable across that switch, which is why the reference re-measures the current best at the switch point.
- The `p_fullpel_thresh` pointer is an optional shared best-cost estimate across reference frames; passing null disables that optimisation but must not change the chosen vector for a single reference.
- The copy macros use strict `<`, never `<=`; ties keep the earlier candidate. This is essential for bit-exact emulation.

---

## 6. Dependencies

- Included by `encoder/me.c` and `encoder/analyse.c`; it includes `common/common.h` indirectly through those files and relies on the `pixel`, `x264_weight_t`, `x264_t` and `x264_pixel_size` definitions from `common/`.
- `x264_rd_cost_part` is defined in `rdo.c`; `x264_me_search_ref` and the refinements in `me.c`.
- The `ALIGNED_*`, `COPY*_IF_LT`, `x264_template` and block-class enumerations come from `common/`.

---

## 7. Constants and tables

- Two saturation constants: `COST_MAX` (2^28) and `COST_MAX64` (2^60).
- Five `COPY*` helper macros with 1, 2, 3 and 4 payload values; one greater-than variant.
- No data tables are defined in this header; the search iteration and pattern tables live in `me.c`, and the MV-bit tables are allocated in `analyse.c`.

---

## 8. Performance notes

1. The forced 64-byte alignment lets the SIMD comparators load `x264_me_t` fields without split loads; a clean implementation should keep it [1].
2. Keeping `p_cost_mv` relative to the MVP avoids a multiply or lookup offset in the innermost loop — a small but measurable gain because the cost lookup happens once per candidate [1].
3. The `COPYn_IF_LT` pattern encourages a branchless conditional move on SIMD-capable compilers; using `<=` or adding epsilon would both change output and hurt predictability.
4. Cache behaviour: a full `x264_me_t` is copied wholesale when a new best is found; keeping it compact (one cache line) is intentional.
5. Concurrency: descriptors are per-MB/per-partition, so separate threads can use separate instances safely; only the read-only cost tables are shared.

---

## 9. References

[1] x264 source documentation on alignment and SIMD function pointers (`common/x86/`, `common/arm/`); general guidance on cache-line-sized hot structures.

[2] ITU-T Rec. H.264 — quarter-pel motion vector representation and prediction.

---

## 10. Verification checklist

1. `sizeof(x264_me_t)` is a multiple of 64 and its address is 64-byte aligned in the reference build.
2. `mvp`, `mv` are at the documented alignments.
3. With a zero-distortion reference, `cost` equals the table value at `p_cost_mv[mv - mvp]`.
4. `COST_MAX` and `COST_MAX64` never overflow in a high-lambda stress test.
5. Copy macros preserve the earlier candidate on exact ties (observable via deterministic MV selection).
6. Each refinement entry point writes all three output fields (`cost`, `cost_mv`, `mv`) on every exit path, including the early boundary exit.

---

## 11. Open questions

- **UNKNOWN:** exact padding/field offsets of `x264_me_t` on 32-bit builds; the definition uses explicit alignment attributes whose layout may differ from a clean implementation using manual padding.
- **UNKNOWN:** whether `p_cost_mv` indexing beyond +/- the configured MV range is ever exercised; the reference allocates extra margin but does not document it in this header.
- **UNKNOWN:** the intended semantics of `x264_me_search_ref`'s `p_fullpel_thresh` when called with a null `mvc` list; behaviour is inferred from `me.c` only.
