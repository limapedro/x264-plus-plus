# CLI black-box specification of the reference encoder

This document describes **observable behaviour only**. It was produced by running the compiled
reference binary `oracle/x264.exe` and by reading its own `--help` / `--longhelp` / `--fullhelp`
text. No reference source code was read. Anything that could not be established from those
sources is marked **UNKNOWN**.

Notation used in the evidence section:

- `X` = `E:\__PROJECTS__\x264++\oracle\x264.exe`
- `$P` = `C:\Users\multi\AppData\Local\Temp\opencode\x264-probes` (probe scratch directory, outside the repo)
- All runs had `PATH` prefixed with `C:\msys64\mingw64\bin;C:\msys64\usr\bin;`
- `\dense` = `test.y4m`: 64x64, 30 frames, 10 fps, `yuv420p`, SAR 1:1, generated once with
  `ffmpeg -f lavfi -i testsrc2=size=64x64:rate=10 -frames:v 30 -pix_fmt yuv420p`.
- `\dense.raw` = the same 30 frames as raw I420 (`test.i420`, 184320 bytes).
- `\scenes` = `scenes.y4m`: 60 frames, three 20-frame solid-colour segments (red/green/blue), 64x64, 10 fps.
- `\big` = `big.y4m`: 320x240, 60 frames, 25 fps.

Small reusable probe scripts live in `spec/probes/` (`probe.ps1` records stdout, stderr and exit
code; `sei.ps1` extracts the SEI `options:` payload from an Annex B file). Generated media is
**not** stored in the repository.

---

## 1. Build identity

`X --version` → stdout, exit 0:

```
x264 0.164.x
(libswscale 9.5.102)
built on Nov 27 2026, gcc: 16.2.0
x264 configuration: --chroma-format=all
libx264 configuration: --chroma-format=all
x264 license: GPL version 2 or later
libswscale license: GPL version 3 or later
```

`X --fullhelp` → stdout, exit 0, first line `x264 core:164`, 435 lines.
`X --help` and `X -h` are identical (95 lines); `X --longhelp` is 193 lines; each is a prefix
of the next longer one. Help text goes to **stdout**; all errors and all encoding progress /
statistics go to **stderr**.

Build features explicitly reported as unavailable by `--fullhelp`: Avisynth (`no`), libav/ffms
input (`no`), MP4 output (`no`). Available output depths: 8/10.

---

## 2. Invocation syntax

```
x264 [options] -o outfile infile
```

- Short and long GNU-style options are accepted; `--name value` and `--name=value` both work
  (evidence E1). `-q 20` works as short form for `--qp 20`.
- Options may appear **before or after** the input filename; a later explicit option overrides a
  preset/tune regardless of order (E2).
- The **first non-option argument** is the input file; further non-option arguments are ignored
  (E3). `--` terminates option parsing, so any `-o` after it is treated as a filename and the run
  fails with `x264 [error]: No output file. Run x264 --help for a list of options.` (E4).
- Unknown option → `<full path>\x264.exe: unknown option -- <name>` on stderr, exit −1.
  Option missing its argument → `<full path>\x264.exe: option requires an argument -- <name>`,
  exit −1.
- An input filename of `-` means **stdin**. An output filename of `-` means **stdout**; the raw
  bitstream is then written to stdout while logs stay on stderr (E5).
- No input / no output:
  - no arguments, or only stdin with no `-o`: `x264 [error]: No output file. Run x264 --help for a list of options.`
  - `-o file` with no input: `x264 [error]: No input file. Run x264 --help for a list of options.`
  Both exit −1.

---

## 3. Inputs

### 3.1 Formats

`--demuxer auto|raw|y4m` (default `auto`).

- **Y4M (`.y4m`)**: autodetected from the `YUV4MPEG2` header. On success the first log line is
  e.g. `y4m [info]: 64x64p 1:1 @ 10/1 fps (cfr)`. If the header magic is wrong:
  `y4m [error]: bad sequence header magic` then
  `x264 [error]: could not open input file '<path>'`, exit −1 (E6).
- **Raw video**: any non-Y4M file (and **`stdin` by default**) is treated as raw. It requires
  `--input-res WxH`; without it:
  `raw [error]: raw input requires a resolution.` then
  `x264 [error]: could not open input file '<path>' via any method!`, exit −1 (E7).
  Observed raw defaults: frame rate **25/1 fps** when `--fps` is absent; SAR is reported as
  `0:0` (undefined) and the `using SAR=...` info line is omitted (E8).
  `--input-csp` selects the planar/packed layout; for raw I420 the frame size was
  `W*H*3/2` bytes (184320 bytes for 30 frames of 64x64). Valid raw CSP names listed by
  `--fullhelp`: `i400, i420, yv12, nv12, nv21, i422, yv16, nv16, yuyv, uyvy, i444, yv24, bgr, bgra, rgb`.
  `--input-depth` selects raw input bit depth.
