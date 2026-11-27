# H.264/AVC bitstream surface — implementer specification

Status: **normative surface for the clean-room encoder**. Everything below is derived only from (a) the published ITU-T H.264 / ISO IEC 14496-10 standard, (b) public documentation, and (c) black-box observation of `oracle/x264.exe` **output**. No reference encoder source code was read, quoted, or summarized.

## 0. Sources and probe evidence

Published standard (freely available from ITU):

- ITU-T Rec. H.264 (05/2003), *Advanced video coding for generic audiovisual services* — <https://www.itu.int/rec/dologin_pub.asp?lang=e&id=T-REC-H.264-200305-S!!PDF-E&type=items>. Used for Annex B, base SPS/PPS/slice/macroblock syntax, CAVLC/CABAC, deblocking filter, VUI, SEI message types.
- ITU-T Rec. H.264 (01/2012), 7th edition — <https://www.itu.int/rec/dologin_pub.asp?lang=e&id=T-REC-H.264-201201-S!!PDF-E&type=items>. Used for High-profile SPS/PPS extensions, current syntax-element names, mb_type tables, CAVLC/CABAC table numbering.
- Recommendation landing page: <https://www.itu.int/rec/T-REC-H.264/en>.
- Public overview: Wikipedia, *Advanced Video Coding* — <https://en.wikipedia.org/wiki/Advanced_Video_Coding>; RFC 3984 — <https://www.rfc-editor.org/rfc/rfc3984> (NAL/parameter-set concepts only).

Black-box probes used here (probe scripts in `spec/probes/`, media kept outside the repo):

- `oracle/x264.exe` version banner: `x264 0.164.x`, built Nov 27 2026, configuration `--chroma-format=all`.
- `ffmpeg -bsf:v trace_headers` on oracle output gave the exact syntax-element names, bit widths and bit offsets quoted in the "Observed" notes below.
- `ffprobe -show_frames` on a 10-frame 64×64 encoding gave display order `I B B P B B P B B P`.
- A byte scan of a 10-frame 64×64 Annex B file gave the NAL header bytes and start-code widths quoted below.

Where a behaviour was not checked by these probes, it is marked **UNKNOWN**.

## 1. Bit-level conventions

### 1.1 Descriptors (standard 7.2)

| Descriptor | Meaning | Bits |
|---|---|---|
| `f(n)` | fixed-value bit pattern of `n` bits | `n` fixed |
| `u(n)` | unsigned integer, MSB first | `n` |
| `b(8)` | byte | 8 |
| `ue(v)` | unsigned Exp-Golomb | variable |
| `se(v)` | signed Exp-Golomb | variable |
| `te(v)` | truncated Exp-Golomb | variable |
| `me(v)` | mapped Exp-Golomb (used for `coded_block_pattern`) | variable |
| `ce(v)` | context-adaptive VLC (CAVLC coefficient code) | variable |
| `ae(v)` | CABAC binary arithmetic coded bin string | variable |

Bit order is **MSB first** within every byte. All syntax structures terminate with `rbsp_trailing_bits()`: `rbsp_stop_one_bit = 1`, followed by zero or more `rbsp_alignment_zero_bit = 0` until byte-aligned (standard 7.3.2.11).

### 1.2 Exp-Golomb (standard 9.1, Tables 9-1…9-3)

A `ue(v)` value `codeNum` is encoded as `k` leading zero bits, a `1`, then a `k`-bit suffix; `codeNum = 2^k − 1 + suffix`. Examples: `1 → 1`, `010 → 2`, `011 → 3`, `00100 → 4`.

`se(v)` maps `codeNum` as: `0 → 0`, odd `codeNum → (codeNum+1)/2`, even `codeNum → −codeNum/2` (i.e. `1→1, 2→−1, 3→2, 4→−2`).

`te(v)` is `ue(v)` when the inferred maximum is > 1, else a single bit `0` when the maximum is 1. `me(v)` uses Table 9-4 to map `codeNum` to `coded_block_pattern`.

### 1.3 RBSP / EBSP / emulation prevention (standard 7.3.1)

`nal_unit( NumBytesInNALunit )`:

```
forbidden_zero_bit          f(1)   // shall be 0
nal_ref_idc                 u(2)
nal_unit_type               u(5)
for each subsequent byte:
  if next 24 bits == 0x000003:
    2 RBSP bytes, then emulation_prevention_three_byte = 0x03 (f(8), discarded)
  else:
    rbsp_byte               b(8)
```

An encoder must insert `0x03` after any two consecutive zero bytes whenever the next byte would be `0x00`, `0x01`, `0x02` or `0x03`, so that neither `0x000001` nor `0x000000` appears inside a NAL unit. **Observed:** the oracle SPS payload contains the byte sequence `00 00 03 00 04 00 00 …` for a 32×32 encoding, confirming insertion and decoding of `emulation_prevention_three_byte`.

### 1.4 Annex B byte stream (standard B.1.1)

```
byte_stream_nal_unit( NumBytesInNALunit ) {
  while next_bits(24) != 0x000001 && next_bits(32) != 0x00000001
      leading_zero_8bits                 // 0x00, only at stream start
  if next_bits(24) != 0x000001
      zero_byte                          // 0x00
  start_code_prefix_one_3bytes           // 0x000001, f(24)
  nal_unit( NumBytesInNALunit )
  while more_data && not a new start code
      trailing_zero_8bits                // 0x00
}
```

