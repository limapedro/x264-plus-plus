# Project decisions

| Date | Decision | Rationale |
| --- | --- | --- |
| 2026-11-27 | Repo name `x264-plus-plus` (brand "x264++") | GitHub allows only letters/digits/`.`/`-`/`_`; `+` is invalid. |
| 2026-11-27 | Public repository under `limapedro` | User request. |
| 2026-11-27 | Speed-first: bitstream bytes need not match the reference | User choice after being told 2x byte-exact on every preset is not credible. Validity and a documented quality envelope replace byte identity. |
| 2026-11-27 | Clean-room reimplementation, not a fork | User correction. Reference source removed from working tree and repository history; only a compiled black-box oracle remains. |
| 2026-11-27 | IBM Cleanroom process (boxes, increments, verification, independent statistical certification) | User asked to follow the IBM case/material. See `CLEANROOM.md`. |
| 2026-11-27 | Local Windows machine is the first benchmark target | MSYS2 MinGW GCC 16.2 + NASM available; i5-12400F (AVX2, no AVX-512). |
| 2026-11-27 | `score.md` is owned by certification, never by implementation | Prevents optimistic self-reporting. |

## Withdrawn

- The initial fork-and-optimize iteration and all of its measurements were discarded when the clean-room direction was established.
