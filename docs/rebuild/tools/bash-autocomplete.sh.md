# `tools/bash-autocomplete.sh` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/bash-autocomplete.sh`.
- Role: a Bash programmable-completion helper that translates the shell's current command line into a `x264 --autocomplete <prev> <cur>` query and installs the returned candidates as completions.
- Marker: this file is a **dirty-side behavioural specification**; the calling convention and transformations are described in our own words.

## 2. Purpose

The encoder already knows its own option vocabulary and value enumerations; duplicating that list in a shell script would make it go stale whenever an option is added. This helper delegates the work to the executable itself: it extracts from Bash's completion state the executable path, the token before the cursor and the partially typed word, calls the encoder's completion query mode, and hands the printed candidates back to Bash. It is installed by the build's bash-completion install target and is purely interactive tooling — it does not affect encoded output.

## 3. Public interface

- There are no positional parameters or command-line options of its own; the script is sourced by Bash, not executed as a program.
- It defines one function, `_x264`, which takes no arguments and reads Bash's `COMP_LINE` and `COMP_POINT` variables.
- It registers the function with `complete -o default -F _x264 x264`, so completion is attached to commands named exactly `x264`.
- Output channel: `COMPREPLY`, the array variable Bash reads after a completion function returns. Each whitespace-separated word printed by the encoder becomes one array element because the command substitution is unquoted in the assignment.
- Exit status of the function is the status of its last command; Bash treats a non-empty `COMPREPLY` as a successful completion. The `-o default` registration makes Bash fall back to its normal completion (usually filenames) when the function produces nothing.
- Protocol toward the encoder: `x264 --autocomplete <prev> <cur>`, where `prev` is the preceding whitespace-delimited token and `cur` is the word being completed. A zero return from the encoder means "candidates were printed"; a non-zero return means "use default completion".

## 4. State and data structures

All state is local to a single completion invocation:

- `path`: the leading run of non-blank characters of `COMP_LINE`, interpreted as the command name (possibly a path). After quoting and tilde handling it becomes the executable actually invoked.
- `args`: the slice of `COMP_LINE` starting just after `path` whose length is `COMP_POINT - length(path)`. It represents the portion of the command line up to the cursor, excluding the command word.
- `cur`: the trailing portion of `args` after the last blank character or `=` sign. Both blanks and `=` therefore act as word separators for the current word.
- `prev`: derived from the part of `args` before `cur` by removing trailing blanks/`=` and then removing everything through the last blank; effectively the last token before the word being completed, or the empty string.
- `COMPREPLY`: the array filled from the encoder's stdout.
- Shell options: the entire function body is wrapped so that its standard error is discarded, keeping interactive completion silent when the encoder is missing or fails.

## 5. Algorithm / behaviour

1. **Extract the command word.** Take `COMP_LINE` up to the first whitespace character; this is `path`. Everything before any blank belongs to the command, so an absolute or relative path is preserved.
2. **Extract the text before the cursor.** Starting at the character after `path`, take exactly `COMP_POINT - length(path)` characters. Note that `COMP_POINT` is a cursor *index*, but it is used here as a *length* relative to the end of `path`; the two coincide only when the cursor is at the end of the line. With the cursor in the middle of the line, the captured slice is shorter than the text actually typed.
3. **Find the current word.** Strip from `args` everything through the last blank or `=` character. The remainder is `cur`. If no separator exists, `cur` is all of `args`; if the line ends with a separator, `cur` is empty.
4. **Find the previous word.** Take the prefix of `args` before `cur`, trim trailing blanks and `=` characters, then delete everything up to and including the last blank. The result is `prev`; it is empty when there is only one word after the command.
5. **Quote the command path.** Pass `path` through the shell's `%q` quoting formatter, then evaluate a substitution that changes only a leading escaped tilde into a literal tilde. This re-enables tilde expansion by the shell (so `~/bin/x264` resolves to the home directory) while leaving all other metacharacters quoted and inert.
6. **Query the encoder.** Invoke the resolved executable with the literal argument `--autocomplete`, followed by `prev` and `cur` as two separate arguments. The two are always passed, even when `prev` or `cur` is empty.
7. **Install results.** Assign the command's stdout to `COMPREPLY` via word splitting, so the encoder's space-separated candidate list becomes the completion array. Only when that command succeeds is Bash's default completion behaviour disabled for this completion (`compopt +o default`). If the encoder returns non-zero, the array is empty and Bash's `-o default` fallback remains active.
8. **Suppress noise.** All diagnostics from the function, including a missing executable or a malformed line, go to `/dev/null`; the user sees only candidates or normal shell behaviour.

Ordering and edge cases:

- The `=` separator means completion after `--preset=` treats the value after the equals sign as `cur` and `--preset` (minus the `=`) as `prev`, which is what the encoder's option classifier expects.
- A command path containing spaces is safe: the `%q` quoting happens before the executable is invoked.
- Tilde expansion is intentionally the only unquoting performed; an unescaped `$VAR` in the command word remains literal.
- Because `prev` is computed from `args` rather than from `COMP_LINE`, the command word can never appear as `prev`.
- If the command line has no arguments at all, `args`, `cur` and `prev` are all empty, and the encoder offers its top-level options including help/version entries.

