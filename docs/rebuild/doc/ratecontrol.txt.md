# `doc/ratecontrol.txt` — dirty-side behavioural specification

## 1. Header

- Source path: `doc/ratecontrol.txt`.
- Role: a qualitative, historically dated design note describing the encoder's empirical rate-control strategies and the reasoning behind them.
- Marker: this file is a **dirty-side behavioural specification** — it records behaviour and intent in our own words and contains no verbatim source text.

## 2. Purpose

The file is not executable code; it is an explanatory design document. It exists to give implementers a mental model for *why* the encoder chooses quantizers the way it does, rather than a precise algorithm description. Its stated purpose is to justify a set of mostly empirical heuristics with a small number of general principles, and to warn the reader that the note predates several important changes.

For a clean-room implementation the document is normative only at the level of **observable intent**: which modes exist, what each mode optimises, and which qualitative side effects each mode is expected to produce. Exact integers, tables and update rules must be taken from the encoder source specifications, not from this note.

## 3. Public interface

No declarations or callable entry points. The document exposes conceptual modes and parameters:

| Concept in the note | Meaning for an implementation |
| --- | --- |
| 2-pass | Two encodes: a first pass gathers per-frame size/complexity statistics, a second pass spends a fixed total bit budget using them. |
| 1-pass ABR | Single-pass average-bitrate target; statistics must be estimated on the fly. |
| 1-pass CBR / VBV | Single-pass constant bitrate with a leaky-bucket (VBV) constraint. |
| 1-pass CRF | Constant-rate-factor: a fixed quality target with no bit budget. |
| Constant QP | Quantizer derived solely from picture type. |
| `complexity ** 0.6` | Empirical exponent used to turn estimated frame complexity into relative bit allocation. |
| MB-tree | Per-macroblock propagation of frame importance; supersedes global qcomp weighting and constant B-offset in normal operation. |
| Adaptive quantization | Per-macroblock quality redistribution within a frame. |
| Per-row VBV | VBV accounting granularity improved from per-frame to per-row after the note was written. |

## 4. State and data structures

The note implies, without defining layouts, the following state that any implementation must maintain:

- **Per-frame statistics**: a measured or estimated bit cost at a fixed reference quantizer, plus the quantizer at which it was measured. In 2-pass these come from the first pass; in 1-pass from a cheap analysis of the current picture.
- **Global model state**: a mapping from complexity to a relative share of the total budget, plus a scale factor converting relative shares into quantizers.
- **Feedback state**: accumulated error between predicted and realised produced bits, split into a long-term/global component and a short-term/local component.
- **VBV state**: bucket occupancy, size and refill rate, plus a safety margin. The note states that modern implementations track this per picture *row* rather than per picture.
- **Frame-type importance**: an added or subtracted quality bonus for I, reference-B and non-reference-B pictures, derived from how many later pictures depend on them.
- **Per-macroblock state** for MB-tree and adaptive quantization: propagated cost values and per-block offsets.

Ownership and lifetime are entirely internal to the encoder; nothing is exposed to callers.

## 5. Algorithm / behaviour

### 5.1 Underlying principles (as stated by the note)

1. The encoder aims for approximately constant *perceived* quality, not constant PSNR and not constant QP. Complex or fast-moving material tolerates a higher quantizer for the same subjective quality, because detail is masked.
2. Spending bits where motion compensation works well is more efficient: a single corrected reference artifact can benefit several subsequent pictures in a low-motion sequence.
3. Both effects correlate with the bit cost of encoding a frame at a fixed quantizer, so that cost is used as the complexity proxy.
4. Given one measured cost at one quantizer, the cost at another quantizer can be estimated; accuracy degrades as the quantizer difference grows. Implementations should therefore re-anchor predictions frequently rather than extrapolate across a wide range.
5. Frame importance is proportional to how many other pictures predict from it. I pictures receive a quality bonus scaled by the count and complexity of following inter pictures; disposable B pictures are encoded at a worse quantizer than P pictures; B pictures used as references sit between the two.

### 5.2 Two-pass mode

