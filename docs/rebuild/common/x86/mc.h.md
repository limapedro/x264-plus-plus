# common/x86/mc.h — behavioural specification

**Role:** public C header for the x86 motion-compensation initialiser; the only symbol it exports is `x264_mc_init_mmx`.
**Marker:** dirty-side behavioural specification.

---

## 1. Header

- Source path: `common/x86/mc.h`
- Role: architecture-specific header included by `common/x86/mc-c.c` (and potentially by x86 glue elsewhere) to declare the CPU-feature-driven MC initialiser.
- Guard: `X264_X86_MC_H`.

---

## 2. Purpose

The generic MC interface (`common/mc.h`) exposes the `x264_mc_functions_t` table but not the architecture-specific initialiser. This tiny header provides the one declaration needed to plug the x86 kernels into that table, applying the bit-depth-specific symbol prefix via `x264_template`.

---

## 3. Public interface

```c
#define x264_mc_init_mmx x264_template(mc_init_mmx)
void x264_mc_init_mmx( uint32_t cpu, x264_mc_functions_t *pf );
```

- `x264_template` (defined in the global build configuration) prefixes the symbol with the current bit-depth convention — typically `x264_mc_init_mmx` for 8-bit builds and `x264_mc_init_mmx_<depth>` (or an equivalent mangled name) for high-depth builds. UNKNOWN the exact templating suffix without the build macros, but the mechanism is shared with all other x86 headers.
- Parameters:
  - `cpu`: the `X264_CPU_*` bitmask returned by `x264_cpu_detect()` (see `cpu-a.asm.md`). Only x86 bits are interpreted; irrelevant bits are ignored.
  - `pf`: pointer to the function table to fill. Must have been initialised with the C defaults first (by `x264_mc_init` in `common/mc.c`); this function overwrites entries selectively.
- Contract: returns nothing; never allocates; safe to call once at startup. Calling twice with different masks would leave stale faster functions from the first call (the cascade only overwrites, never clears) — UNKNOWN whether this is intended.

---

## 4. State and data structures

- No state of its own; the only data structure involved is `x264_mc_functions_t` from `common/mc.h` (documented in `mc-c.c.md`).
- The header itself contains no definitions, only the macro and prototype.

---

## 5. Algorithm / behaviour

Including this header makes the name `x264_mc_init_mmx` refer to the templated symbol and declares its signature. At link time the definition in `common/x86/mc-c.c` resolves it. The implementation (see `mc-c.c.md`) checks CPU flags in ascending feature order and assigns the corresponding assembly kernels into `pf`.

Important call-order invariant: `x264_mc_init_mmx` must be called *after* the portable C defaults are installed, because it deliberately relies on earlier assignments surviving when a higher feature is absent. A rebuild must preserve that ordering.

---

## 6. Dependencies

- Requires `x264_template` and `x264_mc_functions_t`/`uint32_t` to be defined — supplied by `common/common.h` and `common/mc.h`, which the including translation unit must have included first.
- Corresponds to `common/x86/mc-c.c`, which defines the function.
- Parallel headers exist for other architectures (`arm/mc.h`, `ppc/mc.h`, etc.) with the same pattern.

---

## 7. Constants and tables

None defined here. The CPU bit constants (`X264_CPU_MMX` … `X264_CPU_AVX512`, cacheline and slow-device modifiers) are in `x264.h`; the function-pointer tables are in `mc-c.c`.

---

## 8. Performance notes

This header has no runtime cost. Its significance is ensuring the correct bit-depth-templated symbol is linked, which matters because 8-bit and high-depth builds can coexist in the same library (two copies of the function with different names).

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):** not applicable to a header. The selected kernels' vectorisation behaviour is documented in `mc-c.c.md`, `mc-a.asm.md` and `mc-a2.asm.md`; the AVX-512 dispatch choices made through this entry point include masked-tail plane copies and wider avg kernels (Intel Intrinsics Guide).

---

## 9. References

- `common/mc.h` (generic MC interface) and `common/x86/mc-c.c` (definition) in the reference tree.
- `x264.h` for CPU flag definitions.
- Standard C header guard conventions.

---

## 10. Verification checklist

1. Compile a translation unit that includes `common/common.h`, `common/mc.h`, then this header, and confirm the prototype compiles with no implicit declaration warnings.
2. Link against the 8-bit and high-depth objects separately and verify each resolves `x264_mc_init_mmx` to the correct templated symbol (`nm`/`dumpbin`).
3. Call the function with `cpu=0` and confirm it returns without modifying `pf` (all MMX checks fail) — consistent with the reference implementation's early `if (!(cpu&X264_CPU_MMX)) return;`.
4. Confirm no additional symbols are exported from this header.

---

## 11. Open questions

- The exact expansion of `x264_template` for high-depth builds is UNKNOWN from this file alone (defined in the build system); the rebuild must obtain it from `common/common.h`/configure.
- Whether any other translation unit includes this header directly (rather than only `mc-c.c`) is UNKNOWN; no such use was observed in the assigned files.
