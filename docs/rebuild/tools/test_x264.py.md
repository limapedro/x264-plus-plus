# `tools/test_x264.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/test_x264.py`.
- Role: a Python 2 test driver built on the Digress framework that compiles the encoder, checks decoded-output equivalence against reference decoders, and tracks PSNR/SSIM regression values across a large set of randomly generated command lines.
- Marker: this file is a **dirty-side behavioural specification**; classes, options, patterns and control flow are described in our own words.

## 2. Purpose

Correctness of an encoder cannot be proved by a single encode: combinations of options interact with slice decisions, entropy coding and rate control, and a source change can alter bit-exact output or just shift quality metrics. This driver automates three kinds of checks: (1) the build succeeds from a clean configure, (2) the encoder's reconstructed YUV matches an independent decoder's output, and (3) PSNR/SSIM on a fixed clip match recorded baselines. It generates many pseudo-random option combinations from a curated list, so a single run explores a broad cross-product of tunings, presets and special modes without hand-writing each case.

## 3. Public interface

At import time the script changes the working directory to the parent of its own directory (the source tree root), defines its classes and options, constructs a fixture, and immediately calls `Dispatcher(fixture).dispatch()`. Importing the module therefore runs the test program.

Classes and callables:

- `compare_yuv_output(width, height)` — factory returning a comparator that takes two YUV filenames, raises `ComparisonError` on any difference, and otherwise returns normally. The comparator is used by the Digress comparison machinery.
- `program_exists(program)` — returns the first executable path found for `program`, or `None`. A name containing a directory component is tested directly; a bare name is searched along the entries of `PATH`.
- `x264(Fixture)` — the fixture class; its source-control manager is the Git backend supplied by Digress.
- `Compile(Case)` — build cases `test_configure` and `test_make`.
- `Regression(Case)` — quality cases `test_psnr` and `test_ssim`.
- `_YUVOutputComparisonFactory()` — returns a dynamically created case class containing the YUV comparison tests.
- `_generate_random_commandline()` — returns a list of encoder arguments chosen randomly from the option groups.
- `Dispatcher(_Dispatcher)` — front-end subclass adding x264-specific options and generated cases; `pre_dispatch()` seeds the RNG and registers the generated cases before the framework dispatches commands.

Command-line options added by this driver (all implemented as callbacks that set attributes on the dispatcher):

| Option | Effect |
| --- | --- |
| `-v`, `--video FILENAME` | Select the input clip; default `akiyo_qcif.y4m`. |
| `-s`, `--seed SEED` | Fix the random seed; default is the current Unix timestamp. |
| `-p`, `--product-tests NUM` | Number of random command lines to generate; default 50. |
| `--configure-with FLAGS` | Extra flags for `./configure`, split on shell rules. |
| `--yuv-tests LIST` | Comma-separated, whitespace-trimmed names of YUV comparison tests to keep. |
| `--x264-with FLAGS` | Extra flags appended to every encoder invocation. |

The framework's own options (for example case selection, flushing cached results and command discovery) remain available through the base dispatcher.

## 4. State and data structures

- `OPTIONS`: seven groups of alternative option spellings. Six groups contain either one option or an empty string (meaning "choose nothing"), and two groups enumerate values: the tune group (`film`, `zerolatency`) and the preset group (`ultrafast` through `placebo`, ten entries). The empty-string entries in the singleton groups make each of those features present in roughly half of generated command lines.
- `_generated`: module-level list of every command line generated in the current run, used to reject duplicates.
- Module-level fixture instance, with cases `Compile` and `Regression` registered immediately.
- Dispatcher defaults: `video`, `products`, `configure`, `x264`, `yuv_tests` (default `["jm"]`).
- Generated case classes: one per product test, each carrying its own `options` list and a class name embellished with the joined command line so reports identify the exact configuration.
- Compiled regular expressions: one for the encoder's printed dimensions and frame rate, one for the PSNR summary line, one for the SSIM summary line.
- Per-test temporaries: captured subprocess stdout, the list of output lines, parsed numeric metric values, and the frame dimensions returned by the encoder run.
- Cleanup state: lists of output filenames removed in `finally` blocks so a failed assertion or a decoder error still leaves the tree clean.

