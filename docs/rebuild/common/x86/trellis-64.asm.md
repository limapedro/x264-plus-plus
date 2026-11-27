# `common/x86/trellis-64.asm` — x86-64 trellis quantisation

**Dirty-side behavioural specification.** Source path:
`common/x86/trellis-64.asm`. Role: SIMD implementation of x264's trellis-based
rate–distortion-optimal quantiser for CABAC entropy coding, including the psy
(psycho-visual) variant and the DC/chroma-422-DC specialisations.

---

## 1. Header

Assembly source compiled only on x86-64. It includes `x86inc.asm`/`x86util.asm`
and declares a small `.rodata` section with constants (`pd_m16`, `sq_1`,
`pq_128`, `pq_ffffffff`) and `cextern`s the CABAC tables and coefficient weight
tables from the C layer (`cabac_entropy`, `cabac_transition`,
`cabac_size_unary`, `cabac_transition_unary`, `dct4_weight_tab`,
`dct8_weight_tab`, their `_2_` variants, and the 8x8 scan-offset tables).

The source begins with a long design note describing which optimisations were
tried and rejected; its substance is captured in section 8.

## 2. Purpose

Conventional quantisation rounds each transform coefficient independently.
Trellis quantisation instead finds the coefficient sequence that minimises a
Lagrangian cost `SSD + lambda * bits` over all sequences, using the Viterbi
algorithm over CABAC states. This is the highest-quality quantisation mode and
one of the most complex kernels in the encoder. This file implements it with
four-way SIMD over the Viterbi node contexts (nodes 0–3 in one pass, nodes 1–7
in a second), with interprocedural register allocation to avoid argument passing.

## 3. Public interface

Six entry points, each instantiated twice (SSE2 baseline and SSSE3):

| Symbol | `num_coefs` | `dc` | `psy` | Purpose |
|---|---|---|---|---|
| `trellis_cabac_4x4` | 16 | 0 | 0 | normal 4x4 block |
| `trellis_cabac_8x8` | 64 | 0 | 0 | normal 8x8 block |
| `trellis_cabac_4x4_psy` | 16 | 0 | 1 | 4x4 with psycho-visual term |
| `trellis_cabac_8x8_psy` | 64 | 0 | 1 | 8x8 with psycho-visual term |
| `trellis_cabac_dc` | 16 | 1 | 0 | luma/chroma DC 4x4 cascade |
| `trellis_cabac_chroma_422_dc` | 8 | 1 | 0 | 4:2:2 chroma DC |

Signature (from the source comment, exact types):

```
int trellis_...(
    const int *unquant_mf,      /* r0: quantisation multiplier per position */
    const uint8_t *zigzag,      /* r1: scan order for coefficient i */
    int lambda2,                /* r2: lambda^2, QP-dependent */
    int last_nnz,               /* r3: index of last non-zero coefficient */
    dctcoef *orig_coefs,        /* original (unquantised) coefficients */
    dctcoef *quant_coefs,       /* pre-quantised coefficients (drives abs_level) */
    dctcoef *dct,               /* output quantised coefficients */
    uint8_t *cabac_state_sig,   /* significant-coeff CABAC states */
    uint8_t *cabac_state_last,  /* last-coeff CABAC states */
    uint64_t level_state0,      /* packed level1/gt1 CABAC states (r9/r10) */
    uint16_t level_state1,      /* second half of level states (r11) */
    int b_ac,                   /* first AC coefficient index (4x4 only) */
    dctcoef *fenc_dct,          /* original fenc DCT for psy */
    int psy_trellis );          /* psy strength */
```

Return value: 1 in the normal path, 0 in the special "return zero" path for
4x4 AC-only blocks where all quantised coefficients become zero (the output
`dct` array is zeroed there).

### 3.1 Contract details

* `quant_coefs[i]` is the *pre*-quantised coefficient; the algorithm derives
  `abs_level = |quant_coefs[i]|` (then subtracts 1 for the coef1/coefn cases) and
  evaluates two candidate absolute levels per coefficient: `abs_level` and
  `abs_level+1` (the second via the `.entry*b` entry points).
* `orig_coefs[zigzag[i]]` is the unquantised value whose sign is applied to the
  chosen level in writeback.
* `unquant_mf[zigzag[i]]` is the reconstruction multiplier: the dequantised
  magnitude is `(unquant_mf * abs_level + 128) >> 8`.
* Coefficients are processed in reverse zig-zag order (from `last_nnz` down to
  `b_ac` or 0), as required by the last-coefficient syntax.

## 4. State and data structures

