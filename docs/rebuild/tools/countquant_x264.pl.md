# `tools/countquant_x264.pl` — dirty-side behavioural specification

## 1. Header

- Source path: `tools/countquant_x264.pl`.
- Role: a Perl report generator that summarises per-frame quantiser, frame-type and bit-cost statistics from x264 multipass log/stats output read from files or standard input.
- Marker: this file is a **dirty-side behavioural specification**; the parsing rules and report layout are described in our own words.

## 2. Purpose

Multipass encoding writes one line per processed frame containing its type, quantiser and a breakdown of the bits spent on texture, motion vectors and miscellaneous syntax. To inspect the rate-control behaviour of a long encode, a human needs aggregates rather than raw lines: how many frames used each QP, the average QP per frame type, the average byte cost per frame, and the bitrate implied by a few standard frame rates. This tool performs that aggregation in a single pass and prints a compact, fixed-format report. It is a development aid for understanding rate control, not part of the encoder.

## 3. Public interface

- Invocation: `countquant_x264.pl [FILE ...]`. With no file arguments, input is read from standard input. With one or more file arguments, each file is opened in text mode and processed in the order given.
- No options are parsed; every argument is treated as a filename. A filename beginning with `-` is not special.
- Output is plain text on standard output: a QP histogram, a frame-count/average table, a total-size block and a three-line bitrate block.
- Error behaviour: a file that cannot be opened terminates the program immediately with a diagnostic naming the file and the operating-system error. Input containing no usable I/P/B frame records terminates immediately with the message `unrecognized input` and a newline.
- Return status: zero on a successful report; non-zero on the fatal conditions above (Perl's default `die` behaviour).
- The script does not write files, does not modify its input, and has no interaction with the encoder process.

## 4. State and data structures

All counters are file-scope globals and are **not reset between input files**, so multiple files accumulate into one combined report.

- `%size`: map from frame-type key to accumulated byte count. Initialised for `I`, `P` and `B` with zero, and extended on demand for any other type character seen.
- `%n`: map from frame-type key to frame count; initialised the same way.
- `@q`: histogram indexed by integer QP; conceptually slots `0..51` are printed. A computed bucket outside that range can be stored but is never displayed.
- `$avgq`: running sum of all accepted quantiser values, regardless of type.
- `%avgq`: running sum of quantisers keyed by frame type.
- `$size` and `$n`: totals computed only from the `I`, `P` and `B` entries after all input has been read. Frames of other types contribute to their per-type row but not to these totals.
- Per-line temporaries: the one-character frame type (uppercased before use), the decimal quantiser, and the three integer bit counts.
- The byte count for a frame is the sum of the three bit counts divided by eight, so it is a floating-point value; averages and totals inherit that precision.

Invariants: the histogram count for a bucket equals the number of accepted records whose rounded quantiser landed in that bucket; `$n` is the number of I, P and B records and is used as the denominator for every reported percentage and average.

## 5. Algorithm / behaviour

### 5.1 Input scanning

1. Read each line in turn. A line is considered only if it contains, in order, the literal labels for type, quantiser, texture bits, motion bits and misc bits, with a single character after the type label, a decimal number with at least one digit before and after the point after the quantiser label, and three integers after the remaining labels. The labels need not be at the start of the line: leading log prefixes are tolerated because the match is not anchored.
2. Lines that do not match are silently ignored. There is no warning, no count of skipped lines and no strict validation that the line came from x264.
3. The frame-type character is converted to upper case, so lower-case type letters in a log are counted together with their upper-case forms.
4. The frame count for that type is incremented.
5. The quantiser is rounded to the nearest integer using the rule "add one half, then truncate" and the corresponding histogram slot is incremented. A value of `25.5` therefore lands in slot 26; `25.49` lands in slot 25.
6. The unrounded quantiser is added both to the global sum and to the sum for its frame type.
7. The three bit-count fields are added together and divided by eight to obtain bytes; this amount is added to the byte total for the frame type.

### 5.2 Totals and validation

8. After processing all input, the overall byte total and overall frame count are formed by adding only the `I`, `P` and `B` entries.
9. If that total frame count is zero, the program dies with `unrecognized input`. This means a log containing only unknown frame types is rejected even though it matched the line pattern.

### 5.3 Report emission

10. **QP histogram**: iterate integer slots 0 through 51 in ascending order; skip empty slots. For each non-empty slot print the slot number right-aligned in a two-character field, the count right-aligned in a six-character field, and the count as a percentage of the overall I/P/B frame count with one decimal place.
11. Print a blank line.
12. **Column width**: compute the number of decimal digits needed for the overall frame count by taking the base-10 logarithm of `count + 1`, truncating, and adding two. The extra room accommodates grouping and the following columns.
13. **All line**: print the literal label `All`, the overall frame count in the computed width, then an extra blank column when the count equals the number of I frames (i.e. there were no P or B frames). Follow with the average quantiser over all I/P/B frames with two decimals and the average bytes per frame as an integer field.
14. **Per-type lines**: iterate the frame types `I`, `P`, `B`, `S` in that fixed order, skipping types with zero frames. For each, print the count in the computed width, its percentage of the overall count with one decimal, its own average quantiser with two decimals, and its own average bytes as an integer field. The order is fixed regardless of the order types first appeared in the input.
15. Print a blank line.
16. **Size block**: print the overall byte total as an integer, then the same value divided by 1024 and by 1048576 with two decimals, labelled KiB and MiB.
17. **Bitrate block**: for each of the three frame rates 23.976, 25 and 29.97, compute `rate * total_bytes * 8 / 1000 / frame_count` kilobits per second. Print each value with two decimals and its frame rate, joining the three entries with a newline followed by seven spaces so the subsequent entries align under the first. End with a newline.

Ordering and edge cases:

- Percentages in the histogram and per-type table use the I/P/B-only total as denominator, so a non-I/P/B type can push displayed percentages above the intuitive share of all matched records.
- The per-type loop includes `S` even though `S` is not part of the totals; an `S` row can therefore be printed while contributing nothing to the `All` row.
- Because slot 51 is the last printed bucket, a quantiser of 51.5 or higher increments a slot that never appears in the histogram. The quantiser still contributes to the averages.
- Integer fields formatted with `%d` from floating-point values truncate toward zero; average bytes are therefore floored, not rounded.
- The script uses floating-point division for the byte conversion, so a line with bit counts not divisible by eight contributes a fractional byte that accumulates until the final integer formatting.
- Reopening the same file twice in one invocation doubles its statistics; there is no de-duplication.

## 6. Dependencies

- **Runtime**: Perl, core modules only; no external commands and no non-core imports.
- **Input producer**: x264's multipass statistics/log output, which emits one record per frame with the five labelled fields described above.
- **Related documentation**: the rate-control and multipass documentation describing what the texture/motion/misc fields mean.
- **Consumers**: human developers and scripts that pipe the report into text processing; no encoder code reads this output.

## 7. Constants and tables

| Item | Value / meaning |
| --- | --- |
| Printed QP slots | 0 through 51 inclusive. |
| Rounding rule | Add 0.5 then truncate to integer. |
| Bits per byte | 8. |
| Byte units | 2^10 = KiB, 2^20 = MiB (binary, not decimal). |
| Frame rates reported | 23.976, 25 and 29.97 fps, in that order. |
| Frame types in the per-type table | `I`, `P`, `B`, `S`. |
| Types included in totals | `I`, `P`, `B` only. |
| Column width | `int(log10(n + 1)) + 2`. |
| Output precision | percentages 1 decimal, averages 2 decimals, sizes 2 decimals, bitrates 2 decimals. |

The three reported frame rates correspond to common film, PAL and NTSC playback rates; no other rates are offered and there is no option to change them.

## 8. Performance notes

- The tool makes one pass over each input with a single regular expression per line; log files with millions of frames are handled in linear time and constant memory apart from the fixed histogram and the small type maps.
- The histogram is a flat integer-indexed array, so incrementing a bucket is O(1) with no string hashing; type-keyed maps stay tiny because they hold at most a handful of characters.
- Output volume is bounded by the 52 histogram slots plus four type rows, independent of input size.
- **Optimization tip:** if this report is generated routinely in a benchmark pipeline, feed it a filtered log containing only the needed passes and avoid reopening the same file multiple times in one invocation, since counters accumulate across arguments.
- **Correctness tip for rate-control work:** compare the `All` average QP and the bitrate rows between builds; because the denominators are I/P/B-only, a change in the number of `S`-type records can shift percentages without changing actual encoding behaviour. Keep the frame-type mix constant when comparing runs.

### 8.1 Relation to faster-kernel development

This tool does not measure speed, but it detects rate-control side effects of encoder changes: a new SIMD kernel that alters RD decisions can change the QP distribution even when PSNR is similar. A practical loop is to encode a fixed clip, summarise its stats with this script, and confirm that the QP histogram and average bytes per frame stay within the expected range before trusting a speed comparison. Use a single input file per invocation to keep the totals unambiguous.

## 9. References

- The source file itself, `tools/countquant_x264.pl`, attributed in-file to Loren Merritt (2005).
- x264 multipass statistics format documentation and the encoder's stats-writing code (see `doc/ratecontrol.txt.md`).
- General references on H.264 rate control: QP distributions, multipass statistics, and the relationship between bits per frame and bitrate at a given frame rate.
- Perl documentation for `printf` formats, `log`, `die` and filehandle handling, used to interpret the exact output formatting.

## 10. Verification checklist

- Feeding a single hand-written record with type `i`, QP `25.4` and bit counts divisible by eight produces an `I` row with one frame and a histogram entry in slot 25.
- Feeding QP `25.5` places the frame in slot 26, confirming the add-half-and-truncate rule.
- Lines without the five labelled fields are ignored without diagnostics.
- Two files supplied as arguments produce totals equal to the sum of two separate invocations.
- Repeating the same filename twice doubles every count.
- A log containing only unrecognised frame types dies with `unrecognized input`.
- An unreadable filename dies with a message containing that filename.
- The `All` row's extra blank column appears only when there are no P or B frames.
- The `S` row can be printed without contributing to the `All` count.
- Histogram and per-type percentages sum consistently against the I/P/B-only frame count.
- Size output matches the byte total divided by 1024 and 1048576 to two decimals.
- Bitrate values scale linearly with frame rate and with the number of frames for a fixed total size.
- Output contains exactly two blank separator lines, before the `All` table and before the size block.

## 11. Open questions

- UNKNOWN: which x264 versions emit a frame type of `S` in this log format and what that type denotes; the script only uppercases and counts it.
- UNKNOWN: the intended behaviour for quantisers that round outside 0..51; they contribute to averages but their histogram buckets are never printed.
- UNKNOWN: whether non-integer bit counts can appear in real logs; the parser requires integers, so such lines would be silently skipped.
- UNKNOWN: whether the three fixed frame rates were chosen for a specific benchmark suite; no configuration mechanism is provided.