`zero_byte` **shall** be present for SPS (type 7), PPS (type 8), and for the first NAL unit of an access unit; it is optional elsewhere. **Observed:** SPS, PPS and every VCL slice NAL in the probe file used a **4-byte** prefix `00 00 00 01`; the SEI NAL used a **3-byte** prefix `00 00 01`. A 4-byte prefix is decoded as `zero_byte` + `start_code_prefix_one_3bytes`.

## 2. NAL unit layer

### 2.1 Header (standard 7.3.1 / Table 7-1)

| Element | Descriptor | Constraint |
|---|---|---|
| `forbidden_zero_bit` | `f(1)` | 0 |
| `nal_ref_idc` | `u(2)` | 0 for SEI(6), AUD(9), end-of-seq(10), end-of-stream(11), filler(12); **nonzero** for SPS(7), PPS(8), IDR(5) and any reference-picture slice |
| `nal_unit_type` | `u(5)` | see below |

| Type | Content | Typical header byte (observed) |
|---|---|---|
| 1 | Coded slice of a non-IDR picture | `0x41` (ref=2), `0x01` (ref=0) |
| 5 | Coded slice of an IDR picture | `0x65` (ref=3) |
| 6 | SEI (`sei_rbsp()`) | `0x06` (ref=0) |
| 7 | SPS (`seq_parameter_set_rbsp()`) | `0x67` (ref=3) |
| 8 | PPS (`pic_parameter_set_rbsp()`) | `0x68` (ref=3) |
| 9 | Access unit delimiter | `0x09` (ref=0) — not emitted by default in probes |
| 10 | End of sequence | `0x0A` |
| 11 | End of stream | `0x0B` |
| 12 | Filler data | `0x0C` |

Types 2–4 (data partitions), 13–23 (reserved) and 24–31 (unspecified) are in the standard but were **not** emitted by the oracle in these probes. Support for them by a new encoder is **UNKNOWN / not required by the usage model**.

### 2.2 Access unit ordering (standard 7.4.1.2)

Within an access unit, in decoding order: optional AUD(9) → optional SEI(6) → SPS(7)/PPS(8) needed by the primary coded picture → one or more VCL slice NALs (1 or 5) → optional end-of-sequence(10)/end-of-stream(11)/filler(12). **Observed first AU:** SPS, PPS, SEI, IDR slice. **Observed later AUs:** one VCL NAL per frame; SPS/PPS were not repeated by default (repeat-headers behaviour is option-dependent and marked UNKNOWN).

A picture with `nal_unit_type == 5` in any VCL NAL is an IDR picture; all VCL NALs of that picture must have type 5.

## 3. Sequence parameter set (SPS)

`seq_parameter_set_rbsp()` = `seq_parameter_set_data()` followed by `rbsp_trailing_bits()` (standard 7.3.2.1 / 7.3.2.1.1, 2012 edition).

```
seq_parameter_set_data() {
  profile_idc                                  u(8)
  constraint_set0_flag                         u(1)
  constraint_set1_flag                         u(1)
  constraint_set2_flag                         u(1)
  constraint_set3_flag                         u(1)
  constraint_set4_flag                         u(1)
  constraint_set5_flag                         u(1)
  reserved_zero_2bits                          u(2)   // = 0
  level_idc                                    u(8)
  seq_parameter_set_id                         ue(v)
  if profile_idc in {100,110,122,244,44,83,86,118,128}:
    chroma_format_idc                          ue(v)
    if chroma_format_idc == 3:
      separate_colour_plane_flag               u(1)
    bit_depth_luma_minus8                      ue(v)
    bit_depth_chroma_minus8                    ue(v)
    qpprime_y_zero_transform_bypass_flag       u(1)
    seq_scaling_matrix_present_flag            u(1)
    if seq_scaling_matrix_present_flag:
      for i = 0 .. ((chroma_format_idc != 3) ? 8 : 11):
        seq_scaling_list_present_flag[i]       u(1)
        if present:
          if i < 6: scaling_list(ScalingList4x4[i],16,UseDefaultScalingMatrix4x4Flag[i])
          else:     scaling_list(ScalingList8x8[i-6],64,UseDefaultScalingMatrix8x8Flag[i-6])
  log2_max_frame_num_minus4                    ue(v)
  pic_order_cnt_type                           ue(v)
  if pic_order_cnt_type == 0:
    log2_max_pic_order_cnt_lsb_minus4          ue(v)
  else if pic_order_cnt_type == 1:
    delta_pic_order_always_zero_flag           u(1)
    offset_for_non_ref_pic                     se(v)
    offset_for_top_to_bottom_field             se(v)
    num_ref_frames_in_pic_order_cnt_cycle      ue(v)
    for i = 0 .. cycle-1: offset_for_ref_frame[i] se(v)
  max_num_ref_frames                           ue(v)
  gaps_in_frame_num_value_allowed_flag         u(1)
  pic_width_in_mbs_minus1                      ue(v)
  pic_height_in_map_units_minus1               ue(v)
  frame_mbs_only_flag                          u(1)
  if !frame_mbs_only_flag:
    mb_adaptive_frame_field_flag               u(1)
  direct_8x8_inference_flag                    u(1)
  frame_cropping_flag                          u(1)
  if frame_cropping_flag:
    frame_crop_left_offset                     ue(v)
    frame_crop_right_offset                    ue(v)
    frame_crop_top_offset                      ue(v)
    frame_crop_bottom_offset                   ue(v)
  vui_parameters_present_flag                  u(1)
  if vui_parameters_present_flag:
    vui_parameters()                           // clause E.1, see §10
}
```

