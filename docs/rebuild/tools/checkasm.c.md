# `tools/checkasm.c` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/checkasm.c`.
- Role: the assembly conformance-test and microbenchmark tool that compares every SIMD-dispatched kernel against a portable C reference across all available CPU feature tiers, reporting failures and optional cycle counts.
- Marker: this file is a **dirty-side behavioural specification**. It describes the framework's behaviour, tolerances, ISA coverage and extension points in our own words, without reproducing source or near-verbatim pseudocode.

## 2. Purpose

A SIMD kernel can be fast and still wrong: it may clobber a callee-saved register, assume an argument is zero-extended when it is not, mis-handle an alignment edge, overflow an intermediate, or diverge only for a rare input pattern. Such bugs often crash only intermittently or produce a subtle quality loss. This tool exists to catch them deterministically, early, and on every supported architecture and feature tier.

It has two modes:

- **Correctness mode** (default): enumerate kernels, call both the C reference and the candidate implementation on adversarial and randomised inputs, compare outputs bit-for-bit or with an explicitly documented tolerance, and also verify ABI compliance through an assembly wrapper.
- **Benchmark mode** (`--bench`): measure cycle counts for each kernel variant on the same inputs, subtract a measured no-op overhead, and print a per-ISA table.

The tool is deliberately compiled with the GCC dialect only, as stated in `doc/standards.txt.md`.

## 3. Public interface

### Command-line interface

| Invocation | Behaviour |
| --- | --- |
| `checkasm` | Run all correctness tests using a random seed derived from the system clock. |
| `checkasm <seed>` | Run correctness tests with an explicit decimal seed (parsed as base 0, so hex is accepted). |
| `checkasm --bench` | Run correctness tests and then print benchmark results for all kernels. |
| `checkasm --bench=<pattern>` | Benchmark only kernels whose generated name begins with the given literal prefix. |
| `checkasm --bench <seed>` | Benchmark mode with an explicit seed. |

Benchmark mode is unavailable on architectures without a port of the cycle-counter reader; the tool prints a message asking for a port and exits with status 1 in that case.

### Exit status

- 0: all tests passed (and benchmarks, if requested, completed).
- −1: at least one test failed, or allocation failed.

### Observable output

- A seed line on standard error.
- One line per feature tier: `x264: <tier name>`.
- Quiet-by-default per-kernel lines when a kernel differs from the reference implementation: ` - <name padded> [OK|FAILED]`.
- Diagnostic lines on mismatch, usually including input indices, both results and a `FAILED` marker.
- Final line: "All tests passed Yeah :)" or a failure message.
- In benchmark mode, a measured no-op time and a sorted table of `name_<isa>: cycles`.

### Internal declarations consumed from assembly

- `x264_checkasm_call(func, ok, ...)`: ABI-checking trampoline (see the per-architecture documents).
- `x264_checkasm_stack_clobber(value, ...)`: fills stack memory below the stack pointer with junk.
- `x264_stack_pagealign(func, align)`: invokes a function with the stack aligned to a page boundary minus an offset.
- `x264_checkasm_warmup_avx`, `x264_checkasm_warmup_avx512`: vector-unit warmup routines.
- `x264_checkasm_sve_length()`: reports the SVE vector length in bits.

These are defined in `tools/checkasm-a.asm.md`, `tools/checkasm-aarch64.S.md`, `tools/checkasm-arm.S.md` and `tools/checkasm-loongarch.S.md`.

## 4. State and data structures

### Global buffers

| Buffer | Purpose |
| --- | --- |
| `buf1` | Random byte data; read-only input. |
| `buf2` | Alias into `buf1` at an offset; read-only input. |
| `buf3`, `buf4` | Output buffers for C and candidate implementations respectively. |
| `pbuf1`, `pbuf2` | Random pixel-valued inputs (typed alias of pixel data). |
| `pbuf3`, `pbuf4` | Typed aliases of `buf3`/`buf4` for pixel kernels. |

