# `doc/threads.txt` — dirty-side behavioural specification

## 1. Header

- Source path: `doc/threads.txt`.
- Role: a historical design note describing the two threading models (slice-based and frame-based), their serialization points, quality penalties and published scaling benchmarks.
- Marker: this file is a **dirty-side behavioural specification**; all description is our own paraphrase of the documented observations.

## 2. Purpose

The note explains why the encoder moved from slice threads to frame threads, why slice threading was later reintroduced as an option for low latency, and what each model costs in bitrate and quality. It also reports measured scaling on an eight-core machine so implementers can calibrate expectations. For a clean-room implementation it is primarily a behavioural reference for *what threading must preserve* (reference availability, frame-type decisions, rate-control feedback) rather than a scheduling specification.

## 3. Public interface

No declarations. The note exposes two conceptual modes and their observable properties:

| Mode | Summary |
| --- | --- |
| Slice-based (legacy, optional) | One frame is split into slices; a thread encodes each slice; deblocking and hpel filtering are serial after the slices join. |
| Frame-based (current) | Multiple frames are in flight; a separate lookahead thread performs B-adaptation and rate control; each frame thread encodes, deblocks and filters its own frame. |

Historical revision markers in the note: slice threads were replaced in r607; frame threads were significantly changed by threaded lookahead in r1246; slice threads were restored as an option in r1364 for low-latency encoding.

## 4. State and data structures

The note implies the following state without giving layouts:

- **Per-thread work unit**: in slice mode, a slice of one picture; in frame mode, a whole picture.
- **Lookahead buffer**: a queue of size `sync-lookahead` separating the lookahead thread (B-adaptation, rate control) from the encoding threads.
- **Reference availability tracker**: in frame mode, a record of which parts of reference pictures have already been encoded, so a macroblock only uses motion vectors from available reference regions.
- **Frame-type commitment**: the chosen picture type is fixed before a frame thread begins; the note states scenecut detection therefore runs during low-resolution pre-motion-estimation rather than by full re-encoding.
- **Rate-control latency**: planned quantizer for picture N must be decided before picture N−1 completes.
- **Input decode thread**: present in the CLI for slice mode; in frame mode it is only added when input decoding is slower than slice+deblock+hpel work.

## 5. Algorithm / behaviour

### 5.1 Slice-based model — execution order

1. Application calls the encoder.
2. B-adaptation and rate control run serially.
3. The picture is split into slices and one thread is spawned per slice.
4. The caller waits for all slice threads to finish.
5. Deblocking and half-pel filtering run serially.
6. Control returns to the application. In the CLI an additional thread decodes input concurrently.

### 5.2 Frame-based model — execution order

1. Application calls the encoder.
2. The encoder requests a picture from the lookahead stage, which runs B-adaptation and rate control in parallel with the current thread, separated by a buffer of size `sync-lookahead`.
3. A thread is spawned for that picture.
4. The thread performs encode, deblock and half-pel filter.
5. Meanwhile the caller waits for the *oldest* outstanding thread to finish.
6. Control returns to the application while the remaining threads continue in the background.
7. No extra input-decode thread is needed unless decoding is slower than the combined per-frame encode+filter work; in that case one can be added.

### 5.3 Penalties of slice threading

- Each slice costs bits: a slice header is emitted, CABAC contexts are reset, and motion vectors and intra samples cannot be predicted across slice boundaries.
- In CBR mode multiple slices encode simultaneously, increasing the maximum possible VBV misprediction because several encoders draw from the same bucket without seeing each other's results.
- Parts of the encoder remain serial, so scaling is limited with many CPUs.

Measured proportional bitrate costs at 720p with 45 slices (one per macroblock row), averaged over four movies at CRF 20 and CRF 30; total cost was about +30 % bitrate at constant PSNR:

| Component | Share of total slice penalty |
| --- | --- |
| Intra prediction | 34 % |
| Redundant slice/NAL headers and whole-byte rounding | 25 % |
| Motion-vector prediction | 16 % |
| CABAC context resets | 16 % |
| Deblocking between slices | 6 % |
| CABAC neighbour contexts (cbp, skip, etc.) | 2 % |

The note adds two qualifications: header overhead should scale with bitrate because its size is roughly fixed while everything else grows; deblocking cost should also vary with deblocking strength. It also says the proportions should not depend strongly on slice count as long as there is at most one slice per row, because the relative frequency of per-slice and per-edge-macroblock effects is then set by picture width.

### 5.4 Penalties of frame threading

- To allow parallel pictures, a macroblock may only use motion vectors from portions of reference pictures that have already been encoded. This is normally imperceptible but can matter for very fast upward motion.
- Picture type must be committed before work begins, so scenecut detection is merged into the low-resolution pre-motion-estimation with B-adaptation — faster, but less accurate than re-encoding the whole picture.
- Rate control receives delayed feedback because it plans picture N before picture N−1 has finished.

### 5.5 Benchmarks (as reported)

Test platform: eight-core Nehalem (two E5520 at 2.27 GHz), hyperthreading disabled, Linux 2.6.34.7 64-bit, x264 r1732 b20059aa, 1080p park_joy source. Important caveat from the note: the "thread count" excludes the lookahead thread, which is why veryfast speedup at two and three threads can exceed the nominal core-limited expectation.

Reported speedups and PSNR deltas:

