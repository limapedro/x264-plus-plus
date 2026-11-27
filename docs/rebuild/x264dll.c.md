# x264dll.c — rebuild specification

## 1. Header

- Source path: `x264dll.c` (root of the reference tree).
- Role: supplies the Windows dynamic-link-library entry point that initializes and tears down the pthreads-win32 runtime when the static pthread library is linked into the DLL.
- Marker: **dirty-side behavioural specification** — behaviour and structure only, no source text.

## 2. Purpose

On Windows, libx264 can be built as a shared library (`libx264-<API>.dll`). When the build uses a statically linked pthreads-win32 implementation, that library needs explicit process- and thread-level initialization callbacks; they cannot be run from a C constructor because pthreads-win32 requires them to be invoked from the loader's `DllMain` notification path. This tiny translation unit exists solely to provide that hook. It is compiled into the shared library only on Windows and only when a DLL (SONAME) is requested; static-library and CLI-only builds never include it.

## 3. Public interface

- `BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)` — the standard Windows DLL entry point. Return value is always `TRUE` (success). The three input arguments are not used for any decision other than `fdwReason`.
  - `hinstDLL` — module instance handle; ignored.
  - `fdwReason` — one of the four loader notifications described below.
  - `lpvReserved` — reserved loader value; ignored.
- No functions or data are exported from this file for use by applications. The only observable behaviour is the side effect on the pthread runtime during loader notifications.

## 4. State and data structures

No mutable state of its own. The file relies on these externally supplied facilities:

- The pthreads-win32 API functions `pthread_win32_process_attach_np`, `pthread_win32_process_detach_np`, `pthread_win32_thread_attach_np`, `pthread_win32_thread_detach_np`, declared through `common/base.h`'s Windows threading layer.
- The compile-time switch `PTW32_STATIC_LIB`, defined by `configure` only when a static pthreads-win32 library is detected (one of the `cc_check` variants that adds `-DPTW32_STATIC_LIB` to the link test). When it is absent the whole body compiles to "return true" and no pthread calls are emitted.

## 5. Algorithm / behaviour

The entry point is a four-way dispatch on the reason code, guarded by `PTW32_STATIC_LIB`:

1. **Process attach** (`DLL_PROCESS_ATTACH`): call the pthread process-attach routine. The implementation deliberately does *not* break out of the switch here; control falls through into the thread-attach case, so the initializing thread is also registered. This is the only place where fall-through is intentional.
2. **Thread attach** (`DLL_THREAD_ATTACH`): call the pthread thread-attach routine, then break. Every newly created thread in the process that loads this DLL gets its pthread per-thread state initialized.
3. **Thread detach** (`DLL_THREAD_DETACH`): call the pthread thread-detach routine, then break.
4. **Process detach** (`DLL_PROCESS_DETACH`): call the pthread thread-detach routine for the calling thread, then the pthread process-detach routine; break.

After the switch (or immediately, if `PTW32_STATIC_LIB` is not defined), the function returns `TRUE`. It never performs any other work: no allocator setup, no console handling, no encoder initialization.

Consequences to preserve:

- Process-attach must register both the process and the calling thread; failing to fall through would leave the first thread without pthread thread state.
- Process-detach must detach the calling thread *before* detaching the process, mirroring the attach order in reverse at the thread level.
- The function must never block, allocate encoder structures or call arbitrary user code — the Windows loader serializes `DllMain` calls and holds the loader lock, so doing so risks deadlock.
- When pthreads is not statically linked (for example a dynamically linked win32 pthread library, or MSVC native threads), no hook is required and the function is a no-op returning success.

## 6. Dependencies

- Includes `common/base.h` (which supplies the pthread declarations, the `PTW32_STATIC_LIB` test context and Windows compatibility definitions) and the Windows SDK header `windows.h`.
- Linked into: the shared library target `$(SONAME)` on Windows, via `SRCSO`/`OBJSO` in the `Makefile`. It is not part of `libx264.a` and not part of the CLI executable.
- `configure` controls whether this file is compiled at all: the `Makefile` adds `x264dll.c` to `SRCSO` only when `SYS == WINDOWS` and `SONAME` is non-empty, and adds the resource object as well when a resource compiler is available.
- `configure` controls whether the body does anything via `PTW32_STATIC_LIB`.

## 7. Constants and tables

| Item | Value / meaning |
| --- | --- |
| `PTW32_STATIC_LIB` | Defined by `configure` for the static pthreads-win32 link variants; enables the whole dispatch body |
| `DLL_PROCESS_ATTACH` / `DLL_PROCESS_DETACH` | Loader notifications for process lifetime |
| `DLL_THREAD_ATTACH` / `DLL_THREAD_DETACH` | Loader notifications for each thread |
| Return value | `TRUE` for every notification; no failure path exists |

There are no lookup tables, constants of x264's own, or global variables.

## 8. Performance notes

This code runs at most a few times per process and per thread; it is never on a per-frame or per-macroblock path. The cost of the extra pthread attach at process start is negligible relative to encoder setup. The important performance-related property is negative: because `DllMain` runs under the loader lock, it must not perform slow or lock-taking work. There is no SIMD involvement and no cache behaviour worth considering.

Optimization guidance (and pitfalls) for an independent implementation:

- Keep the function free of logging, heap allocation and calls into x264's encoder API; do not "improve" it by adding diagnostics.
- Preserve the fall-through from process attach to thread attach — replacing it with a duplicate call in each branch is fine functionally, but omitting the thread registration is not.
- Build it only into the DLL: including it in a static archive would add an unused `DllMain` symbol and potentially conflict with an application's own entry point.
- Public reference: the pthreads-win32 documentation on process/thread attach helpers, and the Microsoft `DllMain` documentation describing loader-lock restrictions.

## 9. References

- Microsoft Windows SDK documentation for `DllMain` and the `DLL_PROCESS_ATTACH`/`DLL_THREAD_ATTACH` reason codes.
- pthreads-win32 project documentation for `pthread_win32_process_attach_np` and the related `_np` helper functions.
- Reference tree `configure` — the `PTW32_STATIC_LIB` detection and the Windows shared-library naming rules.
- Reference tree `Makefile` — the `SRCSO`/`OBJSO` conditional that compiles this file into `$(SONAME)`.

## 10. Verification checklist

- Build a Windows shared-library configuration and confirm the resulting DLL exports/uses no extra public symbols beyond the normal x264 API (the entry point is not part of the exported API surface).
- Under a configuration with `PTW32_STATIC_LIB` defined, attach a debugger or instrument the pthread helpers and confirm the call sequence on process load: process-attach then thread-attach; on unload: thread-detach then process-detach.
- Under a configuration without `PTW32_STATIC_LIB`, confirm that no pthread helper calls occur and that loading the DLL succeeds.
- Confirm every notification reason (including thread attach/detach triggered by creating and joining a worker thread) returns success and does not prevent library load.
- Confirm that a static-library build does not contain this translation unit.
- Confirm the library can be unloaded and reloaded repeatedly without leaking per-thread state (observable through repeated create/join cycles succeeding).

## 11. Open questions

- The behaviour of pthreads-win32 when `DLL_PROCESS_DETACH` occurs during process termination (`lpvReserved` non-null) is delegated to that library; details are **UNKNOWN** from this file.
- Whether the resource-compiler object (`x264res.dll.o`) carries any additional manifest-side threading declarations is outside this file's scope.
