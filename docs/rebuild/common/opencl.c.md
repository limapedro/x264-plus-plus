# `common/opencl.c` - OpenCL loading, device selection and kernel setup

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\opencl.c` (mirrored at `common/opencl.c`). Role in one sentence: dynamically loads OpenCL, selects a compatible GPU, compiles or reuses cached kernels, creates the shared device resources, and tears them down safely. Marker: **dirty-side behavioural specification**.

## 2. Purpose

GPU-accelerated lookahead is optional and must degrade gracefully. This file centralises the fragile parts: library symbol resolution, platform/device enumeration, image-format compatibility checks, kernel compilation with a persistent binary cache, host-side pinned memory, and a workaround for hybrid graphics systems where OpenCL misbehaves.

## 3. Public interface

Definitions of the five functions declared in `opencl.h.md`. Internal helpers: `opencl_cache_load`, `opencl_cache_save`, `opencl_compile`, `opencl_lookahead_alloc`, `opencl_lookahead_free`, `opencl_error_notify`, `detect_switchable_graphics`, plus `adl_malloc_wrapper` for AMD's Display Library.

## 4. State and data structures

`x264_opencl_function_t` is allocated zeroed and filled by `x264_opencl_load_library`; the library handle is closed only after all symbols are resolved successfully. `x264_opencl_t` inside `x264_t` holds the context/queue/program/kernels and shared buffers. The binary cache file stores four NUL-terminated strings (device name, vendor, driver version, kernel source hash) followed by the program binary; the strings are validated before reuse so a stale binary from a different driver is ignored. `thread_control` in `win32thread.c` is unrelated; here the AMD ADL function pointers are resolved locally inside `detect_switchable_graphics`.

## 5. Algorithm / behaviour

**Library loading.** On Windows, `LoadLibraryW(L"OpenCL")`; on Linux `dlopen("libOpenCL.so", RTLD_NOW)`; on macOS the OpenCL framework path. A macro loads each symbol by name; any required symbol missing causes a jump through one of three distinct failure labels: close library, free struct, or return NULL. `x264_opencl_close_library` closes and frees; a null input is ignored. On success all pointers are non-null.

**Binary cache.** `opencl_cache_load` opens `param.psz_clbin_file`, reads the whole file, then consumes four length-prefixed-by-NUL strings from its front, comparing them against the current device name, vendor, driver version and `x264_opencl_source_hash`. A mismatch or short file fails and returns NULL; a match passes the remaining bytes to `clCreateProgramWithBinary` for the selected device. `opencl_cache_save` queries `CL_PROGRAM_BINARY_SIZES` and `CL_PROGRAM_BINARIES`, then writes the four strings and the binary back. Any failure logs at INFO level and leaves no cache file.

**Kernel compilation.** Query device name/vendor/driver version. Assume vectorised kernels for vendor "Advanced Micro Devices, Inc."; on such systems, first call `detect_switchable_graphics` and disable OpenCL entirely if a hybrid configuration is found. For AMD, also query the AMD-specific SIMD instruction width; a value of 1 means scalar registers (Southern Islands or newer) and clears the vectorise flag while setting `b_device_AMD_SI`. Try the cached binary; on miss, create a program from the generated source array (`x264_opencl_source` in `common/oclobj.h`) and build with `-DVECTORIZE=1` when vectorising. On build failure, query the build log and write it to `x264_kernel_build_log.txt`, logging a warning. On success, save the binary to the cache.

**Lookahead allocation.** Requires a nonzero `rc.i_lookahead`. Create twelve kernels by name: `mb_intra_cost_satd_8x8`, `sum_intra_cost`, `downscale_hpel`, `downscale1`, `downscale2`, `memset_int16`, `weightp_scaled_images`, `weightp_hpel`, `hierarchical_motion`, `subpel_refine`, `mode_selection`, `sum_inter_cost`; each must be created successfully. Allocate a 32 MiB buffer with `CL_MEM_WRITE_ONLY|CL_MEM_ALLOC_HOST_PTR` and map it for both reading and writing into host memory. Failure calls `x264_opencl_lookahead_delete`.

**Device selection.** Query platform count; iterate platforms and request GPU devices. For each device, honour `param.opencl_device_id` as an exact opaque identifier and `param.i_opencl_device` as a skip count of matching devices (decrementing it on each candidate). Require `CL_DEVICE_IMAGE_SUPPORT` and support for two specific image formats: `CL_R`/`CL_UNSIGNED_INT32` and `CL_RGBA`/`CL_UNSIGNED_INT8`. Create a context with the error callback, query supported formats, and reject a device missing either format; log at WARNING if it was explicitly selected, DEBUG otherwise. Create a command queue on the first acceptable device, adopt the context, and stop searching. Default the cache filename to `x264_lookahead.clbin` if unset. If no device is found, log a warning; otherwise allocate kernels and buffers.

**Error callback.** Any context error notification is treated as fatal: set `param.b_opencl = 0`, set `b_fatal_error`, log the message and an "aborting encode" notice. The encoder then continues on CPU.

**Hybrid-graphics detection.** On Windows load `atiadlxx.dll` then `atiadlxy.dll`; on Linux `libatiadlxx.so`. Resolve `ADL_Main_Control_Create/Destroy`, `ADL_Adapter_NumberOfAdapters_Get` and `ADL_PowerXpress_Scheme_Get`; initialise ADL with an allocation callback backed by `x264_malloc`, enumerate adapters and query the PowerXpress scheme. If any adapter's scheme range is at least `ADL_PX_SCHEME_DYNAMIC` (2), return 1 (hybrid detected). Always destroy ADL and close the library before returning.

**Teardown.** `x264_opencl_lookahead_delete` returns immediately without a function table, otherwise finishes the queue, releases all twelve kernels, the program, the page-locked buffer, all named shared buffers (two luma images, four weighted scaled images, weighted hpel, two row-SATD buffers, two MV buffers, MV costs, MVP buffer, two lowres-cost buffers, two frame-stats buffers), then the queue and context. `x264_opencl_frame_delete` releases the frame's four scaled images and seven named per-frame buffers, but not the function table.

### Additional behavioural detail

**Failure paths and logging levels.** Library loading, platform enumeration, device enumeration, kernel creation and pinned-buffer allocation all log warnings and return failure; cache read/write and build-log issues log at INFO level because they do not disable encoding. A missing cache file is not an error: the program source is compiled and then cached. An unreadable or stale cache file is silently ignored after its header strings fail to match.

**Cache-file format contract.** The four identity strings are written with `fputs` followed by a newline byte each, then the raw program binary is appended. The reader consumes each string by comparing a prefix and then advancing past the string length plus one terminator byte. It checks that at least `len+1` bytes remain before comparing, so a truncated file fails safely. The binary length passed to `clCreateProgramWithBinary` is the number of bytes left after the four headers. A clean implementation must reproduce this framing to interoperate with existing cache files.

**Device selection precedence.** `opencl_device_id` is an exact match filter applied before capability checks; a non-matching device is skipped silently. `i_opencl_device` is a skip counter applied after capability checks, so it counts only *compatible* devices. If both are set, the exact filter wins first and the skip counter applies among matching devices. The search order is platform order, then GPU device order within a platform, and stops at the first fully acceptable device.

**Error callback consequences.** The callback runs on an OpenCL error and immediately disables OpenCL for that encoder. It does not attempt to release resources or terminate the process; subsequent lookahead calls must notice `b_opencl == 0` and fall back. `b_fatal_error` is a sticky flag that prevents re-enablement. The callback receives an opaque user pointer that is the encoder handle, so the encoder must still be alive when the context reports an error - teardown finishes the queue before releasing the context for this reason.

**ADL workaround conditions.** The hybrid-graphics check runs only for AMD vendors (vectorise assumed). ADL is loaded dynamically; missing DLL/library, missing functions, failed `ADL_Main_Control_Create`, or failed adapter enumeration all result in "no hybrid detected" rather than an error. A PowerXpress scheme range of at least 2 marks the system as switchable and disables OpenCL. ADL's allocation callback uses x264's aligned allocator, matching the rest of the build.

**Teardown ordering rationale.** Finishing the queue before releasing resources is required because OpenCL commands are asynchronous: without `clFinish`, a kernel could still read a buffer that has just been released. Kernels are released before the program, the program before the context, and the command queue before the context. Per-frame buffers are released by `x264_opencl_frame_delete` when a frame is deleted, independently of encoder teardown; frames created before OpenCL was disabled still carry the shared function table, so their buffers can be released safely.

**Sizing constants in context.** Four image scales mean each frame owns four scaled image buffers plus a half-pel luma buffer; two alternating luma images allow frame-to-frame pipelining. `MAX_FINISH_COPIES` bounds how many pending transfers can accumulate before a flush; `PAGE_LOCKED_BUF_SIZE` bounds the pinned staging area. Together they control the latency/bandwidth trade-off of GPU lookahead.

## 6. Dependencies

Includes `common.h`, Windows headers or `dlfcn.h`, `common/oclobj.h` (generated kernel source), and the local `extras/cl.h` via `opencl.h`. Calls `x264_fopen`/`fputs`/`fwrite` for the cache, `x264_malloc/free`, `x264_log`, `x264_pthread_*` indirectly via thread priorities elsewhere, and the resolved OpenCL entry points. Called by encoder open/close and frame deletion.

## 7. Constants and tables

`CL_DEVICE_SIMD_INSTRUCTION_WIDTH_AMD = 0x4042`; `ADL_OK = 0`; `ADL_PX_SCHEME_DYNAMIC = 2`; default cache filename `x264_lookahead.clbin`; build log filename `x264_kernel_build_log.txt`. Kernel count 12; image scale count 4; page-locked size 32 MiB; maximum queued copies 1024 (declared in `opencl.h`). Required image formats listed in section 5.

## 8. Performance notes

- **PCIe/DMA:** the 32 MiB pinned buffer is mapped once and reused for host-device transfers; batching up to 1024 copies before a finish avoids a synchronisation per frame. Preserve both behaviours.
- **Cache layout:** compiled program binaries are cached on disk with a four-field identity header, avoiding compilation cost on subsequent runs at identical driver versions. The binary itself is large but read sequentially.
- **Dispatch hierarchy:** twelve kernels cover the GPU part of lookahead; the CPU continues encoding while lookahead runs asynchronously, so queue depth is a performance knob.
- **AMD vectorisation:** build option `-DVECTORIZE=1` is applied only to pre-SI AMD GPUs; SI and newer use scalar-width code paths. This is a correctness/performance choice driven by `CL_DEVICE_SIMD_INSTRUCTION_WIDTH_AMD`.
- **False sharing / allocation:** all device buffers are owned by one encoder or one frame and released explicitly; the pinned buffer is shared, so avoid concurrent writes from multiple threads without external coordination.
- Public references: Khronos OpenCL 1.2 spec, AMD OpenCL optimization guide (vectorisation, pinned memory), Khronos `cl_ext.h`, AMD ADL documentation.

## 9. References

- Khronos OpenCL 1.2 API specification, sections on programs, kernels, command queues, memory objects and events.
- Khronos `cl_ext.h` for `CL_DEVICE_SIMD_INSTRUCTION_WIDTH_AMD`.
- AMD Display Library (ADL) SDK documentation for PowerXpress scheme queries.
- Microsoft `LoadLibraryW`/`GetProcAddress`, POSIX `dlopen`/`dlsym`.
- x264 `doc/threads.txt` for lookahead threading context.

## 10. Verification checklist

- On a machine without OpenCL, verify library load returns NULL and the encoder proceeds with `b_opencl = 0` (observable from log output).
- With OpenCL present, verify all twelve kernels are created; remove/rename one kernel name in a test build and verify the documented warning and failure path.
- Verify the cache file begins with four newline-terminated strings and that changing any one (device name, vendor, driver version, source hash) forces a recompile (observable by log message).
- Select an explicitly incompatible device via `opencl-device-id` and verify a WARNING (not DEBUG) is logged and another device is chosen or the whole init fails.
- Set `i_opencl_device` to 1 and verify the first matching device is skipped.
- Force `detect_switchable_graphics` to return 1 (stubbed ADL) and verify OpenCL is disabled with the "switchable graphics detected" message.
- Force a context error callback and verify `b_fatal_error` is set, `b_opencl` becomes 0, and the encode continues to completion on CPU.
- Delete an encoder and then a frame created from it; verify no use-after-free of the shared function table under a memory checker.

Additional black-box checks:

- Verify that with `b_opencl = 0` no OpenCL symbols are loaded (instrument the loader in a harness) and no cache file is written.
- Verify that a cache file written by one encoder can be read by a second encoder on the same device without recompilation (observe the absence of the "Compiling OpenCL kernels" message).
- Verify that corrupting the middle of the binary payload causes `clCreateProgramWithBinary` to fail and the source path to be used instead.
- Verify that a device with image support but without `CL_R`/`CL_UNSIGNED_INT32` is rejected, and that the log level of the rejection message depends on whether the device was explicitly selected.
- Verify that setting `i_opencl_device` higher than the number of compatible devices leaves OpenCL disabled with a warning.
- Verify that all shared buffers are released exactly once by teardown and all per-frame buffers by frame deletion (instrument the release function in a harness).
- Verify that a fatal error callback sets both flags and that a subsequent encode call on the same encoder completes on CPU without touching the queue.
- Verify that the kernel build log file is created only on a forced build failure and contains the compiler diagnostics.
- Verify that on a non-AMD vendor the ADL hybrid-graphics check is never invoked (instrument `LoadLibraryW`/`dlopen`).

## 11. Open questions

- UNKNOWN: the exact list of device image formats required by every kernel; only two format pairs are checked here, and a device could pass the check yet fail at kernel arg setup.
- UNKNOWN: whether `copies[]`/`num_copies` and `pl_occupancy` are consumed by code outside this file (lookahead module); their update policy is not visible here.
- UNKNOWN: behaviour when multiple processes share the same `x264_lookahead.clbin` path; no locking or atomic rename is performed.
- UNKNOWN: how thread priorities are applied on non-Windows platforms; fields are declared but not used in this file.