- **stdin**: `-` is accepted, but autodetection does **not** apply — it is opened as raw, so
  `--input-res` is required unless `--demuxer y4m` is given (E9).
- Avisynth, libav* and ffms input are **not compiled in**; `--input-fmt` is therefore not usable.

### 3.2 Frame selection

- `--frames N`: encode at most N frames. `--frames 0` means "all frames" (E10).
- `--seek N`: skip N input frames. Seeking past EOF succeeds with exit 0, produces a **0-byte**
  output and no summary line (E11).
- `--fps` overrides the container frame rate for VUI/timing purposes; the demuxer info line still
  reports the source rate. Accepts a float (`29.97`) or rational (`30000/1001`); a non-numeric
  value gives `x264 [error]: invalid argument: fps = <v>`, exit −1.
- `--sar W:H` sets sample aspect ratio; invalid value → `x264 [error]: invalid argument: sar = <v>`.
  Observed: `--sar 4:3` produced a bitstream whose `sample_aspect_ratio` reads `4:3`.
- `--timebase` with CFR input is rejected: `x264 [error]: --timebase is incompatible with cfr input`.

---

## 4. Outputs

Muxer is chosen from the output filename suffix unless `--muxer` overrides it.

| Suffix / `--muxer` | Observed result |
| --- | --- |
| `.264` | Raw Annex B bytestream. Starts `00 00 00 01 67 ...` (SPS), then PPS (`68`), SEI (`06`), slices. |
| any other suffix (e.g. `.bin`) | Also raw Annex B (byte-identical to `.264` in the probe). |
| `.mkv` / `--muxer mkv` | Matroska; SPS/PPS moved to container extradata (per-frame `size` in log was 1149 vs 1861 for raw). |
| `.flv` / `--muxer flv` | Flash Video; same in-band-frame-size behaviour as MKV. |
| `.mp4` | `x264 [error]: not compiled with MP4 output support`, exit −1. |
| `-` | Annex B written to stdout. |

`--muxer` accepts `auto, raw, mkv, flv`. `--output` / `-o` with a path in a non-existent
directory → `x264 [error]: could not open output file '<path>'`, exit −1.

Annex B raw output contains an SEI NAL whose user-data payload begins:

```
x264 - core 164 - H.264/MPEG-4 AVC codec - Copyleft 2003-2024 - http://www.videolan.org/x264.html - options: ...
```

This string is an **observable part of the bitstream** and encodes the effective parameter set
(`cabac`, `ref`, `deblock`, `analyse`, `me`, `subme`, `psy`, `psy_rd`, `mixed_ref`, `me_range`,
`chroma_me`, `trellis`, `8x8dct`, `cqm`, `deadzone`, `fast_pskip`, `chroma_qp_offset`, `threads`,
`lookahead_threads`, `sliced_threads`, `nr`, `decimate`, `interlaced`, `bluray_compat`,
`constrained_intra`, `bframes`, `b_pyramid`, `b_adapt`, `b_bias`, `direct`, `weightb`, `open_gop`,
`weightp`, `keyint`, `keyint_min`, `scenecut`, `intra_refresh`, `rc_lookahead`, `rc`, `mbtree`,
`crf`/`qp`/`bitrate`, `qcomp`, `qpmin`, `qpmax`, `qpstep`, `ip_ratio`, `pb_ratio`, `aq`,
`ratetol`, `filler`, etc.). It is the most precise black-box oracle for defaults and
interactions used in this document; an independent implementation that wants byte-identical
output must reproduce it exactly.

---

## 5. Option surface and defaults

The following is taken from `--fullhelp`; the "effective" column is what the SEI string showed
for a default 8-bit 4:2:0 encode at 64x64 or 320x240. **Help text and effective value differ in
a few places — those are noted explicitly.**

### 5.1 Frame type / structure

| Option | Help default / syntax | Effective default observed | Notes |
| --- | --- | --- | --- |
| `-I, --keyint` | `[250]`, or `infinite` | `keyint=250` | `--keyint 5` → `keyint=5 keyint_min=1 rc_lookahead=5`; `--keyint infinite` → `keyint=infinite` |
| `-i, --min-keyint` | `[auto]` | `keyint_min=10` | Auto = `keyint/25`; can be set independently |
| `--scenecut`, `--no-scenecut` | `[40]` | `scenecut=40` | Both `--scenecut 0` and `--no-scenecut` → `scenecut=0` |
| `--intra-refresh` | off | `intra_refresh=0` | |
| `-b, --bframes` | `[3]` | `bframes=3` | `--bframes 0` omits B-related SEI fields |
| `--b-adapt` | `[1]`, 0/1/2 | `b_adapt=1` | |
| `--b-bias` | `[0]` | `b_bias=0` | |
| `--b-pyramid` | `[normal]` (none/strict/normal) | `b_pyramid=2` | |
| `--open-gop` | off | `open_gop=0` | `--open-gop` → `open_gop=1`; only meaningful with B-frames |
| `--no-cabac` | CABAC on | `cabac=1` | `--no-cabac` → `cabac=0` |
| `-r, --ref` | `[3]` | `ref=3` | `ref=1` forces `mixed_ref=0` |
| `--no-deblock`, `-f, --deblock <a:b>` | `[0:0]`, deblock on | `deblock=1:0:0` | `--no-deblock` → `deblock=0:0:0`; `-2:-2` → `deblock=1:-2:-2` |
| `--slices`, `--slices-max`, `--slice-max-size`, `--slice-max-mbs`, `--slice-min-mbs` | unset | UNKNOWN (not serialised in SEI) | Accept integers; no error observed for out-of-range in limited probes |
| `--tff`, `--bff` | progressive | `interlaced=0` | |
| `--constrained-intra` | off | `constrained_intra=0` | |
| `--pulldown`, `--fake-interlaced`, `--frame-packing` | none | UNKNOWN | Accept the documented strings/integers |