Allocation is one block for byte buffers and one for pixel buffers, with fixed internal offsets that create deliberately unaligned or differently aligned sub-buffers. On ARM, the alignment macro is overridden to force static 16-byte-aligned storage because GCC does not align stack variables there.

### Benchmark state

- `bench_t`: a per-kernel-variant record with the function pointer (used only to detect duplicate variants), a CPU flag word, accumulated cycle count and a sample count denominator.
- `bench_func_t`: a kernel name plus an array of up to `MAX_CPUS` variants.
- `benchs[]`: a table of up to `MAX_FUNCS` kernel entries; names are duplicated into heap storage.
- `do_bench`, `bench_pattern`, `bench_pattern_len`: benchmark mode selection.
- `func_name[100]`: the current kernel name, set by a formatting helper immediately before each test group.
- `quiet`, `ret`, `ok`, `used_asm`: per-group reporting state. The `report` helper prints a line only when a candidate implementation exists, sets the global failure flag when `ok` is false, and otherwise remains silent.

### Timing state

- A per-architecture cycle-counter reader using inline assembly: `lfence`+`rdtsc` on x86, `mftb` on PowerPC, a coprocessor register read on ARMv7, `pmccntr_el0` on AArch64, `rdhwr` on MIPS, and `rdtimel.w` on LoongArch. On other architectures it returns zero, which is why benchmark mode is rejected.

### Constants

| Constant | Value | Purpose |
| --- | --- | --- |
| `BENCH_RUNS` | 2000 | Iterations for a candidate variant; C reference uses one quarter. |
| `MAX_FUNCS` | 1000 | Capacity of the benchmark table. |
| `MAX_CPUS` | 30 | Maximum distinct CPU flag combinations per kernel. |
| `rand30()` | Combine two `rand()` results | Produce 30 random bits because `RAND_MAX` is only guaranteed ≥ 32767. |

## 5. Algorithm / behaviour

### 5.1 Invocation wrappers and ABI checking

Two families of wrappers exist:

- **C wrappers** (`call_c1`, `call_c`, `call_c2`): invoke the portable function directly, optionally after registering a benchmark sample.
- **Assembly wrappers** (`call_a1`, `call_a`, `call_a2`, `call_a64`): invoke through `x264_checkasm_call`, which preloads callee-saved registers with random sentinels, passes all arguments on the stack, calls the candidate, and verifies the sentinels afterwards. On mismatch the wrapper prints "failed to preserve register", writes zero into the `ok` flag, and restores the return registers.

The distinction between `call_*1` and `call_*2` matters: functions that modify their inputs cannot safely be benchmarked and correctness-checked in the same invocation, so some tests call the benchmark wrapper and the ABI wrapper separately.

Platform-specific wrapper behaviour:

- On x86-64 with MMX, before each candidate call the stack below the stack pointer is filled with a repeated 16-bit random pattern, dummy arguments are inserted so all real arguments land on the stack, and SIMD warmup is invoked. This detects code that wrongly assumes 32-bit integers are zero-extended to 64-bit in the upper half of a stack slot.
- The same stack-clobber trick is used on AArch64 (except Apple platforms) and on LoongArch with LSX.
- On ARMv6, two trampolines exist — one preserving NEON q4–q7 and one not — and the tool selects the NEON variant when NEON is detected. A 64-bit-return variant is used for functions returning a 64-bit value, because the generic trampoline's return type cannot express that.
- On other platforms the assembly wrapper falls back to a direct call.

### 5.2 Warmup

YMM and ZMM registers can be power-gated after periods of inactivity, producing inconsistent first measurements. The tool defines warmup routines that issue a trivial vector operation and selects the AVX-512 warmup when that tier is present, otherwise the AVX warmup. Warmup is called before each candidate invocation in the ABI wrapper and before each benchmark iteration.

### 5.3 Benchmark procedure

For each registered function and CPU tier:

