# `tools/digress/scm/dummy.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/scm/dummy.py`.
- Role: a no-op source-control backend for Digress, used to exercise the framework without a real repository and to supply a placeholder revision identifier.
- Marker: this file is a **dirty-side behavioural specification**; behaviour is paraphrased in our own words.

## 2. Purpose

The module implements the backend interface with minimal behavior so that a fixture can be run in environments without git, or so that the framework's control flow can be tested in isolation. It deliberately does not provide bisection, because that operation has no meaningful no-op implementation in this context.

## 3. Public interface

| Function | Contract |
| --- | --- |
| `checkout(revision)` | Accept a revision and do nothing. |
| `current_rev()` | Return a string produced by converting a random floating-point value to a string. |
| `revisions(rev_a, rev_b)` | Accept two revisions and do nothing; returns `None`. |
| `stash()` | Do nothing. |
| `unstash()` | Do nothing. |
| `bisect(command, revision)` | Always raise `NotImplementedError` with a message stating that the dummy backend does not support bisection. |

Note the signature mismatch: the real backend's `bisect` is variadic, while the dummy declares two parameters. The fixture calls it with varying argument counts, so using the dummy backend with bisection would fail on argument binding as well as by design.

## 4. State and data structures

No persistent state. The module-level random generator supplied by the standard library is used to fabricate revision identifiers. Each call to `current_rev()` produces a different string with high probability; none of these identifiers corresponds to a real commit.

## 5. Algorithm / behaviour

1. `checkout(revision)` consumes its argument and returns immediately. No working-tree change occurs.
2. `current_rev()` calls the standard random-number generator, converts the result to a string and returns it. The value is not cached, so two calls generally differ.
3. `revisions(rev_a, rev_b)` consumes both arguments and returns nothing. A fixture command that expects a list of revisions would receive `None` and fail later; using this backend with `multicompare` is therefore not meaningful.
4. `stash()` and `unstash()` are empty operations, consistent with the absence of a real working tree.
5. `bisect(command, revision)` unconditionally raises `NotImplementedError`. The fixture's `finally` blocks still run, but a bisection attempt cannot complete.

The module does not implement `rev_parse`, `current_branch` or `dirty`, even though the fixture may call them. UNKNOWN: whether a front end is expected to supply these when using the dummy backend; no adapter is present.

## 6. Dependencies

- **Imports**: `random.random` from the standard library.
- **Imported by**: front-end scripts that select `digress.scm.dummy` as `Fixture.scm`, typically for smoke-testing the framework.
- **Related**: `tools/digress/testing.py.md` consumes the backend interface; `tools/digress/scm/git.py.md` is the complete implementation.

## 7. Constants and tables

None.

## 8. Performance notes

All operations are constant-time no-ops apart from random-number generation, so the dummy backend adds no measurable overhead. This makes it useful for validating framework logic (caching, dependency resolution, result aggregation) without the cost of checkouts and builds.

### 8.1 Relation to faster-kernel development

During rapid SIMD iteration, a dummy backend can be useful to run the framework's own control flow on a dirty tree with no cache keyed to a revision, avoiding repeated checkout/stash cycles. Once correctness is established, switch to the git backend and use bisection to locate regressions. Do not expect bisection or multi-revision comparison to work with this backend.

References: `tools/digress/testing.py.md` for the backend interface; `tools/digress/scm/git.py.md` for the real implementation.

## 9. References

- The source file itself, `tools/digress/scm/dummy.py`.
- Python documentation for the `random` module.
- `tools/digress/testing.py.md`, `tools/digress/scm/git.py.md`.

## 10. Verification checklist

- `checkout`, `stash` and `unstash` return without error for arbitrary arguments.
- `current_rev()` returns a string and typically differs between calls.
- `revisions()` returns nothing.
- `bisect()` raises `NotImplementedError` with the documented message.
- A fixture configured with this backend can run a simple case list without invoking git.
- A fixture configured with this backend fails cleanly when asked to bisect or multicompare.

## 11. Open questions

- UNKNOWN: whether the missing `rev_parse`, `current_branch` and `dirty` functions are intentional omissions or an unfinished stub.
- UNKNOWN: whether the two-argument `bisect` signature is intentional or a leftover from an earlier interface version.
