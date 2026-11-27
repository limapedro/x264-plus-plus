# version.sh — rebuild specification

## 1. Header

- Source path: `version.sh` (root of the reference tree), a POSIX shell script.
- Role: emits the preprocessor macros that carry the x264 API/build number and optional git-derived revision string into the generated public configuration header.
- Marker: **dirty-side behavioural specification** — describes the observable output contract, not the script text.

## 2. Purpose

`configure` needs to stamp every build with a version string that is meaningful both for installed libraries and for the `--version` CLI output. The API version is a fixed number in `x264.h`; the revision suffix is derived from the local git history when available. `version.sh` centralizes this: `configure` appends its standard output to `x264_config.h`, which is then included by `x264.h`, so all consumers see the same four macros. The script is deliberately tolerant of non-git source archives: a release tarball simply gets an "x" placeholder revision.

## 3. Public interface

- Invocation: `./version.sh` run from the source directory (in practice `configure` invokes it by absolute/relative path and redirects stdout).
- Output: zero or four preprocessor `#define` lines on standard output.
  - Always: `X264_VERSION "<string>"` and `X264_POINTVER "0.<api>.<ver>"`.
  - Only in a git working tree with more than one commit: `X264_REV <number>` and `X264_REV_DIFF <number>` before the other two.
- Exit behavior: exits `1` without output if the script is not run from a directory containing `x264.h`; otherwise exits `0`. All other failures are silent.

## 4. State and data structures

- `api` — numeric build/API identifier extracted from the `#define X264_BUILD` line in `x264.h`.
- `ver` — revision component: initially `"x"`; becomes a commit-count-derived number in a git tree, possibly suffixed with `+<diff>`, `M`, and a hash.
- `version` — the human-readable suffix, empty for non-git builds and `" r<ver>"` for git builds.
- `localver` — total number of commits reachable from `HEAD`.
- `ver_diff` — number of commits in `origin/master..HEAD` (local commits not on the tracked master branch).
- No files are written by this script; all state is internal and flushed as text to stdout.

## 5. Algorithm / behaviour

Step-by-step behaviour:

1. Change to the directory containing the script, discarding output. If `x264.h` is not present there, exit with status `1` and emit nothing. This makes the script location-independent as long as it sits beside the header.
2. Extract `api` by locating the `X264_BUILD` define in `x264.h` and taking the first token that starts with a non-zero digit. The value must therefore be a positive decimal integer.
3. Set the placeholder revision `ver = "x"` and the human-readable suffix `version = ""`.
4. If a `.git` directory exists and a `git` executable is available:
   - Count commits reachable from `HEAD`. If the count is greater than one, the build is considered versionable.
   - Compute `ver` as the total commit count minus the number of commits since `origin/master`; emit `X264_REV` and `X264_REV_DIFF` with those two numbers.
   - If the diff count is non-zero, append a plus sign and the diff count to `ver`.
   - If `git status` reports any modified files, append `M` to `ver`.
   - Append a space and the first seven hexadecimal characters of the `HEAD` hash to `ver`.
   - Set `version` to `" r"` followed by that composed `ver` string.
   - Note: this block only runs when there is more than one commit; a one-commit or empty repository yields the placeholder path.
5. Emit `X264_VERSION` with the value of `version` enclosed in double quotes (empty for non-git builds), then `X264_POINTVER` with `"0.<api>.<ver>"`.

Concrete examples of the contract:

- Non-git source tree with `X264_BUILD 164`: `X264_VERSION ""`, `X264_POINTVER "0.164.x"`.
- Clean git checkout at the tracked master tip with 1000 commits and no local diff: `X264_REV 1000`, `X264_REV_DIFF 0`, `X264_VERSION " r1000 <7-hex-hash>"`, `X264_POINTVER "0.164.1000"`.
- Dirty checkout with two local commits: `X264_REV` = base count, `X264_REV_DIFF 2`, `ver` like `998+2M`, `X264_POINTVER "0.164.998+2M"`.

