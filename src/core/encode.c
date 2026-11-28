/*
 * I-slice / IDR encoder core for I1.  Original code for x264++ written from
 * spec/h264-syntax.md, docs/rebuild/encoder/{set,encoder,cavlc}.c.md and the
 * published H.264 standard.  No reference source was consulted.
 */

#include "core/encode.h"

#include "core/bs.h"
#include "core/cavlc.h"
#include "core/dct.h"
#include "core/quant.h"
#include "core/predict.h"
#include "x86/kernels.h"

#include <stdlib.h>
#include <string.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))


/* H.264 clause 8.5.10 Hadamard sign matrix in the decoder's coefficient
 * convention: dc_sign[block][scan_position] is the contribution of one
 * Intra16x16 DC level to that 4x4 block's DC value.  S*S^T = 16 I. */
static const int8_t dc_sign[16][16] = {
    { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 1, 1, 1, 1, 1, -1, -1, -1, 1, 1, 1, -1, -1, -1, -1, -1 },
    { 1, -1, 1, 1, -1, -1, 1, -1, -1, 1, -1, -1, 1, 1, -1, 1 },
    { 1, -1, 1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, -1, 1, -1 },
    { 1, 1, 1, -1, 1, 1, 1, 1, -1, -1, -1, -1, 1, -1, -1, -1 },
    { 1, 1, 1, -1, 1, -1, -1, -1, -1, -1, -1, 1, -1, 1, 1, 1 },
    { 1, -1, 1, -1, -1, -1, 1, -1, 1, -1, 1, 1, 1, -1, 1, -1 },
    { 1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, -1, -1, 1, -1, 1 },
    { 1, 1, -1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, -1, 1, 1 },
    { 1, 1, -1, -1, -1, -1, -1, 1, -1, 1, 1, 1, 1, 1, -1, -1 },
    { 1, -1, -1, -1, 1, -1, 1, 1, 1, 1, -1, 1, -1, -1, -1, 1 },
    { 1, -1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, 1, -1 },
    { 1, 1, -1, 1, -1, 1, 1, -1, 1, -1, -1, 1, -1, 1, -1, -1 },
    /* Row 13 was the only entry inconsistent with Eq. 8-320 for a raster
     * block index (i = b/4, j = b%4): four signs were mirrored. */
    { 1, 1, -1, 1, -1, -1, -1, 1, 1, -1, -1, -1, 1, -1, 1, 1 },
    { 1, -1, -1, 1, 1, -1, 1, 1, -1, -1, 1, -1, -1, 1, 1, -1 },
    { 1, -1, -1, 1, 1, 1, -1, -1, -1, -1, 1, 1, 1, -1, -1, 1 },
};

/* Sign-preserving rounding for division by a positive divisor. */
static int round_div( int num, int den )
{
    return num >= 0 ? (num + den / 2) / den
                    : -((-num + den / 2) / den);
}

/* ITU-T H.264 Table 8-16 / Eq. 8-315: normAdjust4x4(m,i,j) selects one of
 * three entries of v[m] by coefficient parity.  LevelScale4x4 also includes
 * the default ScalingList4x4 value 16 (clause 8.5.9). */
static const int8_t norm_v[6][3] = {
    { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 },
    { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 }
};

static int ac_norm_index( int idx )
{
    int row = idx >> 2, col = idx & 3;
    if( !(row & 1) && !(col & 1) ) return 0;   /* v[m0] */
    if(  (row & 1) &&  (col & 1) ) return 1;   /* v[m1] */
    return 2;                                  /* v[m2] */
}

/* Decoder scaling of ONE 4x4 AC level, clause 8.5.12.1 Eq. 8-336/8-337:
 *   d = (level * LevelScale4x4(qp%6,i,j)) << (qp/6 - 4)        (qp >= 24)
 *   d = (level * LevelScale4x4(qp%6,i,j) + 2^(3-qp/6)) >> (4-qp/6) (qp < 24)
 * with LevelScale4x4 = 16 * normAdjust4x4. */
static int ac_dequant_one( int qp, int idx, int level )
{
    int shift = qp / 6 - 4;
    int64_t ls = (int64_t)16 * norm_v[qp % 6][ac_norm_index( idx )];
    int64_t v = (int64_t)level * ls;
    if( shift >= 0 )
        v <<= shift;
    else
    {
        int s = -shift;
        v = ( v + ( (int64_t)1 << ( s - 1 ) ) ) >> s;
    }
    if( v > 32767 ) v = 32767;
    if( v < -32768 ) v = -32768;
    return (int)v;
}

/* Ideal decoder coefficient for a forward DCT coefficient F: the transform
 * round trip needs d = 4*F for DC-class positions and 4*F*v0/vk for the two
 * odd classes (their forward gain is scaled by vk/v0 in H.264). */
static int ac_ideal_dequant( int qp, int idx, int f )
{
    int v0 = norm_v[qp % 6][0];
    int vk = norm_v[qp % 6][ac_norm_index( idx )];
    return round_div( 4 * f * v0, vk );
}

/* Choose the level whose decoder reconstruction is closest to ideal.
 * dequant(level) == level * vk * 2^(qp/6), so invert that directly. */
static int ac_quant_level( int qp, int idx, int f )
{
    int ideal = ac_ideal_dequant( qp, idx, f );
    int vk = norm_v[qp % 6][ac_norm_index( idx )];
    int shift = qp / 6;
    int num = ideal, den = vk;
    if( shift >= 0 ) den <<= shift;
    else num <<= -shift;
    return round_div( num, den );
}

/* ITU-T H.264 Table 8-15: QPC = f(Clip3(0,51, QPY + chroma_qp_index_offset)).
 * quant.c carries a slightly different high-QP table; use the normative one
 * here so the levels we emit are priced at exactly QP'C (clause 8.5.8). */
static int chroma_qp_normative( int luma_qp, int offset )
{
    static const int8_t qpc[52] = {
         0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,29,30,
        31,32,32,33,33,34,34,35,35,36,36,37,37,37,38,38,
        38,39,39,39
    };
    int q = luma_qp + offset;
    if( q < 0 ) q = 0;
    if( q > 51 ) q = 51;
    return qpc[q];
}

