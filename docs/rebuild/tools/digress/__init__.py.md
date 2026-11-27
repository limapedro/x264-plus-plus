# `tools/digress/__init__.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/__init__.py`.
- Role: the package initialiser for the Digress automated regression/unit-test suite; it publishes the version string and the single convenience entry point used by front-end scripts.
- Marker: this file is a **dirty-side behavioural specification**; behaviour is described in our own words.

## 2. Purpose

The module turns the `digress` directory into an importable Python package and provides the minimal public surface that a front-end test script needs: a version constant and a helper that constructs the command-line dispatcher for a supplied fixture. Keeping this surface small means front ends do not need to know the internal class layout of the framework.

## 3. Public interface

| Symbol | Kind | Contract |
| --- | --- | --- |
| `digress` module | Package | Importable as `digress`; its submodules provide CLI, comparers, constants, errors, testing core and SCM backends. |
| `__version__` | String constant | Version identifier of the framework, reported by the CLI's version option. |
| `digress(fixture)` | Function | Accepts a fixture object (an instance of a `Fixture` subclass), constructs the CLI `Dispatcher` around it and immediately hands control to that dispatcher's `dispatch()` method. |

The helper does not return a value to the caller in normal operation: it delegates to a command-line loop that prints results to standard output/error and may terminate via the front end's own error paths.

## 4. State and data structures

No persistent state of its own. The only module-level data is the version string. All mutable state lives in the `Dispatcher` instance that is created inside the helper and in the fixture it wraps:

- The dispatcher stores a reference to the fixture and, during population, writes itself back into the fixture as `fixture.dispatcher`, giving test cases access to parsed command-line settings.
- The fixture owns the list of registered cases, the SCM backend module, the data-store path and the `flush_before` flag.

## 5. Algorithm / behaviour

On import:

1. The module docstring identifies the package as an automated regression/unit testing suite.
2. The version constant is assigned.
3. The convenience function is defined but its body is deferred: imports of `digress.cli.Dispatcher` happen only when the function is called, not at package import time. This avoids a circular import, because the CLI module imports the version constant from this module.

On calling `digress(fixture)`:

1. Resolve `Dispatcher` from `digress.cli` at call time.
2. Instantiate it with the caller's fixture object.
3. Invoke `dispatch()`, which parses options, discovers dispatchable methods on the fixture, validates the command name and argument count, and invokes the selected command.

Failure behaviour:

- If the fixture object is not of the expected shape, the failure occurs inside `Dispatcher`, not here. UNKNOWN: any explicit validation in this module; there is none.
- Import-time failures propagate normally.

## 6. Dependencies

- **Imports at call time**: `digress.cli.Dispatcher`.
- **Imported by**: front-end scripts such as the x264-specific test driver, which typically call `Dispatcher(fixture).dispatch()` directly rather than through this helper, or call `digress(fixture)`.
- **Version consumer**: `tools/digress/cli.py.md`, which reads `__version__` to build the version option text.
- Sibling modules (`testing.py`, `comparers.py`, `errors.py`, `constants.py`, `scm/`) are not imported here.

## 7. Constants and tables

| Constant | Value | Notes |
| --- | --- | --- |
| `__version__` | A short dotted version string identifying the framework generation. | Reported verbatim by the CLI. Exact literal is in the source; treat it as a fact, not as a behavioural rule. |

No other constants.

## 8. Performance notes

- Import cost is negligible: the only import at package load is nothing beyond the interpreter's own machinery; the CLI import is deliberately deferred.
- Because the dispatcher construction and command execution happen synchronously, the helper's overhead is irrelevant compared with compiling and running the encoder in the test cases.
- Extensibility tip: add new fixture commands by decorating methods with the dispatchable decorator described in `tools/digress/cli.py.md`; no change to this initialiser is required.

### 8.1 Relation to faster-kernel development

For SIMD work, this helper is the one-line entry point that makes the regression framework available from a standalone script. A practical development loop is: run the per-kernel conformance suite first (see `tools/checkasm.c.md`), then invoke the fixture through this helper with a filtered case list (`--cases`) so only affected regression cases run; finally use the bisect command to locate the commit that introduced a failure.

References: `tools/digress/cli.py.md` for the dispatcher, `tools/digress/testing.py.md` for the fixture contract.

## 9. References

- The source file itself, `tools/digress/__init__.py`.
- Python package documentation for `__init__.py` semantics and import-time evaluation.
- `tools/digress/cli.py.md`, `tools/digress/testing.py.md` for the modules referenced above.

## 10. Verification checklist

- Importing the package does not fail due to a circular import.
- `__version__` is a string and matches the version text printed by the CLI.
- Calling the helper with a valid fixture runs exactly one command chosen from the command line.
- Calling the helper with no command produces the CLI's "no command specified" error path, not a Python traceback from this module.
- The helper does not mutate the fixture beyond whatever `Dispatcher.__init__` and `dispatch()` do.

## 11. Open questions

- UNKNOWN: whether any front end actually uses this helper versus constructing `Dispatcher` directly; both patterns appear plausible from the source tree.
- UNKNOWN: any intended future expansion of the package-level API; none is declared.