1. Skip if the generated name does not match the benchmark pattern.
2. Perform one ABI-checked call to warm caches and verify correctness.
3. Repeat `BENCH_RUNS` times (or one quarter of that for the C variant), each time issuing warmup, reading the cycle counter, invoking the function four times, reading the counter again and accumulating the delta.
4. Adopt a sample only if it is not much slower than the running average — specifically, if the new time times the current sample count is at most four times the accumulated total — which rejects outliers such as interrupts and frequency changes.
5. Store the accumulated cycles and sample count. Duplicate pointers across tiers are suppressed in the printed table so a shared implementation is not listed twice.

`print_bench` first estimates a no-op overhead: it takes 10000 pairs of cycle reads, sorts the differences, averages the middle 9000, and prints that value. Then it sorts kernels with a comparison that is alphabetical except that digit runs are compared as numbers, and prints each variant's name with the ISA suffix derived from the highest applicable flag, plus optional microarchitecture suffixes such as cache-line size, slow shuffle, Atom and LZCNT. The printed figure is `(10 * cycles / samples - nop_time) / 4`, i.e. a per-call cycle estimate with the no-op and four-call batching removed.

### 5.4 Test orchestration

`check_all_funcs` calls, in order: pixel, DCT, motion compensation, intra prediction, deblocking, quantisation, CABAC and bitstream checks. Each returns a nonzero value if any of its groups failed; the sum is the process result.

`check_all_flags` drives the tier sweep:

1. Detect CPU capabilities once.
2. Select the SIMD warmup routine.
3. For each supported architecture, walk the feature flags from weakest to strongest, calling `add_flags` for each tier:
   - x86: MMX/MMX2, cache-line-64 variant, cache-line-32 on 32-bit, SSE, SSE2-slow, SSE2-fast, fast cache-line-64, slow-shuffle, LZCNT, SSE3, SSSE3 with cache/shuffle/Atom/LZCNT variants, SSE4, SSE4.2, AVX, XOP, FMA4, FMA3, BMI1, BMI2, AVX2, AVX512.
   - PowerPC: Altivec against C.
   - ARM: ARMv6, NEON, fast-NEON-MRC; select the NEON-preserving trampoline when NEON is present.
   - AArch64: ARMv8, NEON, SVE and SVE2 with the vector length reported in the tier name.
   - MIPS: MSA.
   - LoongArch: LSX and LASX.
4. `add_flags` sets the reference tier to the previous candidate tier, adds the new flags to the candidate tier, forces the stack-mod-4 flag when stack alignment is below 16, clears the slow-SSE2 flag when fast SSE2 is set, prints the tier name, and runs all functions with that reference/candidate pair.

This pairwise design is crucial: each tier is compared against the *previous* tier, not always against C, so a regression introduced anywhere in the chain is localised to exactly one feature step. Benchmark mode uses the same pairs, printing the C result under the bare name and each candidate under its ISA suffix.

### 5.5 Main routine

1. Disable the Windows error-report dialog.
2. Parse `--bench` and optional seed; reject benchmark mode on unsupported CPUs.
3. Seed the C random generator from the argument or the current time and report it.
4. Allocate and initialise the global buffers with random data, with a deliberately zeroed tail region.
5. Establish internal pointer offsets that create multiple alignment relationships between input and output buffers.
6. Invoke the tier sweep through the page-align helper, so the sweep itself runs with a controlled stack boundary.
7. Report overall success or failure, then print benchmarks if enabled.

### 5.6 Pixel and intra-cost kernels