### 4.1 Viterbi node storage (struct-of-arrays layout)
The C `trellis_node_t` array-of-structs is transformed into three parallel
arrays in a large stack allocation `level_tree`:

* `node_score(x)` at offset `x*8` (64-bit scores) — eight current nodes and
  eight previous nodes, each 8 bytes.
* `node_level_idx(x)` at `64+x*4` (32-bit level-tree index).
* `node_cabac_state(x)` at `96+x*4` (32-bit packed CABAC states).
* `SIZEOF_NODE = 16`; `nodes_cur`/`nodes_prev` alternate between two 16-node
  slots at the top of `level_tree`.
* `level_tree` itself holds a linked list of `trellis_level_t` records: each
  record is `(next:16 bits, abs_level:16 bits)` packed into one dword.
  `levels_usedd` is the allocation pointer into this array.

### 4.2 Score bias and sentinel
* `TRELLIS_SCORE_BIAS = 1<<60` is added to the initial node score so that all
  costs remain non-negative and can be compared as unsigned 64-bit values (the
  `score<0` early-out test becomes a sign bit check).
* `TRELLIS_SCORE_MAX = 0xFFFFFFFFFFFFFFFF` is used to initialise inactive nodes.

### 4.3 Constants
* `CABAC_SIZE_BITS = 8`, `LAMBDA_BITS = 4`, so cost scaling is
  `(bits * lambda2) >> (8-4) = (bits*lambda2)>>4`.
* `pd_m16` = four dwords of -16, used to round DC coefficients to multiples of
  16; `pq_128` = 128 for the dequant rounding; `sq_1` = `[1,0]` for the
  `abs_level-1` adjustment.

### 4.4 ABI / register allocation
`DEFINE_ARGS` remaps the x86-64 Windows/SysV argument registers into named
locals. In the Windows ABI `level_state0/1` arrive in r9/r10 and are accessed
through stack slots; on SysV they are stack arguments. Additional stack slots:
`last_nnzm`, `zigzagm`, `orig_coefsm`, `quant_coefsm`, `unquant_mfm`,
`levelgt1_ctxm`, `ssd` (32 bytes for two candidate SSDs), `cost_siglast`
(three dwords), and the `level_tree`.

## 5. Algorithm / behaviour

### 5.1 Initialisation
1. Reserve `pad = 96 + level_tree_size + 16*SIZEOF_NODE + alignment` bytes.
2. Initialise `nodes_cur[0].score = 1<<60`, level index 0, CABAC state packed
   from `level_state0/1`; all other scores to `TRELLIS_SCORE_MAX`.
3. Set `levels_used = 1`, `levelgt1_ctx = 9` (or 8 for chroma 422 DC).
4. For psy, broadcast `psy_trellis` to both dword lanes; for DC-only, broadcast
   twice the unquant multiplier (`unquant_mf*2`) for the special DC rounding.
5. Establish a PIC-style global offset register (`lea r0,[$$]`) so all C tables
   can be addressed relative to one base.

### 5.2 Main loop (two passes)
`TRELLIS_LOOP 0` processes node contexts 0–3; `TRELLIS_LOOP 1` processes 1–7.
Both iterate `i` from `last_nnz` down to `b_ac`/0.

For each coefficient:
1. **Load quantised coefficient**: `abs_level = quant_coefs[i]` (dword for
   high-bit-depth, sign-extended word for 8-bit). If zero, apply the `cost_sig0`
   penalty (CABAC decision "significant=0" times lambda) to node 0 only, then
   call `ZERO_LEVEL_IDX` (all live nodes keep their level index but a new
   level-tree record with `abs_level=0` is allocated), and continue.
2. **Non-zero path**: load `sign_coef = orig_coefs[zigzag[i]]`, compute
   `abs_coef = |sign_coef|`, `q = |quant_coefs[i]|`, `abs_level = q-1`. Swap
   current/previous node buffers.
3. **CABAC cost setup**: compute `sigindex` from the significant-coeff offset
   table (8x8 frame/interlaced, chroma-422 table, or plain index). Read
   `cost_siglast[0] = size_decision(sig_state,0)`, and `cost_sig1 =
   size_decision(sig_state,1)`; for non-last coefficients also read
   `cost_siglast[1] = size_last(last_state,0)+cost_sig1` and
   `cost_siglast[2] = size_last(last_state,1)+cost_sig1`. For the last
   coefficient these are zeroed.
