# `common/win32thread.c` - Win32 threading shim implementation

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\win32thread.c` (mirrored at `common/win32thread.c`). Role in one sentence: implements the pthread-like thread/mutex/condition-variable API on Windows, including dynamic detection of native condition variables and a semaphore-based emulation for older systems. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Rather than depend on pthreads-win32, the encoder ships a minimal shim over Win32 primitives. It must work on Windows XP-era systems (no native condition variables), on WinRT (different thread-creation and critical-section APIs), and on modern Windows (native condition variables). It must also count CPUs correctly in the presence of processor groups.

## 3. Public interface

Implementations of every function declared in `win32thread.h.md`: create/join, mutex init/destroy/lock/unlock, cond init/destroy/broadcast/signal/wait, `x264_win32_threading_init/destroy`, `x264_pthread_num_processors_np`. Two compile-time branches: `HAVE_WINRT` uses native condition variables and `_beginthreadex = CreateThread`; the general branch discovers condition-variable functions dynamically and provides the fallback emulation.

## 4. State and data structures

Globals:
- `static x264_pthread_mutex_t static_mutex`: the guard used to lazily initialise statically declared critical sections and to serialise emulation state changes; initialised by `x264_win32_threading_init` and destroyed/zeroed by `x264_win32_threading_destroy`.
- `thread_control`: a static struct of four function pointers (`cond_broadcast`, `cond_init`, `cond_signal`, `cond_wait`) loaded from `kernel32.dll`; a non-null `cond_init` means the native path is active.

Emulated condition object (`x264_win32_cond_t`, allocated by `cond_init` when no native support): two critical sections (`mtx_broadcast` serialises broadcasters, `mtx_waiter_count` protects the counter), a volatile `waiter_count`, a semaphore used as the wakeup mechanism, an auto-reset event `waiters_done` used to let a broadcaster know all waiters have consumed the wakeup, and a volatile `is_broadcast` flag.

Worker trampoline `win32thread_worker` is `__stdcall` because `_beginthreadex` requires that calling convention; it invokes `h->func(h->arg)`, stores the result through `h->p_ret`, and returns 0.

## 5. Algorithm / behaviour

**Threads.** `create` fills the descriptor and calls `_beginthreadex(NULL, 0, trampoline, thread, 0, NULL)`. `join` waits indefinitely, returns -1 if the wait did not yield `WAIT_OBJECT_0`, copies the result, closes the handle and returns 0.

**Mutexes and lazy init.** `mutex_init` calls `InitializeCriticalSectionAndSpinCount(mutex, 0)`; the return value is negated because Win32 returns a BOOL. `mutex_lock` compares the object bytewise to the all-zero static initializer; on a match it acquires `static_mutex`, rechecks (double-checked locking), initialises if still zero, releases the guard, then enters the critical section. This is why statically initialised mutexes work without a constructor.

**Native condition path.** `cond_init` calls `InitializeConditionVariable`; `destroy` is a no-op (native condition variables need no destruction); `broadcast`/`signal` call `WakeAllConditionVariable`/`WakeConditionVariable`; `wait` calls `SleepConditionVariableCS(cond, mutex, INFINITE)` and returns nonzero on failure (pthread convention inverted).

**Emulated condition path.** `cond_init` allocates the object with `calloc`, creates a semaphore with initial count 0 and maximum `0x7fffffff`, and creates an auto-reset event. Failure at any step returns -1; the partially built object is not fully unwound in the reference (UNKNOWN: leak on partial failure). `cond_destroy` closes both handles, destroys both critical sections and frees the object.

`cond_wait` increments `waiter_count` under `mtx_waiter_count` (while holding `mtx_broadcast` only for the increment), releases the caller's mutex, waits on the semaphore, decrements the count, determines `last_waiter = (count == 0 || !is_broadcast)`, signals `waiters_done` when appropriate, then re-acquires the caller's mutex before returning. The two-mutex protocol prevents a broadcaster from missing a waiter that is still registering.

`cond_broadcast` locks `mtx_broadcast` then `mtx_waiter_count`; if any waiter is registered it sets `is_broadcast = 1`, releases `waiter_count` semaphore slots equal to the waiter count, unlocks the count mutex, waits for `waiters_done`, clears `is_broadcast`, and finally unlocks the broadcast mutex. Holding `mtx_broadcast` across the whole exchange prevents two broadcasters from interleaving. `cond_signal` is similar but releases exactly one semaphore slot and does not set `is_broadcast`.

**Initialisation.** `x264_win32_threading_init` calls `GetModuleHandleW(L"kernel32.dll")` and resolves `InitializeConditionVariable`; when present it also resolves `WakeAllConditionVariable`, `WakeConditionVariable` and `SleepConditionVariableCS`. It then initialises `static_mutex`. `x264_win32_threading_destroy` destroys and zeroes that mutex.

**Processor count.** In the general branch, on 64-bit builds it tries `GetThreadGroupAffinity` via `GetProcAddress`; if available it uses the current thread's group mask. Otherwise it falls back to `GetProcessAffinityMask(GetCurrentProcess(), &process_cpus, &system_cpus)`. It counts set bits one at a time with a shifting loop and returns at least 1. WinRT builds simply return `GetNativeSystemInfo().dwNumberOfProcessors`.

### Additional behavioural detail

**Two build branches.** When `HAVE_WINRT` is set, the whole file takes a simplified path: `_beginthreadex` is aliased to `CreateThread`, critical-section initialisation uses `InitializeCriticalSectionEx` with `CRITICAL_SECTION_NO_DEBUG_INFO`, waits use `WaitForSingleObjectEx`, condition variables use the native types directly, and processor count comes from `GetNativeSystemInfo`. The general branch targets desktop Windows and includes `process.h` for `_beginthreadex`.

**Double-checked static-mutex initialisation.** A statically declared `CRITICAL_SECTION` consists of zeroed bytes and is not a valid initialised object. The lock function compares the object against a zeroed template with `memcmp`; on a match it acquires the global guard, compares again (because another thread may have initialised it while this one waited), initialises if necessary, releases the guard, and only then enters. This makes `X264_PTHREAD_MUTEX_INITIALIZER` usable in file-scope structures without a constructor. Destroying such a mutex is a plain `DeleteCriticalSection`.

**Emulated condition protocol in detail.** The broadcaster holds `mtx_broadcast` for the entire operation, which serialises broadcasters and prevents the wakeup state from being observed by two of them at once. It briefly holds `mtx_waiter_count` to read the waiter count and set `is_broadcast`, then releases the semaphore once per waiter. Each waiter, after waking, decrements the count and computes `last_waiter` as either an empty count or a non-broadcast wakeup. The last waiter signals `waiters_done`, allowing the broadcaster to finish and release `mtx_broadcast`. This handshake prevents the classic lost-wakeup race where a broadcaster signals before a waiter has finished registering.

**Signal versus broadcast.** `cond_signal` also takes `mtx_broadcast`, reads the waiter count under `mtx_waiter_count`, and releases exactly one semaphore slot if any waiter exists. It does not set `is_broadcast`, so the woken waiter will treat itself as the last waiter and signal `waiters_done`. `cond_broadcast` sets `is_broadcast` so all woken waiters consume the semaphore slots before the event is signalled by the last one. Native condition variables replace all of this with one OS call.

**Thread lifecycle.** The descriptor passed to `create` is owned by the caller and must outlive the thread. The trampoline writes the result through `p_ret` before returning, which means `join`'s `WaitForSingleObject(INFINITE)` is a sufficient synchronisation point to read it. Closing the handle in `join` means a descriptor cannot be joined twice; the encoder's pool joins exactly once.

**Processor-group note.** The source cites Microsoft's guidance for systems with more than 64 logical processors but deliberately avoids using more than 64. On 64-bit builds it tries `GetThreadGroupAffinity` on the current thread and uses that group's mask; if the function is unavailable or fails it falls back to the process affinity mask. The bit-count loop stops when the shifting bit becomes zero, which naturally handles a 32- or 64-bit mask. A zero result is clamped to one.

## 6. Dependencies

Includes `base.h`, `windows.h`, and on non-WinRT builds `process.h`. Uses `x264_malloc` only indirectly through `calloc` for the emulated condition, `x264_log` not at all, and is called from `osdep.c` init plus every encoder synchronisation site.

## 7. Constants and tables

`X264_SPIN_COUNT = 0`. Semaphore maximum `0x7fffffff`. `ADL` constants belong to `opencl.c`, not here. `WAIT_OBJECT_0`, `INFINITE`, `CRITICAL_SECTION_NO_DEBUG_INFO` are Win32 constants. No lookup tables.

## 8. Performance notes

- **Spin count:** zero means no busy-wait before blocking; suitable for long-held locks but potentially slower for very short critical sections. The value is deliberate and should be preserved for behavioural equivalence.
- **False sharing:** `static_mutex` is touched only on first use and during init/teardown; the emulated condition's two critical sections and counters are distinct objects, so no hot counter shares a line with another thread's data.
- **Allocation strategy:** native conditions require none; the fallback allocates one object per condition at init and frees at destroy. Condition objects are created per encoder/frame, so on pre-Vista systems this is a measurable cost - keep the native path whenever available.
- **Dispatch hierarchy:** one-time `GetProcAddress` resolution selects native vs emulated; no per-wakeup branching beyond a single function-pointer null test.
- **Scalability limit:** the source explicitly notes it does not use more than 64 logical processors; processor groups beyond the current one are ignored by design.
- Public references: Microsoft docs on condition variables, critical sections, processor groups, and "Supporting Systems with More Than 64 Logical Processors".

## 9. References

- Microsoft Win32 API documentation: `InitializeConditionVariable`, `SleepConditionVariableCS`, `WakeConditionVariable`, `WakeAllConditionVariable`, `InitializeCriticalSectionAndSpinCount`, `_beginthreadex`, `GetThreadGroupAffinity`, `GetProcessAffinityMask`.
- Microsoft guidance on systems with more than 64 logical processors (referenced in the source comments).
- POSIX condition-variable semantics for the behaviour being emulated.
- x264 `doc/threads.txt`.

## 10. Verification checklist

- On a modern Windows host, verify `thread_control.cond_init` is non-null after `x264_win32_threading_init` and that native functions are used (instrument by overriding `GetProcAddress` in a harness).
- Force the fallback path by clearing the function pointers; with four waiter threads, broadcast must wake all four and signal must wake exactly one, repeatedly with no lost wakeups.
- Verify a statically initialised mutex is usable without `mutex_init` and that concurrent first use does not double-initialise (under a thread checker).
- Verify `join` returns the exact pointer produced by the worker and that a second join on the same descriptor is not attempted by the encoder.
- Verify `x264_pthread_num_processors_np` equals the affinity-mask population count and is at least 1 when affinity is restricted to a single CPU.
- Verify `x264_win32_threading_destroy` leaves `static_mutex` zeroed so a later re-init works in the same process.
- Verify no `ERROR` macro interference when compiling encoder code including this header indirectly.

Additional black-box checks:

- Create and join 100 threads sequentially, each returning a distinct pointer, and verify all results and handle cleanup.
- Initialise the same static mutex from many threads simultaneously and verify exactly one initialisation occurs (instrument the initialiser in a harness).
- With the emulated condition path forced, run a producer/consumer test with one broadcaster and eight waiters for 1000 rounds; verify the round counter observed by every waiter matches and no waiter is stranded.
- With the emulated path forced, call `cond_signal` repeatedly with no waiters and verify it returns immediately without hanging on `waiters_done`.
- Verify that `cond_destroy` on the native path is a no-op and does not invalidate a still-running wait (the reference relies on this).
- Verify that `x264_win32_threading_destroy` followed by `x264_win32_threading_init` works in the same process and that a statically initialised mutex still works afterwards.
- On a multi-group system, verify the reported processor count is at least one and at most 64; document the observed value as environment-dependent rather than asserting an exact number.
- Verify that including this header after `windows.h` does not reintroduce the `ERROR` macro into encoder code.

## 11. Open questions

- UNKNOWN: whether the fallback condition's `cond_init` failure path leaks the partially allocated object; no cleanup is performed on intermediate failures.
- UNKNOWN: the exact behaviour on Windows systems with more than 64 processors spanning groups: only the current thread's group is counted, so a process spanning groups may report fewer CPUs than available.
- UNKNOWN: whether `mutex_lock`'s `memcmp` against a zeroed `CRITICAL_SECTION` is reliable across all SDK versions; it is the documented approach used here.
