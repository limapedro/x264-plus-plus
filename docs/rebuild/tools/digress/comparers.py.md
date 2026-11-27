# `tools/digress/comparers.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/comparers.py`.
- Role: a small library of value-comparison functions used by Digress to decide whether two revisions' test results differ, and to compare files byte-for-byte.
- Marker: this file is a **dirty-side behavioural specification**; behaviour is paraphrased in our own words.

## 2. Purpose

The framework separates *producing a value* (a test method) from *deciding whether two values are equivalent* (a comparer). That separation lets the same test be reused with strict equality, a numeric tolerance or a file comparison depending on the question being asked. Comparers are attached to test methods by a decorator and are invoked by the `compare` and `bisect` commands.

## 3. Public interface

| Function | Contract |
| --- | --- |
| `compare_direct(value_a, value_b)` | Strict equality. Raises `ComparisonError` with a message of the form "A is not B" when unequal. Returns nothing on success. |
| `compare_pass(value_a, value_b)` | Always succeeds. Used for tests whose mere completion is the pass criterion; the values are ignored. |
| `compare_tolerance(tolerance)` | Factory: returns a comparer closure that succeeds when the absolute difference of the two values is at most the supplied tolerance, and raises `ComparisonError` otherwise. |
| `compare_files(file_a, file_b)` | Byte-level file comparison. Checks equal size first, then reads both files in fixed-size chunks and reports the first differing byte offset. |

`compare_files` prints the two file names to standard output before comparing, which is a observable side effect useful for logging.

## 4. State and data structures

- `compare_tolerance` is a closure factory: the returned function captures the tolerance value. Each application therefore has its own independent comparer.
- `compare_files` uses a buffer constant of 8196 bytes (unusual but intentional) and an `offset` counter that advances by the length of each chunk read from the first file.
- Result values are normally strings or numbers produced by test methods. The runner converts them to strings for caching, so comparers may receive strings even when the original test returned a number; this is an important caveat for tolerance comparisons.

## 5. Algorithm / behaviour

### 5.1 Direct comparison

Evaluate `value_a != value_b`. If true, raise `ComparisonError` including both values in the message. No coercion is performed, so numeric and string representations that look the same but have different types may be considered unequal.

### 5.2 Pass-through comparison

Do nothing. This is used by tests such as configure/make and decoder comparisons where the test method raises on failure and returns no meaningful value; "the test passed" is the only condition needed.

### 5.3 Tolerance comparison

1. The factory receives a tolerance t.
2. It returns a function taking two values.
3. On invocation, compute the absolute difference `|a − b|`.
4. If it exceeds t, raise `ComparisonError` including both values and the tolerance in the message.
5. Otherwise return successfully.

Edge behaviour: because the runner stringifies values before caching, a numeric tolerance comparer applied to cached results may compare strings rather than numbers. UNKNOWN: whether the front end converts them back; this is a caveat rather than a documented guarantee.

### 5.4 File comparison

1. Obtain the size of each file. If sizes differ, raise `ComparisonError` reporting that the files are not the same size.
2. Print both file names.
3. Iterate over `size / BUFFER_SIZE + 1` chunks from each file in lockstep, reading at most 8196 bytes per chunk.
4. For each chunk pair, if the chunks differ, walk byte by byte to find the first differing index and raise `ComparisonError` reporting the absolute offset.
5. Advance the offset counter by the number of bytes read from the first file.
6. Complete successfully if all bytes match.

Notes: chunk sizes are derived from the first file's size; both files are known to be the same size at that point. The comparison is strict — no tolerance and no normalisation of line endings.

## 6. Dependencies

- **Imports**: `digress.errors.ComparisonError`, `os`, and Python 2 itertools helpers `imap` and `izip`.
- **Imported by**: `tools/digress/testing.py.md` (compare/bisect dispatchable commands), front-end scripts such as the x264 test driver, which defines its own YUV-aware comparer using the same pattern.
- **Attached to tests via**: the `comparer` decorator in `tools/digress/testing.py.md`.

## 7. Constants and tables

| Constant | Value | Meaning |
| --- | --- | --- |
| `BUFFER_SIZE` | 8196 | Chunk size for file comparison. |

No other constants.

## 8. Performance notes

- File comparison streams data in ~8 KiB chunks, so memory use is bounded regardless of file size; this matters for raw YUV comparisons that can be hundreds of megabytes.
- The byte-by-byte scan happens only for a mismatching chunk, so the common all-equal path is fast.
- Strict size checking avoids reading past the end of a shorter file.
- `compare_pass` is the cheapest comparer and should be used whenever the test itself already validates the outcome.

### 8.1 Relation to faster-kernel development

File comparison is the mechanism behind the end-to-end reconstruction test: the encoder dumps reconstructed YUV, an independent decoder produces its own YUV, and the two files must match exactly. For kernel work this is a useful *final gate* but a poor *inner loop*: a single differing byte is reported with its offset, and a front end can map that offset to a frame, macroblock and plane. Use it after the assembly conformance suite has already localised which kernel differs. See `doc/regression_test.txt.md` and `tools/checkasm.c.md`.

References: the x264 test driver that builds a YUV-aware comparer on the same pattern; `tools/digress/testing.py.md` for compare/bisect commands.

## 9. References

- The source file itself, `tools/digress/comparers.py`.
- Python documentation for `itertools.imap`/`izip` (Python 2) and file I/O.
- `tools/digress/errors.py.md` for `ComparisonError`.
- `tools/digress/testing.py.md` for the decorators and commands that invoke comparers.

## 10. Verification checklist

- `compare_direct` passes for equal values and raises for unequal values.
- `compare_pass` never raises regardless of its arguments.
- `compare_tolerance(t)` passes when the difference equals t exactly and raises when it exceeds t.
- `compare_files` passes for identical files, reports a size mismatch for different lengths, and reports the exact first differing offset for same-length files.
- `compare_files` prints both file names before comparing.
- A comparer attached to a test is invoked by both `compare` and `bisect`.
- A result whose status is not pass is not handed to a comparer.

## 11. Open questions

- UNKNOWN: whether the unusual 8196-byte buffer is deliberate or historical; no rationale is given.
- UNKNOWN: how stringified cached values interact with a numeric tolerance comparer in practice; the source does not convert them back.
- UNKNOWN: whether `IncomparableError` is ever used by an extended comparer in a front end.
