# `common/threadpool.c` - worker pool implementation

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\threadpool.c` (mirrored at `common/threadpool.c`). Role in one sentence: implements a fixed-size worker pool with three bounded queues (idle, queued, completed) and token-based result collection. Marker: **dirty-side behavioural specification**.

## 2. Purpose

Lookahead and other encoder stages need bounded parallelism with deterministic result collection. This file provides that: a fixed number of worker threads repeatedly take jobs from a queue, execute them, and place the results in a completion queue; the submitting thread later claims a specific result by matching the original argument pointer.

## 3. Public interface

Definitions of `x264_threadpool_init`, `x264_threadpool_run`, `x264_threadpool_wait`, `x264_threadpool_delete`; contracts in `threadpool.h.md`. Internal items: `x264_threadpool_job_t` (private struct with `func`, `arg`, `ret`), `struct x264_threadpool_t` (private handle with `exit`, `threads`, `thread_handle`, and the three synchronised lists), `threadpool_thread` (worker loop, marked `REALIGN_STACK` for x86 stack alignment), and `threadpool_list_delete` (frees every element of a list then destroys the list).

## 4. State and data structures

The pool handle contains five logical parts: a volatile exit flag, a worker count, an array of thread handles of that length, and three synchronised lists named `uninit` (idle job objects), `run` (queued job objects) and `done` (finished job objects), each with capacity equal to the worker count. A job object contains three values: a function pointer of type `void *(*)(void *)`, the caller's opaque argument, and the opaque result returned by that function.

Every job object exists in exactly one place at any instant: one of the three lists, or held by a worker between dequeue and completion push. The `done` list is the only place where results accumulate; its mutex protects both the array and the size field. `exit` is written only by `delete` and read by workers.

## 5. Algorithm / behaviour

**Worker loop.** While `exit` is false: lock `run.mutex`, wait on `run.cv_fill` while `exit` is false and `run.i_size` is zero, dequeue the oldest job by shifting the list and decrementing the size if any job exists, unlock. If no job was obtained (which happens when `exit` became true during the wait), continue the loop so the `exit` test terminates it. Otherwise call `job->func(job->arg)`, store the returned pointer in `job->ret`, and push the job onto `done` via the generic synchronised-list push. The worker never touches `uninit`.

**Init.** Reject `threads <= 0`. Call `x264_threading_init()`; failure returns -1. Allocate the pool with zeroing, set `*p_pool`, store the count, allocate the handle array. Initialise `uninit`, `run`, `done` with capacity `threads`; any failure jumps to the failure label. Allocate one job object per worker and push each to `uninit` (a push on an empty list with capacity N never blocks). Create each worker thread; a creation failure again jumps to failure. Return 0.

**Run.** Pop one job from `uninit` (blocking if all jobs are in flight), set its function and argument, push to `run`. Notice that the list capacity equals the worker count, so if every worker is busy the `uninit` pop blocks naturally; there is no separate semaphore.

**Wait.** Lock `done.mutex`; loop forever: scan indices 0..`done.i_size-1` comparing each job's `arg` to the requested token. On the first match, remove it with the plain shift helper (which compacts the tail), decrement the size, unlock, copy `job->ret` into a local, push the job back to `uninit`, and return the local. If no match is found, wait on `done.cv_fill` while still holding the mutex; wakeups are broadcast by every `sync_frame_list_push` on `done`, so all waiters recheck their predicates. Because the token comparison uses pointer identity, distinct jobs must use distinct argument pointers.

**Delete.** Lock `run.mutex`, set `exit = 1`, broadcast `run.cv_fill`, unlock. Join every worker. Free each job object in all three lists and destroy the lists, free the handle array and the pool. Jobs already executing finish before their worker exits; jobs still queued are never executed and their memory is freed with the list.

### Additional behavioural detail

**State machine of a job.** A job begins in `uninit` with no function or argument. `run` copies in the function and argument and moves it to `run`. A worker removes it, executes it, stores the result, and moves it to `done`. A waiter removes it from `done`, reads the result, and returns it to `uninit`. At every point the job is owned by exactly one list or one worker; no reference counting is used. The caller must keep the argument pointer alive until its `wait` returns, because that pointer is both the job input and its identity.

**Why matching by argument works.** The encoder passes a pointer to a per-job control block as the argument. Since those blocks are distinct and live for the lifetime of the job, pointer equality is a unique token. The `done` scan therefore needs no serial numbers or completion flags. A caller that reuses one argument pointer for two simultaneously queued jobs cannot distinguish them; this is an explicit caller obligation.

**Blocking points.** There are three: the `uninit` pop in `run` (all job objects in use), the `run` push in `run` (queue full, which can occur if the pool capacity and job count diverge during a partial state), and `cond_wait` in `wait` (no matching completed job). The worker's `cond_wait` is the fourth. The delete path wakes only workers waiting on `run`; threads blocked elsewhere are the caller's responsibility.

**Exit semantics.** Setting `exit` under `run.mutex` and broadcasting there is correct because that is the condition variable workers wait on. A worker that is executing a job does not check `exit` until it pushes the result and re-enters the loop, so in-flight work always completes. Queued-but-unstarted jobs are abandoned and freed at delete. Workers do not signal `done` after the exit flag is set if they never started a job; therefore a waiter for an abandoned job would block forever, which is why callers must drain results before deleting.

**List implementation reuse.** Using `x264_sync_frame_list_t` for job objects means the pool inherits its two-condition-variable protocol (`cv_fill`, `cv_empty`) and its `max_size+1` pointer array. The code manipulates `run.i_size` directly when dequeuing instead of calling `x264_sync_frame_list_pop`, because it must check `exit` under the same lock and avoid blocking; this is a deliberate deviation from the generic helper. `done` removal also manipulates the list directly under its mutex so a specific element can be extracted rather than the tail.

**Deletion order.** Workers are joined before lists are freed, ensuring no worker can touch a job after its memory is released. `threadpool_list_delete` walks each list, frees every non-null element, nulls the slot, then calls the generic list destructor (which destroys the mutex and conditions and frees the pointer array). Finally the handle array and pool are freed. No `x264_threading_init` counterpart is called at delete: threading remains initialised for the process lifetime, as documented in `osdep.c.md`.

## 6. Dependencies

Includes `common.h`. Uses `x264_sync_frame_list_*` and `x264_frame_shift` from `frame.c`, `x264_pthread_create/join/mutex_lock/unlock/cond_wait/broadcast` from `osdep.h`/`win32thread.c`, `x264_threading_init` from `osdep.c`, and `x264_malloc/free` from `base.c`. Called from encoder open and lookahead code.

## 7. Constants and tables

No constants or tables. The only numeric rules are the capacity equality (all three lists have size `threads`) and the `REALIGN_STACK` attribute on the worker entry point.

## 8. Performance notes

- **Hot path cost:** submission is one mutex lock/unlock on `uninit`, one on `run`; completion is one on `done`; waiting is one lock plus an O(N) scan. With N <= 16 this is cheap. Avoid submitting per-macroblock jobs; submit coarse bands of rows.
- **False sharing:** `exit` is read by all workers but written once; the three list headers are in the same cache line family. If extending the pool, isolate per-worker state. The reference keeps per-worker state entirely on each thread's stack, which naturally avoids false sharing.
- **Allocation strategy:** N job objects allocated at init and recycled; no allocation in submit/wait/execute. Preserve this to keep latency predictable and avoid allocator lock contention.
- **Cache layout:** each job is a three-pointer object (24 bytes on 64-bit), so two jobs per cache line; the `done` scan is sequential and prefetch-friendly.
- **Determinism:** explicit token-based collection makes result order independent of completion order - essential for bitstream reproducibility. Never replace the token match with "pop whatever finished first".
- Public references: POSIX pthread/condition-variable docs; x264 `doc/threads.txt`; Drepper (2007) on false sharing.

## 9. References

- POSIX.1 pthread_create/pthread_join and condition-variable semantics.
- x264 `doc/threads.txt` for the encoder's threading models and deterministic mode.
- `frame.c.md` in this directory for synchronised-list semantics.
- Drepper, *What Every Programmer Should Know About Memory* (2007).

## 10. Verification checklist

- Submit N slow jobs with distinct tokens, then wait in reverse order; every result must match its token (proves out-of-order completion correctness).
- Submit jobs whose functions sleep for varying durations; verify the number of concurrently executing functions never exceeds N.
- Verify a fourth submission blocks when N=3 and all workers are occupied by blocking jobs; release one job and verify the submission proceeds.
- Verify that `wait` for a token not yet submitted blocks and wakes when that token is later submitted and completed.
- Delete a pool with no outstanding jobs and verify all threads terminate and all memory is freed (thread and memory sanitizers).
- Delete a pool while a slow job is executing and verify the job finishes and no use-after-free occurs (documented behaviour: in-flight jobs complete).
- In a no-thread build, verify the macros compile away and no symbols are referenced.

Additional black-box checks:

- Submit zero jobs, immediately delete the pool, and verify no hang and no leaked threads.
- Submit N jobs whose functions block on a barrier until all N are running; verify exactly N are ever concurrently active, demonstrating job-count conservation.
- Verify that a job's return value pointer is delivered unchanged, including NULL and a pointer to a stack object owned by the caller.
- Verify that waiting for a token whose job is still queued blocks until that job's function has returned, not merely until it was dequeued.
- Submit jobs in batches of varying size and verify that all results are collected with no duplicates and no missing tokens over thousands of iterations.
- Verify that deletion while workers are idle is prompt (all threads joined) and that the process does not retain stale wakeups from the broadcast.
- Verify that a second pool can be created and used after the first is deleted, with identical behaviour and no resource conflict.
- Instrument allocation calls and verify zero allocations between `run` and `wait` for a steady-state pool.

## 11. Open questions

- UNKNOWN: whether partial init failure leaks the handle array, lists or already-created threads; the failure label returns -1 without unwinding, so callers should treat a failed init handle as unusable and must not call delete on it.
- UNKNOWN: behaviour with duplicate argument tokens; two waiters could both match the same job, and one would then block forever.
- UNKNOWN: whether recursive job submission is supported; with all N job objects checked out it will deadlock.
- UNKNOWN: whether the pool guarantees FIFO execution order; workers dequeue from the head, but concurrent scheduling means start order is not guaranteed, only collection order is.