## 6. Dependencies

- **Bash features**: programmable completion (`complete`, `compopt`), `COMP_LINE` and `COMP_POINT`, `local`, `printf -v` with `%q`, word-splitting in array assignments, `eval`, here-strings, and parameter expansion for substring/prefix removal. This script is Bash-specific; it is not portable POSIX sh.
- **External tool**: `sed`, used twice in one expression to trim the `prev` string.
- **Target program**: the `x264` executable compiled with the `--autocomplete` fast path (see `autocomplete.c.md`). The build must install this script for the integration to be active.
- **Consumers**: interactive Bash shells of users who installed the completion file.

## 7. Constants and tables

| Item | Meaning |
| --- | --- |
| Completion command | `complete -o default -F _x264 x264` — exact command name `x264`. |
| Query flag | `--autocomplete`, always the first argument after the executable path. |
| Word separators | Blank characters and `=`. |
| Cursor variables | `COMP_LINE` (full line), `COMP_POINT` (cursor position, used as a relative length). |
| Result variable | `COMPREPLY`. |
| Tilde rewrite | A leading escaped tilde is converted back to a real tilde before evaluation. |
| Error handling | Function-level standard-error redirection to `/dev/null`. |

There are no numeric thresholds, no option tables and no state retained between invocations.

## 8. Performance notes

- Completion is interactive and one-shot; latency is dominated by process startup and dynamic linking of the encoder, not by this script's string operations.
- The script performs one `sed` invocation and one encoder invocation per Tab press. Keeping the `sed` expression compact matters slightly on slow shells; folding the two trims into a single process (as done here) avoids a second fork.
- The unquoted array assignment is deliberate: it performs word splitting in the shell instead of requiring the script to parse the candidate list character by character.
- **Optimization tip:** avoid spawning the encoder for cases whose answer is already known locally (for example when `cur` is empty and no `prev` exists, the full option list could be cached). Any such cache must be invalidated when the installed encoder changes, otherwise newly added options would never appear.
- **Optimization/correctness tip:** pass the cursor offset as an absolute position rather than subtracting the command-word length; the current arithmetic gives wrong results when the user completes in the middle of a line. Fixing this improves both perceived speed (correct candidates the first time) and reliability.

### 8.1 Relation to faster-kernel development

Completion tooling is not on the encode path, but it helps benchmark workflows: tab-completing long option names such as `--no-mbtree` or `--tune zerolatency` reduces mistyped flags in repeatable benchmark commands. Because the query is answered by the executable, a newly added tuning option becomes completable as soon as the rebuilt binary is installed — no separate edit of this script is required. Always verify the benchmark command line after using completion, since an accidentally completed filename option changes what the encoder measures.

## 9. References

- The source file itself, `tools/bash-autocomplete.sh`.
- Bash reference manual: programmable completion, `COMP_LINE`, `COMP_POINT`, `COMPREPLY`, `compopt`, and `complete -o default`.
- Bash manual on `printf -v` and the `%q` format, and on `eval` / tilde expansion.
- `autocomplete.c.md` — the encoder side that classifies `prev` and produces the candidate list.
- `x264.c.md` — the `main()` fast path that recognizes `--autocomplete`.
- The build's bash-completion install rule (see `Makefile.md`).

## 10. Verification checklist

- Sourcing the file in Bash defines `_x264` and registers completion for the exact command name `x264`, with default completion retained.
- With `COMP_LINE="x264 "` and `COMP_POINT=5`, the encoder is called with an empty `prev` and an empty `cur`.
- With a partially typed option such as `x264 --pre`, only candidates beginning with that prefix appear in `COMPREPLY`.
- After `--preset=`, the value portion is treated as `cur` and `--preset` as `prev`; the encoder returns preset names.
- After a filename-valued option, `COMPREPLY` is empty and Bash's default filename completion remains enabled.
- A command path written as `~/bin/x264` is expanded to the home directory before execution; a path containing spaces is quoted correctly.
- When the encoder prints several candidates separated by spaces, each becomes a distinct `COMPREPLY` element.
- A failing or missing encoder produces no error text on the terminal and falls back to default completion.
- Completion invoked with the cursor before the end of the line uses the truncated slice, confirming the documented `COMP_POINT`-as-length behaviour.

## 11. Open questions

- UNKNOWN: whether `COMP_POINT` is intended to be a byte index or a character index for multibyte command lines; Bash documents byte offsets, but the script's arithmetic assumes ASCII prefixes.
- UNKNOWN: behaviour when `COMP_LINE` begins with leading whitespace — the first token would be empty and the executable path would be an empty string.
- UNKNOWN: whether the `=` handling was designed for `--opt=value` syntax only, or also for values containing `=`; the last `=` wins in the current logic.
- UNKNOWN: whether other shells (zsh, fish) can source this file; no alternative completion scripts are shipped alongside it.