`scaling_list()` (7.3.2.1.1.1): `lastScale = 8; nextScale = 8; for j: if nextScale != 0 { delta_scale se(v); nextScale = (lastScale + delta_scale + 256) % 256; defaultMatrixFlag = (j==0 && nextScale==0) }; scalingList[j] = (nextScale==0) ? lastScale : nextScale; lastScale = scalingList[j]`.

### 3.1 Constraints (standard 7.4.2.1)

- `profile_idc`: 66 Baseline, 77 Main, 88 Extended, 100 High, 110 High 10, 122 High 4:2:2, 244 High 4:4:4, etc. Constraint flags signal constrained subsets (e.g. `constraint_set0_flag=1, constraint_set1_flag=1` is Constrained Baseline).
- `seq_parameter_set_id` ∈ 0…31.
- `log2_max_frame_num_minus4` ∈ 0…12.
- `pic_order_cnt_type` ∈ 0…2; `log2_max_pic_order_cnt_lsb_minus4` ∈ 0…12.
- `max_num_ref_frames` limited by the level's DPB capacity.
- `pic_width_in_mbs_minus1 + 1` = picture width in macroblocks; `pic_height_in_map_units_minus1 + 1` = height in map units (macroblock pairs if MBAFF). Luma width = `(pic_width_in_mbs_minus1+1)*16`; luma height = `(pic_height_in_map_units_minus1+1)*16*(2 − frame_mbs_only_flag)` minus cropping.
- `frame_mbs_only_flag = 1` means progressive frame coding only; `field_pic_flag`, `bottom_field_flag` and `mb_adaptive_frame_field_flag` are then absent/inferred. The usage-model encodings are progressive.
- `direct_8x8_inference_flag` shall be 1 for Baseline/Main/Extended profiles in non-field mode; High profile may set it 0.
- Cropping offsets are constrained by chroma format and `frame_mbs_only_flag` (standard 7.4.2.1.3).

### 3.2 Black-box observations

32×32, 1 frame, `--preset medium --crf 30` (High profile):

`profile_idc=100`, all six constraint flags 0, `reserved_zero_2bits=0`, `level_idc=10`, `seq_parameter_set_id=0`, `chroma_format_idc=1` (4:2:0), `bit_depth_luma_minus8=0`, `bit_depth_chroma_minus8=0`, `qpprime_y_zero_transform_bypass_flag=0`, `seq_scaling_matrix_present_flag=0`, `log2_max_frame_num_minus4=0`, `pic_order_cnt_type=0`, `log2_max_pic_order_cnt_lsb_minus4=2`, `max_num_ref_frames=4`, `gaps_in_frame_num_value_allowed_flag=0`, `pic_width_in_mbs_minus1=1`, `pic_height_in_map_units_minus1=1`, `frame_mbs_only_flag=1`, `direct_8x8_inference_flag=1`, `frame_cropping_flag=0`, `vui_parameters_present_flag=1`.

`--profile baseline` produced `profile_idc=66`, `constraint_set0_flag=1`, `constraint_set1_flag=1`, and **none** of the High-profile extension fields. The 64×64, 5 fps encoding had the same SPS fields with `time_scale=10` in VUI, confirming `num_units_in_tick=1`, `time_scale = 2 × fps` for integer frame rates (black-box; fractional-rate behaviour UNKNOWN).

## 4. Picture parameter set (PPS)

`pic_parameter_set_rbsp()` (standard 7.3.2.2, 2012 edition):

```
pic_parameter_set_rbsp() {
  pic_parameter_set_id                         ue(v)
  seq_parameter_set_id                         ue(v)
  entropy_coding_mode_flag                     u(1)   // 0=CAVLC, 1=CABAC
  bottom_field_pic_order_in_frame_present_flag u(1)   // older name: pic_order_present_flag
  num_slice_groups_minus1                      ue(v)
  if num_slice_groups_minus1 > 0:
    slice_group_map_type                       ue(v)
    ... (slice group map syntax per 7.3.2.2; not used by oracle in probes)
  num_ref_idx_l0_default_active_minus1          ue(v)  // older: num_ref_idx_l0_active_minus1
  num_ref_idx_l1_default_active_minus1          ue(v)  // older: num_ref_idx_l1_active_minus1
  weighted_pred_flag                           u(1)
  weighted_bipred_idc                          u(2)
  pic_init_qp_minus26                          se(v)
  pic_init_qs_minus26                          se(v)
  chroma_qp_index_offset                       se(v)
  deblocking_filter_control_present_flag       u(1)
  constrained_intra_pred_flag                  u(1)
  redundant_pic_cnt_present_flag               u(1)
  if more_rbsp_data():                                 // High-profile extension
    transform_8x8_mode_flag                    u(1)
    pic_scaling_matrix_present_flag            u(1)
    if present:
      for i = 0 .. 6 + ((chroma_format_idc != 3) ? 2 : 6)*transform_8x8_mode_flag − 1:
        pic_scaling_list_present_flag[i]       u(1)
        if present: scaling_list(...) as in SPS
    second_chroma_qp_index_offset              se(v)
  rbsp_trailing_bits()
}
```