Stage 1 — before the second pass, allocate *relative* shares of the budget using the first-pass frame complexities raised to an empirical power of 0.6. The note explicitly says this ignores the absolute size target; it only establishes proportions. The exponent is a compromise between the masking principle and the motion-compensation-efficiency principle.

Stage 2 — scale those relative shares so their sum matches the requested total size, optionally applying VBV limits. Because both the frame-size predictor and the VBV model are nonlinear, this scaling is iterative rather than a single division.

Stage 3 — during encoding, correct future quantizers from observed size errors:

- A *global* component: if the second pass is systematically larger or smaller than predicted (commonly because it uses slower, more efficient tools than the first pass), multiply all future quantizer scales by the reciprocal of the observed error ratio.
- A *short-term* component: keep the running total close to the ideal curve near the start (where no global statistics exist yet) and near the end (where the global correction has no time left to act).

### 5.3 One-pass average bitrate

Same three-stage shape as 2-pass, with substitutions:

- Complexity is not available from a prior encode. The encoder runs a cheap motion search on a half-resolution version of the current picture and uses the sum of absolute transformed differences (SATD) of the residuals as the complexity proxy. The same low-resolution analysis also feeds the P/B picture-type decision.
- Because future pictures are unknown, the I-picture bonus is estimated from past pictures rather than from a look-ahead of following frames.
- The scale factor is chosen as the value that would have produced the requested bitrate if applied to all pictures seen so far; this is an accumulating, retrospective estimate.
- Overflow compensation is the same mechanism as in 2-pass. Tuning its strength trades bitrate predictability against quality: the note quotes roughly ±10 % size variation at near-2-pass quality as one end of the spectrum, and tighter size control with lower quality at the other.

### 5.4 One-pass constant bitrate (VBV-compliant)

- The scale factor is derived from a *local* average whose window is tied to the VBV buffer size, rather than from all history.
- Overflow compensation is stricter and includes an additional term that hard-limits quantizers when the VBV buffer is nearly empty.
- There is deliberately no hard limit when the buffer is nearly full, so a CBR encode may undershoot the requested bitrate.
- A picture that violates the VBV constraint despite accurate prediction is **not** re-encoded; the encoder accepts the violation.

### 5.5 One-pass constant rate factor

Uses the ABR complexity estimate, a scale factor that is a constant derived from the CRF argument, and no overflow compensation at all. The bitrate is an output, not an input.

### 5.6 Constant quantizer

Quantizer is a function of picture type only, with no rate model and no feedback.

### 5.7 Post-document corrections declared by the note

The note labels itself outdated and lists three changes that must be treated as current behaviour:

- MB-tree replaces qcomp-based frame weighting and the fixed B-frame quantizer offsets; the legacy algorithm remains available for low-latency operation.
- Adaptive quantization redistributes quality inside each picture, so pictures are no longer constant-quantizer even with MB-tree disabled.
- VBV accounting runs per row rather than per picture.

## 6. Dependencies

- **Consumers**: the main rate-control implementation, the lookahead / low-resolution analysis stage, the MB-tree propagation code and the quantizer tables.
- **Producers**: first-pass statistics files (2-pass), low-resolution SATD costs (1-pass), VBV configuration, picture-type decisions and macroblock-level adaptive-quantization offsets.
- The note states that the encoder's rate control derives from libavcodec's and is mostly empirical.

## 7. Constants and tables

| Value | Meaning / rationale |
| --- | --- |
| 0.6 | Empirical exponent applied to complexity for relative bit allocation; the stated compromise between perceptual masking and coding efficiency. |
| 1/2 resolution | Downscale factor of the fast motion-estimation pass used when first-pass statistics are unavailable. |
| ±10 % | Illustrative size variation of the aggressive-feedback end of ABR, not a guaranteed bound. |
| 16..235 / 16..240 | Not from this file; appear only in the VUI document. |
| `QP_MAX_SPEC`-like bounds | Not specified here; see the rate-control implementation specifications. |

No numeric deadzone, offset table or VBV formula is given in this document; such values are UNKNOWN from this source alone.

## 8. Performance notes

The note is about *rate* control rather than encoder speed, but it has speed consequences:

