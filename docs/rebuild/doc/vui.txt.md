# `doc/vui.txt` — dirty-side behavioural specification

## 1. Header

- Source path: `doc/vui.txt`.
- Role: a user-oriented guide to the Video Usability Information settings — sample aspect ratio, overscan, video format, full range, colour description and chroma sample location — including how to derive each value and whether to set it.
- Marker: this file is a **dirty-side behavioural specification**; the advice is paraphrased and reorganised, not copied.

## 2. Purpose

The guide explains metadata that does not change decoded sample values but changes how a player interprets them. Its purpose is twofold: define each VUI field in plain language, and give decision rules for when setting it improves playback correctness. For a clean-room implementation the document defines the *semantic intent* of the corresponding bitstream syntax elements and the default values that are considered "unspecified" by the encoder.

## 3. Public interface

The guide discusses six conceptual options:

| Option | Domain | Default stated in the guide |
| --- | --- | --- |
| Sample Aspect Ratio (SAR) | Ratio of sample width to sample height | "SAR not defined" |
| Overscan | Whether the display crops the image | `undefined` |
| Video Format | Analog source format (informative) | Not stated in the guide; UNKNOWN |
| Full Range | Whether samples use full or limited luma/chroma range | off |
| Colour primaries / transfer characteristics / matrix coefficients | H.264 Annex E enumerations | unspecified |
| Chroma Sample Location | Position of chroma samples relative to luma in 4:2:0 | Not stated in the guide; UNKNOWN |

## 4. State and data structures

No code structures are defined. The state is the set of VUI fields written into the sequence parameter set:

- Aspect-ratio identifier plus optional explicit SAR numerator/denominator.
- Overscan flags and optional horizontal/vertical crop offsets.
- Video-format code and video-full-range flag.
- Colour-primaries, transfer-characteristics and matrix-coefficients codes, each with an "unspecified" value.
- Chroma sample location code in the range 0..5.

These fields are metadata: they accompany the video but do not alter the coded sample arrays. Ownership is with the encoder's parameter set writer; there is no runtime lifecycle described in this document.

## 5. Algorithm / behaviour

### 5.1 Sample Aspect Ratio

SAR is the width-to-height ratio of an individual sample. Computer displays generally use square samples (1:1), whereas digitised video often has non-square samples. Playing material with one SAR on a system expecting another stretches or squashes the image; correction requires knowing both.

Derivation from picture dimensions and display aspect ratio, as given by the guide:

```
SAR_x / SAR_y = (DAR_x * height) / (DAR_y * width)
```

Worked example in the guide: a 704×576 picture with DAR 4:3 gives SAR 2304:2112, reducible to 12:11.

Important exception: for digitised analogue material the guide says **not** to use this equation; consult the digitising equipment documentation instead, because the storage SAR is determined by the capture process.

Recommendation: set SAR explicitly, even when it is 1:1, because the encoder's default is "SAR not defined" and most modern decoders honour the signaled value.

### 5.2 Overscan

The guide distinguishes three notions: letterbox borders, borders added during analogue digitisation, and the display's tendency to show only part of the image. Only the last is meant by this option.

Usage rule: the guide declines to give a universal rule because the visible region depends on the display; the safe choice is `overscan=show` because it requests that the whole image be displayed. Use `crop` only when the exact display behaviour is known; `undefined` is also acceptable.

Recommendation: only use it deliberately. Do not use it for streams that have general overscan; instead crop borders before encoding and spend the saved bits on image quality. Standard behaviour: a decoder **must** respect `show`, but **may** ignore `crop`; in practice most playback equipment ignores `crop` and shows the whole image.

### 5.3 Video Format

Purely informative: describes the analogue source format before digitisation (e.g. NTSC or PAL). When transcoding from MPEG-2, the value can be read from the MPEG-2 bitstream (ITU-T H.262 / ISO/IEC 13818-2). The guide considers it unlikely to matter to playback and leaves the decision to the user.

### 5.4 Full Range

Analogue digitisation historically restricted luma to 16..235 and chroma to 16..240; playback equipment assumes that convention. Many DVDs nevertheless use the full 0..255 range for both luma and chroma, which can cause oversaturation when played on equipment expecting limited range.

Decision rule: if the source is known to be range-limited, setting the flag off is safe. If uncertain, or if oversaturation must be avoided, set it on. The guide explicitly warns that the encoder's default is off, which it calls an unsafe assumption, and notes that few decoders distinguish the two settings.

### 5.5 Colour primaries, transfer characteristics, matrix coefficients

These describe the colour profile of the capture equipment and the intended colour/gamma correction. Different monitors and digitising devices reproduce colours differently; knowing the equipment's characteristics allows a decoder to correct colour and gamma so the stream appears consistent across devices.

Usage: determine the characteristics from equipment documentation or measurements, select the matching H.264 Annex E enumeration, and pass it to the encoder; otherwise leave unspecified. MPEG-2 bitstreams carry analogous values that can be reused when transcoding.

Recommendation: only set these when the values are known; a correct default is better than a wrong explicit value. The guide states it knows of no decoder or player that actually applies colour/gamma/matrix correction.

### 5.6 Chroma Sample Location