- **SAD/SSD/SATD/SA8D**: for every function pointer that differs from the reference tier, run 64 cases with varying start offsets and one deliberately odd stride case, then an overflow-focused pass using pixel patterns built from extreme values (0 and pixel maximum) and a pseudo-random pattern of maximal differences. Results must match exactly.
- **SA8D+SATD combined**: verify that the two 32-bit halves of a 64-bit return equal the separately computed SA8D and SATD values, including overflow cases. This is why the tool has a 64-bit-call variant on ARM.
- **SAD x3 / x4**: compare against three or four independent C SAD calls with different reference offsets, including a deliberately misaligned output array; also verify the C multi-reference implementation itself through the benchmark path.
- **Variance and variance2**: compare 64-bit return values and, for `var2`, the two additional SSD outputs. The ABI wrapper cannot return a 64-bit value, so the correctness call and the ABi/benchmark calls are separated.
- **Hadamard AC**: compare both halves of the 64-bit result across alternating input patterns.
- **VSAD and ASD**: exercise several heights and a single ASD configuration.
- **Intra SATD/SAD x3**: build a reference prediction with a C predictor, apply the candidate fused mode-selection kernel, and compare both the returned three costs and the written prediction samples.
- **Intra SATD/SAD x9**: precompute costs for all nine intra modes plus a bit-cost penalty for modes other than a chosen one, then compare the candidate's selected mode, packed cost and full per-mode cost array. Predictions written by both implementations must match. This checks tie-breaking and output side effects, not just the scalar result.
- **NV12 SSD core**: sweep widths from 8 to 360 in steps of 8 and compare both U and V 64-bit sums.
- **SSIM**: compare the aggregate floating-point result with a tolerance of 1e-5, then check the per-block core and the final accumulator separately, including a call that deliberately passes a short block count to detect zero-extension assumptions. This is one of the few explicitly tolerant checks.
- **ESA ADS**: generate many cost arrays with mixed distributions (small multiples and wide random values), thresholds and motion-vector cost tables; compare the count and the entire returned motion-vector array.

### 5.7 Transform, zigzag and quantisation kernels

- **Forward DCT**: compare coefficient arrays for multiple input patterns, including constructed overflow cases where encoder and decoder residuals alternate between extremes; compare 4×4, 8×8 and 8×8-with-8×8-transform variants and DC-only variants.
- **Inverse DCT**: prepare coefficients by forward transforming, quantising and dequantising so they lie in the range the inverse transform expects, then compare reconstructed pixel blocks byte-for-byte.
- **DC transforms**: test maximum-DC, maximum-element and general random coefficient cases for luma and chroma DC variants, comparing in-place outputs.
- **Zigzag/scan**: use a ramp input so the permutation is directly observable; test both frame and field modes. Sub- and sub-AC variants also compare modified residuals, the returned nonzero count and a separate DC output. Interleaving tests use 100 random sparse coefficient arrays and compare both the coefficient ordering and ten trailing bytes.
- **Quantisation**: sweep all six custom-quantisation configurations (flat, JVT, two hand-built matrices, random scaling, all-ones), all QP values and multiple coefficient distributions. Compare output coefficients, returned nonzero counts and the dequantised results. Chroma-DC optimisers are tested with exponentially increasing magnitudes up to the pixel maximum times 16.
- **Denoise DCT**: compare coefficient values and the accumulated sum array.
- **Decimate score**: generate levels with a skewed distribution and sparse zeros; compare results after clamping to the threshold, because the contract only promises correctness up to that point.
- **Coefficient last / level-run**: random sparse blocks; compare the last index, the run/level structure, the mask and the level array.

### 5.8 Motion-compensation kernels

- **Luma MC / get_ref**: sample a subset (one in sixteen) of motion vectors over a wide fractional-pel range and several block sizes; compare full destination buffers. For 10-bit 12- and 20-wide cases, compare a reduced width because the kernel may legally write padding beyond it.
- **Chroma MC**: similar fractional-pel sweep over smaller blocks; ignore bytes to the right of width 2 because the kernel may write garbage there by design.
- **Weighted prediction / averaging**: sweep the entire legal weight range for average mode and a sampled subset of scale/offset/denominator combinations for weighted mode, alternating source alignment. Compare each row's requested width.
- **Interleave/deinterleave copy families**: a table of awkward width/height/stride combinations includes tiny sizes, non-multiples of vector width, negative strides and very wide rows; compare each produced plane. RGB deinterleave is tested for three- and four-plane inputs; v210 uses a computed source stride from the packed format.
- **Hpel filter**: compare three half-pel planes over ten rows, skipping the first two samples of each row because those are not used in production and need not match.
- **Low-resolution init**: sweep widths around a vector boundary and compare four downsampled planes.
- **Integral image initialisation**: compare horizontal and vertical passes with a fixed stride and check both the working region and, for larger sizes, a distant row to catch stray writes.
- **MB-tree kernels**: propagate-cost compares 16-bit outputs with a tolerance of either one unit or a relative error of 1e-4, explicitly because the floating-point implementation has different rounding; propagate-list compares reference costs with a tolerance of one unit; fix8 pack/unpack require exact output and a sentinel after the output array to detect overrun.
- **Aligned copy/zero**: sweep sizes and place guard bytes immediately before and after the destination, verifying they are untouched.