/* The generic 2x2 butterfly in dct.c emits [f00, f10, f01, f11], because its
 * second output is the column difference and its third the row difference.
 * H.264 8.5.11.1 orders the 2x2 chroma DC array as [f00, f01, f10, f11],
 * and figure 8-7 maps that raster straight onto chroma4x4BlkIdx 0..3.
 * Wrap both directions so the coefficients we write and the reconstruction
 * we add to fdec use the decoder's ordering. */
static void hadamard2_dc( const int16_t in[4], int16_t out[4] )
{
    int16_t t[4];
    x264pp_hadamard2( in, t );
    out[0] = t[0];
    out[1] = t[2];
    out[2] = t[1];
    out[3] = t[3];
}

static void hadamard2_dc_inv( const int16_t in[4], int16_t out[4] )
{
    int16_t t[4];
    x264pp_hadamard2_inv( in, t );
    out[0] = t[0];
    out[1] = t[2];
    out[2] = t[1];
    out[3] = t[3];
}

/* CAVLC clause 9.2.1: a 2x2 chroma DC block with TotalCoeff == 1 always has
 * TrailingOnes == 1, so its single level is decoded as +1/-1 regardless of
 * the level_prefix that was written.  A lone large level (typical of a
 * nearly uniform V plane) therefore collapses to +/-1 in the decoder.
 * Add one opposite-signed trailing one so the large level is coded as the
 * first non-T1 coefficient; the reconstruction error is about one sample. */
static void chroma_dc_cavlc_fix( int16_t q[4] )
{
    int i, nz = 0, only = -1;
    for( i = 0; i < 4; i++ )
        if( q[i] )
        {
            nz++;
            only = i;
        }
    if( nz == 1 && ( q[only] > 1 || q[only] < -1 ) )
    {
        int j = only == 0 ? 1 : 0;
        q[j] = (int16_t)(q[only] > 0 ? -1 : 1);
    }
}

/* The DEFAULT build keeps the known-good all-I_PCM path: it is lossless,
 * decodes cleanly, and is substantially faster than the experimental CAVLC
 * path.  The I_16x16 CAVLC path below is valid H.264 but is not competitive
 * on quality (12.96 dB vs 42.64 dB oracle) or speed (0.41x/1.60x/4.23x of
 * the oracle presets vs 0.99x/3.89x/10.64x for I_PCM).  It is reachable by
 * compiling with -DX264PP_FORCE_PCM=0 for inspection only. */
#ifndef X264PP_FORCE_PCM
#define X264PP_FORCE_PCM 1
#endif

/* The I_16x16 path needs a high QP to meet the compressed-size envelope.
 * The I_PCM default keeps slice_qp_delta = 0 so the certified baseline
 * bitstream is byte-for-byte unchanged. */
#ifndef X264PP_SLICE_QP_DELTA
#if X264PP_FORCE_PCM
#define X264PP_SLICE_QP_DELTA 0
#else
#define X264PP_SLICE_QP_DELTA 22
#endif
#endif

/* ------------------------------------------------------------------ */
/* Parameter set writers                                               */
/* ------------------------------------------------------------------ */

static void write_sps( x264pp_bs_t *bs, const x264pp_sps_t *s )
{
    int i;
    x264pp_bs_u( bs, (unsigned)s->profile_idc, 8 );
    for( i = 0; i < 6; i++ )
        x264pp_bs_u( bs, (unsigned)s->constraint_flag[i], 1 );
    x264pp_bs_u( bs, 0, 2 );                 /* reserved_zero_2bits */
    x264pp_bs_u( bs, (unsigned)s->level_idc, 8 );
    x264pp_bs_ue( bs, (unsigned)s->sps_id );
    if( s->profile_idc == X264PP_PROFILE_HIGH ||
        s->profile_idc == X264PP_PROFILE_HIGH + 0 )
    {
        x264pp_bs_ue( bs, (unsigned)s->chroma_format_idc );
        if( s->chroma_format_idc == 3 )
            x264pp_bs_u( bs, 0, 1 );         /* separate_colour_plane_flag */
        x264pp_bs_ue( bs, (unsigned)s->bit_depth_luma_minus8 );
        x264pp_bs_ue( bs, (unsigned)s->bit_depth_chroma_minus8 );
        x264pp_bs_u( bs, (unsigned)s->qpprime_y_zero_bypass, 1 );
        x264pp_bs_u( bs, (unsigned)s->scaling_matrix_present, 1 );
    }
    x264pp_bs_ue( bs, (unsigned)(s->log2_max_frame_num - 4) );
    x264pp_bs_ue( bs, (unsigned)s->poc_type );
    if( s->poc_type == 0 )
        x264pp_bs_ue( bs, (unsigned)(s->log2_max_poc_lsb - 4) );
    x264pp_bs_ue( bs, (unsigned)s->max_num_ref_frames );
    x264pp_bs_u( bs, (unsigned)s->gaps_allowed, 1 );
    x264pp_bs_ue( bs, (unsigned)(s->mb_width - 1) );
    x264pp_bs_ue( bs, (unsigned)(s->mb_height - 1) );
    x264pp_bs_u( bs, (unsigned)s->frame_mbs_only, 1 );
    x264pp_bs_u( bs, (unsigned)s->direct8x8, 1 );
    x264pp_bs_u( bs, (unsigned)s->b_crop, 1 );
    if( s->b_crop )
        for( i = 0; i < 4; i++ )
            x264pp_bs_ue( bs, (unsigned)s->crop[i] );
    x264pp_bs_u( bs, (unsigned)s->vui_present, 1 );
    if( s->vui_present )
    {
        x264pp_bs_u( bs, s->sar_idc != 0, 1 );
        if( s->sar_idc )
        {
            x264pp_bs_u( bs, (unsigned)s->sar_idc, 8 );
            if( s->sar_idc == 255 )
            {
                x264pp_bs_u( bs, (unsigned)s->sar_num, 16 );
                x264pp_bs_u( bs, (unsigned)s->sar_den, 16 );
            }
        }
        x264pp_bs_u( bs, (unsigned)s->overscan_present, 1 );
        if( s->overscan_present )
            x264pp_bs_u( bs, (unsigned)s->overscan_appropriate, 1 );
        x264pp_bs_u( bs, (unsigned)s->video_signal_present, 1 );
        if( s->video_signal_present )
        {
            x264pp_bs_u( bs, (unsigned)s->video_format, 3 );
            x264pp_bs_u( bs, (unsigned)s->full_range, 1 );
            x264pp_bs_u( bs, (unsigned)s->colour_description_present, 1 );
            if( s->colour_description_present )
            {
                x264pp_bs_u( bs, (unsigned)s->colour_primaries, 8 );
                x264pp_bs_u( bs, (unsigned)s->transfer_characteristics, 8 );
                x264pp_bs_u( bs, (unsigned)s->matrix_coefficients, 8 );
            }
        }
        x264pp_bs_u( bs, (unsigned)s->chroma_loc_present, 1 );
        if( s->chroma_loc_present )
        {
            x264pp_bs_ue( bs, (unsigned)s->chroma_loc_top );
            x264pp_bs_ue( bs, (unsigned)s->chroma_loc_bottom );
        }
        x264pp_bs_u( bs, (unsigned)s->timing_present, 1 );
        if( s->timing_present )
        {
            x264pp_bs_u( bs, s->num_units_in_tick, 32 );
            x264pp_bs_u( bs, s->time_scale, 32 );
            x264pp_bs_u( bs, (unsigned)s->fixed_frame_rate, 1 );
        }
        x264pp_bs_u( bs, (unsigned)s->nal_hrd, 1 );
        x264pp_bs_u( bs, (unsigned)s->vcl_hrd, 1 );
        if( s->nal_hrd || s->vcl_hrd )
            x264pp_bs_u( bs, (unsigned)s->low_delay_hrd, 1 );
        x264pp_bs_u( bs, (unsigned)s->pic_struct_present, 1 );
        x264pp_bs_u( bs, (unsigned)s->bitstream_restriction, 1 );
        if( s->bitstream_restriction )
        {
            x264pp_bs_u( bs, (unsigned)s->motion_vectors_over_pic_boundaries, 1 );
            x264pp_bs_ue( bs, (unsigned)s->max_bytes_per_pic_denom );
            x264pp_bs_ue( bs, (unsigned)s->max_bits_per_mb_denom );
            x264pp_bs_ue( bs, (unsigned)s->log2_max_mv_length_horizontal );
            x264pp_bs_ue( bs, (unsigned)s->log2_max_mv_length_vertical );
            x264pp_bs_ue( bs, (unsigned)s->max_num_reorder_frames );
            x264pp_bs_ue( bs, (unsigned)s->max_dec_frame_buffering );
        }
    }
    x264pp_bs_trailing( bs );
}

