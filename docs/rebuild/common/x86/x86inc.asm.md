# common/x86/x86inc.asm — behavioural specification

**Role:** NASM/YASM preprocessor abstraction layer ("x264asm") that lets one assembly source compile into many calling conventions and SIMD widths from a single description.
**Marker:** dirty-side behavioural specification (reverse-engineered from the reference source and public ISA documentation; no code is reproduced).

---

## 1. Header

- Source path: `common/x86/x86inc.asm`
- Role: macro-only include file (no executable code of its own) that defines the symbol naming, register naming, stack/ABI prologue–epilogue machinery, CPU-feature gating, dispatch suffixes and VEX/EVEX instruction abstraction used by every other x86 assembly file in the encoder.
- It is licensed separately (ISC-like, "x264asm") from the rest of the encoder, but for the rebuild it is infrastructure that must be reproduced behaviourally.

---

## 2. Purpose

Writing the same DSP kernel for 32-bit x86, Windows x64, Unix x64, and for MMX/SSE/AVX/AVX-512 widths by hand would multiply the source by the product of those choices. This file solves that by:

1. Defining a *virtual register file* (`r0..r14`, `m0..m31`) whose physical mapping is decided at assembly time from the target ABI and the selected SIMD width.
2. Generating prologue/epilogue code that loads arguments into those virtual registers, saves callee-saved state, aligns the stack, and emits `vzeroupper` where required.
3. Appending CPU-feature suffixes to symbol names (`_mmx2`, `_sse2`, `_avx2`, `_avx512`, `_avx512icl`, …) so that one macro invocation emits many independently callable variants, and making `call` automatically target the right variant.
4. Providing an instruction abstraction that emits the shortest legal encoding (legacy, VEX or EVEX) for the selected feature level, and that refuses to assemble ISA use not permitted by the current dispatch level.

It exists so kernel authors write architecture-independent code once and get all variants.

---

## 3. Public interface

All "public" here are preprocessor macros/symbols consumed by other `.asm` files.

### 3.1 Configuration macros (may be predefined by the including file)
- `private_prefix` (default `x264`) and `public_prefix` (default = `private_prefix`): name roots for mangling.
- `STACK_ALIGNMENT`: 16 on x86-64, 4 on x86-32 unless overridden.
- `WIN64`, `UNIX64`: derived from `__OUTPUT_FORMAT__` (`win32`/`win64`/`x64` → WIN64; everything else on x86-64 → UNIX64). Both are 0 on x86-32.
- `FORMAT_ELF`, `FORMAT_MACHO`: derived from output format (`elf`, `elf32`, `elf64` / `macho`, `macho32`, `macho64`).
- `PREFIX`: if defined, mangle prefixes an extra underscore (Mach-O style).
- `FORCE_VEX_ENCODING` (default 0): makes XMM functions use VEX encodings even without AVX.
- `PIC`: always 1 on x86-64; 0 on 32-bit Windows; configurable otherwise. x86-64 also sets NASM `default rel`.
- `BIT_DEPTH` and SIMD width are provided by `x86util.asm`, not here; this file reads `mmsize`, `avx_enabled`, `cpuflags` etc. that `INIT_*` sets.

### 3.2 Function definition
- `cglobal name[, prologue args...]` — define a private (hidden) global function. `cvisible` defines a public one. `cglobal_internal` is the shared implementation.
- `cglobal_label label` — turn a local label into a correctly mangled global symbol.
- `cextern name` — declare an external private-prefixed symbol. `cextern_naked name` — declare an external symbol without the prefix (for library calls).
- `const name, data...` — define a global hidden data object under the private prefix.
- `SECTION_RODATA [align]` — put data in `.rdata` (Windows) or `.rodata` (Else), default alignment 16.

### 3.3 Function body helpers
- `PROLOGUE nargs, nregs, nxmm[, stack_size][, arg names...]` — load args, save callee-saved registers, allocate stack. `cglobal` may take the same parameters directly.
- `RET`, `REP_RET`, `TAIL_CALL target[, nonadjacent]`.
- `DECLARE_REG_TMP list...` — alias `t0`, `t1`, … to chosen `rN` registers (for non-trivial allocations).
- `DEFINE_ARGS names...` — rebind the symbolic argument names after register reuse.
- `PUSH`, `POP`, `SUB`, `ADD` — instruction wrappers that keep the assembler-time `stack_offset` bookkeeping consistent.
- `LEA dst, src` — position-independent address computation on 32-bit PIC.
- `REPX {operation}, args...` — repeat an operation over a list of operands.
- `movifnidn`, `movsxdifnidn`, `ASSERT`.