### 5.9 Deblocking kernels

Alpha, beta and tc parameter arrays are constructed to cover a decayed range rather than using exact table values, which is sufficient to exercise both strong and weak filtering paths. For each filter variant and parameter set, random pixels from two distributions (full range and nibble-limited) are used at several horizontal alignments; the entire output buffer must match. Deblock-strength generation uses random nonzero flags, reference indices and motion vectors, with a pre-filled output buffer to detect missed writes.

### 5.10 Intra prediction kernels and extremal plane

All direction variants of 4×4, 8×8 chroma, 8×16 chroma, 16×16 and 8×8 luma predictions are checked, sweeping alignment offsets when benchmarking. The planar (P) modes get an additional exhaustive-style test: 100 random trials over 128 combinations of top/left neighbour patterns, using extreme sample values so prediction arithmetic overflow paths are covered. The 8×8-filter helper is checked across availability masks for top, top-left and top-right neighbours, clearing an unused edge sample before comparison.

### 5.11 CABAC and bitstream kernels

- Three helpers each encode 4096 symbols (decisions, bypass or terminate) into a fixed-size buffer; C and candidate outputs must match exactly. Their assembly variants exist only on x86 with MMX and AArch64; elsewhere they alias the C versions.
- Residual coding tests iterate context block categories, interlaced flags and 256 random coefficient sets per category, comparing the estimated bit count, the 1024-byte context state array and — for non-RD variants — the emitted bitstream prefix and the remainder of the CABAC state up to the stream pointer. A slow-SSE2 flag forces extra testing even when the candidate pointer equals the reference. Resetting the encoder state before each call prevents buffer overruns during long benchmark runs.
- NAL escape testing covers tiny input sizes, 8 different zero-probability distributions, and a large random input; the emitted length and all bytes must match.

### 5.12 Tolerances summary

| Check | Tolerance |
| --- | --- |
| Pixel costs, transforms, quantisation, copies, intra, deblocking, NAL escape | Exact equality / `memcmp`. |
| SSIM aggregate | Absolute difference ≤ 1e-5. |
| MB-tree propagate cost | Absolute ≤ 1 unit **or** relative ≤ 1e-4. |
| MB-tree propagate list | Absolute ≤ 1 unit. |
| Decimate score | Equality after clamping to the documented threshold. |

Everything else is bit-exact. This is a key design decision: the tool does not hide small numerical differences except where a floating-point implementation legitimately rounds differently.

## 6. Dependencies

- **Calls into**: `x264_pixel_init`, `x264_dct_init`, `x264_mc_init`, `x264_deblock_init`, `x264_quant_init`, `x264_predict_*_init`, `x264_zigzag_init`, `x264_bitstream_init`, `x264_cpu_detect`, `x264_cqm_init/delete`, `x264_param_default`, `x264_emms`, `x264_malloc/free`, `x264_mdate`, CABAC context/encode helpers.
- **Depends on assembly helpers** documented in `tools/checkasm-a.asm.md`, `tools/checkasm-aarch64.S.md`, `tools/checkasm-arm.S.md`, `tools/checkasm-loongarch.S.md`.
- **Uses** `encoder/macroblock.h` for scan tables and constants, and `common/common.h` for architecture and alignment macros.
- **Referenced by**: all other specification documents that discuss kernel verification, including `doc/standards.txt.md`, `doc/ratecontrol.txt.md`, `doc/threads.txt.md`, `extras/avisynth_c.h.md`, `extras/cl.h.md`, `extras/inttypes.h.md`, `extras/intel_dispatcher.h.md` and the Digress framework documents.

## 7. Constants and tables

