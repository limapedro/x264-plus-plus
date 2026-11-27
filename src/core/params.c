/*
 * Parameter derivation, SPS/PPS structures and level limits.
 * Original code for x264++ (increment I1), written only from
 * spec/h264-syntax.md, docs/rebuild/encoder/set.c.md and the published
 * H.264 standard.  No reference source was consulted.
 */

#include "core/params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const x264pp_preset_t x264pp_presets[] = {
    { "ultrafast", 0, 0, 0, 0, 0, 0 },
    { "superfast", 0, 1, 0, 0, 0, 0 },
    { "veryfast",  0, 2, 0, 0, 0, 0 },
    { "faster",    0, 3, 0, 0, 0, 0 },
    { "fast",      1, 4, 0, 1, 0, 0 },
    { "medium",    1, 5, 0, 1, 0, 0 },
    { "slow",      1, 6, 1, 1, 0, 0 },
    { "slower",    1, 7, 2, 1, 0, 0 },
    { "veryslow",  1, 8, 2, 1, 0, 0 },
    { "placebo",   1, 9, 2, 1, 0, 0 }
};
const int x264pp_preset_count = (int)(sizeof(x264pp_presets) / sizeof(x264pp_presets[0]));

/* H.264 Annex A, Table A-1 (the subset the usage model can reach). */
const x264pp_level_t x264pp_levels[] = {
    { 10,    1485,    99,   396,    64,    175, 1, 0, 32 },
    {  9,    1485,    99,    -1,   128,    350, 1, 0, 32 }, /* 1b: unbounded DPB */
    { 11,    3000,   396,   900,   192,    500, 2, 0, 32 },
    { 12,    6000,   396,  2376,   384,   1000, 2, 0, 32 },
    { 13,   11880,   396,  2376,   768,   2000, 2, 0, 32 },
    { 20,   11880,   396,  2376,  2000,   2000, 2, 0, 32 },
    { 21,   19800,   792,  4752,  4000,   4000, 3, 0, 32 },
    { 22,   20250,  1620,  8100,  4000,   4000, 4, 0, 32 },
    { 30,   40500,  1620,  8100, 10000,  10000, 4, 0, 32 },
    { 31,  108000,  3600, 18000, 14000,  14000, 5, 0, 16 },
    { 32,  216000,  5120, 20480, 20000,  20000, 5, 0, 16 },
    { 40,  245760,  8192, 32768, 20000,  25000, 5, 0, 16 },
    { 41,  245760,  8192, 32768, 50000,  62500, 5, 0, 16 },
    { 42,  522240,  8704, 34816, 50000,  62500, 5, 0, 16 },
    { 50,  589824, 22080,110400,135000, 135000, 5, 0, 16 },
    { 51,  983040, 36864,184320,240000, 240000, 5, 0, 16 },
    { 52, 2073600, 36864,184320,240000, 240000, 5, 0, 16 },
    { 60, 4177920,139264,696320,240000, 240000, 5, 1, 16 },
    { 61, 8355840,139264,696320,480000, 480000, 5, 1, 16 },
    { 62,16711680,139264,696320,800000, 800000, 5, 1, 16 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0 } /* terminator */
};

#define LEVEL_COUNT ((int)(sizeof(x264pp_levels) / sizeof(x264pp_levels[0])) - 1)

/* Common sample aspect ratios from H.264 Table E-1 (idc 1..16). */
static const int sar_table[16][2] = {
    { 1, 1 }, {12,11}, {10,11}, {16,11}, {40,33}, {24,11}, {20,11}, {32,11},
    {80,33}, {18,11}, {15,11}, {64,33}, {160,99}, {4,3}, {3,2}, {2,1}
};

static int clamp_i( int v, int lo, int hi )
{
    return v < lo ? lo : (v > hi ? hi : v);
}

const x264pp_preset_t *x264pp_find_preset( const char *name )
{
    int i;
    if( !name )
        return NULL;
    for( i = 0; i < x264pp_preset_count; i++ )
        if( !strcmp( name, x264pp_presets[i].name ) )
            return &x264pp_presets[i];
    return NULL;
}

const x264pp_level_t *x264pp_level_find( int level_idc )
{
    int i;
    for( i = 0; i < LEVEL_COUNT; i++ )
        if( x264pp_levels[i].level_idc == level_idc )
            return &x264pp_levels[i];
    return NULL;
}

int x264pp_qp_for_frame( const x264pp_param_t *param )
{
    int qp;
    if( !param )
        return 26;
    if( param->rc == X264PP_RC_CQP && param->qp >= 0 )
        qp = param->qp;
    else
        qp = (int)(param->crf + 0.5);
    return clamp_i( qp, 0, 51 );
}

