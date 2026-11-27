# cert/ — independent certification

This team measures and certifies; it does not implement or fix implementation code.

- Usage model: `spec/usage-model.md` (owned by specification, read-only here).
- Random test generation from that model.
- Checks: spec conformance, decodability (ffmpeg), quality/size envelope, speed ratio.
- Publishes results to `score.md` and machine-readable files in `cert/results/`.

No writes to `src/` or `spec/`. Defects are reported back to the implementation team.

Current status: harness being rebuilt for the clean-room candidate.
