# src/ — clean implementation

Original source for x264++. Written only from `spec/` and public standards. No reference encoder source is consulted.

## Wall rules (enforced by agent instructions)

- Read: `spec/`, this directory, public H.264 documentation.
- Do not read: `oracle/`, `cert/`, `E:\_x264_dirty_stash\`, or any dirty research notes.
- Questions about missing specification go to `src/verification/questions.md`.

## Structure

```
src/
  core/        C implementation and headers
  x86/         hand-written SIMD (NASM)
  cli/         command-line front end
  verification/  refinement/verification arguments (IBM box-structure reviews)
  Makefile     out-of-tree-friendly build
```

## Increments

See `docs/process/CLEANROOM.md`. Do not implement an increment before its black box and state box specifications exist in `spec/`.