int x264pp_params_validate( const x264pp_param_t *in, x264pp_param_t *out )
{
    if( !in || !out )
        return X264PP_ERR_INVALID;
    if( in != out )
        *out = *in;

    if( out->width <= 0 || out->height <= 0 ||
        out->width > 16384 || out->height > 16384 )
    {
        fprintf( stderr, "x264 [error]: invalid resolution %dx%d\n",
                 out->width, out->height );
        return X264PP_ERR_INVALID;
    }
    if( out->width % 2 || out->height % 2 )
    {
        fprintf( stderr, "x264 [error]: 4:2:0 input requires even dimensions\n" );
        return X264PP_ERR_INVALID;
    }
    if( out->input_csp != X264PP_CSP_I420 && out->input_csp != X264PP_CSP_YV12 )
    {
        fprintf( stderr, "x264 [error]: I1 requires 4:2:0 8-bit input\n" );
        return X264PP_ERR_INVALID;
    }
    if( out->bit_depth != 8 )
    {
        fprintf( stderr, "x264 [error]: I1 supports only 8-bit input\n" );
        return X264PP_ERR_INVALID;
    }

    if( out->fps_num <= 0 || out->fps_den <= 0 )
    {
        out->fps_num = 25;
        out->fps_den = 1;
    }
    out->sar_num = out->sar_num > 0 ? out->sar_num : 0;
    out->sar_den = out->sar_den > 0 ? out->sar_den : 0;
    if( (out->sar_num > 0) != (out->sar_den > 0) )
    {
        out->sar_num = 0;
        out->sar_den = 0;
    }
    out->fullrange = !!out->fullrange;

    /* I1 is strictly CAVLC / 4x4 transform / I-slice only.  Parameters that
     * belong to later increments are accepted by the CLI but have no
     * bitstream effect here; the deviations are recorded in questions.md. */
    out->cabac   = 0;
    out->dct8x8  = 0;
    out->bframes = 0;
    out->ref     = 1;
    out->deblock = 0;   /* deblocking disabled via slice header in I1 */

    out->qp = clamp_i( out->qp, -1, 51 );
    out->crf = (out->crf < 0.0) ? 0.0 : (out->crf > 51.0 ? 51.0 : out->crf);
    out->chroma_qp_offset = clamp_i( out->chroma_qp_offset, -12, 12 );
    return X264PP_OK;
}

int x264pp_level_auto( const x264pp_param_t *param, const x264pp_sps_t *sps )
{
    int i, mbw, mbh, mbs, mbps, dpb;
    const x264pp_level_t *l;

    if( !param || !sps )
        return 0;
    mbw = sps->mb_width;
    mbh = sps->mb_height;
    mbs = mbw * mbh;
    if( param->fps_den > 0 )
        mbps = (int)(((int64_t)mbs * param->fps_num + param->fps_den - 1) / param->fps_den);
    else
        mbps = mbs * 25;
    dpb = mbs * (sps->max_num_ref_frames > 0 ? sps->max_num_ref_frames : 1);

    for( i = 0; i < LEVEL_COUNT; i++ )
    {
        l = &x264pp_levels[i];
        if( l->max_fs >= mbs && l->max_mbps >= mbps &&
            (l->max_dpb_mbs < 0 || l->max_dpb_mbs >= dpb) )
            return l->level_idc;
    }
    return 62;
}

