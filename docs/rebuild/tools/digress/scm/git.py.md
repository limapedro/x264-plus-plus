# `tools/digress/scm/git.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/scm/git.py`.
- Role: the git backend for Digress; it wraps the git command-line tool to provide revision resolution, checkout, stashing, branch discovery, revision ranges, bisection and dirty-tree detection.
- Marker: this file is a **dirty-side behavioural specification**; behaviour is described in our own words, not copied from the source.

## 2. Purpose

The module isolates all git-specific process handling behind a small module-level interface so the fixture can operate on arbitrary revisions without knowing command-line details. It also converts non-zero git exit statuses into a framework exception with a normalised, single-line message.

## 3. Public interface

| Function | Contract |
| --- | --- |
| `checkout(revision)` | Force-check out a revision. Raises `SCMError` on failure. |
| `rev_parse(ref)` | Resolve a reference to a canonical identifier (typically a full hash). Raises `SCMError` on failure. |
| `current_rev()` | Return `rev_parse("HEAD")`. |
| `current_branch()` | Return the active branch name, or `None` when detached. Raises `SCMError` on failure. |
| `revisions(rev_a, rev_b)` | Return a list of revision identifiers in the range from one reference to another. Raises `SCMError` on failure. |
| `stash()` | Stash working-tree changes while keeping the index. Raises `SCMError` on failure. |
| `unstash()` | Pop the most recent stash. Does not check the return code. |
| `bisect(*args)` | Run `git bisect` with the supplied subcommand and arguments; return its output. Raises `SCMError` on failure. |
| `dirty()` | Return a boolean indicating whether tracked files are modified. Raises `SCMError` on failure. |

All functions communicate with git by spawning a subprocess, capturing standard output and merging standard error into it.

## 4. State and data structures

- `GIT_BRANCH_EXPR`: a compiled regular expression matching a line that begins with an asterisk — the marker git uses for the current branch in `git branch` output.
- No mutable module state; each call spawns an independent process.
- Output is stripped of leading/trailing whitespace before parsing (except in `bisect`, which returns raw output).

## 5. Algorithm / behaviour

### 5.1 Common process pattern

For most functions:

1. Spawn `git` with a fixed argument list, capturing stdout into a pipe and redirecting stderr into stdout.
2. Read all output and strip surrounding whitespace.
3. If the process exit status is non-zero, raise `SCMError` with a message prefix identifying the operation (e.g. "checkout error", "rev-parse error") followed by the captured output. The `SCMError` constructor collapses newlines to spaces.
4. Otherwise return the parsed output or nothing.

### 5.2 Checkout

Runs `git checkout -f <revision>`. The force flag means local modifications that conflict with the checkout are discarded; callers are expected to stash first. The fixture indeed checks `dirty()` and stashes before calling this.

### 5.3 Revision resolution

`rev_parse(ref)` runs `git rev-parse <ref>` and returns the stripped output. `current_rev()` delegates with the literal reference `HEAD`. This canonicalisation is what lets the fixture key its cache by a stable identifier rather than by a symbolic name that may move.

### 5.4 Current branch

Runs `git branch --no-color`, applies the asterisk-matching regular expression to find the current branch name, and returns it unless it equals the detached-HEAD sentinel used by git, in which case `None` is returned. If no match is found the code would raise an index error; UNKNOWN: whether callers guard against that.

### 5.5 Revision ranges

Runs `git log --format=%H <a>...<b>` and splits the stripped output on newlines. The symmetric-difference range syntax means the result is the set of commits reachable from either reference but not both; the fixture uses this to enumerate revisions between two points for `multicompare`.

### 5.6 Stashing

`stash()` runs `git stash save --keep-index`, preserving staged changes in the index while stashing the rest. `unstash()` runs `git stash pop` and ignores the exit status, so a pop conflict is not reported by this layer.

### 5.7 Bisection