| Name | Purpose |
| --- | --- |
| `BENCH_RUNS = 2000` | Candidate benchmark iterations. |
| `MAX_FUNCS = 1000`, `MAX_CPUS = 30` | Table capacities. |
| `rand30()` | 30-bit random value from two calls to `rand()`. |
| Pixel size names | Twelve names covering 16×16 down to 2×2 blocks. |
| Intra prediction mode names | Per-block-size mode-name tables used in generated test names. |
| MB-tree fix8 counts | Five specific element counts chosen to exercise vector boundary cases. |
| Plane-copy spec table | Ten width/height/stride tuples including negative strides. |
| Quantisation CQM configurations | Six scaling-list configurations described in section 5.7. |
| Tolerance constants | 1e-5 for SSIM; 1 unit / 1e-4 relative for MB-tree. |

## 8. Performance notes

### 8.1 Hotness and coverage

The tool exercises every SIMD-dispatched function in the encoder, so its cost is dominated by the number of kernels rather than any single one. It is not part of production encoding. The kernels it covers are the hot paths of all presets: pixel costs and motion compensation dominate fast presets; transforms, quantisation, CABAC residual coding and deblocking dominate slower presets.

### 8.2 Benchmark methodology and pitfalls

- Cycle counts include four invocations per sample to amortise timer overhead, then divide by four after subtracting the no-op cost.
- Outlier rejection prevents interrupts and frequency transitions from skewing results, but the tool still assumes a stable clock; measurements across runs should be compared only on an idle machine.
- SIMD warmup is essential on x86 because power-gated YMM/ZMM units make the first measurements unreliable.
- The stack-clobber wrapper slows candidate calls significantly; benchmark samples call the function directly, not through the wrapper, so the reported numbers measure the kernel itself.
- Cache behaviour matters: the same fixed buffers are reused, so results reflect warm-cache performance. Kernels whose real workload streams large frames may scale differently.

### 8.3 Using checkasm-style verification to accelerate faster kernels — with references

This is the primary practical use of the tool for an independent implementation. A productive loop is:

1. **Establish a trusted reference.** Run the suite on the target machine before changing anything. Confirm that all tiers pass and record the benchmark table as a baseline.

2. **Pick a kernel from profiling, not intuition.** Use the benchmark table to find kernels with a high cycle count in the presets that matter. Combine with encoder-level profiling to confirm the kernel is actually hot in production.

3. **Write the candidate inside the existing dispatch structure.** Add the new implementation to the appropriate function table so `x264_*_init` selects it under a CPU flag. Do not change call sites in the test harness; the harness automatically picks up any function pointer that differs from the reference tier.

4. **Use the adversarial inputs already present.** The tool supplies extreme pixels, overflow-prone coefficients, odd strides, unaligned offsets, every fractional motion vector subset, awkward plane dimensions and sentinel bytes around destinations. These are exactly the cases a hand-written test usually forgets. A kernel that passes them is far more likely to be correct on real clips than one that only matches a few random arrays.

5. **Add a case for a new kernel.** When introducing a function not covered by an existing group, add a new test block inside the appropriate `check_*` function using the same pattern: choose a function pointer that differs from the reference tier, set `used_asm`, iterate randomised and extreme inputs, invoke the C and candidate versions through `call_c`/`call_a` (or the split variants for input-modifying functions), compare outputs, and finish with `report`. Add a matching name via the name-setting helper so benchmark filtering works. Register the group in `check_all_funcs`.

6. **Verify ABI compliance before tuning.** The trampoline checks callee-saved registers and the stack-clobber path checks 32-bit zero-extension assumptions. A kernel that is fast but fails these checks will be unstable in production.

7. **Benchmark each tier separately.** The flag sweep prints a row per ISA variant, so a regression in one tier is visible without rerunning with flags disabled. Compare the new number against the previous tier's row, not just against C, to see the real gain of the new instruction set.

8. **Use the regression harness as the final gate.** After the kernel passes conformance, run the end-to-end decoder comparison (`doc/regression_test.txt.md`) and the Digress fixture (`tools/digress/testing.py.md`) with `--flush` so cached results cannot mask a change. Bisect with `tools/digress/scm/git.py.md` if a regression appears.

