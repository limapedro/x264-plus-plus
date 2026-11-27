# `tools/digress/testing.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/testing.py`.
- Role: the core of the Digress regression framework — decorators, fixture/case/test lifecycle, result caching, dependency resolution, SCM-aware revision handling and the comparison/bisection commands.
- Marker: this file is a **dirty-side behavioural specification**; the design is described in our own words.

## 2. Purpose

The module implements the test object model and the workflow around it. It separates three levels — fixture (a whole test program tied to a revision), case (a group of related tests with shared setup) and test (one measured value) — and adds bookkeeping that makes regression testing reproducible: results are cached per revision, dependencies are resolved automatically, revisions are checked out and restored safely, and two revisions can be compared or bisected.

## 3. Public interface

### Decorators

| Decorator | Contract |
| --- | --- |
| `depends(*test_names)` | Attaches dependency names to a test method; the runner executes those tests first and skips the dependent test if any dependency did not pass. |
| `disabled(reason="")` | Wraps a test so it raises `DisabledTestError(reason)` when called. |
| `comparer(comparer_)` | Attaches a comparer function to a test method for use by compare/bisect commands. |
| Internal `_skipped(reason)` | Same shape as `disabled` but raises `SkippedTestError`; used for dependency failures. |

### Classes

| Class | Contract |
| --- | --- |
| `Fixture` | Owns cases, SCM backend, flush flag, datastore path and all dispatchable workflow commands. |
| `Case` | Owns test methods, a dependency list of other Case classes, and a per-case datastore path. |

### Fixture commands (all marked dispatchable)

- `flush(revision=None)`: delete cached results for one resolved revision, or for all revisions if no argument is given.
- `run(revision=None)`: check out a revision (with stash/restore), execute all cases, aggregate results and print a summary.
- `bisect(good_rev, bad_rev=None)`: run the good revision, drive the SCM bisection loop, mark each tested revision good or bad using comparers, and reset on exit.
- `compare(rev_a, rev_b=None)`: run two revisions and apply each test's comparer to corresponding values.
- `multicompare(rev_a=None, rev_b=None, mode="waterfall")`: run every revision in a range and print a table in vertical ("waterfall") or horizontal ("river") layout.
- `list()`: print the names of registered cases, excluding dependency-only cases by convention of the front end.

### Helper methods

- `Fixture.register_case(case)`: attach the fixture to the case class and append it to the case list.
- `Case.run()`: execute all test methods and aggregate.
- `Case._get_test_by_name(name)`: resolve a `test_<name>` method or raise `NoSuchTestError`.
- `Case._run_test(test, results)`: run one test with dependency and cache handling.
- `Fixture._run_case(case, results)`: run one case with case-level dependency handling.
- `Fixture._skip_case(case, depend)`: rewrite all `test_*` methods of a case into skipped wrappers.

## 4. State and data structures

### Fixture state

- `cases`: class-level list (empty by default) of Case subclasses.
- `scm`: module supplying checkout/rev_parse/current_rev/current_branch/revisions/stash/unstash/bisect/dirty.
- `flush_before`: boolean set by the CLI's `--flush` option.
- `dispatcher`: back-reference installed by the CLI dispatcher.
- `datastore`: path to the current revision's cache directory, or `None` for a dirty working tree.

### Case state

- `depends`: list of Case classes that must run successfully first.
- `fixture`: back-reference installed by `register_case`.
- `datastore`: per-case cache directory path, derived from the fixture datastore and a hash of the case class name.

### Result dictionaries

- Test result: `{"status": <test status>, "value": <string>, "time": <float or None>}`.
- Case result: `{"tests": <name→test result>, "time": <sum>, "status": <case status>}`.
- Fixture result: `{"cases": <name→case result>, "time": <sum>, "status": <fixture status>, "revision": <rev>}`.

### Cache layout

- A fixture-level directory named `.digress_<FixtureClassName>` contains one subdirectory per resolved revision.
- Inside a revision directory, each case gets a subdirectory named by the SHA-1 of the case class name.
- Inside a case directory, each test result is a JSON file named by the SHA-1 of the test name (without the `test_` prefix) plus `.json`.

## 5. Algorithm / behaviour

### 5.1 Dependency handling at case level

`_run_case` first rejects a case already present in `results` with `AlreadyRunError`. For each class in `case.depends` it:

1. If the dependency already ran and did not pass, rewrite the case's test methods to skipped wrappers with a "failed dependency" reason.
2. Recursively run the dependency; `AlreadyRunError` is caught and ignored for recursion control.
3. If the recursive result did not pass, rewrite the test methods similarly.