Ownership: output files (`<video>.264`, `x264-output.yuv`, `jm-output.yuv`, `ffmpeg-output.yuv`, `log.dec`, `dataDec.txt`) are transient artifacts created by the subprocesses and deleted by the test; no persistent state is kept between runs. Cache state belongs to the Digress fixture.

## 5. Algorithm / behaviour

### 5.1 Build cases

1. `test_configure` runs `make distclean` first, then `./configure` with the dispatcher's configure flags. Both processes capture stdout and merge stderr into it. A non-zero configure status raises `FailedTestError` with the output collapsed to a single line. The result of `distclean` is not checked.
2. `test_make` declares a dependency on `configure` and runs `make -j5`; a non-zero status raises `FailedTestError` with the collapsed output. The fixed parallelism of five jobs is hard-coded.

### 5.2 YUV comparison machinery

3. The comparator first compares the two files' sizes; unequal sizes are an immediate `ComparisonError`.
4. Both files are read in fixed-size chunks, iterated in lockstep until the shorter stream ends. Each pair of chunks is compared byte by byte; the first differing byte ends the scan.
5. From the byte offset of the difference, the comparator reconstructs the location: planar 4:2:0 geometry is assumed, with a luma plane of `width * height`, a chroma plane of one quarter that size, and the same frame stride for both chroma planes. The frame index is the offset divided by the total frame size; the remainder identifies the plane: luma first, chroma U next, chroma V last.
6. Within the identified plane, x and y are computed modulo the nominal plane width. Macroblock coordinates are derived by dividing by 16 for luma and by 8 for chroma and rounding **up**, then converting to integers. An error message names both files, the frame, the macroblock tuple, the plane and the exact byte offset.
7. `program_exists` decides whether a comparison test is available; missing `ldecod` or `ffmpeg` disables the corresponding test with `DisabledTestError` rather than failing it.
8. `_run_x264` encodes the selected clip with `--dump-yuv x264-output.yuv`, the generated option list, and an output file named after the clip with a `.264` suffix. A non-zero encoder status is a `FailedTestError`. The captured output is searched with a **start-anchored** match for a dimension/frame-rate line; the two captured digits become the dimensions passed to the comparator.
9. `test_jm` runs `ldecod` on the produced stream into `jm-output.yuv`, wraps a `ComparisonError` as a `FailedTestError`, and removes all five artifacts in a `finally` block.
10. `test_ffmpeg` does the same with `ffmpeg`, invoking it with a single argument containing both the `-vsync` flag and its value as one token, and removing its three artifacts.
11. The factory's `__init__` deletes from the generated case class every `test_*` method whose suffix is not in the dispatcher's `yuv_tests` list, so unselected decoders never run.

### 5.3 Regression cases

12. `Regression.__init__` appends the dispatcher's extra encoder flags to the case class name when any are present.
13. `test_psnr` encodes with `--psnr`, scans the captured output for lines beginning with the PSNR summary prefix, extracts the global numeric value (the one after `Global:`) and returns it as a float. The output stream is deleted afterwards; missing summary text raises `FailedTestError`.
14. `test_ssim` encodes with `--ssim` and similarly extracts the Y-component SSIM value, returning it as a float. Its "no output" error message reuses the PSNR wording. Both cases use the dispatcher's extra x264 flags verbatim.

### 5.4 Driver options and case generation

15. `_populate_parser` calls the base implementation, discovers the available YUV test names by introspecting a freshly generated case class, then adds the six-option group described in section 3. Each callback stores parsed values directly on the dispatcher; flag strings are split with shell-like tokenisation, and the YUV test list is split on commas and stripped per entry.
16. `pre_dispatch` chooses the seed (Unix timestamp if not supplied), prints it, and seeds the pseudo-random generator. It then loops `products` times: create a fresh case class, generate a command line by picking one entry per option group, split each entry into tokens and flatten them, dropping empty entries.
17. If the candidate duplicates one already generated, it is regenerated; after more than one hundred consecutive regeneration attempts the driver prints a diagnostic to standard error and exits with status 1. Retry state counts attempts for a single product, not across the whole run.
18. The dispatcher's extra x264 flags are appended to the accepted command line before it is stored, assigned to the case class as its `options`, and included in the class name. The case is then registered with the fixture.
19. Finally `dispatch()` from the base framework parses the final arguments and runs the selected command.

