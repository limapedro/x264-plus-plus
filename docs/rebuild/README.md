# Rebuild specifications — one document per source file

This tree mirrors the **exact file skeleton** of the reference encoder. For every source file `P`, there is a specification document at `P.md` (e.g. `common/dct.c` → `common/dct.c.md`, `common/x86/x86inc.asm` → `common/x86/x86inc.asm.md`).

These documents are **dirty-side** reverse-engineering artifacts: written by the specification team from the reference source and public literature. They are the input to the clean implementation team. They must describe *behaviour and structure*, never reproduce the reference's code.

> Copyright rule: no verbatim source, no near-verbatim pseudocode, no copied comments. Function/type/macro names, numeric constants, bit layouts, and algorithmic ideas are facts and are allowed. Write every explanation in your own words.

## Mandatory document skeleton (every file, no exceptions)

1. **Header** — source path, role in one sentence, "dirty-side behavioural specification" marker.
2. **Purpose** — what problem this file solves and why it exists.
3. **Public interface** — exported functions/types/constants with signatures and contracts; semantics, not code.
4. **State and data structures** — fields, layouts, invariants, ownership/lifetime.
5. **Algorithm / behaviour** — detailed step-by-step description, conditions, equations, rounding, edge cases, ordering. This is the core section; be exhaustive.
6. **Dependencies** — what it calls, what calls it, shared globals/config.
7. **Constants and tables** — values, derivation, why those values; never copy table source text.
8. **Performance notes** — hotness by preset, cache/branch behaviour, SIMD status, and concrete, referenced optimization tips ("how to make it faster").
9. **References** — public standards, papers, or documentation supporting the description.
10. **Verification checklist** — black-box/observable checks an independent implementation can use to confirm equivalence.
11. **Open questions** — unknown or uncertain behaviour.

## Depth target

Core algorithm files (DCT, ME, analyse, quant, prediction, CABAC, bitstream): 250–500 lines each, with equations and decision conditions. Infrastructure/CLI files: 120–300 lines. Trivial headers: at least 60 lines with full interface semantics. If something is uncertain, write `UNKNOWN` — do not guess.

## Wall rules

- Writers: specification (dirty) agents only. Reference source: `E:\_x264_dirty_stash\x264-source`.
- Readers: clean implementation agents may read this tree and `spec/`, but must never read the stashed source or `oracle/`.
- No writes to `src/`, `spec/`, `cert/`, or `score.md` from documentation agents.
