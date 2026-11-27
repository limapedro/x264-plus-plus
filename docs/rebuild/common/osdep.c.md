# `common/osdep.c` - time source and threading-library lifecycle

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\osdep.c` (mirrored at `common/osdep.c`). Role in one sentence: implements the microsecond clock and the process-wide initialisation/teardown of the Win32 threading shim (or statically linked pthread-win32). Marker: **dirty-side behavioural specification**.

## 2. Purpose

Two things cannot be done in a header: choosing among OS clock APIs at run time, and initialising a threading library exactly once per process with correct cleanup. This file provides those services. The clock is used for rate control, VFR timestamp handling and statistics; the threading init is used before the thread pool is created on Windows.

## 3. Public interface

- `int64_t x264_mdate(void)`: returns a timestamp in microseconds. The contract is monotonic progression on platforms providing `clock_gettime(CLOCK_MONOTONIC)`; on Windows it uses `ftime` (wall clock, 1 ms granularity scaled to microseconds); elsewhere `gettimeofday` (wall clock). No specified epoch.
- `int x264_threading_init(void)` (declared in `osdep.h`, actual function only when `HAVE_WIN32THREAD` or `PTW32_STATIC_LIB`): returns 0 on success or if already initialised, -1 on failure. Thread-safe and idempotent. Registers a process-exit cleanup with `atexit`.
- Internal: `static void threading_init(void)` performs the backend-specific attach and registers cleanup; `static void threading_destroy(void)` performs the detach; `static volatile LONG threading_is_init` is the tri-state guard (0 = uninitialised, -1 = initialising, 1 = initialised).
- With `PTW32_STATIC_LIB`, the external `ptw32_processInitialized` global is consulted to avoid double-attaching a statically linked pthread-win32.

## 4. State and data structures

The only persistent state is `threading_is_init`, a `volatile LONG` manipulated with `InterlockedCompareExchange`/`InterlockedExchange`. The tri-state protocol: a caller loops exchanging -1 for 0; if the observed value is positive it returns success immediately; if the exchange succeeds it becomes the sole initialiser; on failure it resets to 0 so a later caller may retry; on success it sets 1. In the Win32 shim backend there is also a static critical section (`static_mutex` in `win32thread.c`) that the shim owns; in the pthread-win32 static backend the library's own global serves that role.

Lifetime: initialisation happens at the first thread-pool creation (or explicit call), cleanup at process termination via `atexit`, not at encoder close. This means a second encoder in the same process reuses the initialised state.

## 5. Algorithm / behaviour

**Clock selection.** Compile-time priority: `SYS_WINDOWS` -> `ftime(&tb)` and `(tb.time*1000 + tb.millitm)*1000`; `HAVE_CLOCK_GETTIME` -> `clock_gettime(CLOCK_MONOTONIC, &ts)` and `ts.tv_sec*1000000 + ts.tv_nsec/1000` (nanoseconds truncated, not rounded); fallback -> `gettimeofday` and `tv_sec*1000000 + tv_usec`. Both 32-bit and 64-bit `time_t` are handled by explicit casts to `int64_t` before multiplication.

**Threading init.** The guarded state machine described in section 4 prevents two threads from attaching simultaneously and allows a retry after a failed attach. `threading_init` picks the backend: with `PTW32_STATIC_LIB`, if `ptw32_processInitialized` is already true it returns success without changing the guard's ultimate outcome; otherwise it calls `pthread_win32_process_attach_np()` and treats false as failure. Without that macro it calls `x264_win32_threading_init()` (implemented in `win32thread.c`, which resolves condition-variable function pointers and creates the static critical section). On success, `atexit(threading_destroy)` is registered exactly once. `threading_destroy` mirrors the backend: `pthread_win32_thread_detach_np()` + `pthread_win32_process_detach_np()`, or `x264_win32_threading_destroy()`.

On POSIX builds and WinRT-free non-Win32 builds, `x264_threading_init` is a macro expanding to `0`, so no code is emitted here.

### Additional behavioural detail

**Clock precision and risk of overflow.** All three clock branches cast to `int64_t` before multiplying seconds by one million, so timestamps remain valid past 2038 on 32-bit platforms. The Windows `ftime` path has one-millisecond granularity but is scaled to microseconds, so successive calls frequently return identical values; rate-control code must not assume distinct timestamps. The `clock_gettime` path truncates nanoseconds rather than rounding, giving at most one microsecond of error. On `gettimeofday` platforms the wall clock may jump backwards; only the monotonic branch is stable under system-clock changes.

**Tri-state init rationale.** A simple boolean would allow two threads to observe "uninitialised" simultaneously and both attach the threading library. Using `InterlockedCompareExchange` to claim the value -1 turns the guard into a mutex-free state machine: a claimer owns the transition, other callers spin until the value becomes positive or returns to zero, and a failure resets to zero so a later attempt can retry. The loop does not sleep, but the critical section of the initialiser is short.

**Backend selection detail.** With `PTW32_STATIC_LIB`, the library's own `ptw32_processInitialized` variable is consulted. If it is already nonzero, `threading_init` returns success without calling attach, but `x264_threading_init` still advances the guard to 1 and registers the cleanup. The cleanup then detaches the static library at exit. Without that macro, the shim's own init/destroy functions are used and the guard is solely owned here.

**Registration timing.** `atexit(threading_destroy)` is called from `threading_init`, not from `x264_threading_init`, so it is registered exactly once even if many threads call the public function. If the backend init fails, no cleanup is registered and the guard is reset. If the process exits while a thread still uses the shim, the ordering of `atexit` handlers matters; the source does not coordinate with other libraries.

**No-op configurations.** When neither Win32 threading nor static pthread-win32 is selected, the entire initialisation block is excluded by the preprocessor and the public name is a macro returning zero in `osdep.h`. Consequently a POSIX build has no symbol to call, and a no-thread build pays nothing. This is important for direct consumers of the public API who call `x264_threading_init` unconditionally.

**Interaction with the thread pool.** `x264_threadpool_init` calls `x264_threading_init` before creating any workers. This guarantees that the global guard critical section exists before the first static mutex is locked by a worker. A clean implementation must preserve this ordering; creating threads first would allow a race on lazy mutex initialisation on Windows.

## 6. Dependencies

Includes `osdep.h`, then `sys/types.h`/`sys/timeb.h` on Windows or `sys/time.h` elsewhere, plus `time.h`. Calls `x264_win32_threading_init`/`destroy` (Win32 shim) or the pthread-win32 attach/detach API. Called by `threadpool.c` at pool creation and indirectly by any user of the public threading-init function.

## 7. Constants and tables

No tables. Sentinel values of `threading_is_init`: 0, -1, 1. The Windows clock multiplies milliseconds by 1000, giving microsecond units with millisecond resolution; the monotonic clock divides nanoseconds by 1000, giving microsecond resolution with truncation.

## 8. Performance notes

- `x264_mdate` may be called in rate-control paths; `clock_gettime(CLOCK_MONOTONIC)` is normally a vDSO call and cheap, whereas `ftime` is comparatively expensive. Do not call it per macroblock; cache the value per frame.
- Initialisation uses interlocked operations rather than holding a mutex for the fast path, so repeated pool creation is nearly free.
- **Cache layout / false sharing:** `threading_is_init` is written only during initialisation; it is not a hot shared counter and does not need padding, though placing it next to frequently written globals in a future implementation would create false sharing.
- **Allocation strategy:** no allocations; `atexit` registration is one-time.
- **Dispatch hierarchy:** compile-time selection of the clock, run-time selection of the threading backend; keep both decisions out of per-frame code.
- Public references: POSIX `clock_gettime(2)`, `gettimeofday(2)`; Microsoft `ftime` and `GetSystemTimeAsFileTime` documentation; VxWorks/Windows threading lifecycle docs.

## 9. References

- POSIX.1-2008 `clock_gettime`, `CLOCK_MONOTONIC`, `gettimeofday`.
- Microsoft Win32 `ftime`, `atexit`, pthread-win32 `pthread_win32_process_attach_np`/`detach_np` documentation.
- `win32thread.c.md` in this directory for the shim initialised here.
- Drepper, *What Every Programmer Should Know About Memory* (2007), for the cost of system-time calls in hot loops (section on system calls and scheduling).

## 10. Verification checklist

- Call `x264_mdate()` twice around a known sleep of at least one millisecond; the difference must be positive and within a generous tolerance of the sleep duration for each backend.
- Call `x264_threading_init()` concurrently from several threads; all calls must return 0 and exactly one `atexit` cleanup must be registered (observable by instrumenting the shim in a harness).
- Force an init failure (e.g. by stubbing `x264_win32_threading_init` to fail) and verify the guard returns to 0 and a subsequent call can succeed.
- On POSIX builds, verify the symbol is absent/macro-expanded: no linker dependency on Win32 libraries.
- Verify that creating and deleting two thread pools in sequence does not detach the threading library until process exit (a second pool creation still succeeds).

Additional black-box checks:

- Capture 100 consecutive `x264_mdate()` values with no intervening work on each platform; verify non-decreasing order (allowing equality) and that the maximum observed jitter is consistent with the clock's documented granularity.
- Verify that calling the clock from multiple threads yields values in a common time base, i.e. no thread sees a value wildly outside the range of the others.
- Verify that `x264_threading_init` is re-entrant: call it from a callback of itself in a harness and confirm the nested call returns immediately without recursion.
- Verify that after a successful init, calling the function from a newly created thread returns success without taking the slow path (observable by instrumenting the backend init count).
- Simulate a failed attach followed by a successful one and verify a second `atexit` registration is not made.
- Verify that a POSIX build exposes no `x264_threading_init` symbol in the object files when the macro path is active.
- Verify that a process which calls init, creates a pool, deletes the pool and exits cleanly produces no shutdown errors from the threading backend.

## 11. Open questions

- UNKNOWN: whether `x264_mdate` is truly monotonic on Windows and `gettimeofday` platforms; the interface does not promise it.
- UNKNOWN: ordering of `atexit` handlers relative to other libraries that also use the threading shim; not specified here.
- UNKNOWN: behaviour if initialisation fails after `atexit` was registered; the code only registers on success.