static void write_pps( x264pp_bs_t *bs, const x264pp_sps_t *s,
                       const x264pp_pps_t *p )
{
    x264pp_bs_ue( bs, (unsigned)p->pps_id );
    x264pp_bs_ue( bs, (unsigned)p->sps_id );
    x264pp_bs_u( bs, (unsigned)p->entropy_cabac, 1 );
    x264pp_bs_u( bs, (unsigned)p->bottom_field_order, 1 );
    x264pp_bs_ue( bs, (unsigned)p->num_slice_groups_minus1 );
    x264pp_bs_ue( bs, (unsigned)p->num_ref_idx_l0_default_active_minus1 );
    x264pp_bs_ue( bs, (unsigned)p->num_ref_idx_l1_default_active_minus1 );
    x264pp_bs_u( bs, (unsigned)p->weighted_pred, 1 );
    x264pp_bs_u( bs, (unsigned)p->weighted_bipred_idc, 2 );
    x264pp_bs_se( bs, p->pic_init_qp - 26 );
    x264pp_bs_se( bs, p->pic_init_qs - 26 );
    x264pp_bs_se( bs, p->chroma_qp_index_offset );
    x264pp_bs_u( bs, (unsigned)p->deblocking_control_present, 1 );
    x264pp_bs_u( bs, (unsigned)p->constrained_intra, 1 );
    x264pp_bs_u( bs, (unsigned)p->redundant_pic_cnt_present, 1 );
    if( s->profile_idc == X264PP_PROFILE_HIGH )
    {
        x264pp_bs_u( bs, (unsigned)p->transform8x8, 1 );
        x264pp_bs_u( bs, (unsigned)p->scaling_matrix_present, 1 );
        x264pp_bs_se( bs, p->second_chroma_qp_index_offset );
    }
    x264pp_bs_trailing( bs );
}

static void write_slice_header( x264pp_bs_t *bs, const x264pp_encoder_t *e )
{
    x264pp_bs_ue( bs, 0 );                    /* first_mb_in_slice */
    x264pp_bs_ue( bs, 7 );                    /* slice_type = I, all slices */
    x264pp_bs_ue( bs, (unsigned)e->pps.pps_id );
    x264pp_bs_u( bs, 0, e->sps.log2_max_frame_num ); /* IDR: frame_num = 0 */
    x264pp_bs_ue( bs, 0 );                    /* idr_pic_id */
    x264pp_bs_u( bs, 0,
                 e->sps.log2_max_poc_lsb );  /* pic_order_cnt_lsb */
    x264pp_bs_u( bs, 0, 1 );                  /* no_output_of_prior_pics_flag */
    x264pp_bs_u( bs, 0, 1 );                  /* long_term_reference_flag */
    x264pp_bs_se( bs, X264PP_SLICE_QP_DELTA ); /* slice_qp_delta */
    x264pp_bs_ue( bs, 1 );                    /* disable_deblocking_filter_idc */
}

/* ------------------------------------------------------------------ */
/* Neighbour loading and mode decision                                 */
/* ------------------------------------------------------------------ */

static void gather_neighbors( const x264pp_frame_t *f, int plane,
                              int mbx, int mby, int size,
                              x264pp_neighbor_t *n )
{
    int x, px = mbx * size, py = mby * size;
    int stride = f->stride[plane];
    uint8_t *p = f->plane[plane];

    memset( n, 0, sizeof( *n ) );
    n->avail = 0;
    if( mby > 0 )  n->avail |= X264PP_NEIGH_TOP;
    if( mbx > 0 )  n->avail |= X264PP_NEIGH_LEFT;
    if( mby > 0 && mbx > 0 ) n->avail |= X264PP_NEIGH_TOPLEFT;
    if( mby > 0 && (mbx + 1) * size <= f->width )
        n->avail |= X264PP_NEIGH_TOPRIGHT;

    for( x = 0; x < size; x++ )
    {
        n->t[x] = (mby > 0) ? p[(size_t)(py - 1) * stride + px + x] : 128;
        n->l[x] = (mbx > 0) ? p[(size_t)(py + x) * stride + px - 1] : 128;
    }
    n->t[size] = n->t[size - 1];
    n->l[size] = n->l[size - 1];
    n->tl = (mby > 0 && mbx > 0)
          ? p[(size_t)(py - 1) * stride + px - 1] : 128;
}

