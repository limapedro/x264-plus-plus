# `common/arm/cpu-a.S` — ARM CPU feature probes

**Dirty-side behavioural specification.** Source path: `common/arm/cpu-a.S`. Role: runtime probes for NEON instruction support and NEON-to-ARM transfer speed.

## 1. Purpose

ARM CPUs can hide optional features from the OS or emulate them in software. This file provides tiny assembly routines used by the generic CPU-detection code to confirm that NEON instructions execute and that NEON-to-core register transfers are fast enough to be worthwhile on the detected core.

## 2. Public interface

- `void cpu_neon_test( void )` — exported; executes one NEON instruction. If the CPU lacks NEON, this signals the operating system's undefined-instruction handler, which the detection code interprets as absence.
- `int cpu_fast_neon_mrc_test( void )` — exported; returns nonzero when transfers from NEON registers to ARM core registers are fast enough, and zero otherwise. The exported name is mangled by the naming layer in `asm.S`.
- `cpu_enable_armv7_counter` and `cpu_disable_armv7_counter` are file-local helpers. The enable routine returns 0 on success, 1 if counters were already enabled, and 9 if low-resolution counters were already enabled.

## 3. State and data structures

All routines operate on ARM coprocessor 15 (the performance-monitor unit) and NEON registers. No memory buffers are used. The enable helper saves the previous counter configuration implicitly through the caller's handling of its return code, and restores it only when the caller determines it had enabled the counters.

## 4. Algorithm / behaviour

**NEON presence probe.** The routine executes a single 128-bit integer add on NEON register `q0` and returns. It is deliberately written in gas because the file-wide `.fpu neon` directive would otherwise suppress an assembler diagnostic when the target architecture does not actually support NEON.

**Fast-transfer probe.**
1. The routine reads the performance monitor control register `PMNC`. If the user is not permitted to access counters (value zero in the user-access register), it returns nonzero immediately, meaning "transfer speed assumed acceptable" rather than failing the feature.
2. Otherwise it enables the cycle counter and selects full-resolution counting through the file-local helper, then performs a repeated loop containing eight NEON-to-core moves (`vmov.u32` from a NEON lane) per iteration.
3. It measures the cycle count before and after each batch. Batches taking more than 30 cycles per eight transfers are discarded as likely context switches.
4. It averages four valid batches and compares the mean against a threshold. The reference uses a right shift by 5 after accumulating four deltas, then checks whether the result is greater than 10; if so, it returns 0 (slow transfers), otherwise nonzero (fast transfers).
5. If the helper had enabled the counters, they are disabled before returning.

## 5. Dependencies

- Includes `common/arm/asm.S`.
- Called from the ARM CPU-detection code in `common/cpu.c`.
- Uses coprocessor 15 registers `c9,c12` (control and enable) and `c9,c13` (cycle count), plus `c9,c14` (user access).

## 6. Constants and tables

- Return codes 0, 1, and 9 for the counter-enable helper.
- Threshold values 30 cycles per eight transfers for rejecting noisy samples, 4 averaging iterations, and 10 cycles as the final fast/slow boundary.
- Bit masks 1 (counter enable), 8 (low-resolution counter), and `1 << 31` (cycle counter enable).

## 7. Performance notes

These routines run only once at startup. Their value is in guiding later dispatch: on cores where NEON-to-core transfers are slow, the encoder may prefer pure-NEON implementations over mixed scalar/vector ones. The measurement loop is carefully designed to be short and to exclude context-switch outliers.

## 8. References

- ARM Architecture Reference Manual, performance monitor unit registers.
- ARM NEON Programmer's Guide for `vmov` between NEON and core registers.
- Public x264 CPU-detection code for how these results map to `X264_CPU_NEON` and related flags.

## 9. Verification checklist

- `cpu_neon_test` returns normally on a NEON-capable CPU and traps on one without NEON.
- `cpu_fast_neon_mrc_test` returns nonzero when counters are inaccessible to user mode.
- The enable helper reports 1 or 9 when counters were already configured and does not restore them in that case.
- Measured cycle deltas are non-negative and the final result is 0 or nonzero as documented.
- Callee-saved registers are preserved.

## 10. Open questions

- `UNKNOWN`: the exact mapping from the returned "fast transfer" boolean to CPU feature flags in `common/cpu.c`.
- `UNKNOWN`: behaviour on 64-bit ARM, where these 32-bit probes may not be compiled at all.