9. **Keep determinism.** Always record the seed. A failure that depends on input generation is easier to reproduce when the exact seed is known.

**References** for this methodology:

- The x264 assembly check tool itself and its per-architecture trampolines (`tools/checkasm-a.asm.md`, `tools/checkasm-aarch64.S.md`, `tools/checkasm-arm.S.md`, `tools/checkasm-loongarch.S.md`).
- FFmpeg's checkasm test suite (`tests/checkasm/` in the FFmpeg source tree) and the FFmpeg developer documentation on assembly testing, which follows the same reference-vs-candidate pattern.
- System V AMD64 ABI, Microsoft x64 ABI and AAPCS (ARM Architecture Procedure Call Standard) for the callee-saved register sets the trampolines verify.
- Intel 64 and IA-32 Architectures Optimization Reference Manual and Arm Cortex series optimization guides for the power-gating and alignment behaviour that motivates warmup and alignment tests.
- `doc/standards.txt.md` for the two's-complement, alignment and `emms` assumptions a new kernel must respect.

## 9. References

- The source file itself, `tools/checkasm.c`, attributed to Loren Merritt, Laurent Aimar and Fiona Glaser.
- `doc/standards.txt.md` for the C dialect and platform assumptions.
- ITU-T Rec. H.264 / ISO/IEC 14496-10 for SAD/SATD/transform/deblocking/CABAC semantics that define correct outputs.
- Intel Intrinsics Guide and ARM NEON/SVE/LSX architecture references for the ISA tiers named in the flag sweep.
- `tools/digress/testing.py.md` and `doc/regression_test.txt.md` for the complementary end-to-end verification.
- FFmpeg developer documentation on assembly testing for the shared checkasm methodology.

## 10. Verification checklist

- Running with no arguments reports a seed and a success line when all tiers pass.
- An explicitly supplied seed produces the same pass/fail pattern on repeated runs.
- Every detected feature tier is printed and each is compared against its immediate predecessor.
- A deliberately broken kernel causes its group's line to read `FAILED` and the process to exit with failure.
- A kernel that clobbers a callee-saved register triggers the "failed to preserve register" message and zeroes the `ok` flag.
- A kernel that assumes zero-extension of a 32-bit argument is detected by the stack-clobber path on x86-64, AArch64 (non-Apple) and LoongArch with LSX.
- Kernels that modify their inputs are checked with separate benchmark and correctness invocations.
- Overflow-specific cases are exercised for pixel costs, DCT and quantisation.
- SSIM differences up to 1e-5 are accepted; larger differences fail.
- MB-tree cost differences of one unit or relative 1e-4 are accepted; larger differences fail.
- NAL escape results match in both length and bytes.
- CABAC residual tests compare bit count, context state and, where required, emitted bytes.
- Benchmark mode prints a no-op calibration and one row per distinct kernel/variant.
- Duplicate function pointers across tiers are not printed twice.
- `--bench=<prefix>` restricts output to matching kernel names.
- Benchmark mode on an unsupported architecture exits with the documented message.

## 11. Open questions

- UNKNOWN: exact compiler flags and include paths used to build this tool in the reference build system; it is compiled against the encoder objects but not shipped as part of the library.
- UNKNOWN: whether the `quiet` flag is ever set by a front end; in this source it is only read, never written.
- UNKNOWN: precise rationale for the 8196-style buffer sizes elsewhere in the tree versus this tool's fixed 0x1000/0x1e00 regions; values are observable but justification is not documented.
- UNKNOWN: behaviour when a kernel returns a 64-bit value on an architecture without the dedicated 64-bit call variant; see the ARM wrapper document.
- UNKNOWN: whether the flag sweep order exactly matches the production dispatcher priority on every architecture; it appears designed to, but the mapping lives in the dispatcher sources.
- UNKNOWN: tolerance for any floating-point kernel other than SSIM and MB-tree propagation; none is implemented here.
