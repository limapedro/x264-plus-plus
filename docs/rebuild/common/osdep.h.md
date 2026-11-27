# `common/osdep.h` - platform abstraction layer

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\osdep.h` (mirrored at `common/osdep.h`). Role in one sentence: normalises operating-system and compiler differences - file I/O, Unicode paths, formatted output, alignment attributes, thread primitives, byte swapping, bit scans and prefetch - behind a single set of macros and inline functions. Marker: **dirty-side behavioural specification**.

## 2. Purpose

The encoder must compile with GCC/Clang/MSVC/Intel compilers across Windows, Linux, macOS, BSDs, BeOS and embedded targets, and with or without threads. This header hides that matrix. It is included very early (by `base.h`), so it also defines the alignment and always-inline vocabulary used throughout the codebase and the `x264_pthread_*` subset that all synchronisation code uses.

## 3. Public interface

**Configuration and compatibility.** Defines `_LARGEFILE_SOURCE` and `_FILE_OFFSET_BITS=64` before system headers, includes `stdio.h`, `sys/stat.h`, `inttypes.h`, `stdarg.h`, `stdlib.h`, `config.h`, math (Intel Math Kernel variant when applicable) and, on Windows, `windows.h`/`io.h`, then public `x264.h`. Fallbacks: `log2f`/`log2` via natural-log division when the platform lacks them; MSVC aliases `inline`, `strcasecmp`, `strncasecmp`, `strtok_r`, `S_ISREG`; Intel compiler `va_copy`; BSD `isfinite` alias; a non-reentrant `strtok_r` fallback. Pre-VS2015 MSVC gets custom `x264_vsnprintf`/`x264_snprintf` that force NUL termination and emulate C99's return value.

**Windows Unicode path helpers.** `x264_utf8_to_utf16` converts a UTF-8 string, returning a malloc'd wide buffer or NULL; `x264_utf8_to_utf16_try_buf` first uses a caller-provided stack buffer of a declared length and falls back to the heap only when needed. `x264_fopen(filename, mode)` is a macro expanding to `x264_fopen_internal(filename, L##mode)`; `x264_rename`, `x264_stat` and `x264_struct_stat`/`x264_fstat` wrap the 64-bit wide-character APIs. On POSIX these macros are simple aliases to `fopen`, `rename`, `struct stat`, `fstat`, `stat`. `x264_is_regular_file_path` additionally treats named pipes as non-regular on Windows; `x264_is_regular_file` works on an open handle. `x264_vfprintf` writes through `WriteConsoleW` when the stream is an unconverted console and falls back to `vfprintf` otherwise.

**Timing.** `X264_API int64_t x264_mdate(void)` returns a monotonically oriented microsecond timestamp (implementation in `osdep.c`).

**Alignment and attributes.** `DECLARE_ALIGNED(var,n)`, `ALIGNED_4/8/16`, and on x86/LOONGARCH `ALIGNED_32/64`; `NATIVE_ALIGN` is 64 on x86/LOONGARCH and 16 elsewhere; on other architectures the wider macros collapse to 16. Stack-array variants `ALIGNED_ARRAY_8/16/32/64` fall back to an over-allocated byte array with pointer rounding (`ALIGNED_ARRAY_EMU`) where the compiler cannot honour stack alignment (notably ARM and macOS ARM). `REALIGN_STACK` asks GCC to force 16-byte stack alignment where needed. `UNUSED`, `ALWAYS_INLINE`, `NOINLINE`, `MAY_ALIAS`, `x264_constant_p`, `x264_nonconstant_p` map to compiler-specific attributes or empty macros. `x264_glue3`/`x264_glue3_expand` implement token pasting used by `x264_template`.

**Threading.** A chain of compile-time backends defines a consistent `x264_pthread_*` API: BeOS threads, POSIX pthreads, Win32 threads (by including `win32thread.h`), or a no-thread stub where create/join and mutex/condition operations are no-ops and `x264_pthread_t`/mutex/cond types are `int`. `x264_threading_init()` is a no-op unless Win32 threading or statically linked pthread-win32 is used. `x264_pthread_fetch_and_add(val, add, mutex)` uses GCC's `__sync_fetch_and_add` on x86 with modern GCC, a mutex-protected read-modify-write otherwise, and an unprotected increment in no-thread builds.

**Bit helpers.** `WORD_SIZE`, `endian_fix`/`endian_fix16/32/64` byte-swap on little-endian builds and are identity on big-endian builds; optimised x86 (bswap) and ARMv6 (rev) inline assembly variants exist. `x264_ctz_4bit` uses a 16-entry table for values of at most four bits. `x264_clz`/`x264_ctz` map to builtins where available, otherwise to branch-light table-based 32-bit implementations. `x264_prefetch(p)` uses `prefetcht0` inline assembly on x86/MMX, `__builtin_prefetch` on GCC for x86/ARM/PPC, and nothing elsewhere; the x86 variant exists because GCC historically failed to use complex address modes with the builtin.

## 4. State and data structures

The no-thread backend defines fake types and macro no-ops; the Win32 backend imports its own types from `win32thread.h`. No global data is defined in this header. The alignment macros impose structural invariants: every `ALIGNED_n` declaration must receive an offset divisible by n, and `ALIGNED_ARRAY_EMU` reserves `sizeof(type...) + mask` bytes and rounds the pointer down by clearing low bits.

## 5. Algorithm / behaviour

The header is almost entirely compile-time selection. Important behavioural contracts: the Windows path helpers always convert with `CP_UTF8` and `MB_ERR_INVALID_CHARS`, so malformed UTF-8 fails rather than being replaced; `x264_rename` deletes the destination first because Win32 rename does not overwrite, matching POSIX semantics. `x264_fopen_internal` chooses the stack wide buffer when the path fits `MAX_PATH`. `x264_vfprintf` limits its conversion buffer to 4096 characters and silently falls back to byte-oriented output for longer lines.

The fallback `x264_clz` algorithm stages the leading-zero computation in 16/8/4-bit steps using masks of the form `(x - 2^k) >> (32-k-...)` and a 16-entry table; `x264_ctz` mirrors it on the low bits. `x264_ctz_4bit`'s table is `{4,0,1,0,2,0,1,0,3,0,1,0,2,0,1,0}`: for a nonzero 4-bit value it returns the index of the least significant set bit, and for zero it returns 4 (a sentinel). Endian fix on 32-bit runs as `(x<<24) + ((x<<8)&0xff0000) + ((x>>8)&0xff00) + (x>>24)`; the 64-bit version fixes each half separately and recombines.

`x264_pthread_fetch_and_add` returns the *old* value (fetch-and-add semantics), and in the mutex fallback performs lock, read, add, unlock so the operation is atomic with respect to other users of the same mutex.

## 6. Dependencies

Includes `config.h` (build-time feature flags) and public `x264.h`; conditionally includes `win32thread.h`, `pthread.h`, `kernel/OS.h`. It is included by `base.h` and therefore by almost everything; `osdep.c`, `win32thread.c` and `cpu.c` use its declarations directly.

## 7. Constants and tables

`NATIVE_ALIGN` 64 (x86/LOONGARCH) or 16; `WORD_SIZE` = sizeof(void*); `X264_SPIN_COUNT` is defined in `win32thread.c`, not here. Lookup tables: `x264_ctz_4bit` 16 bytes, fallback `x264_clz` table `{4,3,2,2,1,1,1,1,0,0,0,0,0,0,0,0}`, fallback `x264_ctz` table identical to `x264_ctz_4bit`. The log2 fallback divisor is 0.693147180559945. MSVC snprintf buffer policies are described in section 3.

## 8. Performance notes

- **Cache layout / alignment:** `NATIVE_ALIGN` of 64 on x86 lets one allocation serve 64-byte SIMD loads without splitting cache lines; use `ALIGNED_ARRAY_64` for local DCT/residual buffers where the platform guarantees stack alignment, otherwise the emulation costs a few extra bytes and one AND.
- **False sharing:** `x264_pthread_fetch_and_add`'s GCC path is lock-free on x86, which is why it is used for rarely incremented shared counters such as B-frame slice counts; the mutex fallback is much slower and should not be used in hot loops.
- **Prefetch:** `x264_prefetch` issues `prefetcht0` (all cache levels) rather than a non-temporal hint; place it one or two reference rows ahead of the current macroblock row, not inside the innermost loop.
- **Allocation strategy:** the UTF-8 helpers prefer caller stack buffers to avoid heap allocation for ordinary paths; preserve that pattern for any new path-with-string API.
- **Dispatch hierarchy:** `x264_clz`/`ctz` use compiler intrinsics where available; bit-scan-based algorithms (e.g. run-level scanning) inherit this speed automatically.
- Public references: GCC/Clang attribute documentation (`aligned`, `always_inline`, `may_alias`), Intel SDM PREFETCH instruction, Microsoft `MultiByteToWideChar` docs, Drepper (2007) on cache-line alignment.

## 9. References

- POSIX.1-2008 pthread and `fopen`/`rename`/`stat` semantics; IEEE 1003.1.
- Microsoft Win32 API documentation: `MultiByteToWideChar`, `WriteConsoleW`, `_wfopen`, `_wrename`, `CRITICAL_SECTION`, `CONDITION_VARIABLE`.
- Intel SDM Vol. 2: PREFETCH, BSWAP; ARM ARM: REV.
- GCC manual: function and type attributes; MSVC `__declspec(align)` documentation.
- Drepper, *What Every Programmer Should Know About Memory* (2007).

## 10. Verification checklist

- Compile on at least two compiler families (GCC/Clang and MSVC) and verify every `ALIGNED_n` member has the requested alignment using `offsetof` or `_Alignof` in a test program.
- On Windows, open a file whose name contains non-ASCII UTF-8 characters and verify `x264_fopen` and `x264_stat` succeed; pass malformed UTF-8 and verify NULL/failure.
- Verify `x264_rename` overwrites an existing destination on Windows (observable from the file system).
- Compare `x264_clz`/`x264_ctz` against compiler builtins for all 32-bit inputs with one bit set and for several random values; verify `x264_ctz_4bit(0) == 4`.
- Verify `endian_fix32(endian_fix32(x)) == x` and `endian_fix16` swaps exactly the two bytes.
- With threading disabled, confirm all `x264_pthread_*` calls compile to no-ops and `x264_threading_init()` expands to 0.
- Confirm `x264_pthread_fetch_and_add` returns the pre-increment value when called concurrently from several threads and the total equals the number of calls (where atomics are available).

## 11. Open questions

- UNKNOWN: whether `x264_prefetch` on non-x86 GCC is guaranteed to be a no-op for invalid addresses; the x86 comment says fault-free behaviour is required, which may not hold for `__builtin_prefetch` on all targets.
- UNKNOWN: exact threshold at which `x264_vfprintf` falls back is 4096 *characters* versus bytes; the conversion uses the formatted byte length, so multibyte UTF-8 may reduce usable characters.
- UNKNOWN: whether users may override `x264_threading_init` on POSIX builds; it is a macro there, so no.
