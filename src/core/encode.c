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

#include <stdlib.h>
#include <string.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))

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
    x264pp_bs_se( bs, 0 );                    /* slice_qp_delta */
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
    for( y = 0; y < size; y++ )
        for( x = 0; x < size; x++ )
        {
            int d = (int)a[y * astride + x] - (int)b[y * bstride + x];
            sum += d < 0 ? -d : d;
        }
    return sum;
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

    /* A CAVLC decoder keeps unavailable neighbours as "64" and computes
     * i = (nA+nB+1)>>1 only when both are present; with one neighbour
     * present i = that count, with none i = 0.  Match that exactly. */
    if( haveA && haveB )
        return MIN( (nA + nB + 1) >> 1, 16 );
    if( haveA )
        return MIN( nA, 16 );
    if( haveB )
        return MIN( nB, 16 );
    return 0;
}

static int encode_mb( x264pp_encoder_t *e, x264pp_bs_t *bs,
                      int mbx, int mby )
{
    const int mbidx = mby * e->mb_width + mbx;
    const int px = mbx * 16, py = mby * 16;
    x264pp_neighbor_t n;
    int16_t ac[16][16], acq[16][16], dcvals[16], dcc[16], dcq[16], dcdeq[16], dcback[16];
    int16_t cdcc[2][4], cdcq[2][4], cdcdeq[2][4], cdcback[2][4];
    int16_t cac[2][4][16], cacq[2][4][16];
    uint8_t pred[16 * 16];
    x264pp_quant_t q;
    int mode, best_mode = 0, best_cost = 0x7fffffff, pred_mode;
    int cbp_luma = 0, cbp_chroma = 0;
    int b, i, plane, cbp, x, y;
    int16_t zl[16];
    int nz_count;

    x264pp_quant_init( &q, e->qp, 0 );
    gather_neighbors( &e->fdec, 0, mbx, mby, 16, &n );

    /* Mode decision: smallest SAD over the four 16x16 predictors. */
    for( mode = 0; mode < 4; mode++ )
    {
        int m = x264pp_pred_fix4( mode, n.avail );
        x264pp_pred16( pred, 16, &n, m );
        {
            int cost = sad_block( e->fenc.plane[0] + (size_t)py * e->fenc.stride[0] + px,
                                  e->fenc.stride[0], pred, 16, 16 );
            if( cost < best_cost )
            {
                best_cost = cost;
                best_mode = m;
            }
        }
    }
    pred_mode = best_mode;
    x264pp_pred16( pred, 16, &n, pred_mode );

    /* Luma residual -> four-by-four DCTs, Hadamard DC, quantisation. */
    for( b = 0; b < 16; b++ )
    {
        int bx = (b % 4) * 4, by = (b / 4) * 4;
        int16_t res[16];
        int x, y;
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
            {
                int sy = py + by + y, sx = px + bx + x;
                res[y * 4 + x] = (int16_t)
                    ((int)e->fenc.plane[0][(size_t)sy * e->fenc.stride[0] + sx]
                     - (int)pred[(by + y) * 16 + bx + x]);
            }
        x264pp_dct4( res, ac[b] );
        dcvals[b] = ac[b][0];
        ac[b][0] = 0;                       /* AC blocks exclude DC */
    }
    x264pp_hadamard4( dcvals, dcc );
    x264pp_quant4( &q, dcc, dcq, 2 );       /* luma DC: decoder normalisation /4 */
    x264pp_dequant4( &q, dcq, dcdeq );      /* dequantised Hadamard coefficients */
    x264pp_hadamard4_inv( dcdeq, dcback );

    for( b = 0; b < 16; b++ )
    {
        int bx = (b % 4) * 4, by = (b / 4) * 4;
        x264pp_quant4( &q, ac[b], acq[b], 4 );
        if( x264pp_last_nonzero( acq[b], 16 ) >= 0 )
            cbp_luma = 15;
        x264pp_dequant4( &q, acq[b], ac[b] );
        ac[b][0] = dcback[b];
        x264pp_idct4_add( e->fdec.plane[0] + (size_t)(py + by) * e->fdec.stride[0]
                          + px + bx, e->fdec.stride[0], ac[b] );
        nz_count = 0;
        for( i = 0; i < 16; i++ )
            nz_count += acq[b][i] != 0;
        e->nz_y[mbidx * 16 + b] = (uint8_t)nz_count;
    }

    /* Chroma: 8x8 block per plane, split into four 4x4 blocks. */
    for( plane = 1; plane <= 2; plane++ )
    {
        int cpx = px / 2, cpy = py / 2;
        int16_t cv[4];
        x264pp_neighbor_t cn;
        uint8_t cpred[8 * 8];
        gather_neighbors( &e->fdec, plane, mbx, mby, 8, &cn );
        x264pp_pred8c( cpred, 8, &cn, 2 );  /* DC prediction for chroma */
        for( b = 0; b < 4; b++ )
        {
            int bx = (b % 2) * 4, by = (b / 2) * 4;
            int16_t res[16];
            int x, y;
            for( y = 0; y < 4; y++ )
                for( x = 0; x < 4; x++ )
                    res[y * 4 + x] = (int16_t)
                        ((int)e->fenc.plane[plane][(size_t)(cpy + by + y) * e->fenc.stride[plane] + cpx + bx + x]
                         - (int)cpred[(by + y) * 8 + bx + x]);
            x264pp_dct4( res, cac[plane - 1][b] );
            cv[b] = cac[plane - 1][b][0];
            cac[plane - 1][b][0] = 0;
        }
        x264pp_hadamard2( cv, cdcc[plane - 1] );
        x264pp_quant2( &q, cdcc[plane - 1], cdcq[plane - 1] );
        if( x264pp_last_nonzero( cdcq[plane - 1], 4 ) >= 0 )
            cbp_chroma = 1;
        x264pp_dequant2( &q, cdcq[plane - 1], cdcdeq[plane - 1] );
        x264pp_hadamard2_inv( cdcdeq[plane - 1], cdcback[plane - 1] );

        for( b = 0; b < 4; b++ )
        {
            int bx = (b % 2) * 4, by = (b / 2) * 4;
            x264pp_quant4( &q, cac[plane - 1][b], cacq[plane - 1][b], 4 );
            if( x264pp_last_nonzero( cacq[plane - 1][b], 16 ) >= 0 )
                cbp_chroma = 2;
            x264pp_dequant4( &q, cacq[plane - 1][b], cac[plane - 1][b] );
            cac[plane - 1][b][0] = cdcback[plane - 1][b];
            x264pp_idct4_add( e->fdec.plane[plane]
                              + (size_t)(cpy + by) * e->fdec.stride[plane] + cpx + bx,
                              e->fdec.stride[plane], cac[plane - 1][b] );
            nz_count = 0;
            for( i = 0; i < 16; i++ )
                nz_count += cacq[plane - 1][b][i] != 0;
            e->nz_c[plane - 1][mbidx * 4 + b] = (uint8_t)nz_count;
        }
    }

    /* H.264 9.2.1/9.2.2: the CAVLC AC path is only used for DC-only
     * macroblocks.  Macroblocks with luma or chroma AC energy fall back to
     * I_PCM (Table 7-11 mb_type 25), which has no CAVLC context dependency
     * and is unconditionally decodable. */
    if( 1 ) /* I_PCM fallback: CAVLC residual path retained below */
    {
        x264pp_bs_ue( bs, 25 );             /* mb_type = I_PCM */
        while( bs->bitpos & 7 )
            x264pp_bs_write( bs, 0, 1 );    /* pcm_alignment_zero_bit */
        for( y = 0; y < 16; y++ )
            for( x = 0; x < 16; x++ )
            {
                uint8_t v = e->fenc.plane[0][(size_t)(py + y) * e->fenc.stride[0] + px + x];
                x264pp_bs_write( bs, v, 8 );
                e->fdec.plane[0][(size_t)(py + y) * e->fdec.stride[0] + px + x] = v;
            }
        for( plane = 1; plane <= 2; plane++ )
        {
            int cpx = px / 2, cpy = py / 2;
            for( y = 0; y < 8; y++ )
                for( x = 0; x < 8; x++ )
                {
                    uint8_t v = e->fenc.plane[plane][(size_t)(cpy + y) * e->fenc.stride[plane] + cpx + x];
                    x264pp_bs_write( bs, v, 8 );
                    e->fdec.plane[plane][(size_t)(cpy + y) * e->fdec.stride[plane] + cpx + x] = v;
                }
        }
        memset( e->nz_y + mbidx * 16, 0, 16 );
        memset( e->nz_c[0] + mbidx * 4, 0, 4 );
        memset( e->nz_c[1] + mbidx * 4, 0, 4 );
        return 0;
    }

    /* ---- bitstream syntax ---- */
    /* H.264 Table 7-11: I_16x16 mb_type = 1 + pred_mode
     * + 4*CodedBlockPatternChroma + 12*CodedBlockPatternLuma_flag. */
    cbp = (cbp_luma ? 12 : 0) + cbp_chroma * 4;
    x264pp_bs_ue( bs, (unsigned)(1 + pred_mode + cbp) );
    x264pp_bs_ue( bs, 0 );                   /* intra_chroma_pred_mode = DC */
    x264pp_bs_se( bs, 0 );                   /* mb_qp_delta */

    for( i = 0; i < 16; i++ )
        zl[i] = dcq[x264pp_zigzag4[i]];
    x264pp_cavlc_residual( bs, zl, 16, 0 );  /* Intra16x16 DC */

    if( cbp_luma )
        for( b = 0; b < 16; b++ )
        {
            /* Intra16x16 AC blocks carry only 15 levels (zigzag positions
             * 1..15); position 0 is the separately coded Intra16x16 DC.
             * Passing a 16th, permanently-zero level would make total_zeros
             * one too large and desynchronise the CAVLC decoder. */
            for( i = 0; i < 15; i++ )
                zl[i] = acq[b][x264pp_zigzag4[i + 1]];
            x264pp_cavlc_residual( bs, zl, 15,
                block_nz_context( e, mbx, mby, b, 4, 4, e->nz_y ) );
        }

    /* H.264 7.3.5.3: both chroma DC blocks are written first (Cb, Cr), then
     * the chroma AC blocks of Cb followed by those of Cr.  Interleaving the
     * planes (DC,AC for Cb then DC,AC for Cr) makes the decoder read the
     * first AC block where it expects the second DC block. */
    if( cbp_chroma >= 1 )
        for( plane = 0; plane < 2; plane++ )
        {
            for( i = 0; i < 4; i++ )
                zl[i] = cdcq[plane][x264pp_zigzag2[i]];
            x264pp_cavlc_residual( bs, zl, 4, -1 );  /* chroma DC: 2x2 (4:2:0) */
        }
    if( cbp_chroma == 2 )
        for( plane = 0; plane < 2; plane++ )
            for( b = 0; b < 4; b++ )
            {
                /* Chroma 4x4 AC blocks also carry 15 levels; the DC level
                 * lives in the separate 2x2 chroma DC block. */
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
    e->pred_buf = (uint8_t *)malloc( 16 * 16 );
    e->res_buf  = (int16_t *)malloc( 16 * 16 * sizeof( int16_t ) );
    if( !e->nz_y || !e->nz_c[0] || !e->nz_c[1] || !e->pred_buf || !e->res_buf )
        goto fail;

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