Constraints: `pic_parameter_set_id` ∈ 0…255; `seq_parameter_set_id` ∈ 0…31; `weighted_bipred_idc` ∈ 0…2; `pic_init_qp_minus26` ∈ −26…+25; `chroma_qp_index_offset` and `second_chroma_qp_index_offset` ∈ −12…+12; `num_slice_groups_minus1` ≤ 7 (standard 7.4.2.2).

**Observed** (High, `--preset medium`): `pic_parameter_set_id=0`, `seq_parameter_set_id=0`, `entropy_coding_mode_flag=1` (or 0 with `--no-cabac`), `bottom_field_pic_order_in_frame_present_flag=0`, `num_slice_groups_minus1=0`, `num_ref_idx_l0_default_active_minus1=2`, `num_ref_idx_l1_default_active_minus1=0`, `weighted_pred_flag=1`, `weighted_bipred_idc=2`, `pic_init_qp_minus26=4`, `pic_init_qs_minus26=0`, `chroma_qp_index_offset=−2`, `deblocking_filter_control_present_flag=1`, `constrained_intra_pred_flag=0`, `redundant_pic_cnt_present_flag=0`, `transform_8x8_mode_flag=1`, `pic_scaling_matrix_present_flag=0`, `second_chroma_qp_index_offset=−2`.

## 5. Slice header

`slice_header()` (standard 7.3.3, 2012 edition) — order is normative:

```
slice_header() {
  first_mb_in_slice                            ue(v)
  slice_type                                   ue(v)
  pic_parameter_set_id                         ue(v)
  if separate_colour_plane_flag == 1:
    colour_plane_id                            u(2)
  frame_num                                    u(v)      // v = log2_max_frame_num_minus4 + 4
  if !frame_mbs_only_flag:
    field_pic_flag                             u(1)
    if field_pic_flag: bottom_field_flag       u(1)
  if IdrPicFlag (nal_unit_type == 5):
    idr_pic_id                                 ue(v)
  if pic_order_cnt_type == 0:
    pic_order_cnt_lsb                          u(v)      // v = log2_max_pic_order_cnt_lsb_minus4 + 4
    if bottom_field_pic_order_in_frame_present_flag && !field_pic_flag:
      delta_pic_order_cnt_bottom               se(v)
  if pic_order_cnt_type == 1 && !delta_pic_order_always_zero_flag:
    delta_pic_order_cnt[0]                     se(v)
    if bottom_field_pic_order_in_frame_present_flag && !field_pic_flag:
      delta_pic_order_cnt[1]                   se(v)
  if redundant_pic_cnt_present_flag:
    redundant_pic_cnt                          ue(v)
  if slice_type == B:
    direct_spatial_mv_pred_flag                u(1)
  if slice_type in {P, SP, B}:
    num_ref_idx_active_override_flag           u(1)
    if override:
      num_ref_idx_l0_active_minus1             ue(v)
      if slice_type == B:
        num_ref_idx_l1_active_minus1           ue(v)
  if nal_unit_type == 20: ref_pic_list_mvc_modification()   // out of scope
  else:                   ref_pic_list_modification()
  if (weighted_pred_flag && slice_type in {P,SP}) ||
     (weighted_bipred_idc == 1 && slice_type == B):
    pred_weight_table()
  if nal_ref_idc != 0:
    dec_ref_pic_marking()
  if entropy_coding_mode_flag && slice_type not in {I, SI}:
    cabac_init_idc                             ue(v)
  slice_qp_delta                               se(v)
  if slice_type in {SP, SI}:
    if slice_type == SP: sp_for_switch_flag    u(1)
    slice_qs_delta                             se(v)
  if deblocking_filter_control_present_flag:
    disable_deblocking_filter_idc              ue(v)
    if disable_deblocking_filter_idc != 1:
      slice_alpha_c0_offset_div2               se(v)
      slice_beta_offset_div2                   se(v)
  if num_slice_groups_minus1 > 0 && slice_group_map_type in 3..5:
    slice_group_change_cycle                   u(v)
}
```

`slice_type` code points (Table 7-6): 0=P, 1=B, 2=I, 3=SP, 4=SI; adding 5 (i.e. 5…9) means "all slices of the picture have this type". **Observed:** oracle used 7 for IDR, 5 for P and 6 for B slices. Other conformance constraints: `slice_qp_delta` is an `se(v)` chosen so that `QPY = 26 + pic_init_qp_minus26 + slice_qp_delta` lies in 0…51 (and subsequent `mb_qp_delta` keeps each macroblock `QPY` in range); `cabac_init_idc` ∈ 0…2; `disable_deblocking_filter_idc` ∈ 0…2; `slice_alpha_c0_offset_div2`, `slice_beta_offset_div2` ∈ −6…+6; `idr_pic_id` ∈ 0…65535 and must not change within a picture.

Sub-structures (standard 7.3.3.1…7.3.3.3):

