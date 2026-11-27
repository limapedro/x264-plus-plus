# input/input.c — shared input helpers and memory-mapped frame support

*Dirty-side behavioural specification. Reference source: `input/input.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `input/input.c`
- **Role:** Implements the colourspace geometry table and picture-allocation/memory-mapping helpers declared in `input/input.h`.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Every demuxer needs to answer the same questions — how big is a plane of format X at resolution WxH, how should a picture be allocated, and how can a file be mapped into frame memory without copying. This file is the single implementation of those answers. Centralising them guarantees that raw, Y4M, and the library-backed demuxers all agree on plane ordering, stride alignment, and safe mapping behaviour. A clean-room encoder can reproduce these helpers exactly from the format geometry described below and does not need any library-specific code.

## 3. Public interface

All functions are declared in `input/input.h` and described in detail there. This file supplies:

- `const x264_cli_csp_t x264_cli_csps[]` — the colourspace descriptor array.
- `int x264_cli_csp_is_invalid( int csp )`
- `int x264_cli_csp_depth_factor( int csp )`
- `int64_t x264_cli_pic_plane_size( int csp, int width, int height, int plane )`
- `int64_t x264_cli_pic_size( int csp, int width, int height )`
- `int x264_cli_pic_alloc( cli_pic_t *pic, int csp, int width, int height )`
- `int x264_cli_pic_alloc_aligned( cli_pic_t *pic, int csp, int width, int height )`
- `int x264_cli_pic_init_noalloc( cli_pic_t *pic, int csp, int width, int height )`
- `void x264_cli_pic_clean( cli_pic_t *pic )`
- `const x264_cli_csp_t *x264_cli_get_csp( int csp )`
- `int x264_cli_mmap_init( cli_mmap_t *h, FILE *fh )`
- `void *x264_cli_mmap( cli_mmap_t *h, int64_t offset, int64_t size )`
- `int x264_cli_munmap( cli_mmap_t *h, void *addr, int64_t size )`
- `void x264_cli_mmap_close( cli_mmap_t *h )`

Contracts: allocation functions return 0 on success, -1 on allocation failure; size functions return 0 for invalid inputs; `get_csp` returns NULL for invalid formats.

## 4. State and data structures

The only global state is the constant descriptor table. All helper functions are pure with respect to global state except the allocation and mapping functions, which return resources the caller must release. `cli_mmap_t` carries per-file OS state for its lifetime; see the `input.h` spec for its field layout. Ownership: a `cli_pic_t` initialised with allocation owns its plane buffers until `x264_cli_pic_clean` is called; a mapping returned by `x264_cli_mmap` must be released with `x264_cli_munmap` using the same handle and original logical size.

## 5. Algorithm / behaviour

### Colourspace table construction
The table is a static array indexed by the base colourspace value. Each entry records a human-readable name, the plane count, a per-plane width scale and height scale (fractions expressed as floats), and the width/height alignment modulus required by that format. The values are the standard geometry of planar, semi-planar, and packed YUV/RGB layouts; they are enumerated in the `input.h` specification and are not repeated as code here.

### Validity and depth
Validity first masks away flag bits, then rejects a base value that is not strictly between NONE and the CLI maximum, rejects V210 (which has no descriptor), and rejects anything flagged `X264_CSP_OTHER` (a library-native pixel format). Depth factor returns 0 for invalid formats, otherwise 2 when `X264_CSP_HIGH_DEPTH` is set and 1 otherwise.

### Plane and picture sizing
Plane size multiplies picture width and height by that plane's two scale factors and by the depth factor, using 64-bit intermediates. Invalid csp or a plane index outside `[0, planes)` gives zero. Picture size sums the sizes of all planes of a valid csp.

### Picture initialisation (shared internal routine)
The routine zeroes the whole picture, chooses the plane count from the descriptor (zero for an invalid csp), and stores the given csp/width/height. For each plane it derives a stride from `width * width_scale[plane] * depth_factor`, rounded up to the requested alignment (1 for packed/basic allocation, the platform native alignment for aligned allocation). When allocation is requested it allocates `height * height_scale[plane] * stride` bytes per plane and returns -1 immediately if any allocation fails. The three public entry points differ only in the alignment (1 vs. native) and whether allocation happens. Cleaning iterates the valid planes, frees each pointer, and zeroes the structure — safe to call repeatedly because the zeroing leaves plane pointers null.

### Memory mapping initialisation
`x264_cli_mmap_init` extracts the file descriptor from the `FILE*`, stats it to learn the file size, then:
- On Windows: obtains the OS file handle, queries `GetSystemInfo` to derive the page mask and allocation-granularity mask, tries to resolve `PrefetchVirtualMemory` dynamically from kernel32 (so the build still works on older Windows), records the current process handle, and creates a read-only file mapping. Failure at any step returns nonzero.
- On POSIX with `HAVE_MMAP`: uses `_SC_PAGESIZE` for the alignment mask, stores the descriptor, and succeeds if the query was valid and the descriptor is non-negative.
- On platforms without either path: always fails.

### Mapping a frame
The requested offset is rounded *down* to the granularity boundary and the size is grown by the same amount; the returned pointer is advanced back up by that amount so the caller sees exactly the requested offset. Negative offsets/sizes and integer-overflow cases are rejected. A 64-byte padding is always added to the mapping length.

- **Windows fast path:** when the padded end stays inside the file, a read-only view of the padded size is created and (if available) `PrefetchVirtualMemory` is asked to read it in; the pointer is returned shifted by the alignment.
- **Windows end-of-file path:** when the padding would pass the file end, a view of the unpadded size is created, then a private read/write anonymous mapping of the padded size is allocated, the valid bytes are copied into it (offset by the alignment), the original view is unmapped, and the private buffer is returned. This guarantees overreads stay inside committed memory.
- **POSIX path:** a read-only private mapping of `size + 64` bytes is created, a `MADV_WILLNEED`/`POSIX_MADV_WILLNEED` hint is issued, and if the mapping reaches or exceeds the file end the tail padding page is remapped with `MAP_FIXED` onto the last valid page of the file, so reads in the padding return valid data instead of faulting.

Any failure returns NULL.

### Unmapping and close
`munmap` reconstructs the true mapping base by masking off the alignment bits of the address, then on POSIX unmaps `size + padding + (addr - base)` bytes (with overflow checks) and on Windows unmaps the view from that base. `mmap_close` only has work on Windows, where it closes the file mapping handle.

## 6. Dependencies

- Header: `input/input.h`; transitively `x264cli.h`.
- Platform APIs: `<io.h>` and Win32 mapping APIs on Windows; `<sys/mman.h>`, `<unistd.h>`, `_SC_PAGESIZE`, `MADV_WILLNEED`/`POSIX_MADV_WILLNEED` on POSIX.
- `x264_malloc`/`x264_free` from the common allocator layer, `ALIGN`/`NATIVE_ALIGN` macros.
- Called by: raw.c, y4m.c (mapping and picture helpers); resize.c and internal.c (picture copy/alloc helpers); the source filter.

## 7. Constants and tables

- `MMAP_PADDING = 64` bytes: safety margin for third-party filters/scalers that over-read. The value is chosen to cover typical SIMD overreads; it is not derived from a standard.
- Alignment masks are `page_size - 1` (Windows) and `sysconf(_SC_PAGESIZE) - 1` (POSIX); allocation granularity on Windows may be larger than the page size, so two masks are stored.
- The descriptor table's numeric geometry is documented in the `input.h` spec.

## 8. Performance notes

- Hotness: helpers are called once per frame; the work is O(planes) and cheap, but mapping/unmapping can dominate for small frames.
- **Zero-copy:** prefer `x264_cli_mmap` for regular files; this is the fastest path and avoids both a read syscall and a buffer copy per frame.
- **Readahead:** the WILLNEED/Prefetch hints should be retained; forcing page faults by touching pages manually is slower (the reference comments explicitly note this).
- **Reservation sizing:** allocate pictures once via `picture_alloc` and reuse; never call `x264_cli_pic_alloc` per frame. Keep the 64-byte padding in every mapping.
- **Branch behaviour:** validity checks are cold; the hot per-plane loop has a constant trip count and folds well.
- References: OS documentation for `mmap`/`MapViewOfFile` and readahead; x264 CLI performance guidance (<https://x264.readthedocs.io/>).

## 9. References

- `input/input.h` (interface contract, companion specification).
- POSIX `mmap(2)`, `madvise(2)`, `sysconf(3)`; Win32 `CreateFileMapping`, `MapViewOfFile`, `GetSystemInfo`, `PrefetchVirtualMemory`.
- ITU-T H.264 / ISO 14496-10 for planar chroma sampling geometry.

## 10. Verification checklist

1. For a known resolution and format, compare `x264_cli_pic_plane_size` against a hand-computed byte count (include high-depth doubling).
2. Check that an invalid csp yields zero from both size functions and a null from `get_csp`.
3. Allocate aligned and unaligned pictures; verify stride divisibility and that all planes are non-overlapping.
4. Clean a picture twice and confirm no crash/double-free.
5. Map several consecutive frames of a regular file; verify returned data matches a plain `fread` of the same bytes.
6. Verify 64-byte overread safety at end-of-file on POSIX and Windows.
7. Verify `munmap` followed by a new map of the same file succeeds repeatedly (no leaked mappings).

## 11. Open questions

- UNKNOWN: exact behaviour when `HAVE_MMAP` is defined but `_SC_PAGESIZE` is not — the code path returns failure without setting `align_mask`; callers should treat a nonzero init result as fatal.
- UNKNOWN: whether the Windows fallback copy path handles zero-length final frames identically to the POSIX remap path; not exercised in ordinary inputs.