### 5.2 Rate control

| Option | Help default / syntax | Effective default observed | Notes |
| --- | --- | --- | --- |
| `-q, --qp` | force constant QP, 0–81 | not set by default | `--qp N` ⇒ `rc=cqp mbtree=0 aq=0`; QP 0 ⇒ lossless (see §6) |
| `-B, --bitrate` | kbit/s | not set by default | `--bitrate 200` ⇒ `rc=abr ratetol=1.0` |
| `--crf` | `[-12-51] [23.0]` | `rc=crf crf=23.0` | Out-of-range numeric values are clamped silently (E12) |
| `--rc-lookahead` | `[40]` | `rc_lookahead=40` | Clamped to `keyint` when keyint smaller; `0` disables mbtree and warns |
| `--vbv-maxrate`, `--vbv-bufsize` | `[0]` | UNKNOWN (omitted when 0) | `--nal-hrd` requires vbv-bufsize |
| `--vbv-init` | `[0.9]` | UNKNOWN | |
| `--crf-max` | unset | UNKNOWN | |
| `--qpmin` | `[0]` | `qpmin=0` | |
| `--qpmax` | `[81]` | **`qpmax=69` for 8-bit, 81 for 10-bit** | Requested 81 on 8-bit was clamped to 69; request 50 was honoured |
| `--qpstep` | `[4]` | `qpstep=4` | Omitted in CQP mode |
| `--ratetol` | `[1.0]` | printed in ABR mode | |
| `--ipratio` | `[1.40]` | `ip_ratio=1.40` | |
| `--pbratio` | `[1.30]` | `pb_ratio=1.30` | Printed in some modes (e.g. CQP, ABR, superfast) and omitted in others |
| `--chroma-qp-offset` | `[0]` | **`-2`** for default 4:2:0 8-bit; `4` for 4:4:4; `0` with `--no-psy` | Explicit `0` still serialised `-2`; explicit `5` with `--qp 20` serialised `3` — value is derived/clamped |
| `--aq-mode` | `[1]` (0–3) | `aq=1:1.00` | `--aq-mode 0` → `aq=1:0.00` (strength 0); `--aq-mode 2/3` → `aq=2/3:1.00` |
| `--aq-strength` | `[1.0]` | `aq=1:1.00` | `--aq-strength 0.5` → `aq=1:0.50` |
| `-p, --pass` | 1/2/3 | off | `--pass 1` with `--bitrate` writes stats; `--pass 2` needs an existing stats file |
| `--stats` | `"x264_2pass.log"` | — | `--pass 2` with missing stats → `x264 [error]: ratecontrol_init: can't open stats file` |
| `--no-mbtree` | mbtree on | `mbtree=1` | Forced off by CQP, ultrafast/superfast/zerolatency, `rc-lookahead 0` |
| `--qcomp` | `[0.60]` | `qcomp=0.60` | |
| `--cplxblur`, `--qblur` | `[20.0]`, `[0.5]` | UNKNOWN | Accepted numerically |
| `--zones`, `--qpfile` | unset | UNKNOWN | Malformed zones → `x264 [error]: invalid zone: "<text>"`; missing qpfile → `x264 [error]: can't open qpfile '<name>'` |

### 5.3 Analysis