```
ref_pic_list_modification() {
  if slice_type % 5 != 2 && slice_type % 5 != 4:
    ref_pic_list_modification_flag_l0          u(1)
    if flag: do {
      modification_of_pic_nums_idc             ue(v)
      if idc == 0 || 1: abs_diff_pic_num_minus1 ue(v)
      else if idc == 2: long_term_pic_num       ue(v)
    } while idc != 3
  if slice_type % 5 == 1:
    ref_pic_list_modification_flag_l1          u(1)
    if flag: do { ...same loop for L1... } while idc != 3
}

pred_weight_table() {
  luma_log2_weight_denom                       ue(v)
  if ChromaArrayType != 0:
    chroma_log2_weight_denom                   ue(v)
  for i = 0 .. num_ref_idx_l0_active_minus1:
    luma_weight_l0_flag[i]                     u(1)
    if set: luma_weight_l0[i] se(v); luma_offset_l0[i] se(v)
    chroma_weight_l0_flag[i]                   u(1)
    if set: for j=0..1: chroma_weight_l0[i][j] se(v), chroma_offset_l0[i][j] se(v)
  if slice_type == B: analogous l1 arrays
}

dec_ref_pic_marking() {
  if nal_unit_type == 5:
    no_output_of_prior_pics_flag               u(1)
    long_term_reference_flag                   u(1)
  else:
    adaptive_ref_pic_marking_mode_flag         u(1)
    if adaptive: do {
      memory_management_control_operation      ue(v)
      if op in {1,3}: difference_of_pic_nums_minus1 ue(v)
      if op == 2: long_term_pic_num            ue(v)
      if op in {3,6}: long_term_frame_idx      ue(v)
      if op == 4: max_long_term_frame_idx_plus1 ue(v)
    } while operation != 0
}
```

**Observed** (64×64, 10 frames, `--preset medium --bframes 2`): IDR slice had `first_mb_in_slice=0`, `slice_type=7`, `pic_parameter_set_id=0`, `frame_num` 4 bits `0000`, `idr_pic_id=0`, `pic_order_cnt_lsb` 6 bits `000000`, `no_output_of_prior_pics_flag=0`, `long_term_reference_flag=0`, `slice_qp_delta=−1`, `disable_deblocking_filter_idc=0`, both offsets 0. P slices had `slice_type=5`, `num_ref_idx_active_override_flag=1`, `cabac_init_idc=0`; B slices had `slice_type=6`, `direct_spatial_mv_pred_flag=1`, both `num_ref_idx_*_active_minus1`, and **omitted** `dec_ref_pic_marking()` when `nal_ref_idc=0`. CABAC slices ended the header with `cabac_alignment_one_bit = 1` bits to reach byte alignment; CAVLC slices did not.

## 6. Slice data and macroblock layer

`slice_layer_without_partitioning_rbsp()` = `slice_header()` + `slice_data()` + `rbsp_slice_trailing_bits()`. `rbsp_slice_trailing_bits()` = `rbsp_trailing_bits()`, plus zero or more `cabac_zero_word = 0x0000` (`f(16)`) when CABAC is active (standard 7.3.2.8 / 7.3.2.10).

`slice_data()` (standard 7.3.4):

```
if entropy_coding_mode_flag:
  while !byte_aligned(): cabac_alignment_one_bit  f(1)  // = 1
CurrMbAddr = first_mb_in_slice * (1 + MbaffFrameFlag)
moreDataFlag = 1; prevMbSkipped = 0
do {
  if slice_type not in {I,SI}:
    if !entropy_coding_mode_flag:
      mb_skip_run                              ue(v)
      advance CurrMbAddr mb_skip_run times
      moreDataFlag = more_rbsp_data()
    else:
      mb_skip_flag                             ae(v)
      moreDataFlag = !mb_skip_flag
  if moreDataFlag:
    if MbaffFrameFlag and ...: mb_field_decoding_flag u(1)|ae(v)
    macroblock_layer()
  if !entropy_coding_mode_flag: moreDataFlag = more_rbsp_data()
  else:
    if slice_type not in {I,SI}: prevMbSkipped = mb_skip_flag
    if MbaffFrameFlag && CurrMbAddr % 2 == 0: moreDataFlag = 1
    else: end_of_slice_flag ae(v); moreDataFlag = !end_of_slice_flag
  CurrMbAddr = NextMbAddress(CurrMbAddr)
} while moreDataFlag
```

`macroblock_layer()` (standard 7.3.5, 2012 edition):