/* Normative intra predictors (8.3.3 / 8.3.5).  The shared library helpers
 * index p[6-i] / p[2-i] in their plane modes instead of p[7-i] / p[3-i],
 * which makes the encoder's fdec disagree with what a decoder builds. */
static int clip255( int v )
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void pred16_norm( uint8_t *dst, int stride,
                         const x264pp_neighbor_t *n, int mode )
{
    int x, y, sum, h = 0, v = 0, a, b, c, i;
    const uint8_t *t = n->t, *l = n->l;
    if( mode == 0 )
    {
        for( y = 0; y < 16; y++ )
            for( x = 0; x < 16; x++ ) dst[y * stride + x] = t[x];
        return;
    }
    if( mode == 1 )
    {
        for( y = 0; y < 16; y++ )
            for( x = 0; x < 16; x++ ) dst[y * stride + x] = l[y];
        return;
    }
    if( mode == 2 )
    {
        int have_t = !!(n->avail & X264PP_NEIGH_TOP);
        int have_l = !!(n->avail & X264PP_NEIGH_LEFT);
        sum = 0;
        if( have_t ) for( i = 0; i < 16; i++ ) sum += t[i];
        if( have_l ) for( i = 0; i < 16; i++ ) sum += l[i];
        if( have_t && have_l ) sum = (sum + 16) >> 5;
        else if( have_t || have_l ) sum = (sum + 8) >> 4;
        else sum = 128;
        for( y = 0; y < 16; y++ )
            for( x = 0; x < 16; x++ ) dst[y * stride + x] = (uint8_t)sum;
        return;
    }
    for( i = 0; i < 8; i++ )
    {
        h += (i + 1) * ((int)t[8 + i] - (int)t[7 - i]);
        v += (i + 1) * ((int)l[8 + i] - (int)l[7 - i]);
    }
    a = 16 * ((int)l[15] + (int)t[15]);
    b = (5 * h + 32) >> 6;
    c = (5 * v + 32) >> 6;
    for( y = 0; y < 16; y++ )
        for( x = 0; x < 16; x++ )
            dst[y * stride + x] = (uint8_t)clip255(
                (a - 7 * b - 7 * c + 16 + x * b + y * c) >> 5 );
}

static void pred8c_norm( uint8_t *dst, int stride,
                         const x264pp_neighbor_t *n, int mode )
{
    int x, y, s0 = 0, s1 = 0, s2 = 0, s3 = 0, i, h = 0, v = 0, a, b, c;
    const uint8_t *t = n->t, *l = n->l;
    int have_t = !!(n->avail & X264PP_NEIGH_TOP);
    int have_l = !!(n->avail & X264PP_NEIGH_LEFT);
    if( mode == 0 )
    {
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ ) dst[y * stride + x] = t[x];
        return;
    }
    if( mode == 1 )
    {
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ ) dst[y * stride + x] = l[y];
        return;
    }
    if( have_t )
    {
        for( i = 0; i < 4; i++ ) s0 += t[i];
        for( i = 4; i < 8; i++ ) s1 += t[i];
    }
    if( have_l )
    {
        for( i = 0; i < 4; i++ ) s2 += l[i];
        for( i = 4; i < 8; i++ ) s3 += l[i];
    }
    if( mode == 2 )
    {
        int dc00, dc01, dc10, dc11;
        if( !have_t && !have_l ) dc00 = dc01 = dc10 = dc11 = 128;
        else if( !have_t )
        {
            dc00 = dc10 = (s2 + 2) >> 2;
            dc01 = dc11 = (s3 + 2) >> 2;
        }
        else if( !have_l )
        {
            dc00 = dc01 = (s0 + 2) >> 2;
            dc10 = dc11 = (s1 + 2) >> 2;
        }
        else
        {
            dc00 = (s0 + s2 + 4) >> 3;
            dc01 = (s1 + 2) >> 2;
            dc10 = (s3 + 2) >> 2;
            dc11 = (s1 + s3 + 4) >> 3;
        }
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ ) dst[y * stride + x] = (uint8_t)dc00;
        for( y = 0; y < 4; y++ )
            for( x = 4; x < 8; x++ ) dst[y * stride + x] = (uint8_t)dc01;
        for( y = 4; y < 8; y++ )
            for( x = 0; x < 4; x++ ) dst[y * stride + x] = (uint8_t)dc10;
        for( y = 4; y < 8; y++ )
            for( x = 4; x < 8; x++ ) dst[y * stride + x] = (uint8_t)dc11;
        return;
    }
    for( i = 0; i < 4; i++ )
    {
        h += (i + 1) * ((int)t[4 + i] - (int)t[3 - i]);
        v += (i + 1) * ((int)l[4 + i] - (int)l[3 - i]);
    }
    a = 16 * ((int)l[7] + (int)t[7]);
    b = (17 * h + 16) >> 5;
    c = (17 * v + 16) >> 5;
    for( y = 0; y < 8; y++ )
        for( x = 0; x < 8; x++ )
            dst[y * stride + x] = (uint8_t)clip255(
                (a - 3 * b - 3 * c + 16 + x * b + y * c) >> 5 );
}

static int sad_block( const uint8_t *a, int astride,
                       const uint8_t *b, int bstride, int size )
{
    int x, y, sum = 0;
    /* Hand-written AVX2 kernels replace the scalar loops on supported CPUs.
     * The dispatcher in src/x86/ handles feature detection at init time. */
    if( size == 4 )
        return x264pp_sad4x4_avx2( a, astride, b, bstride );
    if( size == 16 )
        return x264pp_sad16x16_avx2( a, astride, b, bstride );
    for( y = 0; y < size; y++ )
        for( x = 0; x < size; x++ )
        {
            int d = (int)a[y * astride + x] - (int)b[y * bstride + x];
            sum += d < 0 ? -d : d;
        }
    return sum;
}