Ordering and edge cases:

- The random generator is seeded before any command line is produced, so a fixed `--seed` reproduces the whole generated suite, including the order of cases.
- The preset group has ten choices and the tune group three; with seven groups the space of distinct command lines is far larger than the default 50 products, so duplicate retries are normally rare.
- Empty entries from the singleton groups are filtered out after tokenisation; the resulting command line can be as short as one argument (a preset) or as long as eight.
- `--x264-with` flags are appended after the random ones and therefore take effect in the same position as any other encoder argument; flags that override earlier options win.
- A product class is created per iteration rather than reused, so class-level `options` and `__name__` do not leak between cases.
- Because all subprocesses are launched from the source tree root, `./x264` and `./configure` resolve to the freshly built binaries.

## 6. Dependencies

- **Python 2 environment**: the script uses Python 2 print statements, the two-argument `except X, e` form, `xrange`, `itertools.imap` and `itertools.izip`. It is not Python 3 compatible as written.
- **Digress framework**: `digress.cli.Dispatcher`, `digress.errors` (three exception types), `digress.testing` (decorators and base classes), `digress.comparers.compare_pass`, and `digress.scm.git`.
- **Standard library**: `optparse.OptionGroup`, `subprocess.Popen` with `PIPE`/`STDOUT`, `os`, `re`, `shlex`, `inspect`, `random.randrange`/`seed`, `math.ceil`, `itertools`, `operator`, `time.time`, `sys`.
- **External programs**: `make`, `./configure`, `./x264`, optionally `ldecod` (JM reference decoder) and `ffmpeg`. Missing decoders disable tests; a missing encoder or build tool fails the relevant case.
- **Input asset**: a YUV4MPEG2 clip, by default `akiyo_qcif.y4m`, expected in the source root.
- **Related specifications**: `tools/digress/cli.py.md`, `tools/digress/testing.py.md`, `tools/digress/comparers.py.md`, `tools/digress/errors.py.md` and `doc/regression_test.txt.md`.

## 7. Constants and tables

| Item | Value / meaning |
| --- | --- |
| Default clip | `akiyo_qcif.y4m`. |
| Default product count | 50 generated command lines. |
| Default YUV tests | `jm` only. |
| Default seed | `int(time())` at driver start. |
| Duplicate retry limit | 100 regeneration attempts per product. |
| Compare chunk size | 8196 bytes. |
| Build parallelism | `make -j5`. |
| Tune choices | `film`, `zerolatency`. |
| Preset choices | `ultrafast`, `superfast`, `veryfast`, `faster`, `fast`, `medium`, `slow`, `slower`, `veryslow`, `placebo`. |
| Singleton option group entries | intra-refresh on/off, CABAC on/off, interlaced on/off, slice-max-size 1000 on/off, frame-packing 5 on/off. |
| Plane geometry | luma `w*h`, each chroma `w*h/4`. |
| Macroblock divisor | 16 luma, 8 chroma. |
| Metric patterns | PSNR `Global:` value, SSIM mean Y value. |

The option list intentionally spans both speed extremes and structural modes: presets exercise different tool sets, while interlacing, slice splitting, CABAC toggling and frame packing exercise bitstream paths that a default encode never reaches.

## 8. Performance notes

