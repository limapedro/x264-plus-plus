# `extras/cl.h` — dirty-side behavioural specification

## 1. Header

- Source path: `extras/cl.h`.
- Role: the Khronos OpenCL 1.2 C API header: opaque object types, enumerations, error codes, version macros and declarations of all platform/context/queue/memory/program/kernel/event functions.
- Marker: this file is a **dirty-side behavioural specification**; the API surface is summarised in our own words.

## 2. Purpose

The encoder's optional OpenCL path needs the OpenCL host API declarations in a form that can be vendored into the source tree without requiring an external SDK at build time. This header provides the standard OpenCL 1.2 interface, including deprecated 1.1 entry points, so the same source compiles against this copy or a system installation.

## 3. Public interface

### Opaque object handles

Pointer types to incomplete structures: `cl_platform_id`, `cl_device_id`, `cl_context`, `cl_command_queue`, `cl_mem`, `cl_program`, `cl_kernel`, `cl_event`, `cl_sampler`.

### Scalar and property types

`cl_bool` (an unsigned integer, explicitly not guaranteed to match a kernel-side bool), `cl_bitfield`, and property/info enumerations for devices, contexts, queues, memory objects, samplers, programs, kernels, events and profiling. `cl_context_properties` and partition properties are pointer-sized integers.

### Structures

- `cl_image_format`: channel order and channel data type.
- `cl_image_desc`: image type, width/height/depth/array size, row/slice pitch, mip level count, sample count and an associated buffer.
- `cl_buffer_region`: origin and size for sub-buffer creation.

### Error codes

A contiguous negative range beginning with success (0) and covering device/compiler unavailability, allocation failures, resource limits, profiling issues, overlapping copies, image format problems, build/compile/map failures, misaligned sub-buffer offsets, execution-status errors and invalids. A final value denotes a vendor-specific error range.

### Version and enumerations

Version macros for OpenCL 1.0, 1.1 and 1.2. Platform info constants include profile, version, vendor, extensions and a numeric version identifier; device info constants cover address-space bits, max compute units, work-item dimensions, local/global memory sizes, cache properties, vector widths, compiler availability, driver version and OpenCL C version. Additional enums define device types, fp configurations, cache/local memory types, execution capabilities, queue properties, memory flags, channel orders/types, addressing/filter modes, map flags, build status, kernel argument qualifiers, command types and profiling info.

### Function groups

- **Platform/device**: enumerate platforms and devices, query info, create sub-devices, retain/release devices.
- **Contexts**: create from explicit devices or from a device type, retain/release, query info.
- **Command queues**: create, retain/release, query info.
- **Memory objects**: create buffers, sub-buffers and images; retain/release; query supported formats and object/image info; register destructor callbacks.
- **Samplers**: create, retain/release, query info.
- **Programs**: create from source, binaries or built-in kernels; build, compile and link; unload platform compiler; query program/build info; retain/release.
- **Kernels**: create one or many, retain/release, set arguments, query kernel info, argument info and work-group info.
- **Events**: wait, query info, create user events, set status, register callbacks, query profiling info; retain/release.
- **Synchronisation and commands**: flush, finish, enqueue reads/writes/copies/fills for buffers and images (including rectangular variants and buffer↔image copies), map/unmap, migrate objects, NDRange kernels, single-task and native-kernel enqueue, markers and barriers with wait lists.
- **Extensions and deprecated APIs**: query extension function addresses per platform, plus deprecated 1.1 forms for image creation, marker/wait/barrier and compiler unloading.

## 4. State and data structures

All state lives inside opaque objects owned by the OpenCL implementation. The host holds only pointers. Reference counting is explicit: retain increases it, release decreases it; the application must balance calls. Command queues order work; events track completion and can carry profiling timestamps. Memory objects are shared between host and device according to their flags, and mapping must be paired with unmapping.

## 5. Algorithm / behaviour

The header is declarative, but it encodes the following API contract:

1. Discover platforms with `clGetPlatformIDs`, then devices with `clGetDeviceIDs`.
2. Create a context from selected devices (or a device type), then one or more command queues.
3. Create memory objects and kernels; set kernel arguments by index with pointer-sized argument data.
4. Enqueue kernels with an N-dimensional range and an optional event wait list; the returned event can be waited on or profiled.
5. Read results back with buffer/image read or map commands and finish/flush the queue.
6. Release every retained object exactly once.

Version suffixes on declarations (`CL_API_SUFFIX__VERSION_1_0`, `_1_1`, `_1_2`) are annotations expanded by `cl_platform.h.md` to availability or deprecation attributes. Deprecated 1.1 functions are marked so builds can opt into or out of warnings via feature macros.

## 6. Dependencies

- **Includes**: `cl_platform.h` from the same directory, which supplies scalar types, calling conventions and vector types.
- **Consumers**: the OpenCL encoder implementation, which builds kernels from embedded source strings (see `tools/cltostr.sh.md`) and dispatches them through these functions.
- **Related**: `extras/intel_dispatcher.h.md` handles CPU-side dispatcher initialisation, not OpenCL.

## 7. Constants and tables

| Item | Meaning |
| --- | --- |
| `CL_SUCCESS` = 0 | Successful call. |
| Negative error range | Device/compiler/memory/resource/build/map and invalid-value failures. |
| Vendor range | Implementation-specific errors. |
| Version macros 1.0/1.1/1.2 | API generation supported by this header. |
| Platform info | Profile, version, vendor, extensions, numeric version. |
| Device info | Capabilities and limits consumed by kernel tuning. |
| Deprecated prefix/suffix macros | Control availability and deprecation warnings. |

## 8. Performance notes

- The API is deliberately asynchronous: enqueue calls return immediately; `clFinish` or event waits synchronise. Overlapping host work with device work is the main performance lever.
- Buffer and image read/write commands can use blocking or non-blocking modes; non-blocking modes with events allow pipelines.
- Kernel work-group size should be chosen from `clGetKernelWorkGroupInfo` values; a mismatch can severely reduce throughput.
- The header itself has no runtime cost.

### 8.1 Relation to faster-kernel development

OpenCL kernels can be iterated alongside CPU SIMD kernels:

1. Use the per-kernel conformance harness (`tools/checkasm.c.md`) for CPU kernels and an equivalent GPU reference comparison for OpenCL kernels; both should use the same input generation style.
2. Check `clGetDeviceInfo` limits before choosing work-group sizes; tuning these is often a bigger win than instruction-level changes.
3. Verify the embedded source string and its digest are regenerated after every kernel edit (`tools/cltostr.sh.md`).
4. Use decoder-comparison regression (`doc/regression_test.txt.md`) as the final gate for bitstream-equivalent output.

References: OpenCL 1.2 specification; `extras/cl_platform.h.md`; `tools/cltostr.sh.md`.

## 9. References

- The source file itself, `extras/cl.h`, carrying the Khronos Group licence and revision 2008–2012.
- The OpenCL 1.2 specification published by the Khronos Group.
- `extras/cl_platform.h.md` for the platform types and API-call macros.
- `tools/cltostr.sh.md` for generation of embedded kernel source strings.

## 10. Verification checklist

- A C translation unit can include the header without an external OpenCL SDK.
- All opaque handle types are pointer-to-incomplete-struct types.
- `CL_SUCCESS` is zero and error codes are negative and distinct.
- Every listed function group has declarations for retain/release where applicable.
- Deprecated functions are annotated and can be suppressed or enabled via the documented macros.
- `cl_image_desc` fields match the documented order and types.
- Calls against a minimal OpenCL implementation succeed for platform/device/context/queue creation and kernel enqueue.

## 11. Open questions

- UNKNOWN: which subset of these functions the encoder's OpenCL path actually calls; the header exposes the full API.
- UNKNOWN: whether the vendored copy tracks a specific Khronos revision beyond the 2012 copyright and the `cl_platform.h` revision string.
- UNKNOWN: how extension functions beyond the core set are resolved at runtime; only the address-query functions are declared here.