Then it instantiates the case and calls `run()`, stores the result under the case class name and returns it.

### 5.2 Test execution and caching

`Case.run()` computes the per-case datastore path, creates it if needed, enumerates methods whose names begin with `test_`, and calls `_run_test` on each.

`_run_test` derives the test name by stripping the five-character prefix. It rejects duplicates via `AlreadyRunError`. Test-level dependencies declared by `depends` are handled analogously to case dependencies: a failed dependency rewraps the test as skipped.

Timing starts immediately before execution. If a datastore exists, the runner tries to open the cached JSON file named by the hashed test name. On success it reads the result, converts the stored value to a string and maps stored status integers to human-readable words for display: pass, fail, disabled, skipped, or "???". Cached results are printed with a "(cached)" marker and are **not** re-executed.

On any I/O error (including the deliberate `IOError` raised when there is no datastore), the test is executed:

- `DisabledTestError` → status `TEST_DISABLED`, value is the message.
- `SkippedTestError` → status `TEST_SKIPPED`, value is the message.
- `FailedTestError` → status `TEST_FAIL`, value is the message.
- Any other exception → status `TEST_FAIL`, value is the exception text, printed as "failed with exception".
- Success → status `TEST_PASS`, value is the return value, elapsed time recorded.

The result is written back as JSON whenever a datastore exists, then stored in the results dictionary.

Aggregation: `Case.run()` sums the non-null `time` fields and sets case status to `CASE_FAIL` if any test status is `TEST_FAIL`, otherwise `CASE_PASS`.

### 5.3 Fixture run workflow

1. Ask the SCM backend whether the tree is dirty.
2. If the tree is clean and no revision was given, use `"HEAD"`.
3. If a revision was given:
   - Record the current revision and branch.
   - Stash changes if the tree was dirty.
   - Check out the requested revision with force.
   - Resolve it to a canonical revision identifier via the backend.
   - Set the datastore to the revision-specific directory, creating it or flushing it first if `flush_before` is set.
4. If no revision was given with a dirty tree, use the literal revision label for a dirty working tree and set the datastore to `None` (no caching).
5. Execute every case through `_run_case`, ignoring `AlreadyRunError`.
6. Sum case times, aggregate fixture status, print "passed"/"failed" with elapsed time.
7. In a `finally` block, restore the original revision, then the original branch if one was recorded, then unstash if changes had been stashed.

### 5.4 Flush behaviour

Without a revision, remove the whole `.digress_<ClassName>` directory. With a revision, resolve it through the SCM backend first, then remove only that revision's directory. Failures are printed but do not abort.

### 5.5 Compare and multicompare

`compare` runs both revisions, then for each case and test in the first result:

- Locate the corresponding test method by name.
- If either result is not `TEST_PASS`, print that it cannot be compared.
- Else if the method has a comparer attribute, call it with the two values. A `ComparisonError` prints a difference message; otherwise print that it does not differ.
- Else print that the test has no comparer.

`multicompare` obtains the revision list from the backend, runs each, collects test names from the first result, and prints a table. In "waterfall" mode rows are tests and columns are revisions; in "river" mode rows are revisions and columns are tests. Cell text is `"value (time)"` for passes and `"did not pass: value"` otherwise. Unknown mode strings fall back to "waterfall" with a notice. Columns are wrapped to a 20-character maximum and separated by printed rules.

### 5.6 Bisection

1. Run the good revision to establish reference results and resolve its canonical identifier.
2. Stash if the tree is dirty.
3. Drive the SCM backend: start bisection, mark the bad revision and the resolved good revision.
4. Loop: run the currently checked-out revision; for each test in the reference results, locate the corresponding test method and compare:
   - If the current result failed while the reference did not, mark the revision bad.
   - Else if the test has a comparer, apply it; a comparison failure marks the revision bad.
5. Mark the revision good or bad accordingly and inspect the backend output for the "is the first bad commit" sentinel to terminate.
6. On `SCMError`, print the message. In `finally`, reset bisection and unstash.

### 5.7 Timing helpers

Elapsed time is measured with `time.time()` around test execution only. Cached tests have `time = None`; sums filter out falsy values. The no-op timing calibration and cycle counting belong to the assembly tool, not here.

## 6. Dependencies