| Option | Help default / syntax | Effective default observed |
| --- | --- | --- |
| `-A, --partitions` | `"p8x8,b8x8,i8x8,i4x4"` | `analyse=0x3:0x113`; `--partitions all` → `analyse=0x3:0x133` |
| `--direct` | `"spatial"` (none/spatial/temporal/auto) | `direct=1`; `auto` → `direct=3` |
| `--no-weightb` | weightb on | `weightb=1` |
| `--weightp` | `[2]` | `weightp=2` |
| `--me` | `"hex"` (dia/hex/umh/esa/tesa) | `me=hex`; invalid strings → `x264 [error]: invalid argument: me = <v>` |
| `--merange` | `[16]` | `me_range=16` |
| `--mvrange`, `--mvrange-thread` | `[-1 (auto)]` | UNKNOWN |
| `-m, --subme` | `[7]` (0–11) | `subme=7`; `--subme 11` with `trellis=1` was reduced to `subme=9` |
| `--psy-rd <a:b>` | `"1.0:0.0"` | `psy=1 psy_rd=1.00:0.00` |
| `--no-psy` | psy on | `psy=0`, `chroma_qp_offset=0` |
| `--no-mixed-refs` | mixed on | `mixed_ref=1` |
| `--no-chroma-me` | chroma ME on | `chroma_me=1` (0 for i400) |
| `--no-8x8dct` | 8x8dct on | `8x8dct=1` |
| `-t, --trellis` | `[1]` (0/1/2) | `trellis=1` |
| `--no-fast-pskip` | fast pskip on | `fast_pskip=1` |
| `--no-dct-decimate` | decimate on | `decimate=1` |
| `--nr` | `[0]` | `nr=0` |
| `--deadzone-inter/intra` | `[21]`, `[11]` | `deadzone=21,11` |
| `--cqm`, `--cqmfile`, `--cqm4*`, `--cqm8*` | `"flat"` | `cqm=0` |

### 5.4 VUI, I/O, logging, miscellaneous

- VUI: `--overscan undef`, `--videoformat undef`, `--range auto`, `--colorprim undef`,
  `--transfer undef`, `--colormatrix ???`, `--chromaloc 0`, `--mastering-display`,
  `--cll`, `--alternative-transfer undef`, `--nal-hrd none`, `--filler`, `--pic-struct`,
  `--crop-rect`. These are signals only; no effect on encoding decisions was probe-verified
  (**UNKNOWN** beyond the log/ffprobe check in E13).
- I/O: `--output/-o`, `--muxer auto`, `--demuxer auto`, `--input-fmt` (needs lavf — unavailable),
  `--input-csp`, `--output-csp i420`, `--input-depth`, `--output-depth`, `--input-range auto`,
  `--input-res WxH`, `--index`, `--sar`, `--fps`, `--seek`, `--frames`, `--level`,
  `--bluray-compat`, `--avcintra-class`, `--avcintra-flavor panasonic`, `--stitchable`.
  `--output-csp bogus` → `x264 [error]: Unknown output csp 'bogus'`.
  `--output-csp i422/i444/i400` converts input and switches the signalled profile; a warning
  such as `resize [warning]: converting from yuv420p to yuv422p` is logged (E14).
  `--output-depth 10` ⇒ `profile High 10 ... 10-bit`, `qpmax=81`.
- Logging/misc: `-v/--verbose`, `--no-progress`, `--quiet`, `--log-level none|error|warning|info|debug`
  (default `info`; an unrecognised string silently behaves like a low level — E15),
  `--psnr`, `--ssim`, `--threads`, `--lookahead-threads`, `--sliced-threads`, `--thread-input`,
  `--sync-lookahead`, `--non-deterministic`, `--cpu-independent`, `--asm`, `--no-asm`,
  `--opencl`, `--opencl-clbin`, `--opencl-device`, `--dump-yuv`, `--sps-id 0`,
  `--aud`, `--force-cfr`, `--tcfile-in`, `--tcfile-out`, `--timebase`, `--dts-compress`,
  `--vf/--video-filter`.
  `--threads 0` means automatic; observed automatic values were **2 threads** at 64x64 and
  **7 threads** at 320x240 on the probe machine (12 logical CPUs) — the auto count is
  resolution-dependent. `--aud` inserts an AUD NAL (`00 00 00 01 09 ...`) before the SPS.
  Invalid values: `--me bogus` and `--fps bad` → `x264 [error]: invalid argument: <opt> = <v>`;
  `--log-level bogus` did **not** error.
- Filtering: `--vf crop:left,top,right,bottom`, `resize:[w,h][,sar][,fittobox][,csp][,method]`,
  `select_every:step,offset1[,...]`; invalid filter → `x264 [error]: invalid filter '<name>'`.

---

## 6. Presets, tunes, profiles and interactions

### 6.1 Precedence

Observed order: **defaults → preset → tune → explicit options**; explicit options win regardless
of command-line order, and the last `--preset` / last `--tune` wins. `--tune` accepts a
comma-separated list; only one psy tune at a time (per help; multiple-tune combinations not
probe-verified → **UNKNOWN**).

Invalid names: `x264 [error]: invalid preset '<v>'`, `x264 [error]: invalid tune '<v>'`,
`x264 [error]: invalid profile: <v>` — all exit −1.

### 6.2 Presets — observed effective settings (SEI payload)

