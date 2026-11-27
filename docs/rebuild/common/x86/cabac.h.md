# cabac.h — arithmetic coder state and interface

Source path: `common/cabac.h` (note: located in `common/`, not `common/x86/`, in the
reference tree; documented here per assignment).
Role: defines the CABAC encoder state object and the complete binary-arithmetic encode /
size-estimation API used by the x86 kernels.
**Dirty-side behavioural specification** — own words, no source reproduction.

---

## 1. Header

This header is the central contract for x264's CABAC encoder. It defines `x264_cabac_t`,
declares initialization, encoding, flushing, and bit-cost estimation functions, and selects
asm vs C implementations based on `HAVE_MMX`/`HAVE_AARCH64`. Its field layout is deliberately
matched by the offsets hardcoded in `common/x86/cabac-a.asm`, so it is part of the SIMD ABI.

## 2. Purpose

CABAC encodes nearly all syntax elements in a H.264 slice. The encoder must maintain a
binary arithmetic coder (low/range), a queue of pending bytes, a pointer into the output
buffer, 1024 context states, and a bit-cost accumulator for rate-distortion decisions. This
header centralizes that state so that fast assembly routines and portable C routines can be
interchanged at build time.

## 3. Public interface

### 3.1 State lifecycle

* `x264_cabac_context_init(h, cb, slice_type, qp, model)` — copies a precomputed 460- or
  1024-byte context state block (depending on 4:2:0 vs 4:4:4) into `cb->state`.
* `x264_cabac_encode_init_core(cb)` — resets arithmetic state to `low=0`, `range=0x01FE`,
  `queue=-9`, `bytes_outstanding=0`; does not touch buffer pointers or contexts.
* `x264_cabac_encode_init(cb, p_data, p_end)` — core reset plus `start=p`, `current=p`,
  `end=p_end`.
* `x264_cabac_encode_flush(h, cb)` — terminates the arithmetic code, emits remaining bytes,
  resolves outstanding 0xff/carry state, and appends the frame-dependent terminating bits.

### 3.2 Binary encode primitives

* `x264_cabac_encode_decision_c` / `_asm` `(cb, ctx, b)` — regular context-coded binary
  decision: subtract LPS range, choose MPS/LPS branch, renormalize, update state.
* `x264_cabac_encode_bypass_c` / `_asm` `(cb, b)` — bypass (equiprobable) binary symbol.
* `x264_cabac_encode_terminal_c` / `_asm` `(cb)` — terminate the arithmetic interval.
* `x264_cabac_encode_ue_bypass(cb, exp_bits, val)` — encode an unsigned Exp-Golomb-like value
  using bypass bits, used for large coefficient levels.

Macros `x264_cabac_encode_decision`, `_bypass`, `_terminal` resolve to the asm variants when
`HAVE_MMX` or `HAVE_AARCH64` is true, otherwise to the C variants.
`x264_cabac_encode_decision_noup` aliases the same function (the "no update" distinction
only exists in the size-estimation helpers).

### 3.3 Size-estimation helpers (inline, no bitstream output)

* `x264_cabac_size_decision(cb, ctx, b)` — look up state, advance it via the transition table,
  add `cabac_entropy[state ^ b]` to `cb->f8_bits_encoded`.
* `x264_cabac_size_decision2(state_ptr, b)` — same, operating on a raw state byte pointer and
  returning the entropy value.
* `x264_cabac_size_decision_noup(cb, ctx, b)` / `_noup2(state_ptr, b)` — accumulate/return
  entropy without modifying the state (used when the caller will update it separately).
* `x264_cabac_pos(cb)` — returns the current bit position:
  `(current - start + bytes_outstanding) * 8 + queue`.

## 4. State and data structures

`x264_cabac_t` fields, in declaration order:

| Field | Type | Semantics |
|---|---|---|
| `i_low` | int | lower bound of the current arithmetic interval |
| `i_range` | int | width of the current interval, kept in [256, 511] after renormalization |
| `i_queue` | int | pending output bits, stored with a -8 offset for faster assembly |
| `i_bytes_outstanding` | int | count of `0xff` bytes whose carry is unresolved |
| `p_start` | uint8_t* | beginning of the slice's CABAC output region |
| `p` | uint8_t* | next write position |
| `p_end` | uint8_t* | end of the output region (guard) |
| `f8_bits_encoded` | int | 64-byte-aligned fixed-point bit count (1/256 bits) for RD estimation |
| `state[1024]` | uint8_t[] | one byte per CABAC context; only 460 used in 4:2:0 builds |
| `padding[12]` | uint8_t[] | pads the 1036-byte tail to a multiple of 16 for aligned copies |

Invariants:

* after every decision, `range >= 256`;
* `queue` is negative whenever fewer than 8 bits are ready, except during emission;
* `p` never exceeds `p_end` in normal operation (the caller reserves slack for flushing);
* context state bytes encode MPS in bit 0 and probability state in bits 1..6;
* `f8_bits_encoded` is only meaningful when the caller uses the size-decision helpers (e.g.
  trellis), and is reset externally as needed.

Ownership: a single `x264_cabac_t` is owned by the slice encoder thread; no internal locking.
The context block is produced once per frame in `x264_cabac_init()` and copied per slice.

## 5. Algorithm / behaviour

### 5.1 Context initialization

For each slice type (I vs P/B with two model variants), QP 0..51, and context index, the
initial state is computed from an per-context intercept and slope:
`state = clip( (m * QP >> 4) + b, 1, 126 )`, then mapped into the packed byte form
`min(state, 127-state) << 1 | (state >> 6)`. This packs the MPS bit and the probability
index so that the encoder can use it directly as a table index.

### 5.2 Arithmetic state update

A regular decision reads the packed state, obtains the LPS range from
`range_lps[state>>1][(range>>6)-4]`, subtracts it from `range`, and conditionally adds the
subtracted value to `low` and swaps in the LPS range when the encoded symbol differs from
MPS. The state is then advanced with `transition[state][b]` and the interval renormalized by
shifting `low`/`range` until `range >= 256`, tracking the shift in `queue`.

Bypass doubles `low` and adds `b * range` (equiprobable decision), increments `queue`, and
emits if possible. Terminal subtracts 2 from `range` and renormalizes.

### 5.3 Byte emission and carry

When `queue >= 0`, a byte is ready. If it is `0xff`, emission is deferred and
`bytes_outstanding` increments. Otherwise the carry bit is added to the previous byte and
each outstanding `0xff` is emitted as `0x00`, followed by the current byte. This is required
by the H.264 spec: a carry can propagate through a run of `0xff` bytes but cannot cross a
byte that is not `0xff`.

### 5.4 Flush

Flush adds `range - 2` to `low`, forces bit 0, shifts by 9, emits twice (forcing the carry
to flush), then emits a final byte containing one frame-dependent bit selected from a fixed
32-bit pattern indexed by `frame & 31`. Any remaining outstanding `0xff` bytes are written.
The frame-dependent bit prevents accidental start-code emulation patterns in CABAC-only
streams in some corner cases and is part of x264's bitstream convention.

### 5.5 Size estimation

Entropy values are fixed-point approximations of `-log2(probability)` in 1/256-bit units
(table `cabac_entropy[128]`). Adding `entropy[state ^ symbol]` gives the fractional bit cost
of a decision without writing bits. The `_noup` variants skip the state transition for cases
where the caller models the state separately (e.g. trellis nodes carrying their own state
copies). These helpers are what make RD decisions consistent with the actual encoder.

### 5.6 Position tracking

`cabac_pos` converts the internal -8-offset queue into an absolute bit count by adding
`bytes_outstanding * 8` and `queue`. It is used by the rate-control / slice-size logic to
check whether CABAC data is nearing its budget.

### 5.7 Dispatch and ISA differences

On x86 and AArch64 the public encode functions resolve to assembly. On other architectures
the C functions are used. The `bmi2` variants (x86 only) are internal to `cabac-a.asm` and
use leading-zero-count and bit-field instructions to remove variable shifts; their semantics
are identical to the C functions.

