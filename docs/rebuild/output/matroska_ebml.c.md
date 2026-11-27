# output/matroska_ebml.c — EBML/Matroska writer implementation

*Dirty-side behavioural specification. Reference source: `output/matroska_ebml.c`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/matroska_ebml.c`
- **Role:** Implements a minimal EBML writer: nested contexts with back-patched size fields, header/segment/track emission, cluster management, SimpleBlock frames, and duration patching.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

Matroska requires EBML variable-length element IDs and sizes, and its clusters group frames with 16-bit relative timestamps. This file implements exactly the subset needed for video-only x264 output: a generic buffered context mechanism, fixed header elements, and one-block-per-frame writing with automatic cluster rollover. It is the lowest-level container writer in the tree and has no external dependencies.

## 3. Public interface

Implements the functions declared in `output/matroska_ebml.h`: `mk_create_writer`, `mk_write_header`, `mk_start_frame`, `mk_add_frame_data`, `mk_set_frame_flags`, `mk_close`. Signatures and call order are documented in that spec.

## 4. State and data structures

`mk_context` is a buffered EBML element node with: singly-linked list pointers (`next`, `prev` pointer-to-pointer, `parent`), owner writer, element `id` (0 when its ID/size has already been flushed), and a growable `data` buffer (`d_cur`, `d_max`). Active and free contexts are kept on intrusive lists owned by the writer; closed contexts return to the freelist rather than being freed, so buffers are reused.

`mk_writer` holds: `fp`, `duration_ptr` (absolute file offset of the Duration float), `root`/`cluster`/`frame` contexts, `freelist`/`actlist`, `def_duration`, `timescale`, `cluster_tc_scaled`, `frame_tc`/`max_frame_tc`, and boolean-ish bytes `wrote_header`/`in_frame`/`keyframe`/`skippable`.

Constants: `CLSIZE = 1048576` bytes (1 MiB) — cluster size threshold after which a cluster is closed.

Invariants: a context's ID is written to its parent only when the context is closed (except a top-level Segment, whose ID is flushed early with a placeholder size); a frame's bytes accumulate in `frame` until flushed as a SimpleBlock; at most one cluster is active.

## 5. Algorithm / behaviour

**Dynamic buffers.** `mk_append_context_data` grows capacity from 16 by doubling until the new data fits, reallocates, copies, and advances the used length. Failure is reported upward.

**EBML primitives.** An element ID is emitted as the shortest big-endian byte sequence (1–4 bytes) that preserves the ID's significant bits — the code selects the length by which high bytes are zero. An element size is emitted using EBML's variable-length encoding: a leading marker bit indicates the byte count (1 marker bit for 1 byte, 2 for 2 bytes, etc.), the size is written in the remaining bits big-endian, and the smallest encoding that fits is chosen (thresholds are 0x7f, 0x3fff, 0x1fffff, 0x0fffffff). Unsigned integers are emitted with leading zero bytes stripped and a matching size field; floats are written as raw 4-byte IEEE-754 big-endian; strings are raw bytes with a length field; binary blobs are raw bytes with a length field.

**Context lifecycle.** Creating a context takes one from the freelist or allocates it and links it into the active list. Closing a context writes its ID and size into the parent (using the current data length), optionally records the parent's current offset for later patching, appends its data to the parent (or to the file when it is the root), and returns it to the freelist. A special "flush ID" path writes the Segment ID followed by a single `0xff` byte (an unknown-size marker) and clears the ID so the size is never patched — necessary because the Segment's size is not known when the header is written. Root data is written directly with `fwrite`.

**Header emission.** `mk_write_header` refuses a second call and stores timescale/default duration. It emits:
- An **EBML** element (ID `0x1a45dfa3`) with EBMLVersion 1, EBMLReadVersion 1, EBMLMaxIDLength 4, EBMLMaxSizeLength 8, DocType `"matroska"`, DocTypeVersion 3 if stereo mode is present else 2, DocTypeReadVersion 2.
- A **Segment** (ID `0x18538067`) with its ID flushed using the unknown-size marker.
- **SegmentInfo** (ID `0x1549a966`) with MuxingApp `"Haali Matroska Writer b0"`, the caller's WritingApp, TimecodeScale = timescale, and a placeholder float Duration; the absolute offset of that float is saved in `duration_ptr`.
- **Tracks** (ID `0x1654ae6b`) containing one **TrackEntry** (ID `0xae`) with TrackNumber 1, TrackUID 1, TrackType 1 (video), FlagLacing 0, CodecID from the caller, optional CodecPrivate (the `avcC` blob), optional DefaultDuration, and a **Video** element (ID `0xe0`) with PixelWidth/Height, DisplayUnit, DisplayWidth/Height, and optional StereoMode.
The root context is flushed to disk and `wrote_header` is set.

**Frame assembly.** `mk_start_frame` flushes any previous frame (below), then sets `in_frame` and clears keyframe/skippable. `mk_add_frame_data` lazily creates a parentless frame context and appends bytes. `mk_set_frame_flags` records timestamp and flags and updates `max_frame_tc`.

**Frame flush / SimpleBlock.** On flush, the frame timestamp relative to the cluster's scaled start is computed as `frame_tc/timescale - cluster_tc_scaled`; if it falls outside the signed 16-bit range, the current cluster is closed and a new one started at this frame's timestamp. A new cluster (ID `0x1f43b675`) writes its Timecode element (`0xe7`) and resets the delta to zero. The block is a SimpleBlock (ID `0xa3`) whose size is frame-data length + 4: a 1-byte track number, 2 bytes of signed big-endian relative timestamp, 1 flags byte (keyframe in bit 7, skippable in bit 0), then the frame bytes. After appending, the frame buffer is reset and `in_frame` cleared. If the cluster has grown past `CLSIZE`, it is closed (and flushed to the root).

**Close.** Flush any pending frame and close any cluster. If a header was written and the file is regular, compute `last_frametime = def_duration ? def_duration : last_delta`, `total_duration = max_frame_tc + last_frametime`, seek to `duration_ptr`, write the float `total_duration/timescale` into the root buffer, and flush it. Then all contexts are destroyed (buffers and structs freed), the file is closed, and the writer freed.

## 6. Dependencies

- `output/output.h`, `output/matroska_ebml.h`, `x264cli.h` (`x264_is_regular_file`, `fseek`, logging not used here).
- No external libraries.
- Called only by `output/matroska.c`.

## 7. Constants and tables

- `CLSIZE = 1048576` (1 MiB) cluster rollover threshold.
- Element IDs (facts of the Matroska spec): EBML `0x1a45dfa3`, Segment `0x18538067`, SegmentInfo `0x1549a966`, Tracks `0x1654ae6b`, TrackEntry `0xae`, Video `0xe0`, Cluster `0x1f43b675`, SimpleBlock `0xa3`; plus the sized integer/string/float IDs listed in the emission description (EBMLVersion `0x4286`, DocType `0x4282`, TimecodeScale `0x2ad7b1`, Duration `0x4489`, CodecID `0x86`, CodecPrivate `0x63a2`, DefaultDuration `0x23e383`, PixelWidth `0xb0`, PixelHeight `0xba`, DisplayUnit `0x54b2`, DisplayWidth `0x54b0`, DisplayHeight `0x54ba`, StereoMode `0x53b8`, Cluster Timecode `0xe7`).
- Unknown-size marker byte: `0xff`.

## 8. Performance notes

- Frame/cluster data is accumulated in reusable buffers; EBML size fields are computed at close time, so no second pass over payload data is needed.
- **Reservation sizing:** context buffers grow from 16 by doubling; reusing contexts via the freelist avoids repeated `malloc` across frames and clusters.
- **I/O buffering:** one `fwrite` per context flush (cluster/root), not per element — good locality.
- **Zero-copy:** payload bytes are copied once into the frame context (needed for size back-patching).
- **Branch behaviour:** the cluster-rollover check runs once per frame; the 1 MiB threshold keeps clusters at a size friendly to demuxers.
- Reference: Matroska specification (EBML variable-length integer encoding, Cluster/SimpleBlock rules).

## 9. References

- Matroska/EBML specifications: <https://www.matroska.org/technical/specs/index.html>, <https://www.matroska.org/technical/ebml/index.html>.
- ISO/IEC 14496-15 for the `avcC` blob supplied as CodecPrivate.
- `output/matroska.c`, `output/matroska_ebml.h` (companion specifications).

## 10. Verification checklist

1. Parse the output with an EBML parser; verify EBML header IDs/versions and DocType `"matroska"`.
2. Verify the Segment uses an unknown-size marker and that the file is still parseable.
3. Verify TrackEntry has CodecID `V_MPEG4/ISO/AVC` and CodecPrivate starting with `0x01` (avcC).
4. Verify each frame becomes a SimpleBlock with correct 16-bit relative timestamp and keyframe flag.
5. Force a timestamp jump and confirm a new Cluster with a new Timecode is started.
6. Force a cluster > 1 MiB and confirm rollover.
7. Verify Duration is patched to `(max_frame_tc + last_delta)/timescale` and that stdout output skips the patch.
8. Verify no memory leaks across many frames (freelist reuse).

## 11. Open questions

- UNKNOWN: exact behaviour if `mk_add_frame_data` is called after `mk_set_frame_flags` and then flushed — likely accepted, but the header contract does not state it.
- UNKNOWN: whether `0xff` unknown-size is emitted for any context other than Segment; the shown code uses it only for Segment.
