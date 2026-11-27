# `doc/regression_test.txt` — dirty-side behavioural specification

## 1. Header

- Source path: `doc/regression_test.txt`.
- Role: a short recipe for one end-to-end correctness test that compares the encoder's own reconstructed pictures with those of an independent reference decoder.
- Marker: this file is a **dirty-side behavioural specification** of that workflow, written in our own words.

## 2. Purpose

The note documents a *single* test method, not a general test suite. Its purpose is to establish that the encoder's internal view of decoded pictures equals an external decoder's view of the same bitstream. If those views match byte-for-byte, any remaining distortion is attributable to lossy compression itself and not to a mismatch between encoder-side reconstruction and the normative decoding process.

For a clean-room project this is a high-value acceptance test: it detects reconstruction-path bugs (deblocking, interpolation, transform/inverse-transform, in-loop filtering, reference management) that unit-level kernel tests may miss because they only compare isolated functions.

## 3. Public interface

No code interface. The workflow's observable contract is:

- Inputs: a raw YUV source file and an H.264 bitstream produced by the encoder from that source.
- Outputs: two raw YUV files — one dumped by the encoder during encoding, one produced by the reference decoder from the bitstream.
- Pass condition: the two raw files are byte-identical.

The note names two external programs used in the recipe: the encoder under test and the JM reference decoder (`ldecod`). No third-party converter is involved; both outputs are raw planar YUV.

## 4. State and data structures

- Encoder-side dumped reconstruction file: raw samples in the layout the encoder uses internally.
- Decoder-side file: raw samples produced by the reference decoder from the bitstream.
- Bitstream file: the H.264 elementary stream.
- Build artifacts: an independent encoder checkout/build and an independent JM checkout/build with its decoder executable.

No intermediate metadata, hashes or manifests are defined; comparison is a plain byte-level diff.

## 5. Algorithm / behaviour

The documented workflow, expressed as behaviour rather than shell syntax:

1. **Obtain and build the encoder under test.** Fetch a source checkout into a sibling directory, run its configuration step with default settings, then build it.
2. **Obtain and build the reference decoder.** Download a numbered JM reference-software archive, unpack it, run the Unix preparation script in the JM directory, then build the decoder in its `ldecod` sub-directory.
3. **Encode with reconstruction dump.** Invoke the encoder on the chosen YUV source, requesting that it dump the pictures it itself reconstructs to a named raw file while writing the H.264 bitstream to another named file.
4. **Decode independently.** Invoke the JM decoder on that bitstream, asking it to write a raw YUV output file.
5. **Compare.** Diff the decoder's raw file against the encoder's dumped raw file. Any difference is a failure; identical files mean the test passes.

Important behavioural consequences:

- The comparison is exact, not tolerance-based. Floating-point differences are not expected in this path because both sides reconstruct integer sample values.
- The test covers the *entire* reconstruction chain, including reference picture handling and loop filters, so it is complementary to per-kernel assembly tests.
- The recipe assumes the encoder's dump format and the decoder's output format describe samples in the same order and with the same plane layout; an implementation must verify that convention before relying on the diff.
- The note does not specify a particular source clip, resolution, bit depth, preset or option set. UNKNOWN from this source.

## 6. Dependencies

- An encoder build that supports a reconstruction-dump option.
- The JM reference software package and its Unix preparation script.
- A POSIX-like shell with `wget`, `unzip`, `diff` and a make toolchain.
- Sufficient disk space for two full raw YUV copies plus the bitstream.

The automated regression framework in `tools/digress/testing.py.md` and its x264-specific front end implement a closely related test (`test_jm`): they encode with a dump enabled, decode with `ldecod`, and compare the two YUV outputs with a size check followed by a byte-offset report. They additionally support ffmpeg as a second independent decoder and generate many random option combinations.

## 7. Constants and tables

| Item | Value in the note | Comment |
| --- | --- | --- |
| Encoder checkout directory | `x264` | Sibling directory relative to the working directory. |
| Reference software | JM 17.2 archive | Named explicitly; newer/older JM versions are UNKNOWN. |
| Reference decoder executable | `ldecod` | Path given as `JM/bin/ldecod.exe`. |
| Dumped reconstruction file | `fdec.yuv` | Encoder output in the recipe. |
| Reference reconstruction file | `ref.yuv` | Decoder output in the recipe. |
| Bitstream | `output.h264` | Annex B elementary stream. |

No tolerance constants: the comparison is a literal diff.

## 8. Performance notes

- Building two external projects dominates the wall-clock cost of the recipe; the encode/decode/diff steps are comparatively cheap on a small clip.
- The raw files are uncompressed and can be large; using a short, low-resolution clip keeps the test practical.
- Because the test is exact, a single mismatching sample aborts usefulness of the run; bisection over options or commits is more efficient than re-running the full matrix.

### 8.1 Relation to faster-kernel development

This end-to-end test is too coarse for iterating on a single SIMD kernel: it cannot localise which function is wrong. It is best used as a gate *after* the checkasm-style framework (`tools/checkasm.c.md`) has validated individual kernels, and as a final equivalence check before accepting a performance change. The automated front end described in `tools/digress/cli.py.md` can bisect a regression to a commit, which is the recommended way to find the kernel responsible.

## 9. References

- The source note itself, `doc/regression_test.txt`.
- ITU-T Rec. H.264 / ISO/IEC 14496-10: definition of the normative decoding process that the reference decoder implements.
- JM reference software documentation accompanying the archive named in the note.
- `tools/digress/testing.py.md` and `tools/digress/comparers.py.md` for the automated version of this comparison.
- `tools/checkasm.c.md` for the complementary per-kernel verification approach.

## 10. Verification checklist

- Encoder and reference decoder are built from independent source trees.
- The encoder writes both a bitstream and a reconstruction dump in a single invocation.
- The decoder consumes exactly that bitstream and writes raw samples.
- The two raw files have identical size.
- `diff` reports no differences; a "regression test passed" outcome is recorded.
- Repeating with a different source clip and at least one non-default option (e.g. interlaced or intra-refresh) also passes.
- Introducing a deliberate change to a reconstruction kernel causes the test to fail, confirming the test has sensitivity.

## 11. Open questions

- UNKNOWN: which encoder version/options the note's authors considered canonical.
- UNKNOWN: whether the dumped file includes all pictures or only a subset, and how leading/trailing pictures are handled.
- UNKNOWN: the exact plane order and padding conventions of `fdec.yuv` versus `ref.yuv`.
- UNKNOWN: whether the JM version is still available at the URL given in the note; the recipe is historical.