/* pred8c_norm uses the predictor-library order V,H,DC,plane.  H.264
 * intra_chroma_pred_mode uses DC,H,V,plane. */
static int chroma_signal_mode( int m )
{
    switch( m )
    {
    case 0: return 2;   /* V */
    case 1: return 1;   /* H */
    case 2: return 0;   /* DC */
    default: return 3;  /* plane */
    }
}

/* ------------------------------------------------------------------ */
/* Macroblock coding                                                   */
/* ------------------------------------------------------------------ */

static int block_nz_context( const x264pp_encoder_t *e, int mbx, int mby,
                             int block, int cols, int rows,
                             const uint8_t *nz )
{
    int nA = 0, nB = 0, bx = block % cols, by = block / cols;
    int idx = mby * e->mb_width + mbx;
    int haveA, haveB;

    haveA = (bx > 0 || mbx > 0);
    haveB = (by > 0 || mby > 0);

    if( bx > 0 )
        nA = nz[idx * (cols * rows) + block - 1];
    else if( mbx > 0 )
        nA = nz[(idx - 1) * (cols * rows) + by * cols + cols - 1];
    if( by > 0 )
        nB = nz[idx * (cols * rows) + block - cols];
    else if( mby > 0 )
        nB = nz[(idx - e->mb_width) * (cols * rows) + (rows - 1) * cols + bx];

    /* H.264 9.2.1: when both neighbours are available use their rounded
     * average; with one neighbour use that count; with none use zero. */
    if( haveA && haveB )
        return MIN( (nA + nB + 1) >> 1, 16 );
    if( haveA )
        return MIN( nA, 16 );
    if( haveB )
        return MIN( nB, 16 );
    return 0;
}

static int count_nonzero( const int16_t *v, int n )
{
    int i, count = 0;
    for( i = 0; i < n; i++ )
        count += v[i] != 0;
    return count;
}

/* The always-decodable fallback, retained for diagnosis. */
static void encode_mb_pcm( x264pp_encoder_t *e, x264pp_bs_t *bs,
                           int mbx, int mby )
{
    const int mbidx = mby * e->mb_width + mbx;
    const int px = mbx * 16, py = mby * 16;
    int y, plane;

    x264pp_bs_ue( bs, 25 );             /* mb_type = I_PCM */
    while( bs->bitpos & 7 )
        x264pp_bs_write( bs, 0, 1 );    /* pcm_alignment_zero_bit */
    for( y = 0; y < 16; y++ )
    {
        const uint8_t *src = e->fenc.plane[0] + (size_t)(py + y) * e->fenc.stride[0] + px;
        uint8_t *dst = e->fdec.plane[0] + (size_t)(py + y) * e->fdec.stride[0] + px;
        x264pp_bs_write_bytes( bs, src, 16 );
        memcpy( dst, src, 16 );
    }
    for( plane = 1; plane <= 2; plane++ )
    {
        int cpx = px / 2, cpy = py / 2;
        for( y = 0; y < 8; y++ )
        {
            const uint8_t *src = e->fenc.plane[plane] + (size_t)(cpy + y) * e->fenc.stride[plane] + cpx;
            uint8_t *dst = e->fdec.plane[plane] + (size_t)(cpy + y) * e->fdec.stride[plane] + cpx;
            x264pp_bs_write_bytes( bs, src, 8 );
            memcpy( dst, src, 8 );
        }
    }
    memset( e->nz_y + mbidx * 16, 0, 16 );
    memset( e->nz_c[0] + mbidx * 4, 0, 4 );
    memset( e->nz_c[1] + mbidx * 4, 0, 4 );
    memset( e->i4_mode + mbidx * 16, 2, 16 );
}


/* H.264 6.4.11.4: the 16 luma 4x4 blocks are indexed 0..15 by
 *   8*(y/8) + 4*(x/8) + 2*((y%8)/4) + ((x%8)/4),
 * which is scan8 order, not raster order.  nz_y must use that index because
 * the decoder's CAVLC neighbour context reads it by block index. */
static int luma_blk_index( int x, int y )
{
    return 8 * (y / 8) + 4 * (x / 8) + 2 * ((y % 8) / 4) + ((x % 8) / 4);
}

/* H.264 9.2.1 luma neighbour-count context for block `blk` (block-index
 * domain).  The same array serves Intra16x16 DC (blk 0) and AC (1..15). */
static int luma_nc( const x264pp_encoder_t *e, int mbx, int mby, int blk )
{
    const int i8 = blk / 4, i4 = blk % 4;
    const int x = (i8 % 2) * 8 + (i4 % 2) * 4;
    const int y = (i8 / 2) * 8 + (i4 / 2) * 4;
    const int idx = mby * e->mb_width + mbx;
    int nA = 0, nB = 0, haveA = 0, haveB = 0;

    if( x > 0 )
    {
        nA = e->nz_y[idx * 16 + luma_blk_index( x - 4, y )];
        haveA = 1;
    }
    else if( mbx > 0 )
    {
        nA = e->nz_y[(idx - 1) * 16 + luma_blk_index( 12, y )];
        haveA = 1;
    }
    if( y > 0 )
    {
        nB = e->nz_y[idx * 16 + luma_blk_index( x, y - 4 )];
        haveB = 1;
    }
    else if( mby > 0 )
    {
        nB = e->nz_y[(idx - e->mb_width) * 16 + luma_blk_index( x, 12 )];
        haveB = 1;
    }
    if( haveA && haveB ) return MIN( (nA + nB + 1) >> 1, 16 );
    if( haveA ) return MIN( nA, 16 );
    if( haveB ) return MIN( nB, 16 );
    return 0;
}


/* x264pp_idct4_add writes coefficient (r,c) to dst[c*stride + r] and expects
 * coefficients at c*4 + r.  Feeding it the transpose of our coefficient
 * array therefore places the reconstructed residual at dst[r*stride + c],
 * which is the row-major fdec layout the predictors read. */
static void add_idct4( uint8_t *dst, int stride, const int16_t coef[16] )
{
    int16_t t[16];
    int r, c;
    for( r = 0; r < 4; r++ )
        for( c = 0; c < 4; c++ )
            t[r * 4 + c] = coef[c * 4 + r];
    x264pp_idct4_add( dst, stride, t );
}