| Preset | Selected differences from medium |
| --- | --- |
| ultrafast | `cabac=0 ref=1 deblock=0:0:0 analyse=0:0 me=dia subme=0 trellis=0 8x8dct=0 mixed_ref=0 bframes=0 weightp=0 scenecut=0 mbtree=0 aq=0`, no `rc_lookahead` |
| superfast | `ref=1 me=dia subme=1 trellis=0 mixed_ref=0 weightp=1 mbtree=0` |
| veryfast | `ref=1 mixed_ref=0 subme=2 trellis=0 weightp=1 rc_lookahead=10` |
| faster | `ref=2 mixed_ref=0 subme=4 weightp=1 rc_lookahead=20` |
| fast | `ref=2 subme=6 weightp=1 rc_lookahead=30` |
| medium | `ref=3 subme=7 weightp=2 rc_lookahead=40`, all other defaults |
| slow | `ref=5 subme=8 trellis=2 direct=3 rc_lookahead=50` |
| slower | `ref=8 analyse=0x3:0x133 me=umh subme=9 trellis=2 b_adapt=2 direct=3 rc_lookahead=60` |
| veryslow | `ref=16 me=umh me_range=24 subme=10 bframes=8 b_adapt=2 direct=3 rc_lookahead=60` |
| placebo | `ref=16 me=tesa me_range=24 subme=11 bframes=16 b_adapt=2 direct=3 fast_pskip=0 rc_lookahead=60` |

### 6.3 Tunes — observed effective settings (SEI payload)

| Tune | Selected differences from medium |
| --- | --- |
| film | `deblock=1:-1:-1 psy_rd=1.00:0.15 chroma_qp_offset=-3` |
| animation | `ref=6 bframes=5 deblock=1:1:1 psy_rd=0.40:0.00 aq=1:0.60` |
| grain | `deblock=1:-2:-2 psy_rd=1.00:0.25 deadzone=6,6 decimate=0 qcomp=0.80 ip_ratio=1.10 aq=1:0.50 chroma_qp_offset=-4` |
| stillimage | `deblock=1:-3:-3 psy_rd=2.00:0.70 aq=1:1.20 chroma_qp_offset=-4` |
| psnr | `psy=0 aq=1:0.00 chroma_qp_offset=0` |
| ssim | `psy=0 aq=2:1.00 chroma_qp_offset=0` |
| fastdecode | `cabac=0 deblock=0:0:0 weightb=0 weightp=0` |
| zerolatency | `bframes=0 mbtree=0 threads=1` (sliced threads requested by help but SEI showed `sliced_threads=0` with `threads=1`) |

### 6.4 Rate-control mode implications

| Invocation | Observed SEI fields |
| --- | --- |
| default | `rc=crf crf=23.0 mbtree=1 rc_lookahead=40` |
| `--crf 18` | `rc=crf crf=18.0` (other fields unchanged) |
| `--qp 20` | `rc=cqp qp=20 mbtree=0 aq=0`; `qpmin/qpmax/qpstep`, `rc_lookahead`, `pb_ratio` presentation changes |
| `--qp 0` | lossless: log reports `profile High 4:4:4 Predictive, level 1.0, 4:2:0, 8-bit`; SEI `rc=cqp qp=0 psy=0 trellis=0 fast_pskip=0 bframes=0`. `--profile high --qp 0` errors: `x264 [error]: high profile doesn't support lossless` |
| `--bitrate 200` | `rc=abr bitrate=200 ratetol=1.0 mbtree=1` |
| `--rc-lookahead 0` | warning `x264 [warning]: lookaheadless mb-tree requires intra refresh or infinite keyint`; SEI `mbtree=0 pb_ratio=1.30` |
| `--aq-mode 0` | `aq=1:0.00` (not `aq=0`); output bytes still changed relative to default |

### 6.5 Profiles / chroma / depth

- `--profile baseline` → log `profile Constrained Baseline`; `cabac=0 bframes=0 8x8dct=0 analyse=0x1:0x111 weightp=0`.
- `--profile main` → `profile Main`; `8x8dct=0 analyse=0x1:0x111`.
- `--profile high` → `profile High` (identical to default here).
- `--profile high10/high422/high444` with 8-bit 4:2:0 input still reported `profile High`;
  the requested profile only takes effect when combined with `--output-depth 10` and/or a
  matching `--output-csp`.
- `--output-csp i422` → `profile High 4:2:2, ... 4:2:2, 8-bit`;
  `--output-csp i444` → `profile High 4:4:4 Predictive, ... 4:4:4, 8-bit`, `chroma_qp_offset=4`;
  `--output-csp i400` → `profile High, ... 4:0:0, 8-bit`, `chroma_me=0`.
- `--output-depth 10` → `profile High 10, ... 10-bit`, `qpmax=81`.

---

## 7. Log / stderr format and exit codes

All encoding messages go to **stderr**. Typical successful run (10 frames, `--crf 23`):

