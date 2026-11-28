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

/* The active compressed path codes every macroblock as I_16x16 (H.264
 * Table 7-11).  It deliberately avoids I_NxN: its rem_intra4x4_pred_mode
 * inference still diverges from independent decoders on textured input.
 * I_PCM remains in the tree as a diagnostic fallback; set the macro below
 * to 1 to restore the known-good, uncompressed path. */
#define X264PP_FORCE_PCM 0

/* I1 has no rate controller: the requested CRF becomes the picture QP in
 * params.c.  A constant slice_qp_delta lets the I_16x16 CAVLC path choose the
 * operating point that meets the compressed-size envelope while the decoder
 * and the encoder quantiser stay on the same QP. */
#define X264PP_SLICE_QP_DELTA 28

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

/* x264pp_pred8c uses the predictor-library order V,H,DC,plane.  H.264
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
    int16_t dcv[16], dcc[16], dcq[16], dcdeq[16], dcb[16];
    int16_t cdcc[2][4], cdcq[2][4], cdcdeq[2][4], cdcback[2][4];
    int16_t cac[2][4][16], cacq[2][4][16], cacback[2][4][16];
    uint8_t pred[16 * 16], cpred[8 * 8];
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
    x264pp_quant_init( &qc, x264pp_chroma_qp( qp,
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
        x264pp_pred16( pred, 16, &n, m );
        cost = sad_block( e->fenc.plane[0] + (size_t)py * e->fenc.stride[0] + px,
                          e->fenc.stride[0], pred, 16, 16 );
        if( cost < best_cost )
        {
            best_cost = cost;
            pred_mode = m;
        }
    }
    x264pp_pred16( pred, 16, &n, pred_mode );
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
    x264pp_hadamard4( dcv, dcc );
    /* The luma DC Hadamard pair has an 8x gain for a constant block and the
     * decoder dequantises with the ordinary 4x4 scaling.  Divide the forward
     * output by 8 before quantising (factor 1) so the decoder's inverse
     * Hadamard + IDCT reconstruct the original samples. */
    for( i = 0; i < 16; i++ )
        dcv[i] = (int16_t)((dcc[i] + (dcc[i] < 0 ? -4 : 4)) >> 3);
    x264pp_quant4( &q, dcv, dcq, 1 );
    x264pp_dequant4( &q, dcq, dcdeq );
    /* Independent decoders apply the Intra16x16 DC dequantisation with an
     * extra 2-bit shift; mirror it in the reconstruction buffer. */
    for( i = 0; i < 16; i++ )
        dcdeq[i] = (int16_t)(dcdeq[i] * 8);
    x264pp_hadamard4_inv( dcdeq, dcb );

    for( b = 0; b < 16; b++ )
    {
        const int bx = (b % 4) * 4, by = (b / 4) * 4;
        for( i = 0; i < 16; i++ )
            ac[b][i] = (int16_t)((ac[b][i] + (ac[b][i] < 0 ? -2 : 2)) >> 2);
        x264pp_quant4( &q, ac[b], acq[b], 1 );
        if( x264pp_last_nonzero( acq[b], 16 ) >= 0 )
            cbp_luma = 1;                   /* Table 7-11: 0 or all 15 */
        x264pp_dequant4( &q, acq[b], acr[b] );
        for( i = 0; i < 16; i++ )
            acr[b][i] = (int16_t)(acr[b][i] * 16);
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
        gather_neighbors( &e->fdec, 1, mbx, mby, 8, &n );
        for( mode = 0; mode < 4; mode++ )
        {
            int m = mode, cost = 0;
            if( (m == 1 || m == 3) && !(n.avail & X264PP_NEIGH_LEFT) ) m = 2;
            if( (m == 0 || m == 3) && !(n.avail & X264PP_NEIGH_TOP) ) m = 2;
            x264pp_pred8c( cpred, 8, &n, m );
            for( plane = 1; plane <= 2; plane++ )
                cost += sad_block( e->fenc.plane[plane]
                                   + (size_t)cpy * e->fenc.stride[plane] + cpx,
                                   e->fenc.stride[plane], cpred, 8, 8 );
            if( cost < chroma_cost )
            {
                chroma_cost = cost;
                chroma_mode = m;
            }
        }
        x264pp_pred8c( cpred, 8, &n, chroma_mode );

        for( plane = 1; plane <= 2; plane++ )
        {
            const uint8_t *csrc = e->fenc.plane[plane]
                                + (size_t)cpy * e->fenc.stride[plane] + cpx;
            int16_t cv[4];
            for( y = 0; y < 8; y++ )
                memcpy( e->fdec.plane[plane] + (size_t)(cpy + y) * e->fdec.stride[plane] + cpx,
                        cpred + y * 8, 8 );
            for( b = 0; b < 4; b++ )
            {
                const int bx = (b % 2) * 4, by = (b / 2) * 4;
                int16_t res[16];
                for( y = 0; y < 4; y++ )
                    for( x = 0; x < 4; x++ )
                        res[y * 4 + x] = (int16_t)
                            ((int)csrc[(by + y) * e->fenc.stride[plane] + bx + x]
                             - (int)cpred[(by + y) * 8 + bx + x]);
                x264pp_dct4x4_avx2( res, cac[plane - 1][b] );
                cv[b] = cac[plane - 1][b][0];
                cac[plane - 1][b][0] = 0;
            }
            x264pp_hadamard2( cv, cdcc[plane - 1] );
            /* Chroma DC is quantised with the 2x2 DC factor; independent
             * decoders scale it 4x harder than the scalar dequantiser. */
            for( i = 0; i < 4; i++ )
                cdcc[plane - 1][i] =
                    (int16_t)((cdcc[plane - 1][i] +
                               (cdcc[plane - 1][i] < 0 ? -8 : 8)) >> 4);
            x264pp_quant2( &qc, cdcc[plane - 1], cdcq[plane - 1] );
            if( x264pp_last_nonzero( cdcq[plane - 1], 4 ) >= 0 )
                chroma_dc = 1;
            x264pp_dequant2( &qc, cdcq[plane - 1], cdcdeq[plane - 1] );
            for( i = 0; i < 4; i++ )
                cdcdeq[plane - 1][i] = (int16_t)(cdcdeq[plane - 1][i] * 16);
            x264pp_hadamard2_inv( cdcdeq[plane - 1], cdcback[plane - 1] );
            for( b = 0; b < 4; b++ )
            {
                const int bx = (b % 2) * 4, by = (b / 2) * 4;
                for( i = 0; i < 16; i++ )
                    cac[plane - 1][b][i] =
                        (int16_t)((cac[plane - 1][b][i] +
                                   (cac[plane - 1][b][i] < 0 ? -2 : 2)) >> 2);
                x264pp_quant4( &qc, cac[plane - 1][b], cacq[plane - 1][b], 1 );
                if( x264pp_last_nonzero( cacq[plane - 1][b], 16 ) >= 0 )
                    chroma_ac = 1;
                x264pp_dequant4( &qc, cacq[plane - 1][b], cacback[plane - 1][b] );
                for( i = 0; i < 16; i++ )
                    cacback[plane - 1][b][i] =
                        (int16_t)(cacback[plane - 1][b][i] * 16);
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

    /* Intra16x16 DC: 16 levels in zigzag order, nC from luma block 0. */
    for( i = 0; i < 16; i++ )
        zl[i] = dcq[x264pp_zigzag4[i]];  /* 16-coefficient DC block */
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