Ordering matters: the revision macros come first, then the two string macros, because `configure` appends the whole output to a header whose earlier lines define the public configuration values.

## 6. Dependencies

- Reads: `x264.h` from the same directory (for `X264_BUILD`); `.git` metadata and `git status`/`git rev-list` output when present.
- External tools: shell, `grep`, `sed`, `git`, `wc`, `cut`, `dirname`, `command -v`.
- Called by: `configure`, which appends stdout to `x264_config.h`. No other script in the tree calls it directly.
- Consumers: `x264.h` includes `x264_config.h`, so the macros are visible to every API user; `x264.c` uses `X264_POINTVER` (and falls back to `X264_BUILD` when absent) in its version banner; `configure` extracts the point version from `X264_POINTVER` for `x264.pc`.

## 7. Constants and tables

| Constant / rule | Value |
| --- | --- |
| Placeholder revision | `x` |
| Point-version prefix | `0.` |
| Human-readable version prefix | `r` preceded by a space |
| Git hash length | 7 characters |
| Dirty-tree marker | `M` |
| Local-commit marker | `+<count>` |
| Versionable condition | More than one commit reachable from HEAD |
| Upstream reference | `origin/master` |
| Required header | `x264.h` with a positive `X264_BUILD` define |

There are no lookup tables; the script is a pure derivation of three inputs (API number, local commit count, diff count/dirtiness/hash).

## 8. Performance notes

Runtime cost is a handful of `git rev-list`/`git status` subprocesses executed once at configure time; irrelevant to encoder runtime. Practical guidance:

- Building from an archive without `.git` is fastest and yields the stable placeholder string; useful for reproducible release builds.
- In a large repository, `git rev-list HEAD | wc -l` walks history each configure run; this is still negligible relative to compilation.
- The version macros are compile-time strings, so they add no runtime formatting cost beyond the one-time banner print.
- Public reference: the x264 project's versioning convention documented in its build instructions and the `X264_BUILD`/API-compatibility discussion in `x264.h`.

## 9. References

- Reference tree `configure` — invokes `version.sh` and appends its output to `x264_config.h`.
- Reference tree `x264.h` — source of `X264_BUILD`.
- Reference tree `x264.c` — `print_version_info()` consumes `X264_POINTVER`/`X264_VERSION`.
- Git documentation for `git rev-list`, `git status` and revision ranges of the form `A..B`.

## 10. Verification checklist

- Run `version.sh` in a directory without `x264.h`: confirm exit status `1` and no output.
- Run against a header with `X264_BUILD 164` and no git repository: confirm exactly two lines, `X264_VERSION ""` and `X264_POINTVER "0.164.x"`.
- In a git tree with more than one commit: confirm four lines and that `X264_REV + X264_REV_DIFF` equals the total commit count on the upstream tip.
- Confirm that a local commit increases `X264_REV_DIFF` and reduces `X264_REV` accordingly.
- Confirm that a modified working tree appends `M` to the point-version suffix.
- Confirm the hash suffix is exactly seven characters and matches the current `HEAD`.
- Confirm `X264_VERSION` contains a leading space before `r` and no trailing whitespace in the quoted value.
- Confirm `configure` produces `x264.pc` whose `Version` field matches the numeric part of `X264_POINTVER`.

## 11. Open questions

- Behaviour when `origin/master` does not exist is **UNKNOWN**: the revision-range count may fail and yield an empty or zero value depending on git version and shell settings; the script has no explicit guard for that case.
- The `git status` dirtiness test looks specifically for the word `modified:`; untracked files and staged-only changes may not be detected — exact coverage of the test is **UNKNOWN**.
- The script assumes `X264_BUILD` is a decimal integer larger than zero; a non-numeric value would produce an empty `api` string.
- The interpretation of the revision suffix by downstream packagers is outside this file's contract.
