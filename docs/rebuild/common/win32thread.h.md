# `common/win32thread.h` - Win32 pthread compatibility types

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\win32thread.h` (mirrored at `common/win32thread.h`). Role in one sentence: defines the Windows implementation of the `x264_pthread_*` type and function subset used throughout the encoder. Marker: **dirty-side behavioural specification**.

## 2. Purpose

The encoder's synchronisation code is written against a small pthread-like API. On Windows this header maps that API onto Win32 primitives without pulling in a third-party pthread library: threads are described by a small wrapper struct, mutexes by `CRITICAL_SECTION`, and condition variables either by the native Vista+ `CONDITION_VARIABLE` or by an opaque pointer to an emulation object.

## 3. Public interface

Types:
- `x264_pthread_t` is a struct with `void *handle`, `void *(*func)(void *)`, `void *arg`, `void **p_ret`, and `void *ret`. The handle is the Win32 thread handle; the remaining fields carry the caller's start routine and its result back through a trampoline.
- `x264_pthread_attr_t` is `int` and is not used; `x264_pthread_mutexattr_t` and `x264_pthread_condattr_t` are also `int`.
- `x264_pthread_mutex_t` is `CRITICAL_SECTION`; `X264_PTHREAD_MUTEX_INITIALIZER` is a zeroed aggregate so statically declared mutexes can be lazily initialised.
- `x264_pthread_cond_t` is `CONDITION_VARIABLE` on WinRT; otherwise it is a struct containing a single `void *Ptr` that holds an emulated condition object.

Functions: `x264_pthread_create`, `x264_pthread_join`, four mutex operations (`init`, `destroy`, `lock`, `unlock`), five condition operations (`init`, `destroy`, `broadcast`, `wait`, `signal`), two attribute no-op macros (`x264_pthread_attr_init/destroy` expand to 0), `x264_win32_threading_init`, `x264_win32_threading_destroy`, `x264_pthread_num_processors_np`.

The header also `#undef ERROR` after including `windows.h`, because the `ERROR` macro would otherwise collide with encoder error constants.

## 4. State and data structures

The thread descriptor is stack-allocated by the caller (typically inside the pool's `thread_handle` array) and remains alive until `join`. The result pointer is stored in the descriptor and read by `join` after the thread handle is signalled. Condition variables on pre-Vista systems carry their state indirectly through `Ptr`; the emulated object is allocated on `cond_init` and freed on `cond_destroy`.

## 5. Algorithm / behaviour

`x264_pthread_create` fills the descriptor with the start routine, argument, a pointer to its own `ret` field and an initial null result, then calls `_beginthreadex` (or `CreateThread` on WinRT) with a trampoline. It returns nonzero on failure, matching the pthread convention of returning an error code (0 means success). `x264_pthread_join` waits indefinitely on the thread handle, copies the result into the caller's output pointer if requested, closes the handle and returns 0 (or -1 on a wait error).

Mutexes wrap `InitializeCriticalSectionAndSpinCount` with a spin count of zero, `DeleteCriticalSection`, `EnterCriticalSection` and `LeaveCriticalSection`. Because statically initialised mutexes cannot be passed to the initialiser at load time, `x264_pthread_mutex_lock` first checks whether the object equals the zeroed static initializer and, if so, initialises it under a global guard critical section with a double-check. Condition variables use native APIs when available (`InitializeConditionVariable`, `WakeAllConditionVariable`, `WakeConditionVariable`, `SleepConditionVariableCS`), otherwise the emulation described in `win32thread.c.md`. Threading init resolves those function pointers dynamically from `kernel32.dll` and creates the global guard; destroy releases it. The processor-count function uses the process (or, where available, thread group) affinity mask and counts set bits, clamping to at least one.

## 6. Dependencies

Includes `windows.h` and depends on configuration macros `HAVE_WINRT`, `HAVE_WIN32THREAD`, `PTW32_STATIC_LIB` from `osdep.h`/`config.h`. Included by `osdep.h` when the Win32 backend is selected. Implemented by `win32thread.c`; used by all synchronisation code through the common `x264_pthread_*` names.

## 7. Constants and tables

`X264_SPIN_COUNT` is zero (no pre-block spinning). Static initializer is an all-zero `CRITICAL_SECTION`. `X264_PTHREAD_MUTEX_INITIALIZER` is `{0}`. No tables. The >64-processor note in the source states the implementation intentionally does not use more than 64 logical processors; see `win32thread.c.md` for the group-affinity detail.

## 8. Performance notes

- **Spin count:** zero means threads block immediately on contention. For short critical sections on many-core Windows machines, a small non-zero spin count can reduce wakeups, but the reference deliberately chooses zero; changing it changes fairness/latency, not correctness.
- **False sharing:** the global guard critical section is taken only on first use of a static mutex; it is not on any hot path. Per-thread descriptors are separate stack objects.
- **Allocation strategy:** none in this header; emulated conditions allocate in their init function.
- **Dispatch hierarchy:** native Vista+ condition variables are preferred via runtime `GetProcAddress`; the emulation is a compatibility fallback only.
- Public references: Microsoft documentation for `CRITICAL_SECTION`, `CONDITION_VARIABLE`, `InitializeConditionVariable`, `_beginthreadex`, processor groups.

## 9. References

- Microsoft Win32 API: `CRITICAL_SECTION`, `CONDITION_VARIABLE`, `SleepConditionVariableCS`, `WakeAllConditionVariable`, `_beginthreadex`, `GetProcessAffinityMask`, `GetThreadGroupAffinity`.
- POSIX.1 pthread API for the semantics being emulated.
- Microsoft's "Supporting Systems with More Than 64 Logical Processors" guidance (referenced in the source).
- x264 `doc/threads.txt`.

## 10. Verification checklist

- Compile a test that statically declares a mutex with `X264_PTHREAD_MUTEX_INITIALIZER` and locks it from several threads without an explicit init; all must succeed and no corruption may occur.
- Create and join a thread returning a known pointer; verify `join` delivers exactly that pointer.
- Verify `x264_pthread_create` returns nonzero for an invalid routine/handle scenario and that no thread is leaked.
- On Vista+ Windows, verify `cond_broadcast` wakes all waiters and `cond_signal` wakes exactly one (observable with counted waiters).
- On a pre-Vista emulation path, verify no lost wakeups over many broadcast/signal cycles with several waiters.
- Verify `x264_pthread_num_processors_np()` equals the population count of the process affinity mask, and is at least 1 when the mask is zero.
- Verify `ERROR` is undefined after including this header, so encoder constants named `ERROR` are usable.

## 11. Open questions

- UNKNOWN: which Windows SDK versions define the native condition-variable function pointers; the code discovers them at run time, but the exact minimum OS version is not asserted.
- UNKNOWN: whether the wrapper's return convention for `x264_pthread_create` is fully pthread-compatible for all failure modes; it returns `!handle`, i.e. 1 on failure.