- **Imports**: errors, constants and the dispatchable decorator from sibling modules; `inspect`, `operator`, `os`, `json`, `textwrap`, `shutil.rmtree`, `time`, `functools.wraps`, `itertools.izip_longest`, `hashlib.sha1`.
- **External**: an SCM backend module plugged in via `Fixture.scm`.
- **Imported by**: front-end scripts that subclass `Fixture` and `Case`, register cases and instantiate the dispatcher.
- **Related**: `tools/digress/cli.py.md` drives the commands; `tools/digress/comparers.py.md` supplies comparers; `tools/digress/scm/git.py.md` and `dummy.py.md` supply backends.

## 7. Constants and tables

| Item | Meaning |
| --- | --- |
| Prefix `test_` | Marks a method as an executable test. |
| Prefix `digress_` | Prefix for decorator-attached attributes (`digress_depends`, `digress_comparer`, `digress_dispatchable`). |
| Cache root | `.digress_<FixtureClassName>` |
| Hash function | SHA-1 of case class name / test name, hex digest used as directory/file name. |
| `MAXLEN` equivalent | 20-character column width in multicompare tables. |
| Status text mapping | pass / failed / disabled / skipped / ???. |

Status integers come from `tools/digress/constants.py.md`.

## 8. Performance notes

- Result caching is the dominant performance feature: once a revision has been run, subsequent comparisons and bisection steps reuse JSON results instead of recompiling and re-encoding.
- `--flush` trades this speed for correctness when cached data may be stale; it should be used when a test method's definition or environment changed without a revision change.
- Dirty working trees deliberately disable caching, since there is no stable revision identifier to key on.
- Recursive dependency resolution with `AlreadyRunError` avoids duplicate work and prevents cycles from looping forever.
- Bisection is logarithmic in the number of revisions; each step still runs the full filtered case list.

### 8.1 Using this framework to accelerate faster-kernel development

For SIMD kernel work the recommended loop is:

1. Validate the kernel with the assembly conformance tool (`tools/checkasm.c.md`) on all supported ISA tiers.
2. Run the fixture once on the baseline revision to populate the per-revision cache.
3. Apply the kernel change and run with `--flush` so results are regenerated.
4. Use `--cases` to restrict the run to cases whose kernels changed.
5. If a regression appears, use `bisect` between the known-good and current revision; comparers attached with the `comparer` decorator decide badness numerically (e.g. tolerance-based PSNR) rather than by exact equality.
6. Use `multicompare` to see a metric's evolution across a range of commits when tuning for speed.

Timing values stored in cached results allow performance trends to be tracked alongside correctness, but they are wall-clock measurements of whole tests and should not replace the calibrated cycle counts from `checkasm --bench`.

References: `tools/checkasm.c.md`, `tools/digress/cli.py.md`, `doc/regression_test.txt.md`.

## 9. References

- The source file itself, `tools/digress/testing.py`.
- Python documentation for decorators, `inspect`, `json`, `hashlib` and context managers.
- Git bisection documentation for the workflow wrapped by the SCM backend.
- `tools/digress/constants.py.md`, `tools/digress/errors.py.md`, `tools/digress/comparers.py.md`, `tools/digress/cli.py.md`, `tools/digress/scm/git.py.md`.

## 10. Verification checklist

- A clean tree with no revision argument uses `HEAD` as the effective revision and creates a revision cache directory.
- A dirty tree with no revision argument uses the dirty label and disables caching.
- After `run`, the original revision, branch and stash state are restored even if a test raises.
- A case dependency that fails causes all dependent case tests to be recorded as skipped, not failed.
- A test dependency that fails causes the dependent test to be recorded as skipped.
- A cached test is reported with "(cached)" and is not executed a second time.
- `flush` with no revision removes the whole cache root; with a revision it removes only that revision.
- `compare` reports "no comparer" for a test without the decorator and applies the comparer when present.
- `bisect` terminates on the first-bad-commit sentinel and always resets bisection state.
- `multicompare` accepts both layouts and falls back to waterfall for an unknown mode.
- Result dictionaries have exactly the documented keys and status values from the constants module.

## 11. Open questions

- UNKNOWN: why the case summary prints a fixture-level constant when deciding its own pass/fail wording; the values coincide so behaviour is unaffected.
- UNKNOWN: whether `DisabledCaseError` and `SkippedCaseError` are ever raised by a front end; the core does not use them.
- UNKNOWN: how a comparer receives cached string values that were originally numeric; no conversion back is visible here.
- UNKNOWN: behaviour when two cases share the same class name after a front end mutates `__name__` dynamically; hashing uses the class name at run time.