```
macroblock_layer() {
  mb_type                                      ue(v) | ae(v)
  if mb_type == I_PCM:
    while !byte_aligned(): pcm_alignment_zero_bit f(1)
    for i = 0..255: pcm_sample_luma[i]           u(v)  // BitDepthY bits
    for i = 0..2*MbWidthC*MbHeightC-1: pcm_sample_chroma[i] u(v)
  else:
    noSubMbPartSizeLessThan8x8Flag = 1
    if mb_type != I_NxN && MbPartPredMode(mb_type,0) != Intra_16x16 && NumMbPart(mb_type) == 4:
      sub_mb_pred(mb_type)
      ...derive noSubMbPartSizeLessThan8x8Flag...
    else:
      if transform_8x8_mode_flag && mb_type == I_NxN:
        transform_size_8x8_flag                u(1) | ae(v)
      mb_pred(mb_type)
    if MbPartPredMode(mb_type,0) != Intra_16x16:
      coded_block_pattern                      me(v) | ae(v)
      if CodedBlockPatternLuma > 0 && transform_8x8_mode_flag && mb_type != I_NxN &&
         noSubMbPartSizeLessThan8x8Flag &&
         (mb_type != B_Direct_16x16 || direct_8x8_inference_flag):
        transform_size_8x8_flag                u(1) | ae(v)
    if CodedBlockPatternLuma > 0 || CodedBlockPatternChroma > 0 ||
       MbPartPredMode(mb_type,0) == Intra_16x16:
      mb_qp_delta                              se(v) | ae(v)
      residual(0,15)                           // CAVLC or CABAC, per entropy mode
  }
}
```

`mb_pred(mb_type)` (7.3.5.1) — intra branch: for `Intra_4x4`, 16 × (`prev_intra4x4_pred_mode_flag` `u(1)|ae(v)`, and if 0 `rem_intra4x4_pred_mode` `u(3)|ae(v)`); for `Intra_8x8` (High profile), 4 × (`prev_intra8x8_pred_mode_flag` `u(1)|ae(v)`, if 0 `rem_intra8x8_pred_mode` `u(3)|ae(v)`); `intra_chroma_pred_mode` `ue(v)|ae(v)` when 4:2:0/4:2:2. Inter branch: `ref_idx_l0[]`, `ref_idx_l1[]` (`te(v)|ae(v)`, conditional on list size/direction), then `mvd_l0[][][2]`, `mvd_l1[][][2]` (`se(v)|ae(v)`) in that order. Direct prediction carries no MV/ref-index syntax.

`sub_mb_pred(mb_type)` (7.3.5.2): four `sub_mb_type[i]` (`ue(v)|ae(v)`), then conditional `ref_idx_l0[i]`, `ref_idx_l1[i]`, then `mvd_l0[i][j][2]`, `mvd_l1[i][j][2]`.

`mb_type` code points: I slices Table 7-11 (`0=I_NxN`, `1…24=I_16x16_<pred>_<cbpChroma>_<cbpLuma>`, `25=I_PCM`); P/SP slices Table 7-13 (`0=P_L0_16x16`, `1=P_L0_L0_16x8`, `2=P_L0_L0_8x16`, `3=P_8x8`, `4=P_8x8ref0`, plus I types at +5); B slices Table 7-14 (`0=B_Direct_16x16`, `1…21` the L0/L1/Bi partition combinations, `22=B_8x8`, plus I types at +23). Skip macroblocks are *inferred*, not coded: `P_Skip` / `B_Skip`.

`residual()` (7.3.5.3): `residual_block_cavlc(coeffLevel, maxNumCoeff)` or `residual_block_cabac(coeffLevel, maxNumCoeff)` selected by `entropy_coding_mode_flag`. Order: optional `Intra16x16DCLevel` (16 coefficients) for Intra_16x16; then 16 luma 4×4 blocks (or 8×8 blocks when `transform_size_8x8_flag` applies) in raster order; then for each chroma component the 2×2 DC block and four 4×4 AC blocks. `maxNumCoeff` is 16 for luma 4×4, 15 for Intra16x16 AC / chroma AC, 4 for chroma DC 2×2, and 64 for 8×8 transform blocks (standard 7.3.5.3 / 8.5).

## 7. Entropy coding alternatives

### 7.1 CAVLC (`entropy_coding_mode_flag = 0`)

`residual_block_cavlc()` (standard 7.3.5.3.1) element order and descriptors:

```
coeff_token                                  ce(v)
if TotalCoeff(coeff_token) > 0:
  suffixLength = (TotalCoeff > 10 && TrailingOnes < 3) ? 1 : 0
  for i = 0 .. TotalCoeff-1:
    if i < TrailingOnes: trailing_ones_sign_flag u(1)
    else: level_prefix ce(v);
          if suffixLength > 0 || level_prefix >= 14: level_suffix u(v)
          ... (levelCode adjustment, suffixLength adaptation per standard)
  if TotalCoeff < maxNumCoeff: total_zeros ce(v)
  for i = 0 .. TotalCoeff-2:
    if zerosLeft > 0: run_before ce(v)
  run[TotalCoeff-1] = zerosLeft
```

Code tables (2012 edition, clause 9.2): `coeff_token` = Table 9-5 (separate 4×4, 8×8 and chroma-DC variants, selected by `nC`); `level_prefix` = Table 9-6; `total_zeros` = Tables 9-7…9-9; `run_before` = Table 9-10. `coded_block_pattern` uses `me(v)` via Table 9-4. All other macroblock/slice elements use `ue(v)`, `se(v)` or `te(v)` as listed in §5–§6.

**Observed:** `--no-cabac` produced PPS `entropy_coding_mode_flag=0` and slice headers without `cabac_alignment_one_bit`. The 0x03 emulation-prevention byte in the SPS is independent of entropy mode.

### 7.2 CABAC (`entropy_coding_mode_flag = 1`)

