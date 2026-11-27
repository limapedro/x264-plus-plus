# `common/opencl.h` - OpenCL handle abstraction

## 1. Header

Source path: `E:\_x264_dirty_stash\x264-source\common\opencl.h` (mirrored at `common/opencl.h`). Role in one sentence: declares the dynamically loaded OpenCL entry points and the two handle structures that own GPU context state and per-frame device buffers. Marker: **dirty-side behavioural specification**.

## 2. Purpose

x264's OpenCL lookahead is optional and must work without an OpenCL development library at build time or an OpenCL runtime at run time. This header achieves that by declaring function-pointer typedefs for the required API subset and two owner structs: one for process/encoder-level resources (library, context, device, queue, kernels, shared buffers) and one for per-frame device buffers. Only the first few dozen OpenCL types are included from a minimal local header.

## 3. Public interface

`OCL_API(ret, attr, name)` is a macro that declares a function-pointer typedef named `name##_func` for an OpenCL entry point; the header lists the platform, device, context, command-queue, memory-object, sampler, program, kernel, event and enqueue APIs. `OCL_DECLARE_FUNC(name)` declares a member of that type inside the function table.

`typedef struct { void *library; /* function pointers */ } x264_opencl_function_t`: a dynamically resolved API table. The required functions are: build/create program (source and binary), create command queue, context, buffer (including `clCreateImage2D`), kernel, enqueue copy/map/NDRange/read/write, finish, get command-queue info, device IDs/info, kernel work-group info, platform IDs, program/build info, supported image formats, release command queue/context/kernel/mem object/program, and set kernel argument. All are mandatory: loading fails if any is absent.

`NUM_IMAGE_SCALES = 4` (number of downscale levels for motion search), `MAX_FINISH_COPIES = 1024` (queued PCIe transfers before a flush), `PAGE_LOCKED_BUF_SIZE = 32 MiB` (pinned host buffer for transfers).

`typedef struct ... x264_opencl_t`: encoder-level handle with fields `ocl` (function table), `context`, `device`, `queue`, `lookahead_program`, `last_buf`, a page-locked buffer and its mapped host pointer plus occupancy, a `copies[]` array of `(src,dest,bytes)` records and `num_copies`, flags `b_device_AMD_SI` and `b_fatal_error`, two thread-priority fields, and groups of kernels/buffers for downscale, weightp, intra, hierarchical ME and bidir stages: `downscale_hpel_kernel`, `downscale_kernel1/2`, `luma_16x16_image[2]`, `weightp_hpel_kernel`, `weightp_scaled_images_kernel`, `weighted_scaled_images[4]`, `weighted_luma_hpel`, `memset_kernel`, `intra_kernel`, `rowsum_intra_kernel`, `row_satds[2]`, `hme_kernel`, `subpel_refine_kernel`, `mv_buffers[2]`, `lowres_mv_costs`, `mvp_buffer`, `mode_select_kernel`, `rowsum_inter_kernel`, `lowres_costs[2]`, `frame_stats[2]`.

`typedef struct ... x264_frame_opencl_t`: per-frame handle with `ocl`, `scaled_image2Ds[4]`, `luma_hpel`, `inv_qscale_factor`, `intra_cost`, `lowres_mvs0/1`, `lowres_mv_costs0/1`.

Functions: `x264_opencl_load_library`, `x264_opencl_close_library`, `x264_opencl_lookahead_init`, `x264_opencl_lookahead_delete`, `x264_opencl_frame_delete` (all templated).

## 4. State and data structures

Ownership hierarchy: `x264_opencl_function_t` owns the loaded shared-library handle and is shared by the encoder handle and every frame handle; `x264_opencl_t` owns the context, queue, program, kernels and reusable device buffers; each `x264_frame_opencl_t` owns its per-frame device buffers. `x264_opencl_lookahead_delete` must finish the queue before releasing resources; `x264_opencl_frame_delete` releases only frame-owned buffers and must not release the shared function table.

The `copies[]` array is a deferred-transfer log: records accumulate until a flush policy triggers `clFinish`, bounding the number of in-flight transfers. `last_buf` and `pl_occupancy` track which of the two luma images and how much of the pinned buffer is in use.

