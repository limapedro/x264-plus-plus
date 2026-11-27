# x264++ x86 assembly ABI contract

This directory contains **original** hand-written SIMD kernels for the clean implementation. No reference assembly is copied; kernels are written from the behavioural rebuild specs in `docs/rebuild/common/x86/` and public Intel/AMD ISA documentation.

## Platform

- Target: Windows x86-64 (MSYS2 MinGW64), NASM 2.16 `win64` object format.
- CPU target for the first delivery: AVX2 (i5-12400F, no AVX-512). SSE2/SSSE3/AVX fallbacks are added later.
- Build: objects linked into the candidate CLI binary by `src/Makefile`.

## Calling convention

- Standard Microsoft x64 ABI: integer/pointer arguments in `RCX`, `RDX`, `R8`, `R9`; further arguments on the stack. Caller reserves 32 bytes of shadow space and aligns `RSP` to 16 at the call site.
- Callee-saved: `RBX`, `RBP`, `RSI`, `RDI`, `R12`–`R15`, `XMM6`–`XMM15`.
- Return value in `RAX` (integer/pointer) or `XMM0` (scalar/metric).
- Every exported symbol is prefixed `x264pp_` and suffixed with its ISA tier, e.g. `x264pp_sad_16x16_avx2`.
- Each kernel also exports a dispatch function without the ISA suffix that selects the best available implementation at runtime using the CPU feature flags supplied by the C core.

## Data layout

- All source pointers are byte addresses; strides are in bytes unless the kernel name says otherwise.
- Luma/pixel samples are unsigned 8-bit; residuals are signed 16-bit.
- Buffers used by AVX2 kernels must be 32-byte aligned. Kernels must handle 16-byte-aligned fallbacks where documented.
- Intermediate sums use 16-bit lanes with explicit saturation rules as specified in the per-file rebuild docs; never rely on implicit float conversion.

## Verification

Developer agents may compile-check only. Kernel equivalence is established by the independent certification team using the `cert/` harness and ffmpeg-decoded output, plus differential tests against the behavioural specifications. No developer may run the reference encoder.

## Kernel inventory (planned)

| Kernel group | Source spec | Priority |
| --- | --- | --- |
| Macro framework (`x86inc.asm`, `x86util.asm`) | `docs/rebuild/common/x86/x86inc.asm.md`, `x86util.asm.md` | P0 |
| SAD/SATD metrics | `sad-a.asm.md`, `pixel-a.asm.md` | P0 |
| 4x4/8x8 DCT + Hadamard DC | `dct-64.asm.md`, `dct-a.asm.md` | P0 |
| Intra prediction | `predict-a.asm.md`, `predict-c.c.md` | P1 |
| Quant/dequant | `quant-a.asm.md`, `quant.h.md` | P1 |
| Deblocking | `deblock-a.asm.md` | P2 |
| Bitstream/CAVLC output | `bitstream-a.asm.md`, `cabac-a.asm.md` | P2 |
