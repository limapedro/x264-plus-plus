#ifndef X264PP_CORE_PARAMS_H
#define X264PP_CORE_PARAMS_H

/*
 * Parameter derivation, SPS/PPS structures and level limits.
 *
 * Original code for x264++ (increment I1).  All structures and rules are
 * derived from spec/h264-syntax.md, docs/rebuild/encoder/set.c.md and the
 * published H.264 standard (Annex A, Annex E).  No reference source was used.
 */

#include "core/api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Profile identifiers used here.  I1 emits CAVLC and 4x4 transforms only;
 * High profile is signalled (with transform_8x8_mode_flag = 0) when the
 * caller requests the 8x8 transform or a non-flat quantisation matrix. */
enum
{
    X264PP_PROFILE_BASELINE = 66,
    X264PP_PROFILE_HIGH     = 100
};

/* One row of the level limit table (H.264 Annex A, Table A-1).  Zero means
 * "no explicit limit" for the optional fields. */
typedef struct x264pp_level
{
    int  level_idc;       /* 10, 9 (1b), 11, ... 62 */
    int  max_mbps;        /* MaxMBPS */
    int  max_fs;          /* MaxFS */
    int  max_dpb_mbs;     /* MaxDpbMbs, -1 == unbounded (level 1b) */
    int  max_br;          /* MaxBR in kbit/s */
    int  max_cpb;         /* MaxCPB in kbit */
    int  max_ref;         /* nominal maximum reference frames */
    int  frame_only;      /* 1 if 8x8dct / interlaced tools are forbidden */
    int  mv_range;        /* max vertical MV range */
} x264pp_level_t;

/* Sequence parameter set (only the fields needed by I1). */
typedef struct x264pp_sps
{
    int  profile_idc;
    int  constraint_flag[6];
    int  level_idc;
    int  sps_id;

    int  chroma_format_idc;      /* 1 = 4:2:0 */
    int  bit_depth_luma_minus8;
    int  bit_depth_chroma_minus8;
    int  qpprime_y_zero_bypass;
    int  scaling_matrix_present;

    int  log2_max_frame_num;     /* actual value, 4..16 */
    int  poc_type;               /* 0 or 2 in I1 */
    int  log2_max_poc_lsb;       /* actual value, 4..16 when poc_type == 0 */
    int  max_num_ref_frames;
    int  gaps_allowed;

    int  mb_width;
    int  mb_height;
    int  frame_mbs_only;
    int  mbaff;
    int  direct8x8;

    int  b_crop;
    int  crop[4];                /* left, right, top, bottom */

    int  vui_present;
    int  sar_num, sar_den;
    int  sar_idc;                /* 0 == not in table, 255 == extended */
    int  overscan_present, overscan_appropriate;
    int  video_signal_present;
    int  video_format, full_range, colour_description_present;
    int  colour_primaries, transfer_characteristics, matrix_coefficients;
    int  chroma_loc_present, chroma_loc_top, chroma_loc_bottom;
    int  timing_present;
    unsigned num_units_in_tick, time_scale;
    int  fixed_frame_rate;
    int  nal_hrd, vcl_hrd, low_delay_hrd;
    int  pic_struct_present;
    int  bitstream_restriction;
    int  motion_vectors_over_pic_boundaries;
    int  max_bytes_per_pic_denom, max_bits_per_mb_denom;
    int  log2_max_mv_length_horizontal, log2_max_mv_length_vertical;
    int  max_num_reorder_frames, max_dec_frame_buffering;
} x264pp_sps_t;

/* Picture parameter set (only the fields needed by I1). */
typedef struct x264pp_pps
{
    int  pps_id;
    int  sps_id;
    int  entropy_cabac;          /* always 0 in I1 */
    int  bottom_field_order;
    int  num_slice_groups_minus1;
    int  num_ref_idx_l0_default_active_minus1;
    int  num_ref_idx_l1_default_active_minus1;
    int  weighted_pred;
    int  weighted_bipred_idc;
    int  pic_init_qp;            /* absolute QP, 0..51 */
    int  pic_init_qs;
    int  chroma_qp_index_offset;
    int  second_chroma_qp_index_offset;
    int  deblocking_control_present;
    int  constrained_intra;
    int  redundant_pic_cnt_present;
    int  transform8x8;           /* always 0 in I1 */
    int  scaling_matrix_present; /* always 0 in I1 */
} x264pp_pps_t;

/* A compact preset table.  The CLI has already applied its own preset to the
 * parameter block; this table records only the fields that are meaningful to
 * the I1 core and is kept explicit so later increments can refine it. */
typedef struct x264pp_preset
{
    const char *name;
    int         dct_decimate;
    int         subme;
    int         trellis;
    int         aq_mode;
    int         deadzone_inter;
    int         deadzone_intra;
} x264pp_preset_t;

extern const x264pp_preset_t x264pp_presets[];
extern const int x264pp_preset_count;
extern const x264pp_level_t x264pp_levels[];

const x264pp_preset_t *x264pp_find_preset( const char *name );

/* Validate and derive all parameters that the encoder core relies upon.
 * `out` receives the derived copy; `in` may equal `out`.  Returns X264PP_OK
 * or X264PP_ERR_INVALID (with a diagnostic already written to stderr). */
int x264pp_params_validate( const x264pp_param_t *in, x264pp_param_t *out );

/* Derive the SPS/PPS from a validated parameter block. */
void x264pp_sps_init( x264pp_sps_t *sps, const x264pp_param_t *param );
void x264pp_pps_init( x264pp_pps_t *pps, const x264pp_param_t *param,
                      const x264pp_sps_t *sps );

/* Level helpers.  `x264pp_level_auto` returns a level_idc (0 on failure). */
int x264pp_level_auto( const x264pp_param_t *param, const x264pp_sps_t *sps );
const x264pp_level_t *x264pp_level_find( int level_idc );
int x264pp_level_validate( const x264pp_param_t *param,
                           const x264pp_sps_t *sps, int verbose );

/* Simple deterministic QP selection for the I1 constant-QP/CRF approximation. */
int x264pp_qp_for_frame( const x264pp_param_t *param );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_PARAMS_H */