```
y4m [info]: 64x64p 1:1 @ 10/1 fps (cfr)
x264 [info]: using SAR=1/1
x264 [info]: using cpu capabilities: MMX2 SSE2Fast SSSE3 SSE4.2 AVX FMA3 BMI2 AVX2
x264 [info]: profile High, level 1.0, 4:2:0, 8-bit
[10.0%] 1/10 frames, 1000.00 fps, 161.44 kb/s, eta 0:00:00  <CR><80 spaces><CR>x264 [info]: frame I:1     Avg QP:26.00  size:  2018
x264 [info]: frame P:5     Avg QP:27.44  size:   509
x264 [info]: frame B:4     Avg QP:29.16  size:   264
x264 [info]: consecutive B-frames: 30.0% 40.0% 30.0%  0.0%
x264 [info]: mb I  I16..4: 12.5%  0.0% 87.5%
x264 [info]: mb P  I16..4:  0.0%  0.0%  3.8%  P16..4: 13.8% 40.0% 27.5%  0.0%  0.0%    skip:15.0%
x264 [info]: mb B  I16..4:  0.0%  0.0%  0.0%  B16..8: 10.9% 29.7% 19.5%  direct:17.2%  skip:23.4%  L0:40.9% L1:40.9% BI:18.2%
x264 [info]: 8x8 transform intra:0.0% inter:21.2%
x264 [info]: coded y,uvDC,uvAC intra: 71.1% 100.0% 100.0% inter: 29.8% 65.2% 59.6%
x264 [info]: i16 v,h,dc,p: 50% 50%  0%  0%
x264 [info]: i4 v,h,dc,ddl,ddr,vr,hd,vl,hu: 41% 28% 16%  2%  0%  2%  4%  3%  3%
x264 [info]: i8c dc,h,v,p: 37% 26% 37%  0%
x264 [info]: Weighted P-Frames: Y:0.0% UV:0.0%
x264 [info]: ref P L0: 71.2% 12.7%  8.8%  7.3%
x264 [info]: ref B L0: 79.5% 16.5%  3.9%
x264 [info]: kb/s:44.97

encoded 10 frames, 5000.00 fps, 44.97 kb/s
```

- Prefixes are `<component> [level]: ` where component is `x264`, `y4m`, `raw`, `resize`, etc.,
  and level is `error`, `warning`, `info`, `debug`.
- Progress lines are `[<pct>] <done>/<total> frames, <fps> fps, <kb/s> kb/s, eta H:MM:SS` followed
  by CR, 80 spaces, CR; they are written even with `--quiet` / low `--log-level`.
- Final summary is `encoded <N> frames, <fps> fps, <kb/s> kb/s`. It is absent when zero frames
  are encoded.
- `--no-progress` removes progress lines but keeps info stats and the summary.
- `--quiet` removes all `[info]` and `[warning]` lines (including warnings) but keeps progress
  and summary. `--log-level none|error|warning` behave similarly; `--log-level info` is default;
  `debug` adds lines such as `x264 [debug]: using mv_range_thread = 24`.
- `-v/--verbose` adds per-frame debug lines, e.g.
  `x264 [debug]: frame=   0 QP=26.00 NAL=3 Slice:I Poc:0   I:16   P:0    SKIP:0    size=2018 bytes`.
- `--psnr` / `--ssim` add warnings when psy is on (`--psnr used with psy on: results will be
  invalid!`, `--tune psnr should be used if attempting to benchmark psnr!`) and final lines such as
  `x264 [info]: SSIM Mean Y:0.9840206 (17.964db)` /
  `x264 [info]: PSNR Mean Y:38.749 U:37.754 V:37.819 Avg:38.396 Global:38.207 kb/s:44.97`.

**Exit codes:** success = `0`; any observed failure = `-1` (i.e. 255 / `0xFFFFFFFF` on Windows).
No other exit status was observed. All diagnostic text for failures was on stderr; stdout stayed
empty except for help/version and `-o -`.

---

## 8. Determinism caveats

1. **SEI contains a build/option string.** It includes the literal version line
   `x264 - core 164 - H.264/MPEG-4 AVC codec - Copyleft 2003-2024 - http://www.videolan.org/x264.html`
   plus all effective options. Any byte-identical reimplementation must emit this exact text.
2. **Same command, same binary → identical bytes** for default settings and for fixed
   `--threads`: two identical 60-frame runs produced SHA-256 prefixes `27338E199381A735` both
   times; two `--threads 4` runs both produced `FF19D9C66082DC7A` (E16).
3. **`--non-deterministic` breaks repeatability**: two otherwise identical `--threads 4
   --non-deterministic` runs produced different lengths (81801 vs 81817 bytes) and hashes
   (`484BDD7EBA5FA5C5` vs `745DFD351C910F0A`) (E17).
4. **Thread count changes the bitstream**: 60-frame outputs for `--threads 1`, auto (7) and
   `--threads 4` had different lengths/hashes. Auto thread count is resolution-dependent.
