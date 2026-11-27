# `extras/intel_dispatcher.h` — dirty-side behavioural specification

## 1. Header

- Source path: `extras/intel_dispatcher.h`.
- Role: a small compatibility shim that forces the Intel compiler's CPU dispatcher to initialise correctly when x264's own feature detection is used alongside Intel's runtime dispatch library.
- Marker: this file is a **dirty-side behavioural specification**; the logic is described in our own words.

## 2. Purpose

The Intel C++ compiler can generate multiple versions of a function and select one at runtime using an internal CPU feature indicator. That indicator is initialised with a minimal value until the dispatcher's initialisation routine runs. If x264 links against an Intel-compiled library that uses such dispatch, the indicator may remain uninitialised and dispatch wrongly. This header provides a helper that detects the uninitialised state and triggers proper vendor-independent initialisation.

## 3. Public interface

| Symbol | Contract |
| --- | --- |
| `__intel_cpu_feature_indicator` | External 64-bit unsigned integer holding the compiler's currently enabled feature bits. Declared with C linkage in the source context. |
| `__intel_cpu_feature_indicator_x` | External 64-bit unsigned integer used by the vendor-independent dispatcher. |
| `__intel_cpu_features_init_x()` | External function that performs vendor-independent CPU feature detection and populates the extended indicator. |
| `x264_intel_dispatcher_override()` | Static helper to call from x264 initialisation. Takes no arguments, returns nothing. |

The header is guarded by `X264_INTEL_DISPATCHER_H`.

## 4. State and data structures

No private state. The two indicator variables are process-global and owned by the Intel runtime. The helper reads and writes them; all other state lives inside the Intel initialisation routine.

The feature bits use the `_FEATURE_*` constants from Intel's `immintrin.h`, which is why the header mentions that header even though it does not include it here.

## 5. Algorithm / behaviour

`x264_intel_dispatcher_override()` performs the following:

1. Read the primary indicator. If any bit other than the least significant one is set, assume the dispatcher has already been initialised and return immediately without side effects.
2. Otherwise zero both the primary and extended indicators.
3. Call the vendor-independent initialisation routine.
4. Copy the resulting extended indicator into the primary indicator so subsequent dispatch uses the detected feature set.

Behavioural consequences:

- The test is deliberately conservative: a non-zero bit above bit zero means "already initialised", and the function does nothing. This avoids clobbering a dispatcher state that another library has already configured.
- The sentinel condition is a particular minimal state of the indicator; the source does not enumerate which bits are set in that state, only that the condition means it is safe to reinitialise.
- Calling the helper more than once is harmless: after a successful initialisation the indicator has additional bits set, so later calls return early.
- The routine is intended to be invoked during x264 initialisation on builds using Intel-compiled components; the header itself does not call it.

## 6. Dependencies

- **External symbols**: the two indicator variables and the initialisation function, provided by the Intel compiler runtime / dispatch library.
- **Consumers**: x264 initialisation code on Intel-compiler builds, which includes this header and calls the static helper.
- **Related**: `common/x86/cpu.c` (CPU detection) is conceptually related, but this shim addresses a different mechanism — the Intel compiler's own dispatcher, not x264's function-pointer init.

## 7. Constants and tables

| Item | Meaning |
| --- | --- |
| Bit mask `~1ULL` | Test for any feature bit above the least significant one; if set, skip reinitialisation. |
| Zero | Used to clear both indicators before reinitialisation. |
| Feature constants | Defined by `_FEATURE_*` macros in Intel's `immintrin.h`. |

## 8. Performance notes

- The helper runs once at initialisation; its cost is negligible.
- Avoiding duplicate dispatcher initialisation matters because the routine probes CPU features and may update global function pointers; doing it twice is wasteful and could conflict with another library's state.
- Extensibility tip: if adding a new x86 ISA tier, ensure the Intel dispatcher's feature bits cover it; otherwise Intel-compiled code may dispatch to an older variant than x264's own detection would choose.

### 8.1 Relation to faster-kernel development

When benchmarking a new SIMD kernel on a machine with Intel-compiled libraries, inconsistent dispatch can make results non-reproducible: one run may use an older generated variant. Call this override during initialisation, then verify which kernel is actually selected using the conformance tool's per-ISA flag sweep (`tools/checkasm.c.md`, section describing `check_all_flags`). Only compare benchmark numbers after confirming both the x264 dispatch and the Intel dispatch chose the expected tier.

References: `tools/checkasm.c.md` for ISA-tier reporting; Intel compiler documentation on automatic dispatch and `__intel_cpu_feature_indicator`.

## 9. References

- The source file itself, `extras/intel_dispatcher.h`, carrying the x264 project copyright and author attribution.
- Intel C++ Compiler documentation on CPU dispatch and feature indicators.
- Intel Intrinsics Guide / `immintrin.h` for `_FEATURE_*` constants.
- `tools/checkasm.c.md` for x264's own feature-tier verification.

## 10. Verification checklist

- In an Intel-compiled build, the indicator is zero-like at startup and becomes populated after calling the override.
- Calling the override a second time does not change the indicators.
- If the indicator already has a bit above bit zero set, the function leaves both indicators untouched.
- The initialisation routine is called exactly once in the uninitialised case.
- The header compiles in C and C++ translation units with the Intel runtime symbols available.

## 11. Open questions

- UNKNOWN: exact meaning of the least significant indicator bit; the source treats it as ignorable when deciding whether initialisation is needed.
- UNKNOWN: which x264 build configurations actually include and call this helper; the header does not say.
- UNKNOWN: behaviour if the Intel runtime does not export the extended indicator on a given compiler version; link failure is the likely outcome.