`residual_block_cabac()` (standard 7.3.5.3.2): `coded_block_flag ae(v)`; if set, scan positions emitting `significant_coeff_flag ae(v)`, `last_significant_coeff_flag ae(v)` until the last significant coefficient, then, in reverse scan order, `coeff_abs_level_minus1 ae(v)` and `coeff_sign_flag ae(v)` (bypass-coded) for each significant coefficient.

Binarization, context indices and initial states are specified by Tables 9-11…9-45 (2012 edition); `cabac_init_idc` selects one of three initialization sets. `ae(v)` is also used for `mb_skip_flag`, `mb_type`, `sub_mb_type`, `transform_size_8x8_flag`, `intra_chroma_pred_mode`, intra prediction flags, `ref_idx_*`, `mvd_*`, `coded_block_pattern`, `mb_qp_delta`, `end_of_slice_flag`, and the residual flags above. The encoder must implement the normative arithmetic coder (9.3.3) and the CABAC alignment/termination bits of 7.3.4/7.3.2.10. **Observed:** default oracle encoding used CABAC; `cabac_init_idc=0` in all probed slices.

## 8. Supplemental enhancement information (SEI)

`sei_rbsp()` (standard 7.3.2.3):

```
do { sei_message() } while more_rbsp_data()
rbsp_trailing_bits()
```

`sei_message()`:

```
payloadType = 0
while next_bits(8) == 0xFF: ff_byte f(8) (=0xFF); payloadType += 255
last_payload_type_byte  u(8); payloadType += last_payload_type_byte
payloadSize = 0
while next_bits(8) == 0xFF: ff_byte f(8) (=0xFF); payloadSize += 255
last_payload_size_byte  u(8); payloadSize += last_payload_size_byte
sei_payload(payloadType, payloadSize)
```

`sei_payload` dispatches on `payloadType` (Annex D.1): 0=`buffering_period`, 1=`pic_timing`, 2=`pan_scan_rect`, 3=`filler_payload`, 4=`user_data_registered_itu_t_t35`, 5=`user_data_unregistered`, 6=`recovery_point`, 7=`dec_ref_pic_marking_repetition`, 8=`spare_pic`, 9=`scene_info`, 10=`sub_seq_info`, 11=`sub_seq_layer_characteristics`, 12=`sub_seq_characteristics`, 13=`full_frame_freeze`, 14=`full_frame_freeze_release`, 15=`full_frame_snapshot`, 16/17=progressive refinement segment start/end, 18=`motion_constrained_slice_group_set`, 19=`film_grain_characteristics`, 20=`deblocking_filter_display_preference`, 21=`stereo_video_info`, 22=`post_filter_hint`, 23=`tone_mapping_info`, etc. SEI messages do not affect reconstruction of luma/chroma samples.

`user_data_unregistered(payloadSize)` (D.1.6): 16 × `uuid_iso_iec_11578` `u(8)`, then `payloadSize − 16` × `user_data_payload_byte` `u(8)`. `recovery_point()` carries `recovery_frame_cnt ue(v)`, `exact_match_flag u(1)`, `broken_link_flag u(1)`, `changing_slice_group_idc u(2)`.

**Observed:** oracle default emitted an SEI NAL (`0x06`, nal_ref_idc=0) before the IDR slice, with `payloadType=5`, `payloadSize` encoded as `FF FF 9A` = 154 bytes, a 16-byte UUID, then an ASCII version/configuration string beginning `x264 - core 164 …`. The exact UUID/text are encoder-chosen metadata and are **not** required to match in the clean-room implementation (bitstream bytes need not match). Other SEI messages were not emitted in the default probes; their option-dependent emission is **UNKNOWN**.

## 9. In-loop deblocking filter (standard 8.7)

The filter is applied to the reconstructed picture **before** it is stored as a reference and output, per macroblock in increasing `mbAddr`: luma vertical edges left→right, then luma horizontal edges top→bottom (four 16-sample luma edges in each direction per MB), then Cb and Cr vertical and horizontal edges (two 8-sample edges each). Picture-boundary edges are never filtered. Samples already filtered above/left are used as input for subsequent filtering.

Control from slice header:

| `disable_deblocking_filter_idc` | Effect |
|---|---|
| 0 | Filter all 4×4 block edges, including slice boundaries |
| 1 | No filtering anywhere |
| 2 | Filter all 4×4 block edges **except** slice boundaries |

If absent, inferred 0. Offsets: `FilterOffsetA = slice_alpha_c0_offset_div2 << 1`, `FilterOffsetB = slice_beta_offset_div2 << 1` (each offset in −6…+6 → filter offset −12…+12). For an edge with average quantizer `qPav = (qPp + qPq + 1) >> 1`:

```
indexA = Clip3(0,51, qPav + FilterOffsetA)
indexB = Clip3(0,51, qPav + FilterOffsetB)
alpha = AlphaTable[indexA]; beta = BetaTable[indexB]   // Table 8-14
filterSamplesFlag = (bS != 0 && |p0−q0| < alpha && |p1−p0| < beta && |q1−q0| < beta)
```