### 3.4 Register naming contract
For argument/temporary index `N`: `rN` (native width), `rNq` (64-bit), `rNd` (32-bit), `rNw` (16-bit), `rNb` (8-bit), `rNh` (high byte of the 16-bit word where applicable), `rNm` (original stack/register slot, 32-bit memory form), `rNmp` (native-size form of `rNm`). `tN` and `tNq/d/w/h/b` are the same for aliased temporaries.

### 3.5 Feature/dispatch macros
- `cpuflags_<feature>` bit constants, `cpuflag(x)`, `notcpuflag(x)`.
- `INIT_CPUFLAGS features...` — sets `SUFFIX`, `cpuname`, `cpuflags`, and move-token aliases.
- `INIT_MMX [features...]`, `INIT_XMM`, `INIT_YMM`, `INIT_ZMM` — select SIMD width and register file.
- `RESET_MM_PERMUTATION`, `PERMUTE pairs...`, `SWAP regs...`, `SAVE_MM_PERMUTATION`, `LOAD_MM_PERMUTATION`.
- `call target` — append the current `SUFFIX` when the plain target is unknown but the suffixed one is; also loads a saved register permutation.
- `AVX_INSTR`, `GPR_INSTR`, `FMA_INSTR`, `FMA4_INSTR`, `EVEX_INSTR`, `q000..q3333` shuffle constants.

---

## 4. State and data structures

The file maintains assembler-time state, not runtime state:

- **Symbol state:** `private_prefix`, `public_prefix`, `mangle`, `SUFFIX`, `cpuname`, `current_function`, `current_function_section`, `cglobaled_<name>` (value 1 = private global, 2 = `cextern`, 3 = `cextern_naked`).
- **Stack state:** `stack_offset` (distance from the return address to the current stack pointer), `stack_size` (freely usable bytes), `stack_size_padded` (including alignment/padding and callee-saved XMM area), `rstk` (the register used as the stack pointer, normally `rsp`), `rstkm` (saved original `rsp` when needed), `xmm_regs_used`, `regs_used`, `num_args`, `gprsize`.
- **Register-file state:** `mmsize` (8/16/32/64), `mmtype` (`mm`/`xmm`/`ymm`/`zmm`), `num_mmregs` (8, 16 or 32), `m0..m31` symbolic names, `nn<name>` reverse mapping (register number of a symbolic name), `sizeof<reg>`, `regnumof<reg>`, `avx_enabled`.
- **Feature state:** `cpuflags` bit mask. `required_stack_alignment = (mmsize+15) & ~15`; `vzeroupper_required` is true when `mmsize > 16` and (32-bit, or more than 16 vector registers used, or the target lacks AVX-512 — i.e. upper YMM state must be cleared); `high_mm_regs = 16` when AVX-512 is enabled, else 0.
- **Windows stack probing:** `STACK_PROBE_SIZE` is 8192 on Win64, 4096 on Win32, 0 elsewhere; `PROBE_STACK` touches guard pages for large allocations.
- **Instruction metadata:** each `AVX_INSTR` declaration records (minimum ISA, integer/float flag, 3- or 4-operand emulation, commutativity). `GPR_INSTR` records a minimum ISA for BMI/SSE4.2 scalar instructions.

---

## 5. Algorithm / behaviour

### 5.1 ABI and register mapping
Three prologue implementations are selected by `WIN64`, `ARCH_X86_64`, else 32-bit:

- **Win64:** argument registers rcx, rdx, r8, r9, then r10, r11, rax, rdi, rsi, rbx, rbp, r14, r15, r12, r13 drawn from the stack (offsets 40,48,56,…). Callee-saved GPRs pushed when `regs_used` exceeds 7. XMM6/7 are stored into the 32-byte shadow space; XMM8..15 (or 16..31 with AVX-512) are spilled into additional allocated stack. `WIN64_SPILL_XMM`, `WIN64_PUSH_XMM`, `WIN64_RESTORE_XMM` implement this. A 32-byte shadow space is always included in any non-trivial stack allocation.
- **Unix x64:** argument registers rdi, rsi, rdx, rcx, r8, r9, then rax, r10, r11, rbx, rbp, r14, r15, r12, r13 (stack offsets 8,16,24,…). Callee-saved GPRs (rbx, rbp, r12–r15) pushed when needed. `rax` is reserved as a vararg hidden argument (number of vector registers) — `SETUP_STACK_POINTER` keeps `regs_used >= 8` on Unix64 to avoid clobbering it.
- **x86-32:** arguments live on the stack (offsets 4,8,12,…) except the first few loaded into eax, ecx, edx, ebx, esi, edi, ebp; `regs_used` and `num_args` are clamped to 7. `sp` is aliased as `rsp`.

