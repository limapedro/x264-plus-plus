# `common/threadpool.h` - thread pool interface

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\threadpool.h` (mirrored at `common/threadpool.h`). Role in one sentence: declares the fixed-size worker pool used for asynchronous jobs such as parallel lookahead slice work, with an opaque pool type and a four-call lifecycle. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Some encoder stages need bounded parallelism without creating and destroying threads per frame. This header exposes a small pool abstraction: initialise with a worker count, submit a function plus an opaque argument, wait for the job whose argument matches a supplied token, and delete the pool. When the build has no threading support, all four calls collapse to no-op macros so callers need no conditionals.

## 3. Public interface

`typedef struct x264_threadpool_t x264_threadpool_t;` - opaque handle; all fields are private to `threadpool.c`.

With `HAVE_THREAD`:

- `X264_API int x264_threadpool_init(x264_threadpool_t **p_pool, int threads)`: allocates and starts a pool of exactly `threads` workers; returns 0 on success, -1 for a non-positive count, failed threading initialisation, or allocation/thread-creation failure. On success `*p_pool` is set; on failure it may be left pointing at a partially built pool that only `x264_threadpool_delete` should touch (UNKNOWN: callers in the reference do not rely on this).
- `X264_API void x264_threadpool_run(x264_threadpool_t *pool, void *(*func)(void *), void *arg)`: enqueues a job. It blocks until a free job slot exists. The same `arg` value is the job's identity for the later wait.
- `X264_API void *x264_threadpool_wait(x264_threadpool_t *pool, void *arg)`: blocks until a completed job whose `arg` pointer equals the supplied token is found, removes it, returns its function's result, and recycles its job object. There is no timeout and no cancellation.
- `X264_API void x264_threadpool_delete(x264_threadpool_t *pool)`: signals exit, wakes all workers, joins them, frees job objects, lists and the pool. Must be called only when no jobs are outstanding (UNKNOWN: the reference does not validate this).

Without `HAVE_THREAD`: the macros expand to `-1` for init and to nothing/NULL for the others, so a single build of the caller works in both configurations.

All four names are bit-depth templated via `x264_template`.

## 4. State and data structures

The pool owns three bounded queues (each a `x264_sync_frame_list_t` with capacity equal to the thread count): `uninit` (idle job objects), `run` (submitted jobs awaiting a worker) and `done` (completed jobs). It also owns a `volatile int exit` flag, the worker count, an array of thread handles, and no other mutable state. The job object stores a function pointer, the caller's argument, and the return value. The pool is the sole owner of job objects; arguments and return values are owned by the caller.

The three-queue design is the central invariant: there are exactly `threads` job objects in the whole pool at all times, distributed among the three queues or in flight inside a worker. `run` therefore never needs to grow, and submission blocks when all workers are busy rather than allocating.

## 5. Algorithm / behaviour

Initialisation order: reject non-positive thread counts; call the process-wide threading initialiser; allocate the pool; allocate the thread-handle array; initialise all three synchronised lists with capacity `threads`; allocate one job object per worker and push it into `uninit`; finally create the worker threads. If any step fails, control reaches a failure label that returns -1 (the reference does not fully unwind earlier allocations on this path - see Open Questions).

Submission pops an idle job from `uninit` (blocking until one is available), stores the function and argument, and pushes it to `run` (also potentially blocking if the queue is full, though capacity equals worker count). Completion is signalled by pushing the job to `done`.

Waiting scans the `done` list under its mutex for a job whose `arg` matches the token. On a match it removes that specific element by shifting the tail left, decrements the size, unlocks, reads the return value, and returns the job to `uninit`. Between scans it waits on `done.cv_fill`. Because the wait predicate is checked inside the loop after each wakeup, jobs completing out of order do not confuse callers: each waiter claims only its own token.

Deletion sets `exit = 1` under the `run` mutex, broadcasts `run.cv_fill` so idle workers wake, joins all workers, then frees the contents of all three lists and the pool. Workers exit their loop only at a loop boundary: a job already dequeued will finish before the worker observes the flag, so delete must not be called with outstanding work unless the caller is prepared for those jobs to complete.

## 6. Dependencies

Includes `common.h` (templating, thread primitives) and uses `x264_sync_frame_list_t` from `frame.h`, `x264_pthread_create/join`, `x264_threading_init` from `osdep.h`, and the plain list helper `x264_frame_shift`. Called by encoder-open logic to create `h->threadpool` and `h->lookaheadpool`, and by lookahead code for parallel slice-type analysis.

## 7. Constants and tables

No constants or tables. Capacity rule: each of the three lists has capacity equal to the configured worker count; the total number of job objects is the same count. The null-slot semantics of synchronised lists (array of `max_size+1` pointers, terminator null) are inherited from `frame.h`.

## 8. Performance notes

- **Dispatch hierarchy:** the pool is a coarse-grained stage-level mechanism, not a per-macroblock one. Submit one job per independent band of lookahead work, then wait once; per-job overhead is one mutex-protected push/pop plus one indirect call.
- **False sharing:** the `exit` flag and the three list mutexes live in one struct; workers read `exit` on every loop iteration but only the deleter writes it. If a future implementation adds per-worker counters, pad them to a cache line to avoid false sharing.
- **Allocation strategy:** job objects are allocated once at init and recycled through `uninit`; never allocate in `run` or `wait`. This is essential for deterministic latency.
- **Cache layout:** each job's function/arg/ret triple is a small object; keeping it together means one cache line per job. The `done` list scan is O(threads) and thread counts are small (typically <= 16), so it is cheaper than a hash map.
- **Determinism:** because jobs are identified by their argument pointer and waited on explicitly, results are collected deterministically regardless of completion order. This is the key property for reproducible encodes.
- Public references: x264 `doc/threads.txt` (threading models and determinism), POSIX pthread documentation, Drepper (2007) on false sharing.

## 9. References

- POSIX.1 `pthread_create`, `pthread_join`, condition variables; Win32 shim documented in `win32thread.c.md`.
- x264 `doc/threads.txt` (lookahead threading, frame threading, deterministic mode).
- `frame.h.md` for the synchronised list used as the pool's queue.
- Drepper, *What Every Programmer Should Know About Memory* (2007), section 6 on false sharing.

## 10. Verification checklist

- Initialise a pool with N workers, submit exactly N jobs that each return a unique value derived from their argument, then wait in *reverse* submission order: every wait must return the value associated with its token, proving out-of-order completion is handled.
- Submit more than N jobs sequentially and verify `run` blocks until a worker is free (measure with a timing harness using a deliberately slow job).
- Verify `x264_threadpool_init(pool, 0)` and negative counts return -1 without creating threads.
- After all jobs are collected, verify the pool can be reused for a second batch with no allocation growth (instrument the allocator).
- Delete the pool after all waits and verify no leaked threads or memory under a thread/memory checker.
- In a no-thread build, verify all four calls compile to no-ops and `init` returns -1.
- Run the same encode twice with multiple pool threads and `--deterministic` enabled; bitstreams must be identical.

## 11. Open questions

- UNKNOWN: whether partial initialisation failure leaves a deletable pool or leaks the already-created lists/threads; the reference's failure path returns -1 without full unwinding.
- UNKNOWN: behaviour if two callers wait concurrently on the same argument token; only one can succeed, the other will block forever (no documented guard).
- UNKNOWN: whether jobs may recursively submit further jobs to the same pool; with all job objects in use this can deadlock.