## 5. Algorithm / behaviour

Loading opens `OpenCL` on Windows or `libOpenCL.so` (framework path on macOS) and resolves every required symbol; a single missing symbol fails the whole load. Closing unloads the library and frees the table. The other three functions are implemented in `opencl.c.md`.

The structures encode two invariants: (1) no resource is released before its dependents (queue before context, program before kernels, library last); (2) frames created while OpenCL is active inherit the same function-table pointer (`frame->opencl.ocl = h->opencl.ocl`), so they can release their own buffers safely even if the encoder is being torn down.

## 6. Dependencies

Includes the project's minimal `extras/cl.h` with `CL_USE_DEPRECATED_OPENCL_1_1_APIS` (needed for `clCreateImage2D`). Included by `common.h` when `HAVE_OPENCL`, and by `frame.h` indirectly for the per-frame handle. Implemented by `opencl.c`; consumed by lookahead and frame allocation/deletion.

## 7. Constants and tables

`NUM_IMAGE_SCALES = 4`, `MAX_FINISH_COPIES = 1024`, `PAGE_LOCKED_BUF_SIZE = 32*1024*1024`. No data tables; kernel source lives in a generated `common/oclobj.h` array outside this header.

## 8. Performance notes

- **PCIe strategy:** a 32 MiB page-locked buffer avoids per-transfer allocation and gives the driver DMA-friendly memory; batching up to 1024 copies before `clFinish` keeps the queue deep without unbounded latency.
- **Cache layout:** four downscale levels trade memory for reduced search work; the two alternating `luma_16x16_image` buffers allow double-buffering between frames.
- **Dispatch hierarchy:** GPU kernels handle lookahead stages (downscale, hpel, weightp, intra cost, hierarchical ME, subpel refine, mode selection, row sums); CPU handles the actual encode. Kernel compilation is one-time.
- **AMD vectorisation:** `b_device_AMD_SI` distinguishes Southern Islands and newer devices with scalar-width registers, affecting build options; see `opencl.c.md`.
- **False sharing / allocation:** handles are per encoder and per frame, not per thread; device buffers are released deterministically at frame deletion.
- Public references: Khronos OpenCL 1.2 specification (contexts, queues, memory objects, events), AMD OpenCL optimization guide on vectorisation, NVIDIA/AMD DMA and pinned-memory documentation.

## 9. References

- Khronos OpenCL 1.2 API specification (`clCreateContext`, `clCreateCommandQueue`, `clCreateBuffer`, `clEnqueueNDRangeKernel`, `clFinish`).
- Khronos `cl_ext.h` extension constant for AMD SIMD instruction width.
- AMD OpenCL User Guide / optimization documentation on vectorised vs scalar registers.
- Microsoft `LoadLibraryW`/`GetProcAddress` and POSIX `dlopen`/`dlsym` documentation.

## 10. Verification checklist

- Compile a client that includes only `opencl.h` plus the local `cl.h`; verify all typedefs and constant values.
- Verify that `x264_opencl_load_library` fails cleanly when the OpenCL library is unavailable (return NULL, no crash).
- After a successful load, verify every required function pointer is non-null.
- Confirm `sizeof(x264_frame_opencl_t)` and its field order allow a frame to be deleted without an encoder handle (call `x264_opencl_frame_delete` on a frame with only `ocl` set and all device buffers null).
- Verify `NUM_IMAGE_SCALES == 4`, `MAX_FINISH_COPIES == 1024` and page-locked size is 32 MiB in a static check.
- Verify two encoders can hold separate `x264_opencl_t` handles while sharing one function table without double-unloading the library (reference-count behaviour is external; document what the implementation does).

## 11. Open questions

- UNKNOWN: whether the function table is reference-counted across multiple encoder instances; the header shows one pointer per encoder and no refcount.
- UNKNOWN: the exact meaning of `last_buf` beyond "index of last luma image buffer" is established in `opencl.c` usage, not in this header.
- UNKNOWN: whether thread-priority fields are actually applied on all platforms; they are declared here but their use is outside these files.