| Preset | Threads | Slice speedup | Frame speedup | Slice ΔPSNR | Frame ΔPSNR |
| --- | --- | --- | --- | --- | --- |
| veryfast, CRF 30 | 1 | 1.00× | 1.00× | +0.000 | +0.000 |
| | 2 | 1.41× | 2.29× | −0.005 | −0.002 |
| | 3 | 1.70× | 3.65× | −0.035 | +0.000 |
| | 4 | 1.96× | 3.97× | −0.029 | −0.001 |
| | 5 | 2.10× | 3.98× | −0.047 | −0.002 |
| | 8 | 2.43× | 3.98× | −0.067 | −0.001 |
| | 12 | — | 4.00× | — | +0.001 |
| medium, CRF 30 | 1 | 1.00× | 1.00× | +0.000 | +0.000 |
| | 2 | 1.54× | 1.59× | −0.002 | −0.003 |
| | 4 | 2.51× | 3.11× | −0.009 | +0.000 |
| | 8 | 3.79× | 5.76× | −0.015 | −0.002 |
| | 12 | — | 6.96× | — | +0.000 |
| slower, CRF 30 | 1 | 1.00× | 1.00× | +0.000 | +0.000 |
| | 2 | 1.54× | 1.83× | +0.000 | +0.002 |
| | 4 | 2.50× | 2.61× | −0.011 | +0.002 |
| | 8 | 4.13× | 5.04× | −0.026 | −0.001 |
| | 12 | — | 6.89× | — | −0.001 |

The table is an abridgement of the note's full rows; the omitted rows follow the same trend. Frame threading scales substantially better and has a much smaller PSNR penalty.

## 6. Dependencies

- Lookahead / B-adaptation stage and its `sync-lookahead` buffer.
- Rate control, which must be able to plan future frames before current frames finish.
- Deblocking and half-pel filters, which are serial in slice mode but per-frame in frame mode.
- CLI input decoder, which may need its own thread in frame mode under the condition stated above.
- Slice headers, CABAC context state and neighbouring macroblock prediction data, whose boundaries define the slice-mode penalties.

## 7. Constants and tables

| Constant / value | Meaning |
| --- | --- |
| r607 | Revision where frame threads replaced slice threads. |
| r1246 | Revision adding threaded lookahead. |
| r1364 | Revision restoring optional slice threads for low latency. |
| `sync-lookahead` | Buffer size separating lookahead from encoding threads. |
| 45 slices | Test condition: one slice per macroblock row at 720p. |
| ~+30 % | Total bitrate cost at constant PSNR in that test. |
| 2.27 GHz / 8 cores | Benchmark machine. |
| CRF 20 / CRF 30 | Settings used for the slice-penalty measurements. |

## 8. Performance notes

The note is itself a performance document. Key takeaways for an implementation:

- Prefer frame threading as the default; it scales to roughly four to seven times on eight cores depending on preset, while slice threading plateaus around two to four times.
- Keep the lookahead thread out of the reported thread count when comparing against published figures; otherwise speedups appear inconsistent.
- When low latency forbids frame threading, expect the largest slice penalties from intra prediction and header overhead; reducing slice count below one per row is possible but changes the cost mix.
- Frame threading's reference-availability restriction can hurt fast upward motion; an implementation may need a configurable lookahead depth to trade latency against this artefact.
- Rate-control feedback delay is intrinsic; short-term compensation in ABR/CBR becomes more important with deeper thread queues.

### 8.1 Using checkasm-style verification for faster threaded kernels

Threading changes often expose latent kernel bugs (race on scratch buffers, unaligned stack in worker threads, wrong callee-saved register use in a worker path). The checkasm suite can help isolate these:

- Run the per-ISA conformance suite on each thread entry path using the stack-page-align helper in `tools/checkasm.c.md`, which specifically tests behaviour under a deliberately misaligned/aligned stack boundary.
- Benchmark candidate kernels with `checkasm --bench` before integrating them into a threaded path; unreliable microbenchmarks become much worse when many threads contend for cache.
- Keep the regression harness (`tools/digress/testing.py.md`) as the end-to-end gate for thread-count-dependent output, since frame threading changes reference availability and therefore bitstream output.

References: the source note; the checkasm tool and its assembly wrappers in this tree; the regression recipe in `doc/regression_test.txt.md`.

## 9. References

- The source note itself, `doc/threads.txt`.
- `doc/ratecontrol.txt.md` for the feedback-delay interaction mentioned in section 5.4.
- `doc/regression_test.txt.md` for end-to-end verification of threaded output.
- `tools/checkasm.c.md` for the per-kernel verification and benchmark methodology.
- General literature on frame-parallel video coding and lookahead scheduling (UNKNOWN: specific paper referenced by the note; none is cited).

## 10. Verification checklist

- Slice mode performs B-adaptation and rate control serially before spawning slice threads.
- Slice mode waits for all slices before deblocking and hpel filtering.
- Frame mode has a lookahead stage with a `sync-lookahead` buffer feeding per-picture threads.
- Frame mode waits for the oldest outstanding thread and returns while others continue.
- An input-decode thread is added only when decoding is slower than the per-frame work.
- Frame mode restricts motion-vector references to already-encoded reference regions.
- Picture type is decided during low-resolution analysis, not by full re-encode.
- Relative scaling trends match the published table direction: frame threading beats slice threading at equal thread count across all three presets.
- Slice-mode overhead sources are all present: slice headers, CABAC resets, lost cross-slice intra and MV prediction, optional cross-slice deblocking suppression and CABAC neighbour resets.

## 11. Open questions

- UNKNOWN: exact implementation of the `sync-lookahead` buffer and its interaction with thread count.
- UNKNOWN: how the 45-slice experiment maps to modern slice-count defaults.
- UNKNOWN: whether the reported benchmark machine and revision are still representative.
- UNKNOWN: precise conditions under which the CLI decides to create an input-decode thread in frame mode.
- UNKNOWN: any newer threading changes after the revisions named in the note.