`bisect(*args)` prepends `bisect` to the supplied arguments, runs the command, captures output without stripping, checks the return code and returns the raw output. The fixture looks for the "is the first bad commit" phrase in the first line to decide when to stop iterating.

### 5.8 Dirty detection

Runs `git status` and checks whether the output contains the substring indicating modified files. This is a heuristic: untracked files alone are not detected, because the check looks for a specific word rather than parsing all status categories.

## 6. Dependencies

- **Imports**: `subprocess.Popen`, `subprocess.PIPE`, `subprocess.STDOUT`, `re`, and `digress.errors.SCMError`.
- **Imported by**: front-end scripts that set `Fixture.scm = digress.scm.git`, most notably the x264 test driver.
- **Consumed by**: `tools/digress/testing.py.md`, which calls all nine functions.

## 7. Constants and tables

| Constant | Meaning |
| --- | --- |
| `GIT_BRANCH_EXPR` | Regular expression matching the current-branch marker line in `git branch` output. |
| Detached sentinel | The branch label git prints for a detached HEAD; mapped to `None`. |
| Error prefixes | Operation-specific prefixes used in `SCMError` messages: checkout, rev-parse, branch, log, stash, bisect, status. |
| `--keep-index` | Flag used when stashing so staged changes remain available. |
| `--format=%H` | Log format used to obtain full revision identifiers. |

## 8. Performance notes

- Each function spawns a new process; bisection and multi-revision runs therefore incur many short-lived git processes. This is negligible compared with a full compile+encode cycle.
- Capturing stderr into stdout keeps error messages available for diagnostics but requires stripping before parsing.
- Force checkout is fast but destructive; the fixture's stash/restore discipline is essential to avoid losing user work.
- Extensibility tip: a backend for another SCM needs only the nine functions listed above; no changes to the fixture are required.

### 8.1 Relation to faster-kernel development

Bisection is the main accelerator for finding which commit introduced a kernel regression:

1. Run the fixture on a known-good revision to capture reference values.
2. Mark the current revision bad and let the backend drive the search.
3. Each iteration checks out a candidate, runs the filtered case list and uses the test's comparer (often tolerance-based) to classify it.
4. The loop ends when git reports the first bad commit, and the `finally` block restores the original tree.

Use `dirty()` awareness when working on uncommitted SIMD experiments: a dirty tree is stashed and restored automatically, but the force checkout means any untracked scratch files are the developer's responsibility.

References: `tools/digress/testing.py.md` for the fixture workflow, `tools/digress/comparers.py.md` for comparers, `doc/regression_test.txt.md` for the end-to-end test being bisected.

## 9. References

- The source file itself, `tools/digress/scm/git.py`.
- Git documentation for `checkout`, `rev-parse`, `branch`, `log`, `stash`, `bisect` and `status`.
- `tools/digress/errors.py.md` for `SCMError` message normalisation.
- `tools/digress/testing.py.md` for the consuming workflow.

## 10. Verification checklist

- `rev_parse("HEAD")` returns a full revision identifier.
- `current_rev()` equals `rev_parse("HEAD")` at the same moment.
- `current_branch()` returns a branch name on a branch and `None` in detached-HEAD state.
- `revisions(a, b)` returns a list of full hashes for the symmetric range.
- `dirty()` returns true after modifying a tracked file and false on a clean tree.
- `stash()` followed by `unstash()` preserves working-tree changes.
- A failing git command raises `SCMError` whose string contains no newline and includes the operation prefix.
- `checkout` uses force semantics and can move between revisions even with conflicting local changes (after stashing).
- `bisect` returns command output and raises on failure.
- The fixture restores the original revision, branch and stash after a bisection run.

## 11. Open questions

- UNKNOWN: behaviour when `git branch` output contains no asterisk line; the code indexes the first match without a guard.
- UNKNOWN: whether untracked files should count as dirty; the current string check only looks for modifications.
- UNKNOWN: whether `git stash pop` conflicts are intentionally ignored by `unstash()` or should be escalated.