## 6. Dependencies

* `common/common.h` — `x264_t`, `ALIGNED_64`, slice-type constants, `QP_MAX_SPEC`.
* `common/tables.c` — context initialization tables, `cabac_range_lps`, `cabac_transition`,
  `cabac_entropy`, `cabac_renorm_shift`.
* `common/cabac.c` — C implementations and `x264_cabac_init()`.
* `common/x86/cabac-a.asm` — assembly implementations; its `struc cb` must match the field
  offsets above exactly.
* `encoder/cabac.c` — per-slice use of the encoder.

## 7. Constants and tables

* Initial `range = 0x01FE`, `queue = -9`, `low = 0`.
* Range bucket selector `(range >> 6) - 4` yields 0..3 for normalized ranges.
* `cabac_renorm_shift[64]`: shifts 6,5,4,4,3×4,2×8,1×16,0×32 — derived from the number of
  leading zeros of `range`.
* `cabac_entropy`: 128 fixed-point values, pairs of small/large costs for complementary
  symbols; `entropy[state]` and `entropy[state^1]` are the two decision costs.
* Context count 460 for 4:2:0/4:2:2, 1024 for 4:4:4.
* Flush tag bits from pattern `0x35a4e4f5`, one bit per frame index modulo 32.

## 8. Performance notes

`encode_decision` is likely the single most frequently executed function in the encoder.
Keeping state in registers across consecutive calls would be ideal but conflicts with the
function-pointer ABI; x264 instead reduces per-call overhead with `cmov`, table loads, and
BMI2.

**Concrete SIMD speed opportunities (referenced):**
1. *Wider vectors*: CABAC is inherently scalar; the available parallelism is across
   coefficients, which is why the residual kernels use SIMD for absolute value collection
   and entropy lookup (§5.5 of `cabac-a.asm.md`).
2. *Prefix/scan*: Exp-Golomb suffix cost and UE bypass use leading-zero counts; on BMI2 these
   are single `lzcnt` instructions. A clean implementation should use `clz`/`lzcnt`
   intrinsics rather than a loop.
3. *Branch removal*: use conditional moves for MPS/LPS selection and mask-based emission;
   avoid a branch on `queue >= 0` by predication where the ISA permits.
4. *Cache*: keep `cb` under 256 bytes up to `state[]` so all hot fields share few cache lines;
   the 64-byte alignment of `f8_bits_encoded` exists for this reason.

## 9. References

* ITU-T H.264 / ISO IEC 14496-10, Clause 9.3 — CABAC decoding/encoding process.
* Marpe, Schwarz, Wiegand, op. cit.
* x264 public source: `common/cabac.h`, `common/cabac.c`, `common/tables.c`,
  `common/x86/cabac-a.asm`.
* Intel Intrinsics Guide: `lzcnt`, `shlx`, `shrx`, `bzhi`, `cmov`.

## 10. Verification checklist

1. Verify `sizeof(x264_cabac_t)` and each field offset against the assembly structure
   definition; in particular that `f8_bits_encoded` is 64-byte aligned.
2. Check `cabac_pos` arithmetic by driving a known decision sequence and comparing to the
   number of bytes emitted times eight plus pending bits.
3. Compare `_c` and `_asm` primitives byte-for-byte over randomized decision streams and
   all `bytes_outstanding` states, including flush.
4. Verify size helpers: for a known state array and symbol sequence, the accumulated
   `f8_bits_encoded` must equal the sum of `cabac_entropy[state^b]` with the correct state
   transitions for `_decision` and no transitions for `_noup`.
5. Verify context initialization for representative QPs and all slice types against a
   freshly computed state array.

## 11. Open questions

* **UNKNOWN**: exact semantics and usage of the frame-dependent flush bit beyond the code
  comment; its effect on bitstream compatibility should be verified against H.264 conformance.
* **UNKNOWN**: whether `padding[12]` is relied upon by any SIMD routine other than the
  aligned context copy mentioned in the header comment.
* **UNKNOWN**: whether the AArch64 assembly uses identical field offsets (out of scope here).
