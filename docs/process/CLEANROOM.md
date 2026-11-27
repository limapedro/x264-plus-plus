# x264++ clean-room process

This project is a **clean-room reimplementation**, not a fork of x264. No x264 source code is in this repository, and implementation work is performed without access to it.

The process combines two ideas:

1. **Clean-room design / Chinese wall** (the IBM PC BIOS precedent): a *specification* team observes the reference product's externally visible behaviour and writes specifications; a separate *implementation* team, which has never seen the reference source, writes original code from those specifications.
2. **IBM Cleanroom Software Engineering** (Mills, Linger, Dyer; Hausler/Linger/Trammell, *IBM Systems Journal* 33(1), 1994; SEI reference model CMU/SEI-96-TR-022): box-structure specification and design, stepwise refinement with correctness verification instead of unit debugging, incremental development under statistical quality control, and independent statistical certification testing.

## Roles and the wall

| Role | May read | May write | Must not |
| --- | --- | --- | --- |
| **Specification (dirty)** | `oracle/` (reference CLI binary), published H.264/AVC standard and public literature, black-box probe results | `spec/` only | Write code in `src/`; copy reference source text into `spec/` |
| **Implementation (clean)** | `spec/`, `src/`, public standards | `src/` only | Read `oracle/`, `cert/`, the stashed reference source, or any dirty research notes |
| **Certification (independent)** | `spec/`, `cert/`, built candidate binaries, `oracle/` for comparative testing | `cert/`, `score.md` | Modify `src/` or `spec/` |

The reference source archive and the earlier source-level research are kept **outside the repository** in `E:\_x264_dirty_stash\` and are off-limits to the implementation team. The only reference artifact inside the repository is `oracle/x264.exe`, a compiled binary used strictly as a black-box behavioural oracle and speed baseline.

**Rule:** implementation agents operate only inside `src/` and `spec/`. If a spec is incomplete, the implementation agent records a question in `src/verification/questions.md`; the specification team answers it in `spec/`. This is the Chinese wall.

## Box structures

Every level of the design is expressed in three forms, refined and verified in order:

- **Black box** — required external behaviour: response as a function of the *entire stimulus history*. No internal structure. Example: `CLI((args, frame_0..frame_n)) → (annex_b_bitstream, log, exit_code)`.
- **State box** — same external behaviour, with explicit encapsulated state: `(stimulus, old_state) → (response, new_state)`. For the encoder: frame references, rate-control state, DPB, CABAC state.
- **Clear box** — procedural decomposition into lower-level black boxes, with an intended-function argument for each step. The lower-level black boxes are then refined in their own turn.

Verification is a team review argument that each refinement preserves the behaviour of the box above it. Arguments are recorded in `src/verification/` as they are made. No implementation source was derived from the reference source; correctness claims are argued from the specifications.

## Increments

Development proceeds as a pipeline of accumulating increments. Each increment is specification → state box → clear box → review → certification before the next begins.

| Increment | Function | Exit criterion |
| --- | --- | --- |
| I0 | Build system, CLI parsing, raw I420 input/output, logging | Spec-equivalent CLI behaviour on the usage profile |
| I1 | Baseline H.264: SPS/PPS, I-slices, CAVLC, deblocking | Output decodable by an independent decoder |
| I2 | Inter prediction: P/B slices, motion estimation, reference management | Decodable, rate/distortion within documented tolerance |
| I3 | CABAC, transform/quantisation refinements | Decodable, quality target documented |
| I4 | Rate control, presets/quality levels, lookahead | Full CLI compatibility profile passes |
| I5 | Performance: SIMD (x86 AVX2), threading, memory layout | Speed measured and published in `score.md` |

## Statistical certification

Testing is not debugging. The certification team:

1. Builds a **usage model** (a Markov model over resolutions, frame counts, presets, rates, and input content) from `spec/usage-model.md`.
2. Draws random test cases from that model, executes the candidate, and classifies each response against the black-box specification.
3. Records failures without modifying implementation code. Failures go back to the implementation team as specification/verification defects.
4. Reports reliability estimates (MTTF with confidence bounds) and speed ratios in `score.md`.

Developers may perform **compile checks only**; all functional testing and all published measurements belong to certification. This is the practical adaptation of the IBM rule against developer unit testing to an agentic workflow.

## Note on the reset

An earlier iteration of this repository imported and built x264 as an optimization baseline. That was a misreading of the goal. All such source was removed from the working tree and the repository history was reset to an orphan commit; the reference binary remains only as the black-box oracle described above.
