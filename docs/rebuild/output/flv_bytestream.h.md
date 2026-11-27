# output/flv_bytestream.h — FLV muxer primitives and AMF constants

*Dirty-side behavioural specification. Reference source: `output/flv_bytestream.h`. Behavioural descriptions only; no source or near-verbatim pseudocode is reproduced.*

## 1. Header

- **Source path:** `output/flv_bytestream.h`
- **Role:** Declares the FLV tag/AMF bit-layout constants and the buffered byte-writing utilities used by `flv.c`.
- **Marker:** dirty-side behavioural specification.

## 2. Purpose

FLV is a byte-oriented container with big-endian fields and AMF (Action Message Format) metadata. This header centralises every magic number and helper signature needed to emit those fields so `flv.c` can assemble tags declaratively. It is a pure utility header — all state lives in the `flv_buffer` object and all implementations are in `flv_bytestream.c`.

## 3. Public interface

### Constants / enums
- Audio field bit offsets/masks: sample-size offset 1 / mask 0x02, sample-rate offset 2 / mask 0x0c, codec-id offset 4 / mask 0xf0, channel mask 0x01.
- Video field offsets/masks: frame-type offset 4 / mask 0xf0, codec-id mask 0x0f.
- `AMF_END_OF_OBJECT = 0x09`.
- FLV header flags: `HASVIDEO = 1`, `HASAUDIO = 4`.
- Tag types: audio `0x08`, video `0x09`, script-data/meta `0x12`.
- Channel/sample-rate/codec enums: mono/stereo; 8/16-bit samples; samplerates (special, 11025, 22050, 44100 Hz encoded as shifted values); MP3 codec `2 << 4`, AAC `10 << 4`; H.264 codec id 7; frame types key=`1<<4`, inter=`2<<4`.
- `AMFDataType` enum: number `0x00`, bool `0x01`, string `0x02`, object `0x03`, null `0x05`, undefined `0x06`, reference `0x07`, mixed array `0x08`, object-end `0x09`, array `0x0a`, date `0x0b`, long string `0x0c`, unsupported `0x0d`.

### `flv_buffer`
Fields: `data` (dynamic byte buffer), `d_cur` (bytes currently used), `d_max` (capacity), `fp` (destination `FILE*`), `d_total` (cumulative bytes flushed, used for absolute metadata positions).

### Functions
`flv_create_writer(filename)`, `flv_append_data(c, data, size)`, `flv_write_byte(c, byte)`, `flv_flush_data(c)`, `flv_rewrite_amf_be24(c, length, start)`, `flv_dbl2int(double)`, `flv_put_byte/be16/be24/be32/be64`, `flv_put_tag(c, str)`, `flv_put_amf_string(c, str)`, `flv_put_amf_double(c, d)`. All `put_*` functions append to the buffer; `flush` writes it out; `rewrite` patches a previously emitted 24-bit field in place.

## 4. State and data structures

`flv_buffer` is the only state. It grows geometrically (capacity doubles from 16 until it fits the append) and keeps `d_total` so callers can compute absolute file offsets for later patching of placeholder metadata fields. No per-frame state is stored here — that belongs to `flv_hnd_t` in `flv.c`.

## 5. Algorithm / behaviour

The header itself defines no algorithms, but the declared functions have the following contracts:

- Big-endian writers emit the most significant byte first (`be16` two bytes, `be24` three, `be32` four, `be64` eight via two `be32` calls). Each is implemented by repeated single-byte appends.
- `flv_put_tag` writes a NUL-terminated string's characters without the terminator.
- `flv_put_amf_string` writes a 16-bit big-endian length followed by the raw characters.
- `flv_put_amf_double` writes the AMF number marker (0x00) followed by the 64-bit big-endian IEEE-754 bit pattern of the double; the bit pattern is obtained by type-punning a double to uint64 (a union, not a reinterpretation through a mismatched pointer).
- `flv_rewrite_amf_be24` overwrites three bytes at a saved offset with a 24-bit length — the mechanism used to backfill tag sizes after the payload is known.

## 6. Dependencies

- `output/output.h` (for `hnd_t`/CLI types), `x264cli.h` for `x264_fopen`, `stdout`.
- Used only by `output/flv.c`; no other module includes it.

## 7. Constants and tables

All constants listed in section 3 are direct facts of the FLV/AMF specifications. Notably:
- Tag types: `0x08` audio, `0x09` video, `0x12` script data.
- FLV header version is written by `flv.c` as 1, flags 1 (video only), data offset 9.
- H.264 video codec id = 7; keyframe frame type = `1 << 4`, inter frame = `2 << 4`, so the combined tag header byte is `0x17` or `0x27`.
- `AMF_DATA_TYPE_MIXEDARRAY = 0x08` and `AMF_END_OF_OBJECT = 0x09` are used for `onMetaData`.
- All audio constants are unused by this video-only muxer but retained for completeness.

## 8. Performance notes

- Byte-at-a-time appends are cheap because they hit a reserved buffer; the geometric growth means amortised O(1) per byte.
- **Reservation sizing:** choosing the initial capacity of 16 and doubling avoids many small reallocs; a clean-room implementation may pre-size the buffer to the first tag's expected size to eliminate the first few reallocations.
- **I/O buffering:** `flv_flush_data` is called once per completed tag so metadata patching remains possible; calling it more often is harmless but adds syscalls.
- No SIMD relevance.

## 9. References

- Adobe Flash Video (FLV) File Format Specification v1.0 — tag headers, timestamps, previous-tag-size fields.
- AMF0 specification (Action Message Format) — type markers and string/double encodings.
- `output/flv_bytestream.c`, `output/flv.c` (companion specifications).

## 10. Verification checklist

1. Confirm `flv_put_be16/24/32/64` emit the expected big-endian byte sequences for a known test value.
2. Confirm `flv_put_amf_string("abc")` produces `00 03 61 62 63`.
3. Confirm `flv_put_amf_double(1.0)` produces marker `0x00` followed by `3f f0 00 00 00 00 00 00`.
4. Confirm `flv_rewrite_amf_be24` changes exactly three bytes at the given offset.
5. Confirm repeated appends past the initial capacity keep all prior bytes intact (no corruption on realloc).
6. Confirm `flv_flush_data` resets `d_cur` to zero and accumulates `d_total`.
7. Confirm `"-"` selects stdout and that closing stdout is left to the caller.

## 11. Open questions

- UNKNOWN: the value of `UPDATE_INTERVAL` is unrelated here; no progress logic exists in this header.
- UNKNOWN: whether any caller uses the audio constants; `flv.c` writes video-only tags, so they are present for spec completeness only.
