# `tools/digress/cli.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/cli.py`.
- Role: the command-line front end of the Digress framework: option parsing, command discovery via decorators, help formatting and dispatch to fixture methods.
- Marker: this file is a **dirty-side behavioural specification**; the behaviour is described in our own words.

## 2. Purpose

The module turns a fixture object into a small command-line program. Instead of maintaining a separate command table, it discovers commands by scanning the fixture for methods marked with a decorator, derives their argument counts from the method signatures, and validates calls before invoking them. This lets a front end add a new command simply by decorating a fixture method.

## 3. Public interface

| Symbol | Contract |
| --- | --- |
| `dispatchable(func)` | Decorator that sets a marker attribute on a method so the dispatcher will treat it as a command. |
| `Dispatcher(fixture)` | Constructor that stores the fixture and installs itself on the fixture as `fixture.dispatcher`. |
| `Dispatcher.dispatch()` | Parse options, run `pre_dispatch`, re-parse positional arguments, select and validate the command, invoke it. |
| `Dispatcher.pre_dispatch()` | Extension hook; default no-op. Called after the first parse so options can influence command availability. |
| `Dispatcher._populate_parser()` | Extension hook that builds the option parser and discovers commands; subclasses override and call super. |

Built-in options:

| Option | Effect |
| --- | --- |
| `-f`, `--flush` | Sets `fixture.flush_before = True`, so a run discards cached data for the revision before testing. |
| `-c`, `--cases FOO,BAR` | Restricts the fixture's case list to cases whose class names appear in the comma-separated list. |
| `--version` | Provided by the underlying option parser and reports the framework version. |
| `--help` | Provided by the parser; patched to append a command list. |

The usage string is of the form `usage: %prog [options] command [args]`, and the parser description names the fixture's class.

## 4. State and data structures

- `Dispatcher.fixture`: the wrapped fixture instance.
- `Dispatcher.commands`: a dictionary mapping command names to bound methods, populated at dispatch time.
- `Dispatcher.optparse`: the option parser instance, whose `print_help` method is monkey-patched to an instance-bound variant.
- Back-reference `fixture.dispatcher` allows test cases to read command-line options such as selected cases, video file, seed and encoder flags.

`dispatchable` works by setting an attribute named with a `digress_` prefix on the function object; discovery uses `hasattr` on the bound members returned by introspection.

## 5. Algorithm / behaviour

### 5.1 Command discovery

1. Enumerate all attributes of the fixture with `inspect.getmembers`.
2. Keep members that carry the dispatchable marker attribute.
3. Store them in `commands` keyed by attribute name.

This means only fixture methods are commands; case methods are not.

### 5.2 Help formatting

The parser's `print_help` method is replaced with a wrapper that:

1. Calls the original help printer.
2. Prints a blank line and the heading "Available commands:" to standard error.
3. Computes the longest command name to align descriptions.
4. For each command, folds its docstring into a paragraph (collapsing whitespace) and wraps it to a width derived from 80 columns minus the name column.
5. Prints the command name and wrapped description to standard error.

### 5.3 Option callbacks

- `--flush` uses a callback that sets a flag on the fixture; it takes no argument.
- `--cases` uses a callback that splits its value on commas and filters the fixture's case list by class name, keeping only matching cases.

### 5.4 Dispatch sequence

1. Populate the parser (discover commands and add options).
2. Parse arguments once.
3. Call `pre_dispatch()`. Subclasses may use parsed values here to configure themselves; the x264 front end uses this hook to seed the random generator and generate random command lines. The framework then parses arguments a **second** time because options added or state changed during the hook may require re-parsing.
4. If no positional arguments remain, print "no command specified", print help and return without invoking anything.
5. Take the first positional argument as the command name and the rest as arguments.
6. Validate the command exists. If not, print an error, print help and return.
7. Inspect the command's argument specification. The maximum argument count is the number of declared parameters minus the implicit `self`; the minimum is that maximum minus the number of defaulted parameters.
8. If too few or too many arguments were supplied, print the appropriate error plus the command's docstring and help, and return.
9. Otherwise call the command with the supplied arguments.

Error output goes to standard error; normal command output is produced by the command itself.

### 5.5 Extension pattern

A subclass overrides `_populate_parser`, calls the base implementation, adds an option group with its own callback options, and attaches that group to the parser. It may also override `pre_dispatch`. This is exactly how the x264-specific driver adds video selection, seed, product count, configure flags, YUV test selection and extra encoder flags.

## 6. Dependencies

- **Imports**: `inspect`, `sys`, `optparse.OptionParser`, `textwrap`, `types.MethodType`, and the package version constant.
- **Imported by**: `tools/digress/__init__.py.md` and front-end scripts.
- **Uses**: fixture methods marked by decorators defined in `tools/digress/testing.py.md`; those decorators attach the marker attribute that this module looks for.

## 7. Constants and tables

| Item | Value / meaning |
| --- | --- |
| Usage template | `usage: %prog [options] command [args]` |
| Help width | 80 columns minus the longest command name minus four. |
| Status/error stream | Help and errors go to standard error. |
| Case selection syntax | Comma-separated class names; no spaces are stripped by the base dispatcher. |

No numeric status constants here; those live in `tools/digress/constants.py.md`.

## 8. Performance notes

- Introspection happens once per dispatch and is negligible compared with test execution.
- Double argument parsing is intentional and cheap; it supports front ends that add options dynamically in `pre_dispatch`.
- The help wrapper walks all commands once; with a handful of commands this is trivial.

### 8.1 Relation to faster-kernel development

The `--cases` filter is the main accelerator during SIMD development: run only the cases affected by a kernel change instead of the full suite. `--flush` ensures that cached results from a previous revision are not reused, which is essential when comparing a candidate kernel against a baseline — a cached pass can otherwise hide a regression. For bisecting a performance or correctness regression, use the fixture's `bisect` command; its argument validation is performed here before any checkout occurs.

References: `tools/digress/testing.py.md` for the commands themselves and how caching interacts with `--flush`; `tools/checkasm.c.md` for the faster per-kernel loop that should precede a full fixture run.

## 9. References

- The source file itself, `tools/digress/cli.py`.
- Python documentation for `optparse`, `inspect.getargspec`, `inspect.getmembers` and `MethodType`.
- `tools/digress/testing.py.md` for the dispatchable fixture commands (`run`, `flush`, `bisect`, `compare`, `multicompare`, `list`).
- `tools/digress/__init__.py.md` for the convenience entry point.

## 10. Verification checklist

- A fixture method decorated as dispatchable appears in the help output with its docstring text.
- An undecorated fixture method does not appear as a command.
- Invoking a command with too few or too many arguments prints a diagnostic and does not run the command.
- An unknown command prints an error and help, and does not modify repository state.
- `--flush` sets the fixture flag before commands run.
- `--cases A,B` leaves only cases named A and B in the fixture's case list.
- `pre_dispatch` runs between the two parses; options added there are visible to the command.
- No command specified produces the documented error rather than an exception.

## 11. Open questions

- UNKNOWN: exact behaviour when a dispatchable method has `*args` or `**kwargs` in its signature; `inspect.getargspec` handling of variadic parameters is not documented in this file.
- UNKNOWN: whether `--cases` tolerates whitespace around commas; the base implementation does not strip it.
- UNKNOWN: any compatibility plan for newer Python versions where `inspect.getargspec` and `optparse` are deprecated; the source is Python 2 style.