5. **CPU/ASM path changes the bitstream**: default output hash `FA7573B14897` differed from
   `--no-asm`, `--asm 0` and `--cpu-independent`, which all produced the same hash
   `5D27C9130392` (E18). `--sliced-threads` also changed output; `--sync-lookahead 0` did not in
   this probe.
6. **Container choice changes bytes** (SPS/PPS placement), as does `--aud`, `--sps-id`,
   `--stitchable` (header optimisation) and `--profile`.
7. FPS values in the progress/summary lines are timing-dependent and **not deterministic**; only
   the encoded frame count and kb/s (kb/s derived from output size) are comparable.

---

## 9. Evidence log (observed command → observed result)

Each item was executed against `X` with the PATH prefix above.

**E1 — long-option syntax.** `X --crf=20 --frames 2 -o $P\eqsyntax.264 $P\test.y4m`
→ exit 0, `encoded 2 frames`; `--name=value` accepted.

**E2 — precedence.** `X --preset ultrafast --ref 3 ...` and `X --ref 3 --preset ultrafast ...`
→ both SEI `cabac=0 ref=3 ... subme=0 ... bframes=0`; explicit `--ref 3` wins regardless of
order. `X --preset medium --preset ultrafast` → `cabac=0 ref=1` (last preset wins).

**E3 — extra inputs.** `X $P\big.y4m $P\test.y4m --frames 5 -o $P\two_inputs_detail.264`
→ log `y4m [info]: 320x240p ...`, `encoded 5 frames`; the first positional file was used and
options after it were still parsed.

**E4 — `--`.** `X -- --frames 2 -o $P\dashdash.264 $P\test.y4m`
→ exit −1, `x264 [error]: No output file. Run x264 --help for a list of options.`

**E5 — stdout output.** `X --frames 2 -o - $P\test.y4m`
→ exit 0; stdout began `00 00 00 01 67 64 ...` (Annex B), stderr contained normal logs.

**E6 — bad Y4M.** file containing `NOTY4M`: `X -o $P\bad.264 $P\bad.y4m`
→ exit −1, `y4m [error]: bad sequence header magic` + `x264 [error]: could not open input file ...`.

**E7 — raw needs resolution.** `X --frames 10 -o $P\raw_nores.264 $P\test.i420`
→ exit −1, `raw [error]: raw input requires a resolution.` + `x264 [error]: could not open input file '<path>' via any method!`.

**E8 — raw defaults.** `X --input-res 64x64 --frames 2 -o $P\raw_nofps.264 $P\test.i420`
→ `raw [info]: 64x64p 0:0 @ 25/1 fps (cfr)`, no `using SAR` line. Adding `--fps 30` changed the
info line to `@ 30/1 fps (cfr)`.

**E9 — stdin.** `X --frames 5 -o $P\stdin_y4m.264 -` fed Y4M bytes
→ exit −1, `raw [error]: raw input requires a resolution.` Adding `--demuxer y4m` → exit 0,
`y4m [info]: 64x64p 1:1 @ 10/1 fps (cfr)`. Feeding raw I420 with `--input-res 64x64 --fps 10`
→ exit 0.

**E10 — frames 0.** `X --frames 0 --no-progress -o ... $P\test.y4m`
→ exit 0, `encoded 30 frames` (0 means unlimited).

**E11 — seek past EOF.** `X --seek 1000 --frames 2 --no-progress -o ... $P\test.y4m`
→ exit 0, no summary line, 0-byte output.

**E12 — silent clamping.** `X --crf 60 ...`, `--qp 100 ...`, `--aq-mode 9 ...`, `--subme 20 ...`,
`--threads 0 ...`, `--keyint 0 ...`, `--bframes -1 ...` all exited 0 without warnings;
`--qp 100` produced `Avg QP:51.00` frames and `--keyint 0` produced 30 I-frames in 30 frames.
By contrast `--crf abc`, `--me bogus`, `--fps bad`, `--sar bad`, `--deblock x:y`,
`--level bogus`, `--output-csp bogus` all exited −1 with `invalid argument`/`Unknown output csp`.

**E13 — SAR/VUI.** `X --sar 4:3 --no-progress -o ...` → ffprobe reported
`sample_aspect_ratio=4:3`, `display_aspect_ratio=4:3`. `X --colorprim bt709 --transfer bt709
--colormatrix bt709 --range tv --videoformat pal --overscan show` → exit 0, no warning.

**E14 — chroma/depth.** `X --output-csp i422 ...` → `resize [warning]: converting from yuv420p
to yuv422p` and `profile High 4:2:2, level 1.0, 4:2:2, 8-bit`; `--output-depth 10` →
`profile High 10, level 1.0, 4:2:0, 10-bit`, SEI `qpmax=81`.

**E15 — logging.** `--quiet`, `--log-level none`, `--log-level error`, `--log-level warning`
each produced only progress + `encoded ...` lines; `--log-level bogus` did the same (no error);
`--log-level debug` and `-v` added `x264 [debug]:` lines; `--no-progress` removed progress
lines only.

