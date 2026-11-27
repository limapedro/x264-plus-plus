# `common/common.c` - bit-depth-templated logging dispatch

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\common.c` (mirrored at `common/common.c`). Role in one sentence: supplies the single encoder-context-aware logging entry point that forwards formatted messages to the callback stored in the parameter object. Marker: **dirty-side behavioural specification**.

## 2. Purpose

An encoder needs two logging paths: messages emitted before any encoder exists (handled by `x264_log_internal` in `base.c`) and messages emitted with knowledge of a particular encoder instance and its user-supplied callback. `common.c` implements the latter. It is compiled once per bit depth because it includes `common.h` and therefore participates in the templated symbol naming scheme.

## 3. Public interface

- `void x264_log(x264_t *h, int level, const char *format, ...)`; internal symbol is renamed by `x264_template(log)` to `x264_<depth>_log`. Contract: when `h` is null, or when `level <= h->param.i_log_level`, the variadic arguments are formatted once and delivered to the appropriate sink. If `h` is null the sink is `x264_log_default(NULL, ...)`; otherwise it is `h->param.pf_log(h->param.p_log_private, ...)`. Messages above the configured level are discarded without calling the sink and without consuming any callback time.

Argument contract: `format` is a printf-style string; `level` uses the public `X264_LOG_*` values (ERROR 0, WARNING 1, INFO 2, DEBUG 3, NONE -1). Because the callback receives a `va_list`, it must consume it during the call (no retention). The function is not required to be reentrant-safe with respect to a callback that destroys its encoder.

## 4. State and data structures

The only state read is `h->param.i_log_level`, `h->param.pf_log` and `h->param.p_log_private`, all owned by the parameter object embedded in `x264_t`. There are no globals, caches or allocations in this file. The `va_list` is created and ended within the call, so no state escapes.

## 5. Algorithm / behaviour

The function evaluates a single guard: logging proceeds if the context pointer is null (the pre-encoder case, always logged) or if the requested level is numerically no greater than the configured threshold. Note that `X264_LOG_NONE` is -1, so with the threshold set to NONE every normal level is greater and is filtered; error messages at level 0 are also filtered in that mode. On the pass path, a `va_list` is started after the last named argument (`format`), the correct sink is chosen by the null test on `h`, the variadic payload is forwarded unchanged, and the list is ended. There is no buffering, no prefixing and no newline insertion: formatting and destination handling belong entirely to the sink. A null `pf_log` is not defended against; callers are expected to have used `x264_param_default`, which installs `x264_log_default`.

The bit-depth templating means the exported name differs per build; code inside the encoder calls the macro-shortened `x264_log`, so callers do not need to know the depth. This is part of the link-failure strategy described in `common.h.md`.

### Additional behavioural detail

**Filter semantics at each level.** With `i_log_level = X264_LOG_NONE` (-1) every normal level is filtered because all are greater than -1. At `X264_LOG_ERROR` only error messages pass; at `X264_LOG_INFO` errors, warnings and info pass; at `X264_LOG_DEBUG` everything passes. The comparison is numeric, so an out-of-range level such as 7 is filtered at every normal threshold. A negative level other than NONE would pass at all normal thresholds, which is UNKNOWN as an intended use case.

**Null-context path.** The null path exists because several utilities in `base.c` emit warnings before an encoder is created. It deliberately bypasses the user's callback: there is no parameter object from which to obtain one. The default sink prepends a severity word to stderr, so even callers that never install a callback get recognisable diagnostics. On Windows the sink's console branch converts UTF-8 to UTF-16 and uses `WriteConsoleW`, falling back to byte output when redirected or when the formatted line is too long.

**Variadic forwarding rules.** The callback receives a `va_list` created from the last named parameter before `format`. It must not copy and retain the list beyond the call, because the underlying argument storage may be on the caller's stack. A callback that needs to defer formatting must copy arguments explicitly with `va_copy` and consume them during the call. `va_end` is invoked by this function after the callback returns, regardless of whether the callback consumed any arguments.

**Templating consequences.** Because the symbol is renamed per bit depth, a process linking both 8-bit and 10-bit libraries can hold callbacks for both without symbol collisions. A callback that wants to know which bit depth produced a message cannot do so from the callback signature alone; it must infer this from the private pointer or from message content.

**Threading behaviour.** There is no internal lock. If several encoder threads log concurrently, the user callback may be entered concurrently and interleaved output is possible. The public API leaves serialization to the callback. This matters for the reference sink writing to stderr: individual `fprintf`-style calls are usually atomic for short lines, but no ordering guarantee exists.

**Error-path suitability.** The function performs no allocation and takes no locks, so it is safe to call from allocation-failure handlers, from signal-adjacent contexts in C terms, and during encoder teardown. The only requirement is that `h` either be null or point to an object whose `param` is still alive.

## 6. Dependencies

Includes only `common.h`, which transitively supplies `x264_t`, `base.h`, the log-level constants and `x264_log_default`'s declaration. Calls `x264_log_default` (implemented in `base.c`) in the null-context case; otherwise calls through the function pointer in `x264_param_t`. Called from virtually every encoder module for diagnostics, warnings and debug traces.

## 7. Constants and tables

No constants or tables are defined here. The relevant numeric values are the public log levels listed in section 3; their ordering (lower is more severe) is the reason a single `<=` comparison implements the whole filter policy.

## 8. Performance notes

- The guarded path has one pointer/level comparison, so filtered debug messages are essentially free; keep format strings out of hot inner loops even so, because argument evaluation happens at the call site before the guard.
- There is no allocation, so the function is safe on error paths and during teardown.
- **Cache layout / false sharing:** none; the read of `i_log_level` is from a parameter object that is effectively immutable after encoder open, so it stays shared cleanly across threads.
- **Allocation/dispatch hierarchy:** the callback indirection is the only dispatch; unlike algorithm functions it is not CPU-feature-dispatched.
- Public reference: the x264 public API documentation for `pf_log` / `p_log_private` describes the callback contract users must honour.

## 9. References

- Public `x264.h`: `x264_param_t.pf_log`, `p_log_private`, `i_log_level`, and the `X264_LOG_*` constants.
- ISO C standard variadic-argument facilities (`va_start`, `va_end`, `va_list`) for the forwarding contract.
- x264 `base.c` reference sink (`x264_log_default`) described in `base.c.md`.

## 10. Verification checklist

- Install a counting callback, set `i_log_level` to `X264_LOG_WARNING`, call with levels ERROR, WARNING, INFO and DEBUG: exactly the first two may reach the callback, with the exact same format string and argument values.
- Call with a null context at every level: all messages must reach the default sink with the documented `x264 [<prefix>]: ` prefix on stderr (capture stderr in a harness).
- Verify that a message whose level is above the threshold does not invoke the callback even once (counter unchanged).
- Set `pf_log` to a callback that inspects its `p_log_private` argument; verify the exact private pointer is forwarded.
- In a multi-threaded encode with a thread-safe counting callback, verify no messages are lost or duplicated for a single known trigger (e.g. enabling debug-level headers); ordering across threads is not guaranteed.

Additional black-box checks:

- Install a callback that deliberately calls `x264_param_parse` on the same parameter object while logging; verify no internal state of the logging function is corrupted (the function holds no state across the call).
- Verify that a callback returning normally does not change the return value or side effects of the function that emitted the log (logging is observationally transparent).
- Set the threshold to `X264_LOG_DEBUG` and emit a message with a format containing `%%` and escaped quotes; verify the callback receives the exact format string rather than a pre-expanded one.
- Emit 10000 filtered debug messages in a tight loop and verify the callback counter remains zero and no measurable allocation occurs.
- Emit messages from several encoder threads with a callback that appends to a thread-safe list; verify the multiset of delivered messages equals the multiset emitted, allowing arbitrary order.
- In a build linking both bit depths, open one encoder of each depth, install distinct callbacks via their parameter objects, and verify each callback sees messages only from its own encoder.

## 11. Open questions

- UNKNOWN: whether the callback may be invoked concurrently by multiple encoder threads; the implementation imposes no locking, so a user callback requiring serialisation must provide it.
- UNKNOWN: no length limit is imposed on formatted output; sinks handling fixed-size console buffers must truncate themselves.