4:2:0 subsampling stores one chroma sample per 2×2 luma block, but there are several possible spatial alignments. The location matters when chroma is upsampled, e.g. during colour-space conversion; mismatch can shift chroma by up to one sample relative to luma or blur it.

Because the encoder accepts already-subsampled input, it does not know how the subsampling was done; the user must supply the value. The guide's rules:

- MPEG-1 source with proper 4:2:0 and no colour conversion → value 1.
- MPEG-2 source with proper 4:2:0 and no colour conversion → value 0.
- MPEG-4 source with proper 4:2:0 and no colour conversion → value 0.
- For colour conversion performed by the caller: a subsampling filter kernel of (0.5, 0.5) places the chroma sample *between* the two luma samples in that direction; a kernel of (0.25, 0.5, 0.25) places it *at* one of the luma samples. H.264 Annex E diagrams map this to a code 0..5.

Recommendation: most users need not bother; media players typically ignore the field and assume their own fixed location.

## 6. Dependencies

- The sequence parameter set writer, which serialises these fields into the bitstream VUI.
- The CLI option parser, which must map user-facing names to the coded enumeration values.
- H.264 Annex E for the enumeration tables and chroma-location diagrams.
- ITU-T H.262 / ISO/IEC 13818-2 for reading equivalent metadata when transcoding MPEG-2.
- Input reader / colour-conversion stage, whose subsampling behaviour determines the correct chroma sample location.

## 7. Constants and tables

| Value | Meaning |
| --- | --- |
| SAR 1:1 | Square samples; computer-monitor default. |
| 704×576, DAR 4:3 → SAR 12:11 | Worked conversion example from the guide. |
| `overscan=show` / `crop` / `undefined` | Overscan settings; `show` is the safe choice and is mandatory for decoders to honour. |
| 16..235 luma, 16..240 chroma | Limited-range sample values. |
| 0..255 | Full-range sample values. |
| 0..5 | Valid chroma sample location codes. |
| 1 / 0 / 0 | Chroma location recommendations for MPEG-1 / MPEG-2 / MPEG-4 sources. |
| (0.5, 0.5) / (0.25, 0.5, 0.25) | Subsampling kernels and the corresponding "between" / "at" chroma placement. |

## 8. Performance notes

VUI fields are metadata and have negligible runtime cost; they do not affect compression efficiency. The only practical costs are:

- Signalling a non-default SAR or range may change how a player scales the image, which can indirectly affect perceived quality but not bitrate.
- Explicit VUI values consume a handful of bits in the parameter set; this is irrelevant at normal resolutions.
- Supplying a wrong value is worse than leaving the field unspecified, both for aspect ratio and for colour description, because it forces incorrect correction.

### 8.1 Using checkasm-style verification for faster kernels

VUI settings do not interact with SIMD kernels, so checkasm-style verification is not directly needed when changing them. However, if a change to the chroma-format or bit-depth path accompanies a VUI change (for example a new 4:2:0 chroma-location handling path with a new SIMD upsampler), the new kernels should be validated with the assembly conformance framework first, then the result checked end-to-end with the decoder comparison in `doc/regression_test.txt.md`. The per-ISA sweep in `tools/checkasm.c.md` is the appropriate way to bring a new colour-conversion kernel under test.

References: H.264 Annex E for the metadata semantics; `tools/checkasm.c.md` for kernel verification; `doc/regression_test.txt.md` for end-to-end decoder agreement.

## 9. References

- The source guide itself, `doc/vui.txt`, attributed in-file to Christian Heine.
- The "A Quick Guide to Digital Video Resolution and Aspect Ratio Conversions" URL cited in the note (`http://www.iki.fi/znark/video/conversion/`) for analogue capture SAR.
- ITU-T Rec. H.264 / ISO/IEC 14496-10, Annex E: colour primaries, transfer characteristics, matrix coefficients and chroma sample location semantics.
- ITU-T Rec. H.262 / ISO/IEC 13818-2 (MPEG-2) for transcoding metadata.
- `doc/regression_test.txt.md` for verifying that signalled metadata does not disturb reconstruction agreement.

## 10. Verification checklist

- SAR can be left undefined or set to an explicit rational; when derived from DAR, the guide's equation gives the expected result on the 704×576 / 4:3 example.
- A 1:1 SAR can be signalled explicitly rather than left undefined.
- Overscan can be set to `show`, `crop` or `undefined`; `show` is respected and `crop` is treated as optional by decoders.
- Video format codes can be set from NTSC/PAL or read from an MPEG-2 bitstream.
- Full-range flag default is off; setting it on changes only interpretation, not coded sample values.
- Colour primaries/transfer/matrix values can be left unspecified or set to Annex E values and are not applied by typical players.
- Chroma sample location codes 0..5 are accepted; the MPEG-1/2/4 recommendations (1/0/0) map to the correct codes.
- Changing any VUI field does not change the decoded sample arrays in a decoder-comparison test.

## 11. Open questions

- UNKNOWN: the encoder's exact default for video format and chroma sample location; the guide does not state them.
- UNKNOWN: CLI spellings and accepted aliases for each VUI option in current builds.
- UNKNOWN: how SAR is clamped or reduced to a rational with bounded numerator/denominator.
- UNKNOWN: whether current players have changed their behaviour regarding full range or chroma location since the guide was written.