`PROLOGUE` asserts `regs_used >= num_args` and `regs_used <= 15` (7 on 32-bit), pushes callee-saved registers, allocates stack, loads later arguments from their memory slots into registers, then applies `DEFINE_ARGS` to bind symbolic names.

### 5.2 Stack allocation
`ALLOC_STACK(size, nxmm)` resets state, records XMM usage, and:
- If the required alignment is no larger than the known ABI alignment, it keeps the existing alignment by padding the allocation with `(-pad-stack_offset-gprsize) & (STACK_ALIGNMENT-1)`.
- Otherwise it reserves a GPR (via `SETUP_STACK_POINTER`, avoiding `rax`/`rax`-as-return and protecting argument registers), copies `rsp` into that register (`rstk`), and `and`s `rsp` with `~(required_alignment-1)` before subtracting the padded size. The original pointer is kept in `rstk` or stored just above the allocated frame (`rstkm`) so `RET` restores it in one instruction.
- Negative `size` requests force storing `rsp` on the stack instead of keeping it in a register.

`RET` adds back the padded size or restores `rsp` from `rstkm`, pops callee-saved GPRs, restores XMM6/7 and XMM8+, emits `vzeroupper` when required, and uses `AUTO_REP_RET`. `REP_RET` prefers a plain `RET` when there is an epilogue or the CPU is SSSE3-or-better; otherwise it uses the 2-byte `rep ret` to avoid the old AMD "ret after branch" stall. `BRANCH_INSTR` defines conditional-jump macro variants that record branch addresses for `AUTO_REP_RET`.

### 5.3 Symbol mangling and dispatch
`cglobal`/`cvisible` append `SUFFIX` to the supplied name, then mangle it as `prefix _ name`. For ELF, private symbols are emitted as `global name:function hidden`; for Mach-O with `HAVE_PRIVATE_EXTERN` they are `private_extern`; otherwise plain `global`. Functions are aligned to `function_align` (16) and their state is reset. `cextern` marks targets so `call` can suffix them; `cglobal_label` exports a labelled entry point. A `.skip_prologue` companion label is created for functions that need to bypass a prologue (used by cacheline-split dispatchers). ELF output adds a `.note.GNU-stack` non-exec-stack note and, on x86-64 with a new enough NASM, CET properties marking the object shadow-stack compatible.

`INIT_CPUFLAGS` builds `cpuname` by concatenating feature names with `_`, sets `SUFFIX` to `_cpuname`, ORs the feature bit masks, and defines move aliases: with AVX, `mova/movu` map to `vmovdqa/vmovdqu`; otherwise (and for 16-byte non-SSE2 or 32-byte non-AVX2) legacy forms; `cpuflag(aligned)` makes `movu` an aligned move; SSE3-without-SSSE3 uses `lddqu` for `movu`. It also chooses NASM `ALIGNMODE p6` vs `nop` based on SSE2 availability.

### 5.4 Register-files and widths
`DEFINE_MMREGS` defines `m0..mN` as physical `mm/xmm/ymm/zmm` registers: 8 on 32-bit or MMX; 16 on x86-64 with mmsize>=16; 32 on x86-64 with AVX-512 or mmsize 64. `INIT_MMX` sets mmsize 8 and MMX move tokens; `INIT_XMM` sets 16 and adds broadcast-width tokens (`bcstw`, `bcstd`, `bcstq`); `INIT_YMM`/`INIT_ZMM` set 32/64 and drop `movh`. On AVX-512, `AVX512_MM_PERMUTATION` swaps low and high halves so registers 16–31 are preferred, avoiding `vzeroupper`.

`PERMUTE`/`SWAP` exchange symbolic register *names* (not contents), so the assembler propagates a permutation through subsequent instructions at zero runtime cost. `SAVE_MM_PERMUTATION`/`LOAD_MM_PERMUTATION` let a callee return values in "logical" `mN` registers despite having permuted them.

`call target` chooses the suffixed symbol if the plain name is unknown and the suffixed one is known, adds a PLT relocation for ELF x86-64 external calls, a GOT indirection for PIC 32-bit `cextern_naked` calls, and reloads any saved permutation.

