# `tools/digress/errors.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/errors.py`.
- Role: the exception taxonomy used throughout the Digress framework to distinguish control-flow outcomes (skip, disable, failure) from genuine programming errors.
- Marker: this file is a **dirty-side behavioural specification**; all descriptions are our own words.

## 2. Purpose

The module gives each test-outcome condition a distinct exception class so the test runner can map exceptions to result statuses without inspecting message strings. It also provides an SCM-specific error whose message is normalised for printing on one line. Without this hierarchy, a test that wants to skip itself would have to raise a generic exception and the runner could not tell whether that meant "skip", "fail" or "crash".

## 3. Public interface

All classes derive from `DigressError`:

| Class | Purpose |
| --- | --- |
| `DigressError` | Base class for every framework-specific error. |
| `NoSuchTestError` | A dependency refers to a test that does not exist on the case. |
| `DisabledTestError` | A test is disabled; message is the reason. |
| `SkippedTestError` | A test is skipped; message is the reason. |
| `DisabledCaseError` | A case is disabled. |
| `SkippedCaseError` | A case is skipped. |
| `FailedTestError` | A test failed deliberately; message is the failure detail. |
| `ComparisonError` | A comparer found values that differ beyond its rule. |
| `IncomparableError` | Two values cannot be compared at all. |
| `AlreadyRunError` | A test or case has already been executed in this run; used internally to break dependency cycles. |
| `SCMError` | A source-control backend command failed; message is whitespace-normalised. |

`SCMError` has a constructor that replaces embedded newlines with spaces and a string conversion that returns the normalised message. Other classes carry no custom behaviour and use the built-in exception message handling.

## 4. State and data structures

- Each exception instance carries the standard `args` tuple from `Exception`. `SCMError` additionally stores a `message` attribute.
- `SCMError.__str__` returns that normalised message rather than the default tuple representation, so command-line output stays on one line even if the SCM tool emitted multi-line text.
- No module-level state.

## 5. Algorithm / behaviour

Exception flow through the runner:

1. A test method raises one of these exceptions, either directly (e.g. `FailedTestError("configure failed")`) or via a decorator that wraps the method and raises on entry.
2. `Case._run_test` catches them in order: `DisabledTestError` → status `TEST_DISABLED` with the message as value; `SkippedTestError` → `TEST_SKIPPED`; `FailedTestError` → `TEST_FAIL`; any other `Exception` → `TEST_FAIL` with "failed with exception" wording. Thus a plain Python error is treated as a test failure, not a framework crash.
3. `ComparisonError` is raised by comparers and caught by the compare/bisect commands, which report the difference rather than aborting.
4. `AlreadyRunError` is raised internally when a test or case appears twice in the dependency walk; callers catch it and continue, preventing duplicate execution.
5. `NoSuchTestError` is raised when a dependency name cannot be resolved to a method and is not caught locally, so it propagates as a framework error.
6. `SCMError` is raised by backend helpers on non-zero exit status and is caught by fixture commands, which print the normalised message and unwind via `finally` blocks that restore the original checkout/stash state.

Normalisation rule for `SCMError`: replace every newline character with a single space; carriage returns are not explicitly handled here. The string form prints the normalised message directly.

## 6. Dependencies

- **Imported by**: every other Digress module — `testing.py`, `cli.py` (indirectly), `comparers.py`, `scm/git.py`, `scm/dummy.py` — and front-end scripts.
- **Imports**: none beyond built-ins.
- **Related**: `tools/digress/constants.py.md` defines the statuses these exceptions map to.

## 7. Constants and tables

No numeric constants. The class hierarchy is the table:

```
DigressError
├── NoSuchTestError
├── DisabledTestError
├── SkippedTestError
├── DisabledCaseError
├── SkippedCaseError
├── FailedTestError
├── ComparisonError
├── IncomparableError
├── AlreadyRunError
└── SCMError (custom constructor + __str__)
```

## 8. Performance notes

Exceptions are only raised on exceptional paths, so their cost is irrelevant to steady-state test execution. Design note: using distinct classes instead of status-returning functions keeps the "happy path" of a test method simple — the method just computes and returns its value.

### 8.1 Relation to faster-kernel development

When iterating on SIMD kernels, it is important that an invalid test result not be silently treated as a pass. This taxonomy ensures that:

- A missing external decoder produces `DisabledTestError`, surfacing as `TEST_DISABLED` rather than a crash.
- A mismatch between C and assembly results in an assertion-style failure that becomes `TEST_FAIL` with the mismatch details in the value string.
- A bisection stops cleanly with a normalised message when a git command fails, leaving the working tree restored by the fixture's `finally` block.

References: `tools/digress/testing.py.md` for the mapping from exceptions to statuses; `tools/checkasm.c.md` for the analogous assembly-level failure reporting.

## 9. References

- The source file itself, `tools/digress/errors.py`.
- Python documentation on user-defined exceptions and exception hierarchies.
- `tools/digress/constants.py.md`, `tools/digress/testing.py.md`, `tools/digress/comparers.py.md`, `tools/digress/scm/git.py.md` for consumers.

## 10. Verification checklist

- Every listed class is a subclass of `DigressError`.
- Raising `SCMError("line one\nline two")` yields a string containing both lines separated by a space and no newline.
- Each exception class can be instantiated with an arbitrary message and stringified.
- A test raising `DisabledTestError` is recorded as disabled, not failed.
- A test raising a plain `Exception` is recorded as failed with an "exception" note.
- `AlreadyRunError` raised during dependency resolution does not abort the fixture; the duplicate test is skipped.

## 11. Open questions

- UNKNOWN: whether `DisabledCaseError` and `SkippedCaseError` are actually raised anywhere in the current framework; no use appears in the core module.
- UNKNOWN: whether `IncomparableError` is used by any current comparer; the comparison code appears to use `ComparisonError` only.