4. **SSD for both candidate levels**: for each candidate absolute level `q` and
   `q+1`:
   * `unquant_abs = (unquant_mf * abs_level + 128) >> 8`
   * `d = abs_coef - unquant_abs`
   * `ssd = d*d` (via `SQUARE`: `pabsd` + `pmuludq` on SSSE3, or unsigned
     multiply + mask on SSE2)
   * scaled by `dct_weight2_tab[zigzag[i]]` (or `<<8` for DC-only).
   * **psy term** (psy variants, AC only): `predicted = fenc_dct[zigzag[i]] -
     sign_coef`; `psy_value = |unquant_abs + SIGN(predicted, sign_coef)|`;
     `psy_weight = dct_weight_tab[zigzag[i]] * psy_trellis`; subtract
     `psy_weight * psy_value` from the SSD.
   * **DC rounding special case** (DC-only or AC coefficient i=0): round
     `unquant_abs` to a multiple of 16 with bias from the sign, i.e.
     `(unquant_abs + (sign_coef>>31) + 8) & ~15`, recompute `d`, `ssd`.
   The two SSDs are stored at `[ssd]` and `[ssd+16]` (plus `[ssd+8]`/`[ssd+24]`
   for the second candidate's "other" lane).
5. **Dispatch by abs_level**: if `abs_level < 1`, call `trellis_coef0_<ctx>`;
   else if `abs_level == 1`, call `trellis_coef1.entry<ctx>`; else call
   `trellis_coefn.entry<ctx>`. Each is called twice: once for the smaller
   candidate (`.entry0/1`) and once for the larger (`.entry0b/1b`), which
   selects the corresponding SSD from `[ssd]` and may increment `abs_level`.
6. **Node update subroutines**:
   * `trellis_coef0`: for coefficient level 0, update node 0 with
     `prev[0].score + ssd0 - cost_sig`, copy the other nodes' scores verbatim
     (a zero coefficient cannot change CABAC state except the significance
     decision), and call `ZERO_LEVEL_IDX`.
   * `trellis_coef1`: evaluate the "level==1" CABAC path. For each live node
     `j`, if `prev[j].score >= 0`, add the SSD and the CABAC bit cost
     (`size_decision(level1_ctx, abs_level>1)` + significance/last costs +
     1 bit for the level-1 suffix), scaled by lambda. If the new score improves
     the current node, store it, update CABAC state and level-tree record with
     `abs_level` (or the implicit 1 for coef1).
   * `trellis_coefn`: for levels ≥2, additionally include the unary prefix
     cost `cabac_size_unary[abs_level-1][gt1_ctx]` and, for `abs_level>=15`,
     the Exp-Golomb suffix cost `bs_size_ue_big(abs_level-15)` computed via
     `bsr`. The state transition is delayed until the node wins.
   * `trellis_bnode_<ctx>`: find the minimum-score node among the live range
     (0–3 or 1–7) and return its index in `rax&7`.
7. **Writeback**: start from `nodes_cur[best].level_idx`, walk the level-tree
   linked list backwards from `last_nnz` to `b_ac`, and for each position write
   `dct[zigzag[i]] = SIGN(abs_level, orig_coefs[zigzag[i]])`. The sign is
   applied with `psignw`/`psignd` (SSSE3) or XOR/subtract (SSE2).
8. **Return**: 1 normally; for 4x4 AC-only blocks that end at the zero node,
   zero the output array and return 0 (`.return_zero`).

### 5.3 CABAC bit-cost details
* `cabac_entropy[state*2]` gives the cost (in 1/256 bit units) of decision 0;
  state XOR 1 gives decision 1.
* `cabac_transition[state*2 + gt1]` gives the next state after a level>1
  decision; `cabac_transition_unary` does so for the unary suffix.
* The `>>4` shift after multiplying by `lambda2` realises `lambda = lambda2>>4`
  with 8 fractional bits (`CABAC_SIZE_BITS - LAMBDA_BITS = 4`).

### 5.4 Prev/cur buffer swapping
At the start of a non-zero coefficient, `nodes_cur` and `nodes_prev` are
exchanged (`xchg`). Coefficients with zero quantised value do not swap, which
matches the C algorithm's handling of runs of zeros.

## 6. Dependencies

* Tables from the C layer: `cabac_entropy`, `cabac_transition`,
  `cabac_size_unary`, `cabac_transition_unary`.
* Weight tables: `dct4_weight_tab`, `dct8_weight_tab`, and `_2_` variants;
  `last_coeff_flag_offset_8x8`, `significant_coeff_flag_offset_8x8`,
  `coeff_flag_offset_chroma_422_dc`.
* Called by `common/quant.c` (dispatcher) when `trellis` is enabled.
* Uses x86util macros `SQUARE`, `LOAD_DUP`, `ABSD`, `PSIGN`, `SUMSUB_BA`,
  `SBUTTERFLY`, etc.

## 7. Constants and tables

* `TRELLIS_SCORE_BIAS = 1<<60` — score offset making Viterbi costs unsigned.
* `CABAC_SIZE_BITS = 8`, `LAMBDA_BITS = 4`.
* `SIZEOF_NODE = 16`, `level_tree_size = 64*8*2*4` (2048 bytes).
* `pd_m16` (-16), `sq_1` (`[1,0]`), `pq_128` (128), `pq_ffffffff`.
* `levelgt1_ctx` initial values: 9 normally, 8 for chroma-422 DC.
* `dct_weight*` tables encode per-coefficient visual importance for the SSD
  (larger weight for low frequencies).

## 8. Performance notes

The source's own design notes are unusually detailed; their substance:
* **Two-axis SIMD:** SSD and psy costs are computed two candidates in parallel
  (`m0`/`m4` pairs), while trellis coef0, the zero-level handler, and the
  coef1/coefn node loops process four node contexts at once. This maps the
  Viterbi state machine onto four SIMD lanes.
* **Interprocedural register allocation:** `trellis_coef*` subroutines are local
  labels with no ABI call frame; arguments (scores, SSDs, lambda, level state)
  live in fixed registers throughout, removing argument-passing overhead and
  reducing code size.
* **Rejected optimizations** (with reasons, useful for a clean implementation):
  1. Separate loops for node contexts 4–7 or smaller subsets — insufficient
     speedup for the instruction-cache cost.
  2. A jump table indexed by the 21 possible live-node sets and `abs_level` —
     the table is unpredictable and costs either icache or call/ret overhead.
  3. Separate coef1 versions for the two `abs_level` candidates — the branches
     saved are not worth the code size.
  4. A `coef2` special case — not worthwhile alongside the other optimisations.
  5. SIMD/unrolled writeback — unexpectedly not faster; keep it scalar.
* **Wider vectors:** the current code uses SSE2/SSE3 128-bit registers; an
  AVX2 port could evaluate 8 node contexts per vector, but the live-node count
  is at most 8, so the gain is limited and the shuffle complexity grows.
* **Preprocessing constants:** all CABAC tables are read-only and addressed via
  a single base register (`GLOBAL`); the bit-packed permutation idea from
  `dct-a.asm` could be applied to the scan-offset tables but is not used here.
* **Avoiding spills:** the node arrays are deliberately struct-of-arrays so that
  score vectors can be copied with one `mova`; the large stack `level_tree`
  is the only intentional memory traffic in the hot loop.
* **Branch behaviour:** the switch on `abs_level` (0/1/≥2) is the only data-
  dependent branch in the per-coefficient path; it is well predicted because
  most coefficients are zero or one.

## 9. References

* ITU-T H.264 CABAC specification (context modelling, `coeff_abs_level1`,
  `coeff_abs_levelgt1` contexts).
* Public papers on trellis-based quantisation for H.264 (e.g. "Rate-distortion
  optimized quantization" by x264 project notes).
* Intel Intrinsics Guide for `pmuludq`, `pabsd`, `psignw`, `bsr`.

## 10. Verification checklist

* For each of the six variants, compare output coefficients and return value
  against the portable C trellis quantiser for random DCT blocks and QPs.
* Verify the two `abs_level` candidates are both considered: construct a block
  where `abs_level+1` yields lower RD cost and confirm it is chosen.
* Verify psy variants: increasing `psy_trellis` must change coefficients in the
  direction predicted by the psy formula (fidelity to `fenc_dct`).
* DC and chroma-422 DC: verify the `&~15` DC rounding produces values divisible
  by 16 and matches the C implementation.
* Verify the `last_nnz` handling: the last significant coefficient must use the
  `cost_siglast[0]/[0]` branch (no last-flag cost).
* Verify the all-zero 4x4 path zeroes the output and returns 0.
* Exhaustive small-block test: enumerate all `orig_coefs` in a small range for
  4x4 DC and compare bit-exactly.

## 11. Open questions

* `UNKNOWN`: exact semantics of `level_state0`/`level_state1` packing across the
  Windows vs SysV ABI (r9/r10 vs stack slots) — verify against the caller.
* `UNKNOWN`: the source's note about `bs_size_ue_big` suffix cost for
  `abs_level>=15`; exact bit-cost formula must be cross-checked with the C
  implementation.
* `UNKNOWN`: whether the `ssd+8`/`ssd+24` lanes are always initialised before
  `.entry*b` reads them in the `coef1` path for ctx_hi (appears to rely on
  interprocedural register state).
* `UNKNOWN`: precise behaviour when `psy_trellis` is negative or very large
  (possible signed overflow in the psy weight product).
