# `tools/digress/constants.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/constants.py`.
- Role: the single source of truth for the small integer status codes used by tests, cases and whole fixtures in the Digress framework.
- Marker: this file is a **dirty-side behavioural specification**, paraphrased in our own words.

## 2. Purpose

The module exists to avoid scattered magic numbers for pass/fail/disabled/skipped outcomes. Every layer of the framework reports results in dictionaries whose `status` field is one of the values defined here; comparators, aggregate calculations and bisection decisions all interpret those values. Centralising them makes it possible to change the encoding in one place.

## 3. Public interface

Three families of constants, each a pair or quadruple of small integers:

| Family | Values | Meaning |
| --- | --- | --- |
| Test status | `TEST_PASS`, `TEST_FAIL`, `TEST_DISABLED`, `TEST_SKIPPED` | Outcome of an individual test method. |
| Case status | `CASE_PASS`, `CASE_FAIL` | Aggregate outcome of a case. |
| Fixture status | `FIXTURE_PASS`, `FIXTURE_FAIL` | Aggregate outcome of a whole fixture run. |

All are plain integers and are intended to be imported with a wildcard import; the testing core does exactly that.

## 4. State and data structures

No mutable state. The constants are compile-time-like values used inside result dictionaries:

- A test result has keys `status`, `value` and `time`. Its `status` is one of the four test-status values.
- A case result has keys `tests`, `time` and `status`. Its `status` is one of the two case-status values.
- A fixture result has keys `cases`, `time`, `status` and `revision`. Its `status` is one of the two fixture-status values.

Aggregation rule implied by the values: any failure at a lower level maps to failure at the next level; the absence of failures maps to pass. Disabled and skipped tests do **not** by themselves make a case fail.

## 5. Algorithm / behaviour

The module performs no algorithm; it is a declaration. Its behavioural significance lies in how the values are interpreted by consumers:

1. A test method runs. If it completes without raising, its result status becomes `TEST_PASS` and its return value is stored as the result value. If it raises a disabled/skipped/failed exception, the corresponding status is used and the exception message becomes the value.
2. When a case finishes, it scans all test statuses. If any equals `TEST_FAIL`, the case status is `CASE_FAIL`; otherwise `CASE_PASS`. `TEST_DISABLED` and `TEST_SKIPPED` are tolerated.
3. When a fixture finishes, it scans all case statuses. If any equals `CASE_FAIL`, the fixture status is `FIXTURE_FAIL`; otherwise `FIXTURE_PASS`.
4. Comparison tools skip results whose status is not `TEST_PASS`, because a failed or disabled test has no meaningful value to compare.
5. Dependency handling treats anything other than `TEST_PASS` / `CASE_PASS` as a reason to mark dependent tests or cases skipped.

## 6. Dependencies

- **Imported by**: `tools/digress/testing.py.md`, which uses a wildcard import and all of these constants; `tools/digress/cli.py.md` indirectly relies on them through the fixture; front-end scripts may import them directly for custom result handling.
- **Imports**: none.

## 7. Constants and tables

| Constant | Integer | Interpretation |
| --- | --- | --- |
| `TEST_PASS` | 0 | Test ran successfully; its value is comparable. |
| `TEST_FAIL` | 1 | Test failed; value is typically the error text. |
| `TEST_DISABLED` | 2 | Test was intentionally disabled, usually via a decorator or missing external tool. |
| `TEST_SKIPPED` | 3 | Test was skipped, usually because a dependency failed. |
| `CASE_PASS` | 0 | No test in the case failed. |
| `CASE_FAIL` | 1 | At least one test in the case failed. |
| `FIXTURE_PASS` | 0 | No case in the fixture failed. |
| `FIXTURE_FAIL` | 1 | At least one case failed. |

Note the deliberate reuse of 0 and 1 across the three families: values are only meaningful within their own layer.

## 8. Performance notes

No runtime cost. Design implications:

- Using small integers rather than enums keeps JSON serialisation of cached results simple and stable across Python versions.
- Aggregation is a linear scan over results; with a few dozen cases this is negligible.
- When adding a new status, all three consumers (aggregation, dependency handling, comparison) must be updated; there is no central validation.

### 8.1 Relation to faster-kernel development

Status codes matter for kernel work because they let a developer distinguish "the new kernel is wrong" (`TEST_FAIL`) from "the test could not run on this machine" (`TEST_DISABLED` or `TEST_SKIPPED`). When a conformance run reports skipped tests, the result must not be treated as a pass: the ISA coverage may be incomplete. Always verify that the expected number of tests reached `TEST_PASS` before accepting a SIMD change.

References: `tools/digress/testing.py.md` for aggregation and dependency rules; `tools/checkasm.c.md` for the analogous assembly-level pass/fail reporting.

## 9. References

- The source file itself, `tools/digress/constants.py`.
- `tools/digress/testing.py.md` for the consumers of each value.
- `tools/digress/errors.py.md` for the exceptions that map to disabled/skipped/failed statuses.

## 10. Verification checklist

- All eight constants exist and have the documented integer values.
- A case containing only disabled and skipped tests reports `CASE_PASS`.
- A case containing one `TEST_FAIL` reports `CASE_FAIL`.
- A fixture containing one `CASE_FAIL` reports `FIXTURE_FAIL`.
- A comparison pair with one result not `TEST_PASS` is reported as incomparable rather than compared.
- Cached JSON results round-trip the integers without conversion.

## 11. Open questions

- UNKNOWN: whether any consumer treats `TEST_DISABLED` differently from `TEST_SKIPPED` in aggregate output; both are non-failing in the source.
- UNKNOWN: whether the numeric values are part of an on-disk cache format that must remain stable across framework versions.