Boundary strength `bS` (8.7.2.1) is 4 for intra/SP/SI macroblock edges (with field-mode refinements), 3 for other intra edges, 2 when either adjacent 4×4 block has nonzero transform coefficients, 1 for differing references/MVs (MV difference ≥ 4 quarter-luma samples), else 0. When `filterSamplesFlag=1`: use the strong filter (8.7.2.4) for `bS==4`, otherwise the weak filter (8.7.2.3) with clipping `Δ = Clip3(−tC, tC, ((((q0−p0)<<2)+(p1−q1)+4)>>3))`, `p'0 = Clip1(p0+Δ)`, `q'0 = Clip1(q0−Δ)`, `tC` from Table 8-15; plus the inner-sample rules of 8.7.2.3/8.7.2.4. Chroma edges use the `bS` of the corresponding luma edge and are never filtered when `bS==0`.

All threshold/clipping tables and the exact sample-update equations are normative in standard 8.7.2.2…8.7.2.4 (Tables 8-14, 8-15). A conforming encoder need only emit the control syntax above; the *decoder* applies this process, so the encoder must model it when forming predictions.

**Observed:** all probed slices used `disable_deblocking_filter_idc=0`, `slice_alpha_c0_offset_div2=0`, `slice_beta_offset_div2=0`.

## 10. Video usability information (VUI)

Present iff `vui_parameters_present_flag = 1` in SPS. Syntax (standard E.1.1, 2012 edition):

```
vui_parameters() {
  aspect_ratio_info_present_flag               u(1)
  if set:
    aspect_ratio_idc                           u(8)   // 255 = Extended_SAR
    if aspect_ratio_idc == 255:
      sar_width                                u(16)
      sar_height                               u(16)
  overscan_info_present_flag                   u(1)
  if set: overscan_appropriate_flag            u(1)
  video_signal_type_present_flag               u(1)
  if set:
    video_format                               u(3)
    video_full_range_flag                      u(1)
    colour_description_present_flag            u(1)
    if set:
      colour_primaries                         u(8)
      transfer_characteristics                 u(8)
      matrix_coefficients                      u(8)
  chroma_loc_info_present_flag                 u(1)
  if set:
    chroma_sample_loc_type_top_field           ue(v)
    chroma_sample_loc_type_bottom_field        ue(v)
  timing_info_present_flag                     u(1)
  if set:
    num_units_in_tick                          u(32)
    time_scale                                 u(32)
    fixed_frame_rate_flag                      u(1)
  nal_hrd_parameters_present_flag              u(1)
  if set: hrd_parameters()
  vcl_hrd_parameters_present_flag              u(1)
  if set: hrd_parameters()
  if nal_hrd || vcl_hrd: low_delay_hrd_flag    u(1)
  pic_struct_present_flag                      u(1)
  bitstream_restriction_flag                   u(1)
  if set:
    motion_vectors_over_pic_boundaries_flag    u(1)
    max_bytes_per_pic_denom                    ue(v)
    max_bits_per_mb_denom                      ue(v)
    log2_max_mv_length_horizontal              ue(v)
    log2_max_mv_length_vertical                ue(v)
    max_num_reorder_frames                     ue(v)
    max_dec_frame_buffering                    ue(v)
}
```

`hrd_parameters()` (E.1.2) begins `cpb_cnt_minus1 ue(v)`, `bit_rate_scale u(4)`, `cpb_size_scale u(4)`, then per-ScheduleSel entries and delay-length fields; HRD was absent in all probes, so its option-dependent details are **UNKNOWN** beyond the standard syntax.

**Observed** (both 1-frame 1-fps and 10-frame 5-fps encodings): `vui_parameters_present_flag=1`; all aspect/overscan/video-signal/chroma-loc flags 0; `timing_info_present_flag=1`, `num_units_in_tick=1`, `time_scale=2` (1 fps) or `10` (5 fps), `fixed_frame_rate_flag=1`; both HRD flags 0; `pic_struct_present_flag=0`; `bitstream_restriction_flag=1`, `motion_vectors_over_pic_boundaries_flag=1`, `max_bytes_per_pic_denom=0`, `max_bits_per_mb_denom=0`, `log2_max_mv_length_horizontal=8`, `log2_max_mv_length_vertical=8`, `max_num_reorder_frames=2` (default 3 B-frames), `max_dec_frame_buffering=4`.

## 11. Out of scope / explicitly UNKNOWN

The following are defined by the standard but were neither required by the probe campaign nor observed; an implementation may reject or ignore them unless a future spec extends the usage model:

- Interlaced coding, PAFF, MBAFF, `field_pic_flag`, `mb_field_decoding_flag` behaviour beyond the syntax positions listed above — **UNKNOWN** (probe encodings were progressive, `frame_mbs_only_flag=1`).
- FMO/ASO, slice groups, redundant pictures, data partitions (NAL types 2–4), SP/SI slices — **UNKNOWN** (not emitted).
- Scalable/MVC extensions (Annex G/H, profile_idc 83/86/118/128 etc.), auxiliary pictures, 10-bit/4:2:2/4:4:4 profiles — **UNKNOWN** (oracle built with `--chroma-format=all`, but probes used 8-bit 4:2:0 High).
- Exact rate-control decisions, mode-selection heuristics, lookahead, and all encoder-internal algorithms are explicitly **out of scope**: only the emitted bitstream and decoded output are specified.
- Exact SEI payload bytes, repeat-header policy, and option-dependent emission of AUD/end-of-stream NALs — **UNKNOWN** beyond the default observations recorded above.