**E16 — determinism, fixed threads.** Two default 60-frame runs → identical SHA-256 prefix
`27338E199381A735`; two `--threads 4` runs → identical `FF19D9C66082DC7A`.

**E17 — non-determinism.** Two `--threads 4 --non-deterministic` runs → 81801 / 81817 bytes,
hashes `484BDD7EBA5FA5C5` / `745DFD351C910F0A`.

**E18 — ASM path.** Default hash `FA7573B14897`; `--no-asm`, `--asm 0`, `--cpu-independent`
all hash `5D27C9130392` (same 13907-byte output).

**E19 — scenecut.** 60-frame 3-scene clip, default → `frame I:2`; `--scenecut 0` → `frame I:1`;
`--keyint 10` → `frame I:6`; `--keyint infinite` → SEI `keyint=infinite keyint_min=10`.

**E20 — GOP coupling.** `X --keyint 5 ...` → SEI `keyint=5 keyint_min=1 rc_lookahead=5`.
`X --keyint 10 --min-keyint 1` → no additional I-frames compared with `--keyint 10` alone on the
test clip.

**E21 — rate control.** `--qp 20` → `rc=cqp mbtree=0 qp=20 aq=0`; `--bitrate 200` →
`rc=abr bitrate=200 ratetol=1.0`; `--crf 18` → `crf=18.0`.

**E22 — lossless.** `--qp 0` → `profile High 4:4:4 Predictive, level 1.0, 4:2:0, 8-bit`,
SEI `rc=cqp qp=0 psy=0 trellis=0 fast_pskip=0 bframes=0`. `--profile high --qp 0` → exit −1,
`x264 [error]: high profile doesn't support lossless`.

**E23 — deblock.** `--no-deblock` → `deblock=0:0:0`; `--deblock -2:-2` → `deblock=1:-2:-2`;
default → `deblock=1:0:0`.

**E24 — AQ / lookahead.** `--aq-mode 0` → `aq=1:0.00`; `--aq-mode 2` → `aq=2:1.00`;
`--aq-mode 3` → `aq=3:1.00`; `--aq-strength 0.5` → `aq=1:0.50`. `--rc-lookahead 0` →
warning `lookaheadless mb-tree requires intra refresh or infinite keyint` and `mbtree=0`.

**E25 — B-frames / refs / ME.** `--bframes 0` → no B-frame stats line and SEI omits
B-related fields; `--bframes 5` → `bframes=5`; `--ref 1` → `mixed_ref=0`; `--ref 8` → `ref=8`;
`--me dia` / `--me tesa` → `me=dia` / `me=tesa`; `--subme 11` with default trellis → `subme=9`.

**E26 — muxers.** `.264` and `.bin` produced byte-identical 2159-byte Annex B files;
`.mkv` (2352 bytes) and `.flv` (2411 bytes) are containers; `.mp4` failed with
`not compiled with MP4 output support`.

**E27 — two-pass.** `--pass 1 --bitrate 200 --stats $P\stats.log ...` → exit 0, stats file
1126 bytes; `--pass 2 --bitrate 200 --stats $P\stats.log ...` → exit 0;
`--pass 2 ... --stats $P\nostats.log` → exit −1,
`x264 [error]: ratecontrol_init: can't open stats file`.

**E28 — bad zones / filters / qpfile.** `--zones junk` → `x264 [error]: invalid zone: "junk"`;
`--vf bogus` → `x264 [error]: invalid filter 'bogus'`; missing `--qpfile` → `x264 [error]:
can't open qpfile '<arg>'`. All exit −1.

---

## 10. Explicit unknowns

- Exact numeric ranges/clamping rules for most integer options beyond the cases in E12/E14;
  help text gives nominal ranges, observed behaviour is often silent clamping.
- Exact formula for automatic thread count on a given CPU/resolution.
- Exact derivation of `chroma_qp_offset` from chroma format, psy settings and QP mode
  (observed values: -2 default 4:2:0, -3 film, -4 grain/stillimage, +4 4:4:4, 0 with `--no-psy`,
  3 when user asked for 5 with CQP).
- Whether multiple comma-separated tunes beyond one psy tune are accepted and how they combine.
- Effect of VUI-only options on decoder-visible flags beyond the simple ffprobe check.
- Behaviour of `--opencl`, `--dump-yuv`, `--tcfile-in/out`, `--dts-compress`, `--index`,
  `--mastering-display`, `--cll`, `--avcintra-*`, `--bluray-compat`, `--pulldown`,
  `--frame-packing`, slice-control options, and the CQM sub-options — accepted by the parser
  but not probe-verified in depth.
- Exact floating-point PSNR/SSIM values for arbitrary inputs (they depend on reconstruction and
  are not a CLI contract).
- Exit codes other than 0 and −1 (none observed).