- The dominant cost is subprocess execution: each product runs the encoder at least once for each enabled YUV decoder test plus twice for the regression metrics, so the default 50 products imply hundreds of encodes of the same clip. Reducing `--product-tests` or `--yuv-tests` is the most effective way to shorten a run.
- YUV comparison reads in 8196-byte chunks rather than loading whole frames, keeping memory bounded even for large clips; the byte-by-byte loop only runs after a chunk mismatch is found, so the common case is a fast bulk comparison.
- Generated command lines are de-duplicated with a linear scan of a list; with a few hundred products this is negligible, though a set would scale better.
- Compile and configure output is captured in memory and collapsed for error reporting; normal successful output is discarded, avoiding terminal flooding.
- **Optimization tip:** when iterating on a single kernel, select one YUV test and a small product count, and rely on the per-kernel conformance harness for fast feedback; reserve the full generated suite for pre-merge validation.
- **Correctness tip:** always record the printed seed in regression reports. Because the seed drives every generated command line, two runs with different seeds are not comparable.
- **Robustness tip:** the start-anchored dimension match and the single-token `-vsync 0` argument are fragile; when porting the driver, switch to a per-line search and separate argv tokens before trusting results.

### 8.1 Relation to faster-kernel development

A new SIMD kernel can be correct on conformance tests yet alter bit-exact reconstruction or quality metrics under a specific option combination. This driver covers that gap: run it with a fixed seed before and after the kernel change and compare the returned PSNR/SSIM values. Because the decoded-YUV comparator reports a frame and macroblock for the first differing byte, a mismatch can be localised to a small region of a specific frame, which narrows the search to the corresponding coding decision. Note that PSNR/SSIM regressions can be caused by rate-control redistribution, so confirm with a QP histogram (see `tools/countquant_x264.pl.md`) before attributing a change to a kernel bug.

## 9. References

- The source file itself, `tools/test_x264.py`.
- `tools/digress/cli.py.md` for the dispatcher, option callbacks and two-pass parsing; `tools/digress/testing.py.md` for fixtures, cases, decorators and caching; `tools/digress/comparers.py.md` for `compare_pass`; `tools/digress/errors.py.md` for the exception hierarchy.
- `doc/regression_test.txt.md` for the project's regression-testing philosophy and reference clips.
- JM reference decoder (`ldecod`) documentation and FFmpeg's `ffmpeg` command-line documentation for the two external decoders.
- Python 2 standard library documentation for `optparse`, `subprocess`, `shlex`, `inspect` and `itertools`.

## 10. Verification checklist

- Importing the module from a different working directory still places the process at the source-tree root before launching subprocesses.
- `make distclean` is run before `./configure`, and configure flags from `--configure-with` are passed as separate tokens.
- A non-zero configure or make status produces a `FailedTestError` whose message has newlines replaced by spaces.
- With `ldecod` absent from `PATH`, `test_jm` is disabled; with a stub executable present, it runs.
- Unequal YUV file sizes raise `ComparisonError` before any byte comparison.
- A deliberately altered byte in the first macroblock of frame 0 is reported with frame `0`, the luma plane and a macroblock tuple derived from the 16-pixel grid.
- A differing byte in the chroma region is reported on `U` or `V` with 8-pixel macroblock coordinates.
- `--dump-yuv` and the `<clip>.264` output are used consistently by all encoder invocations.
- All temporary files named in the cleanup blocks are removed even when a comparison fails.
- `test_psnr` returns a float taken from the `Global:` field; `test_ssim` returns a float from the mean-Y field.
- A fixed `--seed` reproduces the identical sequence of generated command lines across runs.
- Generated command lines contain at most one entry per option group and no empty tokens.
- Duplicate command lines are regenerated, and more than 100 failed regenerations terminate the driver with status 1 and a message on standard error.
- `--yuv-tests` with an empty or restricted list removes the corresponding test methods from the generated case classes.
- `--x264-with` flags appear at the end of every generated encoder command line and in the case name.

## 11. Open questions

- UNKNOWN: whether the start-anchored dimension match ever succeeds with real encoder output, which normally begins with banner lines; the code has no per-line search fallback.
- UNKNOWN: whether `ffmpeg` accepts `-vsync 0` as a single argv token in the FFmpeg versions this script was used with; the token is not split into flag and value.
- UNKNOWN: why the chroma macroblock X coordinate uses the luma width rather than half the width; the resulting coordinates are therefore only approximate for chroma.
- UNKNOWN: whether the `Global:` PSNR field and mean-Y SSIM field are sufficient to detect all regressions; only one number per metric is compared.
- UNKNOWN: any Python 3 migration plan; the use of Python 2-only syntax and iterators makes the script unusable on modern default installations without changes.