- One-pass modes deliberately add a half-resolution motion-estimation pass per picture. This is a measurable extra cost and is one reason fast presets may choose different analysis settings.
- Two-pass avoids that per-picture cost in the second pass but doubles total encode time and requires an intermediate statistics store.
- MB-tree adds a propagation pass over macroblocks; the note identifies its low-latency unavailability as the reason the legacy weighting path is retained.
- Per-row VBV improves accuracy but increases the frequency of the accounting update; implementations should keep this update cheap.

### 8.1 Using checkasm-style verification to accelerate faster kernels

Rate-control changes are easy to verify behaviourally (compare bitstream size, PSNR/SSIM and VBV conformance), but SIMD kernel changes need a different harness. The assembly conformance tool described in `tools/checkasm.c.md` can accelerate development:

1. Establish a per-kernel reference result with the portable C implementation on randomised inputs.
2. Extract the hot kernels identified by profiling a rate-control-heavy workload — the half-resolution SATD estimate, cost aggregation and MB-tree propagation are all SIMD-coded in the reference design and are natural candidates.
3. Implement a candidate SIMD kernel and run it through the same ABI-wrapping, stack-clobbering and per-ISA comparison path. Immediate bit-exact (or explicitly tolerance-bounded) feedback removes the long encode-and-compare loop.
4. Use the built-in benchmark mode to compare candidate and reference cycle counts on the same inputs before committing to the change; the harness averages multiple invocations and subtracts a measured no-op overhead.
5. Extend the harness with a new test macro/case when adding a kernel with no existing coverage, keeping the same reference-vs-candidate structure so the test stays portable across ISAs.

References for this methodology: the x264 assembly check tool itself (`tools/checkasm.c`), FFmpeg's checkasm test suite (`tests/checkasm/` and the FFmpeg developer documentation on assembly testing), and the ABI references cited in `tools/checkasm-a.asm.md` and `tools/checkasm-arm.S.md`.

## 9. References

- The source note itself, `doc/ratecontrol.txt`, attributed in-file to Loren Merritt, including its own list of later corrections.
- ITU-T Rec. H.264 / ISO/IEC 14496-10 for the definition of quantizer, picture types, reference pictures and VBV / HRD conformance.
- FFmpeg project documentation on rate control for the shared lineage of libavcodec-derived heuristics.
- `doc/threads.txt.md` in this tree for the interaction of delayed rate-control feedback with frame-threaded encoding.
- `tools/checkasm.c.md` and `tools/digress/testing.py.md` for the two verification tools referenced above.

## 10. Verification checklist

- All five modes (2-pass, ABR, CBR/VBV, CRF, constant QP) are selectable and produce distinct quantizer sequences on the same clip.
- In 2-pass, doubling the requested total size lowers the average quantizer but preserves the *relative* allocation pattern derived from complexity^0.6.
- In ABR, changing only the strength of overflow compensation changes size variance more than mean quality.
- In CBR, the VBV bucket never goes negative in normal operation; deliberate overshoot on a single picture is accepted rather than triggering a re-encode.
- CBR may finish under the requested bitrate if the bucket stays near full.
- CRF produces the same quantizer sequence regardless of clip length or requested bitrate (no feedback).
- With MB-tree enabled, I-picture quality bonus grows with the number/complexity of dependent pictures; with the legacy path, B-picture offsets are constant as described.
- With adaptive quantization enabled, macroblocks within a picture receive different quantizer offsets even when MB-tree and overflow compensation are disabled.
- VBV accounting is observed to update at row granularity, not only at picture boundaries.

## 11. Open questions

- UNKNOWN: the exact quantizer-scale update coefficient and clamping rules for the global and short-term feedback components.
- UNKNOWN: the exact definition of "complexity" used by the first pass beyond "bit size at constant QP".
- UNKNOWN: the low-resolution SATD block sizes, search range and rounding used by the 1-pass estimator (see the lowres implementation specification).
- UNKNOWN: precise VBV bucket model parameters and the formula for the near-empty hard limit.
- UNKNOWN: how the note's ±10 % figure maps to current ABR behaviour after the MB-tree and per-row VBV changes.