void x264pp_sps_init( x264pp_sps_t *sps, const x264pp_param_t *param )
{
    int i, sar_idc = 0;
    const int qp = x264pp_qp_for_frame( param );

    memset( sps, 0, sizeof( *sps ) );
    sps->mb_width  = (param->width + 15) / 16;
    sps->mb_height = (param->height + 15) / 16;

    /* High profile is signalled only when a tool that requires it is active;
     * in I1 the transform is always 4x4, so High is reachable only through a
     * non-flat matrix request (accepted by the CLI but not used here). */
    sps->profile_idc = param->cqm ? X264PP_PROFILE_HIGH : X264PP_PROFILE_BASELINE;
    for( i = 0; i < 6; i++ )
        sps->constraint_flag[i] = 0;
    if( sps->profile_idc == X264PP_PROFILE_BASELINE )
    {
        sps->constraint_flag[0] = 1;
        sps->constraint_flag[1] = 1;
    }
    sps->sps_id = clamp_i( param->sps_id, 0, 31 );
    sps->chroma_format_idc = 1;
    sps->bit_depth_luma_minus8 = 0;
    sps->bit_depth_chroma_minus8 = 0;
    sps->qpprime_y_zero_bypass = (param->rc == X264PP_RC_CQP && qp == 0);
    sps->scaling_matrix_present = 0;

    sps->log2_max_frame_num = 4;
    sps->poc_type = 0;
    sps->log2_max_poc_lsb = 4;
    sps->max_num_ref_frames = 1;
    sps->gaps_allowed = 0;

    sps->frame_mbs_only = 1;
    sps->mbaff = 0;
    sps->direct8x8 = 1;

    sps->crop[0] = sps->crop[1] = sps->crop[2] = sps->crop[3] = 0;
    sps->crop[1] = sps->mb_width  * 16 - param->width;
    sps->crop[3] = sps->mb_height * 16 - param->height;
    sps->b_crop = (sps->crop[0] || sps->crop[1] || sps->crop[2] || sps->crop[3]);

    sps->level_idc = x264pp_level_auto( param, sps );

    /* VUI: timing and bitstream restriction only, matching the documented
     * default observation.  Aspect ratio is added when the CLI supplied one. */
    sps->vui_present = 1;
    sps->sar_num = param->sar_num;
    sps->sar_den = param->sar_den;
    sar_idc = 0;
    if( sps->sar_num && sps->sar_den )
    {
        for( i = 0; i < 16; i++ )
            if( sar_table[i][0] * sps->sar_den == sar_table[i][1] * sps->sar_num )
            {
                sar_idc = i + 1;
                break;
            }
    }
    sps->sar_idc = sar_idc;

    sps->overscan_present = 0;
    sps->overscan_appropriate = 0;
    sps->video_signal_present = 0;
    sps->video_format = 5;
    sps->full_range = param->fullrange;
    sps->colour_description_present = 0;
    sps->colour_primaries = 2;
    sps->transfer_characteristics = 2;
    sps->matrix_coefficients = 2;
    sps->chroma_loc_present = 0;

    sps->timing_present = 1;
    sps->num_units_in_tick = 1;
    if( param->fps_den > 0 )
        sps->time_scale = (unsigned)( (2.0 * param->fps_num / param->fps_den) + 0.5 );
    else
        sps->time_scale = 50;
    if( sps->time_scale < 2 )
        sps->time_scale = 2;
    sps->fixed_frame_rate = 1;

    sps->nal_hrd = sps->vcl_hrd = sps->low_delay_hrd = 0;
    sps->pic_struct_present = 0;
    sps->bitstream_restriction = 1;
    sps->motion_vectors_over_pic_boundaries = 1;
    sps->max_bytes_per_pic_denom = 0;
    sps->max_bits_per_mb_denom = 0;
    sps->log2_max_mv_length_horizontal = 8;
    sps->log2_max_mv_length_vertical = 8;
    sps->max_num_reorder_frames = 0;       /* no B-frames in I1 */
    sps->max_dec_frame_buffering = 1;
}

void x264pp_pps_init( x264pp_pps_t *pps, const x264pp_param_t *param,
                      const x264pp_sps_t *sps )
{
    memset( pps, 0, sizeof( *pps ) );
    pps->pps_id = 0;
    pps->sps_id = sps->sps_id;
    pps->entropy_cabac = 0;                 /* CAVLC, even when --cabac was set */
    pps->bottom_field_order = 0;
    pps->num_slice_groups_minus1 = 0;
    pps->num_ref_idx_l0_default_active_minus1 = 0;
    pps->num_ref_idx_l1_default_active_minus1 = 0;
    pps->weighted_pred = 0;                 /* no P/B slices in I1 */
    pps->weighted_bipred_idc = 0;
    pps->pic_init_qp = x264pp_qp_for_frame( param );
    pps->pic_init_qs = 26;
    pps->chroma_qp_index_offset = clamp_i( param->chroma_qp_offset, -12, 12 );
    pps->second_chroma_qp_index_offset = pps->chroma_qp_index_offset;
    pps->deblocking_control_present = 1;    /* needed to disable the filter */
    pps->constrained_intra = param->constrained_intra ? 1 : 0;
    pps->redundant_pic_cnt_present = 0;
    pps->transform8x8 = 0;                  /* 4x4 transform only in I1 */
    pps->scaling_matrix_present = 0;
}

int x264pp_level_validate( const x264pp_param_t *param,
                           const x264pp_sps_t *sps, int verbose )
{
    const x264pp_level_t *l = x264pp_level_find( sps->level_idc );
    int mbs, mbps, dpb, bad = 0;
    if( !l )
        return 1;
    mbs = sps->mb_width * sps->mb_height;
    mbps = (param->fps_den > 0)
         ? (int)(((int64_t)mbs * param->fps_num + param->fps_den - 1) / param->fps_den)
         : mbs * 25;
    dpb = mbs * sps->max_dec_frame_buffering;

    if( l->max_fs < mbs )
    {
        if( verbose ) fprintf( stderr, "x264 [warning]: frame size exceeds level limit\n" );
        bad = 1;
    }
    if( l->max_mbps < mbps )
    {
        if( verbose ) fprintf( stderr, "x264 [warning]: MB rate exceeds level limit\n" );
        bad = 1;
    }
    if( l->max_dpb_mbs >= 0 && l->max_dpb_mbs < dpb )
    {
        if( verbose ) fprintf( stderr, "x264 [warning]: DPB exceeds level limit\n" );
        bad = 1;
    }
    return bad;
}