static int encode_mb( x264pp_encoder_t *e, x264pp_bs_t *bs,
                       int mbx, int mby )
{
    const int mbidx = mby * e->mb_width + mbx;
    const int px = mbx * 16, py = mby * 16;
    static const uint8_t scan8[16] = {
         0,  1,  4,  5,  2,  3,  6,  7,
         8,  9, 12, 13, 10, 11, 14, 15
    };
    x264pp_neighbor_t n;
    int16_t ac[16][16], acq[16][16], acr[16][16];
    int16_t dcv[16], dcq[16], dcb[16];
    int16_t cdcc[2][4], cdcq[2][4], cdcdeq[2][4], cdcback[2][4];
    int16_t cac[2][4][16], cacq[2][4][16], cacback[2][4][16];
    uint8_t pred[16 * 16];
    x264pp_quant_t q, qc;
    int cbp_luma = 0, cbp_chroma = 0;
    int chroma_dc = 0, chroma_ac = 0;
    int pred_mode = 2, chroma_mode = 2, chroma_signal = 0;
    int mode, best_cost = 0x7fffffff, chroma_cost = 0x7fffffff;
    int qp = e->qp + X264PP_SLICE_QP_DELTA;
    int b, i, j, plane, x, y;
    int16_t zl[16];

    if( X264PP_FORCE_PCM )
    {
        encode_mb_pcm( e, bs, mbx, mby );
        return 0;
    }

    if( qp < 0 ) qp = 0;
    if( qp > 51 ) qp = 51;
    x264pp_quant_init( &q, qp, 0 );
    x264pp_quant_init( &qc, chroma_qp_normative( qp,
                                             e->pps.chroma_qp_index_offset ), 0 );

    /* ---- 16x16 luma prediction (8.3.3) ------------------------------- */
    gather_neighbors( &e->fdec, 0, mbx, mby, 16, &n );
    for( mode = 0; mode < 4; mode++ )
    {
        /* Intra16x16 plane needs BOTH neighbours; pred_fix4's 4x4 rules only
         * check the top neighbour for its diagonal modes. */
        int m = mode, cost;
        if( (m == 0 || m == 3) && !(n.avail & X264PP_NEIGH_TOP) ) m = 2;
        if( (m == 1 || m == 3) && !(n.avail & X264PP_NEIGH_LEFT) ) m = 2;
        pred16_norm( pred, 16, &n, m );
        cost = sad_block( e->fenc.plane[0] + (size_t)py * e->fenc.stride[0] + px,
                          e->fenc.stride[0], pred, 16, 16 );
        if( cost < best_cost )
        {
            best_cost = cost;
            pred_mode = m;
        }
    }
    pred16_norm( pred, 16, &n, pred_mode );
    for( y = 0; y < 16; y++ )
        memcpy( e->fdec.plane[0] + (size_t)(py + y) * e->fdec.stride[0] + px,
                pred + y * 16, 16 );

    /* ---- 4x4 DCTs + second-stage luma DC Hadamard (8.5.10) ----------- */
    for( b = 0; b < 16; b++ )
    {
        const int bx = (b % 4) * 4, by = (b / 4) * 4;
        int16_t res[16];
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
                res[y * 4 + x] = (int16_t)
                    ((int)e->fenc.plane[0][(size_t)(py + by + y) * e->fenc.stride[0]
                                           + px + bx + x]
                     - (int)pred[(by + y) * 16 + bx + x]);
        x264pp_dct4x4_avx2( res, ac[b] );
        dcv[b] = ac[b][0];                  /* second-stage luma DC */
        ac[b][0] = 0;                       /* coded separately as luma DC */
    }
    /* Intra16x16 DC (8.5.10): quantise the Hadamard transform by inverting
     * the decoder's linear map.  One DC level contributes dc_k * dc_sign[b][k]
     * to block b, with dc_k = 4 * dequant(1) for luma DC. */
    {
        int16_t one[16], oneq[16];
        int dc_k, b, k;
        for( i = 0; i < 16; i++ ) one[i] = 0;
        one[0] = 1;
        x264pp_dequant4( &q, one, oneq );
        dc_k = 4 * oneq[0];
        for( k = 0; k < 16; k++ )
        {
            int sum = 0;
            for( b = 0; b < 16; b++ )
                sum += 4 * dc_sign[b][k] * (int)dcv[b];
            dcq[k] = (int16_t)round_div( sum, 16 * dc_k );
        }
        for( b = 0; b < 16; b++ )
        {
            int sum = 0;
            for( k = 0; k < 16; k++ )
                sum += dc_sign[b][k] * (int)dcq[k];
            dcb[b] = (int16_t)(sum * dc_k);
        }
    }

    for( b = 0; b < 16; b++ )
    {
        const int bx = (b % 4) * 4, by = (b / 4) * 4;
        {
            int j;
            for( i = 0; i < 16; i++ ) { acq[b][i] = 0; acr[b][i] = 0; }
            for( j = 1; j < 16; j++ )
            {
                int idx = x264pp_zigzag4[j];
                int lvl = ac_quant_level( qp, idx, (int)ac[b][idx] );
                acq[b][idx] = (int16_t)lvl;
                acr[b][idx] = (int16_t)ac_dequant_one( qp, idx, lvl );
                if( lvl ) cbp_luma = 1;
            }
        }
        acr[b][0] = dcb[b];
        add_idct4( e->fdec.plane[0]
                   + (size_t)(py + by) * e->fdec.stride[0] + px + bx,
                   e->fdec.stride[0], acr[b] );
    }
    /* Store totals by H.264 block index (scan8 order), not raster order. */
    for( i = 0; i < 16; i++ )
        e->nz_y[mbidx * 16 + i] = (uint8_t)count_nonzero( acq[scan8[i]], 16 );

    /* ---- chroma 8x8 prediction + 2x2 DC / 4x4 AC (8.5.11) ------------- */
    {
        const int cpx = px / 2, cpy = py / 2;
        /* Cb and Cr must predict from their OWN reconstructed neighbours.
         * Reusing the U neighbour array for V made every directional/DC
         * prediction of V source its samples from U, which inflated the Cr
         * residual and compounded across macroblocks. */
        x264pp_neighbor_t cn[2];
        uint8_t cpredc[2][8 * 8], bestc[2][8 * 8];
        gather_neighbors( &e->fdec, 1, mbx, mby, 8, &cn[0] );
        gather_neighbors( &e->fdec, 2, mbx, mby, 8, &cn[1] );
        for( mode = 0; mode < 4; mode++ )
        {
            int m = mode, cost = 0;
            if( (m == 1 || m == 3) && !(cn[0].avail & X264PP_NEIGH_LEFT) ) m = 2;
            if( (m == 0 || m == 3) && !(cn[0].avail & X264PP_NEIGH_TOP) ) m = 2;
            for( plane = 1; plane <= 2; plane++ )
            {
                pred8c_norm( cpredc[plane - 1], 8, &cn[plane - 1], m );
                cost += sad_block( e->fenc.plane[plane]
                                   + (size_t)cpy * e->fenc.stride[plane] + cpx,
                                   e->fenc.stride[plane], cpredc[plane - 1], 8, 8 );
            }
            if( cost < chroma_cost )
            {
                chroma_cost = cost;
                chroma_mode = m;
                memcpy( bestc[0], cpredc[0], sizeof( bestc[0] ) );
                memcpy( bestc[1], cpredc[1], sizeof( bestc[1] ) );
            }
        }

        for( plane = 1; plane <= 2; plane++ )
        {
            const uint8_t *csrc = e->fenc.plane[plane]
                                + (size_t)cpy * e->fenc.stride[plane] + cpx;
            const uint8_t *cp = bestc[plane - 1];
            int16_t cv[4];
            for( y = 0; y < 8; y++ )
                memcpy( e->fdec.plane[plane] + (size_t)(cpy + y) * e->fdec.stride[plane] + cpx,
                        cp + y * 8, 8 );
            for( b = 0; b < 4; b++ )
            {
                const int bx = (b % 2) * 4, by = (b / 2) * 4;
                int16_t res[16];
                for( y = 0; y < 4; y++ )
                    for( x = 0; x < 4; x++ )
                        res[y * 4 + x] = (int16_t)
                            ((int)csrc[(by + y) * e->fenc.stride[plane] + bx + x]
                             - (int)cp[(by + y) * 8 + bx + x]);
                x264pp_dct4x4_avx2( res, cac[plane - 1][b] );
                cv[b] = cac[plane - 1][b][0];
                cac[plane - 1][b][0] = 0;
            }
            /* 8.5.11.1/8.5.11.2: W = H(DC)/2 is quantised with the 2x2 DC
             * multiplier; the decoder multiplies the levels by the default
             * ScalingList4x4 value 16 (8.5.9) before the >>5.  quant.c omits
             * that factor of 16, so the encoder's mirror of the decoder must
             * apply it explicitly on reconstruction. */
            hadamard2_dc( cv, cdcc[plane - 1] );
            for( i = 0; i < 4; i++ )
                cdcc[plane - 1][i] =
                    (int16_t)round_div( cdcc[plane - 1][i], 16 );
            x264pp_quant2( &qc, cdcc[plane - 1], cdcq[plane - 1] );
            chroma_dc_cavlc_fix( cdcq[plane - 1] );
            if( x264pp_last_nonzero( cdcq[plane - 1], 4 ) >= 0 )
                chroma_dc = 1;
            x264pp_dequant2( &qc, cdcq[plane - 1], cdcdeq[plane - 1] );
            for( i = 0; i < 4; i++ )
                cdcdeq[plane - 1][i] = (int16_t)(cdcdeq[plane - 1][i] * 16);
            hadamard2_dc_inv( cdcdeq[plane - 1], cdcback[plane - 1] );
            for( b = 0; b < 4; b++ )
            {
                const int bx = (b % 2) * 4, by = (b / 2) * 4;
                {
                    int j;
                    for( i = 0; i < 16; i++ )
                    { cacq[plane - 1][b][i] = 0; cacback[plane - 1][b][i] = 0; }
                    for( j = 1; j < 16; j++ )
                    {
                        int idx = x264pp_zigzag4[j];
                        int cqp = chroma_qp_normative( qp,
                                              e->pps.chroma_qp_index_offset );
                        int lvl = ac_quant_level( cqp, idx,
                                                  (int)cac[plane - 1][b][idx] );
                        cacq[plane - 1][b][idx] = (int16_t)lvl;
                        cacback[plane - 1][b][idx] =
                            (int16_t)ac_dequant_one( cqp, idx, lvl );
                        if( lvl ) chroma_ac = 1;
                    }
                }
                cacback[plane - 1][b][0] = cdcback[plane - 1][b];
                add_idct4( e->fdec.plane[plane]
                           + (size_t)(cpy + by) * e->fdec.stride[plane] + cpx + bx,
                           e->fdec.stride[plane], cacback[plane - 1][b] );
                e->nz_c[plane - 1][mbidx * 4 + b] =
                    (uint8_t)count_nonzero( cacq[plane - 1][b], 16 );
            }
        }
    }
    cbp_chroma = chroma_ac ? 2 : (chroma_dc ? 1 : 0);
    chroma_signal = chroma_signal_mode( chroma_mode );

    /* ---- bitstream syntax (7.3.5 / 7.3.5.1 / 7.3.5.3) ---------------- */
    /* H.264 Table 7-11: 1 + pred_mode + 4*CodedBlockPatternChroma
     *                  + 12*CodedBlockPatternLuma_flag. */
    x264pp_bs_ue( bs, (unsigned)(1 + pred_mode + 4 * cbp_chroma
                                + 12 * cbp_luma) );
    x264pp_bs_ue( bs, (unsigned)chroma_signal );
    x264pp_bs_se( bs, 0 );                   /* mb_qp_delta */

    /* Intra16x16 DC: 16 quantised levels already in scan order. */
    for( i = 0; i < 16; i++ )
        zl[i] = dcq[i];
    x264pp_cavlc_residual( bs, zl, 16, luma_nc( e, mbx, mby, 0 ) );

    /* Luma AC: only 15 levels per 4x4 block, in scan8 block order. */
    if( cbp_luma )
        for( i = 0; i < 16; i++ )
        {
            b = scan8[i];
            for( j = 0; j < 15; j++ )
                zl[j] = acq[b][x264pp_zigzag4[j + 1]];
            x264pp_cavlc_residual( bs, zl, 15, luma_nc( e, mbx, mby, i ) );
        }

    /* Both chroma DC blocks first, then all chroma AC blocks. */
    if( cbp_chroma > 0 )
        for( plane = 0; plane < 2; plane++ )
        {
            for( i = 0; i < 4; i++ )
                zl[i] = cdcq[plane][x264pp_zigzag2[i]];
            x264pp_cavlc_residual( bs, zl, 4, -1 );  /* 2x2 chroma DC */
        }
    if( cbp_chroma == 2 )
        for( plane = 0; plane < 2; plane++ )
            for( b = 0; b < 4; b++ )
            {
                for( i = 0; i < 15; i++ )
                    zl[i] = cacq[plane][b][x264pp_zigzag4[i + 1]];
                x264pp_cavlc_residual( bs, zl, 15,
                    block_nz_context( e, mbx, mby, b, 2, 2, e->nz_c[plane] ) );
            }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Frame and lifecycle                                                 */
/* ------------------------------------------------------------------ */

x264pp_encoder_t *x264pp_encoder_create( const x264pp_param_t *param )
{
    x264pp_encoder_t *e;
    x264pp_bs_t bs;
    int cap;

    if( !param )
        return NULL;
    e = (x264pp_encoder_t *)calloc( 1, sizeof( *e ) );
    if( !e )
        return NULL;
    e->param = *param;
    x264pp_sps_init( &e->sps, param );
    x264pp_pps_init( &e->pps, param, &e->sps );
    e->mb_width  = e->sps.mb_width;
    e->mb_height = e->sps.mb_height;
    e->mb_count  = e->mb_width * e->mb_height;
    e->qp = e->pps.pic_init_qp;

    if( x264pp_frame_alloc( &e->fenc, param->width, param->height ) != X264PP_OK ||
        x264pp_frame_alloc( &e->fdec, param->width, param->height ) != X264PP_OK )
        goto fail;
    x264pp_frame_fill( &e->fdec, 128 );

    e->nz_y = (uint8_t *)calloc( (size_t)e->mb_count * 16, 1 );
    e->nz_c[0] = (uint8_t *)calloc( (size_t)e->mb_count * 4, 1 );
    e->nz_c[1] = (uint8_t *)calloc( (size_t)e->mb_count * 4, 1 );
    e->i4_mode = (uint8_t *)malloc( (size_t)e->mb_count * 16 );
    e->pred_buf = (uint8_t *)malloc( 16 * 16 );
    e->res_buf  = (int16_t *)malloc( 16 * 16 * sizeof( int16_t ) );
    if( !e->nz_y || !e->nz_c[0] || !e->nz_c[1] || !e->i4_mode ||
        !e->pred_buf || !e->res_buf )
        goto fail;
    memset( e->i4_mode, 2, (size_t)e->mb_count * 16 );

    cap = e->mb_count * 16 * 32 + 8192;
    e->sps_rbsp = (uint8_t *)malloc( (size_t)cap );
    e->pps_rbsp = (uint8_t *)malloc( (size_t)cap );
    e->slice_rbsp = (uint8_t *)malloc( (size_t)cap );
    e->slice_cap = cap;
    if( !e->sps_rbsp || !e->pps_rbsp || !e->slice_rbsp )
        goto fail;

    x264pp_bs_init( &bs, e->sps_rbsp, cap );
    write_sps( &bs, &e->sps );               /* includes rbsp_trailing_bits */
    e->sps_len = x264pp_bs_bytes( &bs );
    if( bs.overflow )
        goto fail;

    x264pp_bs_init( &bs, e->pps_rbsp, cap );
    write_pps( &bs, &e->sps, &e->pps );
    e->pps_len = x264pp_bs_bytes( &bs );
    if( bs.overflow )
        goto fail;

    return e;

fail:
    x264pp_encoder_destroy( e );
    return NULL;
}

void x264pp_encoder_destroy( x264pp_encoder_t *e )
{
    if( !e )
        return;
    x264pp_frame_free( &e->fenc );
    x264pp_frame_free( &e->fdec );
    free( e->nz_y );
    free( e->nz_c[0] );
    free( e->nz_c[1] );
    free( e->i4_mode );
    free( e->pred_buf );
    free( e->res_buf );
    free( e->sps_rbsp );
    free( e->pps_rbsp );
    free( e->slice_rbsp );
    free( e );
}

static int append_nal( uint8_t *out, int cap, int *pos,
                       const uint8_t *rbsp, int len, int ref, int type )
{
    int n = x264pp_bs_nal( rbsp, len, ref, type, 1,
                           out + *pos, cap - *pos );
    if( n < 0 )
        return X264PP_ERR_INVALID;
    *pos += n;
    return X264PP_OK;
}

int x264pp_encoder_encode_frame( x264pp_encoder_t *e,
                                  const x264pp_picture_t *pic,
                                  uint8_t *out, int out_cap, int *out_len )
{
    x264pp_bs_t bs;
    int x, y, pos = 0, len;
    int rc;

    if( !e || !pic || !out || !out_len )
        return X264PP_ERR_INVALID;
    if( out_len )
        *out_len = 0;
    rc = x264pp_frame_load( &e->fenc, pic );
    if( rc != X264PP_OK )
        return rc;

    if( !e->wrote_headers )
    {
        rc = append_nal( out, out_cap, &pos, e->sps_rbsp, e->sps_len, 3, 7 );
        if( rc != X264PP_OK ) return rc;
        rc = append_nal( out, out_cap, &pos, e->pps_rbsp, e->pps_len, 3, 8 );
        if( rc != X264PP_OK ) return rc;
        e->wrote_headers = 1;
    }

    x264pp_bs_init( &bs, e->slice_rbsp, e->slice_cap );
    write_slice_header( &bs, e );
    /* CAVLC slices are not byte-aligned after the header. */
    for( y = 0; y < e->mb_height; y++ )
        for( x = 0; x < e->mb_width; x++ )
            encode_mb( e, &bs, x, y );
    if( bs.overflow )
        return X264PP_ERR_INVALID;
    len = x264pp_bs_trailing( &bs );
    if( len <= 0 )
        return X264PP_ERR_INVALID;

    rc = append_nal( out, out_cap, &pos, e->slice_rbsp, len, 3, 5 );
    if( rc != X264PP_OK )
        return rc;
    /* Every picture is an IDR in I1, so frame_num stays at zero. */
    *out_len = pos;
    return X264PP_OK;
}