### 5.5 Instruction abstraction
`RUN_AVX_INSTR` is the core. Given an instruction mnemonic, a minimum ISA, an integer/float flag, an emulation mode, commutativity, and operands, it:
1. Determines the register width from the operands (or `mmsize`).
2. Chooses the VEX form (`v` prefix) when `avx_enabled` and width >= 16, else legacy. It validates that every chosen instruction is legal at the current `cpuflags` level (emitting an assembler error otherwise), including special cases for `pextrw` with memory operands.
3. When a 3- or 4-operand form must be emulated with legacy 2-operand instructions, it emits a separate move and, for commutative operations, swaps operands so the memory operand occupies the position the legacy form permits; it also swaps operands to reduce VEX byte count when a high register is involved.
4. For EVEX-capable instructions it prefers the shorter VEX encoding unless a register >=16, a >32-byte operand, or an explicit preference forces EVEX.

`AVX_INSTR` declares roughly 180 SIMD mnemonics with their minimum feature level and flags; `GPR_INSTR` declares scalar BMI/BMI2/SSE4.2 instructions with feature checks; `FMA_INSTR` gives XOP-based 4-operand multiply-accumulate emulation; `FMA4_INSTR` unifies FMA3 and FMA4 into a 4-operand interface, rewriting FMA3 operand order depending on which operand aliases the destination; `EVEX_INSTR` maps VEX mnemonics to EVEX equivalents (broadcasts, extracts, inserts, `vpandd`, `vrcp14ps`, etc.). A base-4 table (`q000`…`q3333`) converts immediate shuffle specifications into the decimal digits NASM expects.

### 5.6 Initialization order
A typical source includes this file, includes `x86util.asm`, emits constants with `const`, then enters feature sections: `INIT_MMX mmx2` … `INIT_XMM sse2` … `INIT_YMM avx2` … `INIT_ZMM avx512`, each followed by `cglobal name, args, regs, xmms` definitions. The current `SUFFIX` is applied automatically, so `pixel_sad_16x16` defined under `INIT_XMM sse2` becomes `x264_pixel_sad_16x16_sse2`.

---

## 6. Dependencies

- Consumed by **every** x86 assembly file: `x86util.asm`, `const-a.asm`, `cpu-a.asm`, `sad-a.asm`, `sad16-a.asm`, `mc-a.asm`, `mc-a2.asm`, and the other DSP asm files.
- `x86util.asm` extends it with `BIT_DEPTH`/`SIZEOF_PIXEL` macros and utility SIMD macros; the two are always included together.
- The C side (`common/cpu.c`, `common/pixel.c`, `common/x86/mc-c.c`) supplies the CPU flag bitmask and the function tables whose suffixed symbol names must match this file's mangling exactly.
- NASM/YASM version features are conditionally used (`__NASM_VERSION_ID__`, `__YASM_VER__`), with fallbacks for older assemblers.

---

## 7. Constants and tables

- `cpuflags_*` bit masks: a dense hierarchy where higher features OR in all prerequisites (e.g. `avx512` includes `avx2`, which includes `fma3|bmi2`, etc.; `aligned`, `cache32`, `cache64`, `atom` are variant/modifier flags rather than CPU features).
- `STACK_ALIGNMENT`: 16 (x86-64), 4 (x86-32). `STACK_PROBE_SIZE`: 8192 (Win64), 4096 (Win32), 0 (else).
- `gprsize`: 8 or 4. `function_align`: 16.
- `required_stack_alignment = (mmsize+15) & ~15`; `high_mm_regs = 16` with AVX-512.
- Base-4 shuffle table: for each 8-bit immediate, its four 2-bit fields are converted to decimal digits with place values 1000/100/10/1, yielding `q000`…`q3333` names.
- Move-token table: `mova`/`movu`/`movh`/`movnta` are redefined per width (MMX: `movq`/`movd`/`movntq`; XMM: `movdqa`/`movdqu`/`movq`/`movntdq`; YMM/ZMM: no `movh`).

---

## 8. Performance notes

This file emits no hot loops itself, but its choices directly determine kernel performance:

- Register allocation avoids spills by mapping arguments to registers on 64-bit ABIs; on x86-32 it clamps to 7 registers because saving/restoring costs more than keeping some args on the stack.
- Stack alignment is chosen so aligned moves (`mova`) are legal without extra `and rsp` where the ABI already guarantees enough alignment; the `and rsp` path is only used for over-aligned (AVX-512) frames.
- VEX/EVEX selection minimises instruction length; high-register swapping for commutative operations removes an extra REX/EVEX byte, which matters in tight loops.
- Preferring registers 16–31 under AVX-512 removes `vzeroupper` from the epilogue.
- `rep ret` avoids the pre-Nehalem AMD branch-after-return stall at no cost on newer CPUs.

