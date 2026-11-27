# Specifications (the clean side's only input)

Everything in this directory is a **black-box specification**: it describes observable behaviour (CLI, bitstream syntax, quality envelope, usage distributions) without copying or describing the reference implementation's source.

The specification team writes these files from:

- probing `oracle/x264.exe` as a black box (inputs → outputs),
- the published ITU-T H.264 / ISO IEC 14496-10 standard and public documentation,
- public, source-independent descriptions of encoder behaviour.

Implementation agents: read this directory and `src/` only. See `docs/process/CLEANROOM.md` for the wall rules.

## Inventory

- `cli-blackbox.md` — CLI arguments, defaults, input/output formats, log format, exit codes.
- `h264-syntax.md` — the normative bitstream surface the encoder must produce (SPS, PPS, slice headers, macroblock layer, CAVLC/CABAC, SEI).
- `usage-model.md` — Markov usage model for statistical certification.
- `quality-envelope.md` — accepted quality/speed contracts per "preset" concept.

Stub files are intentional placeholders until the specification team completes them.
