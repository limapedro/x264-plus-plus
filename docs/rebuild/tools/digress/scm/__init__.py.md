# `tools/digress/scm/__init__.py` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/digress/scm/__init__.py`.
- Role: package initialiser for the source-control backends used by the Digress framework.
- Marker: this file is a **dirty-side behavioural specification**; its tiny surface is described in our own words.

## 2. Purpose

The module makes `digress.scm` an importable package so front ends can select a backend module such as `digress.scm.git` or `digress.scm.dummy` and assign it to `Fixture.scm`. It contains no code beyond a docstring, deliberately leaving the backend interface entirely to the individual backend modules.

## 3. Public interface

No symbols are exported. The package's contract is structural: a backend module placed under `digress.scm` should expose the function names the fixture expects — `checkout`, `rev_parse`, `current_rev`, `current_branch`, `revisions`, `stash`, `unstash`, `bisect`, `dirty` — as module-level callables. The fixture invokes them as `self.scm.<name>(...)`.

## 4. State and data structures

No state. No registration mechanism and no default backend are defined here; selecting a backend is the front end's responsibility.

## 5. Algorithm / behaviour

On import, the Python package machinery executes the module, which defines only a documentation string describing the directory as "Source control backends for Digress". No side effects occur: no subprocesses, no filesystem access, no environment inspection.

Because no names are exported, `from digress.scm import *` imports nothing useful; callers must import a concrete submodule.

## 6. Dependencies

- **Imported by**: front-end scripts and `tools/digress/testing.py.md` indirectly, when a backend module is selected.
- **Imports**: none.
- **Sibling modules**: `tools/digress/scm/git.py.md` (real backend) and `tools/digress/scm/dummy.py.md` (stub backend).

## 7. Constants and tables

None.

## 8. Performance notes

Import cost is effectively zero. Extensibility note: adding a new backend requires only creating a module in this directory that implements the expected function names; no change to this initialiser or to the fixture is needed.

### 8.1 Relation to faster-kernel development

For kernel development it is useful to run the fixture against a dirty working tree (no backend operations) while iterating, then switch to the git backend for bisection once a regression is identified. The dummy backend can be used to exercise the framework itself without touching a real repository; see `tools/digress/scm/dummy.py.md`.

References: `tools/digress/testing.py.md` for the SCM interface actually consumed, `tools/digress/scm/git.py.md` for a complete implementation.

## 9. References

- The source file itself, `tools/digress/scm/__init__.py`.
- Python documentation on package initialisers.
- `tools/digress/testing.py.md`, `tools/digress/scm/git.py.md`, `tools/digress/scm/dummy.py.md`.

## 10. Verification checklist

- Importing `digress.scm` succeeds without side effects.
- Importing `digress.scm.git` and `digress.scm.dummy` works after importing the package.
- No backend is implicitly selected by the package.
- Adding a new module to the directory does not require editing this file.

## 11. Open questions

- UNKNOWN: whether a future version is intended to expose a registry or default backend; none exists here.
