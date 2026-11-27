# `common/aarch64/mc.h` — AArch64 motion-compensation dispatcher declaration

> Dirty-side behavioural specification (reverse-engineered from the reference encoder; describes behaviour, not code).

## 1. Header

- Source path: `common/aarch64/mc.h`
- Role: declares the single entry point that installs AArch64 motion-compensation kernels into the generic function table.
- This is a **dirty-side behavioural specification** artifact for the clean-room reimplementation.

## 2. Purpose

The generic MC code includes this header to obtain the prototype for the architecture initializer. Keeping all other kernel declarations in the C file (`mc-c.c`) hides implementation details while exposing the one symbol the generic dispatcher needs.

## 3. Public interface

```
void x264_mc_init_aarch64( uint32_t cpu, x264_mc_functions_t *pf );
```

- `cpu`: bit mask of detected CPU features (`X264_CPU_NEON`, `X264_CPU_SVE`, etc.).
- `pf`: pointer to the function table to be populated.
- Contract: for every field, either leave the generic C implementation in place or replace it with an AArch64 implementation whose behaviour is bit-identical. Higher feature tiers override lower ones.

The symbol is wrapped by the build's `x264_template` macro, so the externally visible name is the architecture-templated one.

## 4. State and data structures

- `x264_mc_functions_t`: the generic MC function-pointer table (defined in common headers). Fields include `prefetch_fenc_420/422`, `prefetch_ref`, `mc_luma`, `get_ref`, `mc_chroma`, `copy`, `avg`, `weight`, `offsetadd`, `offsetsub`, `weight_cache`, `hpel_filter`, `frame_init_lowres_core`, `integral_init4h/8h/4v/8v`, plane-copy functions, chroma load/store, and MB-tree functions.
- No state owned by this header.

## 5. Algorithm / behaviour

No computation. The header's semantic content is the guarantee that exactly one initializer exists for AArch64 and that it takes the feature mask and table pointer in that order. The implementation's tiered assignment order is documented in `mc-c.c.md`.

## 6. Dependencies

- Included by `common/mc.c`.
- Uses `x264_mc_functions_t` from common headers.
- Implementation in `mc-c.c`.

## 7. Constants and tables

None. Feature-flag constants are defined in the common CPU-detection headers.

## 8. Performance notes

Declaration-only. For a clean-room port the important design decision is to keep the initializer idempotent and feature-driven; no runtime branches should be added in the hot kernel dispatch path beyond the existing function-pointer indirection. Public references: x264 CPU detection and MC dispatcher architecture.

## 9. References

- x264 `common/mc.c` (generic dispatcher).
- `docs/rebuild/common/aarch64/mc-c.c.md`.
- ARM CPU feature detection documentation (`HWCAP_NEON`, `HWCAP_SVE`).

## 10. Verification checklist

- The header compiles standalone with the common headers included.
- Exactly one definition of the templated symbol exists across the build.
- Calling the initializer with a zero feature mask leaves the table unchanged (verify against a pre-filled sentinel table).

## 11. Open questions

- UNKNOWN: whether the initializer is called more than once with different masks in the reference build.