**AVX2/AVX-512 vectorization opportunities and bottlenecks (with references):**
- The framework's `mmsize`-generic macros are what allow a single SAD kernel to be emitted at 256-bit (AVX2) and 512-bit (AVX-512) widths. On AVX-512, using 64-byte vectors halves loop count versus AVX2, but requires `vzeroupper` discipline on mixed legacy code and can downclock on some Intel parts when 512-bit units are active (Intel optimisation guide, "AVX-512 frequency effects"; Travaglini et al., *Measuring AVX-512 Frequency Throttling*).
- Masked EVEX moves (`vmovdqu8/16/32 {k}{z}`) let `plane_copy` handle tails without scalar cleanup — a concrete AVX-512 advantage over the AVX2 path, which needs a separate tail loop.
- `vzeroupper_required` correctly avoids the AVX-SSE transition penalty (Intel SDM, "AVX-SSE Transition") but forces an instruction whenever >16 vector registers are used on non-AVX-512 targets; a rebuild should preserve that rule.
- Register pressure: SAD x4 kernels need many accumulators; AVX-512's 32 registers help, but `high_mm_regs`/`AVX512_MM_PERMUTATION` deliberately bias to the upper half. On AVX2 only 16 YMM registers exist, so x4 SAD needs careful accumulator reuse (see `sad-a.asm` spec).
- Bottleneck: on AVX2, 256-bit `psadbw` accumulates into four 64-bit lanes; combining them costs shifts/adds at loop exit. AVX-512's `vpermd`/`vpcompressd` and mask registers reduce this (Intel Intrinsics Guide; x264 `sad-a.asm` AVX-512 variants).

---

## 9. References

- NASM manual (macro/preprocessor, `smartalign`, `%use`, output formats): https://nasm.us/doc/
- Intel® 64 and IA-32 Architectures Software Developer's Manual, Vol. 1–3 (calling conventions, VEX/EVEX encoding, AVX-SSE transition, CET).
- Intel® Intrinsics Guide (instruction availability and semantics).
- Intel® Optimization Reference Manual (AVX-512 frequency and code-size considerations).
- AMD APM and AMD64 ABI (Win64 shadow space, callee-saved XMM6–15).
- System V AMD64 ABI (argument register order, vararg `al` hidden argument).
- Agner Fog, *Calling conventions for different C++ compilers and operating systems*.

---

## 10. Verification checklist

Equivalence is checked at the **assembled-output** level, not by executing the macro file:

1. Preprocess representative kernels for each target (`WIN64`, `UNIX64`, x86-32) and confirm each emitted symbol exists with the expected prefix and suffix (e.g. `x264_pixel_sad_16x16_sse2`) via `nm`/`dumpbin`.
2. For every `cglobal foo, nargs, nregs, nxmm[, stack]` combination used in the project, check that the prologue loads exactly the declared number of arguments and that the stack pointer is restored and `vzeroupper` emitted iff `vzeroupper_required`.
3. Disassemble a WIN64 function using >=8 XMM registers and verify XMM6/7 land in shadow space and XMM8+ in the allocated frame, restored before return.
4. Disassemble an AVX-512 function and confirm EVEX encodings only where expected (registers >=16, 64-byte operands), VEX elsewhere.
5. Confirm `cpuflag()` rejects an instruction below its minimum ISA: assembling a fake function under `INIT_XMM sse2` that uses an SSSE3 mnemonic must fail.
6. Differential test: compile the whole library with the rebuilt framework and run the existing `checkasm` comparisons of every DSP function against the C reference across all detected CPU variants; outputs must be bit-identical.
7. Verify symbol visibility (ELF `hidden`, Mach-O `private_extern`) with `readelf`/`otool`.

---

## 11. Open questions

- Exact behaviour of `annotate_function_size` with YASM vs NASM is macro-version-dependent; UNKNOWN whether the rebuild must reproduce the size symbols for tooling compatibility.
- `HAVE_PRIVATE_EXTERN` is disabled for NASM < 2.14; the precise Mach-O visibility fallback on such old assemblers is UNKNOWN.
- The optimal value of `STACK_PROBE_SIZE` and its interaction with unusual stack sizes is UNKNOWN beyond the constants stated above.
- Some rarely used macros (`cglobal_label`, `DECLARE_REG_TMP_SIZE` variants) have no observed call sites in the assigned files; their exact output is UNKNOWN but follows directly from the definitions.
