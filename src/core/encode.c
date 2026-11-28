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

#include <stdio.h>
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

/* Gather the neighbours of ONE 4x4 block addressed in global 4x4 block
 * coordinates (gather_neighbors above takes MB coordinates).  The
 * directional 4x4 predictors read up to t[7]/l[7]; H.264 8.3.1.2.2 defines
 * those out-of-range positions as the last available edge sample. */
static void gather4( const x264pp_frame_t *f, int plane,
                     int blkx, int blky, int pic_blocks_w,
                     x264pp_neighbor_t *n )
{
    int x, px = blkx * 4, py = blky * 4;
    const int stride = f->stride[plane];
    uint8_t *p = f->plane[plane];
    const int have_t = blky > 0, have_l = blkx > 0;

    memset( n, 0, sizeof( *n ) );
    n->avail = 0;
    if( have_t ) n->avail |= X264PP_NEIGH_TOP;
    if( have_l ) n->avail |= X264PP_NEIGH_LEFT;
    if( have_t && have_l ) n->avail |= X264PP_NEIGH_TOPLEFT;
    if( have_t && blkx + 1 < pic_blocks_w ) n->avail |= X264PP_NEIGH_TOPRIGHT;

    for( x = 0; x < 4; x++ )
    {
        n->t[x] = have_t ? p[(size_t)(py - 1) * stride + px + x] : 128;
        n->l[x] = have_l ? p[(size_t)(py + x) * stride + px - 1] : 128;
    }
    for( x = 4; x < 8; x++ )
    {
        n->t[x] = n->t[3];
        n->l[x] = n->l[3];
    }
    n->t[16] = n->t[15];
    n->l[16] = n->l[15];
    n->tl = (have_t && have_l) ? p[(size_t)(py - 1) * stride + px - 1] : 128;
}

/* H.264 8.3.1.1: the Intra4x4PredMode inferred for
 * prev_intra4x4_pred_mode_flag from the left/top already-coded blocks. */
static int derive_mode4( const x264pp_encoder_t *e, int mbx, int mby,
                         int b, const int modes[16] )
{
    const int bx = b & 3, by = b >> 2;
    const int have_l = (bx > 0) || (mbx > 0);
    const int have_t = (by > 0) || (mby > 0);
    int left = 2, top = 2;

    /* H.264 8.3.1.1: dcPredModePredictedFlag is set when either neighbour
     * macroblock is unavailable, and then BOTH neighbour modes are treated
     * as DC (2).  FFmpeg's decoder uses the same -1-sentinel/min rule. */
    if( !have_l || !have_t )
        return 2;
    if( bx > 0 )
        left = modes[b - 1];
    else if( mbx > 0 )
        left = e->intra4_mode[(mby * e->mb_width + mbx - 1) * 16 + by * 4 + 3];
    if( by > 0 )
        top = modes[b - 4];
    else if( mby > 0 )
        top = e->intra4_mode[((mby - 1) * e->mb_width + mbx) * 16 + 12 + bx];

    if( !have_l ) return top;
    if( !have_t ) return left;
    return left < top ? left : top;
}

/* Keep at most one non-zero level, clamped to +/-1, at the highest non-zero
 * scan position.  Then TotalCoeff=1 and TrailingOnes=1, so the CAVLC block
 * is just coeff_token + one trailing sign + total_zeros: no level_prefix,
 * level_suffix or run_before is emitted at all. */
static int restrict_levels( int16_t *zl, int n )
{
    int i, last = -1;
    for( i = 0; i < n; i++ )
        if( zl[i] ) last = i;
    if( last < 0 ) return 0;
    for( i = 0; i < last; i++ )
        zl[i] = 0;
    zl[last] = zl[last] < 0 ? -1 : 1;
    return 1;
}

/* FFmpeg's ff_h264_check_intra4x4_pred_mode checks cache positions
 * scan8[0]..scan8[0]+3 (4x4 blocks 0,1,4,5) for top availability and
 * scan8[0]+8*i (blocks 0,2,8,10) for left availability.  A top-dependent
 * mode on 0,1,4,5 or a left-dependent mode on 0,2,8,10 is rejected at the
 * picture border even when the in-MB neighbour exists.  Restrict the mode
 * choice so the stream is accepted by that decoder as well as the spec. */
static int legal_mode4( int m, int b, int mbx, int mby, unsigned avail )
{
    int legal = x264pp_pred_fix4( m, avail );
    if( mby == 0 && (b == 0 || b == 1 || b == 4 || b == 5) &&
        (legal == 0 || legal == 3 || legal == 4 || legal == 5 ||
         legal == 6 || legal == 7) )
        legal = 2;
    if( mbx == 0 && (b == 0 || b == 2 || b == 8 || b == 10) &&
        (legal == 1 || legal == 3 || legal == 4 || legal == 5 ||
         legal == 6 || legal == 8) )
        legal = 2;
    return legal;
}

/* H.264 8.3.6 / FFmpeg's ff_h264_check_intra_pred_mode: at a picture border
 * the vertical / horizontal / plane chroma modes are replaced by DC. */
static int fix8c( int s, unsigned avail )
{
    const int have_t = !!(avail & X264PP_NEIGH_TOP);
    const int have_l = !!(avail & X264PP_NEIGH_LEFT);
    if( !have_t && !have_l ) return 0;                  /* DC */
    if( !have_t ) return (s == 2 || s == 3) ? 0 : s;    /* V, plane -> DC */
    if( !have_l ) return (s == 1 || s == 3) ? 0 : s;    /* H, plane -> DC */
    return s;
}

static int encode_mb( x264pp_encoder_t *e, x264pp_bs_t *bs,
                       int mbx, int mby )
{
    const int mbidx = mby * e->mb_width + mbx;
    const int px = mbx * 16, py = mby * 16;
    x264pp_neighbor_t n;
    int16_t res[16], coef[16], qc[16], rec[16];
    int16_t lz[16][16], cdzl[2][4], cazl[2][4][15];
    int16_t cv[4], cdcc[4], cdcq[4], cdcdeq[4], cdcback[4];
    int16_t cac[4][16], cacq[16];
    uint8_t pred[16], cpred[8 * 8];
    x264pp_quant_t q;
    int modes[16], chroma_syn, chroma_pred;
    int derived, b, i, m, s, plane, x, y, cbp_luma = 0, cbp_chroma = 0;

    x264pp_quant_init( &q, e->qp, 0 );

    /* ---- luma: sixteen Intra4x4 blocks, raster order ---- */
    for( b = 0; b < 16; b++ )
    {
        const int bx = (b & 3) * 4, by = (b >> 2) * 4;
        const int blkx = mbx * 4 + (b & 3), blky = mby * 4 + (b >> 2);
        const uint8_t *src = e->fenc.plane[0]
            + (size_t)(py + by) * e->fenc.stride[0] + px + bx;
        uint8_t *dst = e->fdec.plane[0]
            + (size_t)(py + by) * e->fdec.stride[0] + px + bx;
        int best = 2, best_cost = 0x7fffffff, nz;

        gather4( &e->fdec, 0, blkx, blky, e->mb_width * 4, &n );
        for( m = 2; m == 2; m++ )
        {
            int legal = legal_mode4( m, b, mbx, mby, n.avail );
            int cost;
            x264pp_pred4( pred, 4, &n, legal );
            cost = sad_block( src, e->fenc.stride[0], pred, 4, 4 );
            if( cost < best_cost )
            {
                best_cost = cost;
                best = legal;
            }
        }
        modes[b] = best;
        e->intra4_mode[mbidx * 16 + b] = (uint8_t)best;
        x264pp_pred4( pred, 4, &n, best );
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
                res[y * 4 + x] = (int16_t)
                    ((int)src[(size_t)y * e->fenc.stride[0] + x] - (int)pred[y * 4 + x]);

        x264pp_dct4( res, coef );
        x264pp_quant4( &q, coef, qc, 4 );
        for( i = 0; i < 16; i++ )
            lz[b][i] = qc[x264pp_zigzag4[i]];
        nz = restrict_levels( lz[b], 16 );
        if( nz ) cbp_luma |= 1 << (b >> 2);
        e->nz_y[mbidx * 16 + b] = (uint8_t)nz;

        /* Reconstruction must use exactly the levels that are sent. */
        memset( qc, 0, sizeof( qc ) );
        for( i = 0; i < 16; i++ )
            qc[x264pp_zigzag4[i]] = lz[b][i];
        x264pp_dequant4( &q, qc, rec );
        x264pp_idct4_add( dst, e->fdec.stride[0], rec );
    }

    /* ---- chroma: one Intra_chroma_pred_mode shared by Cb and Cr ---- */
    {
        const uint8_t *csrc = e->fenc.plane[1]
            + (size_t)mby * 8 * e->fenc.stride[1] + mbx * 8;
        int best = 2, best_cost = 0x7fffffff;
        gather_neighbors( &e->fdec, 1, mbx, mby, 8, &n );
        for( s = 0; s < 4; s++ )
        {
            /* Syntax mode 0=DC, 1=H, 2=V, 3=plane; pred8c uses
             * 0=V, 1=H, 2=DC, 3=plane. */
            const int legal = fix8c( s, n.avail );
            const int p8 = (legal == 0) ? 2 : (legal == 1) ? 1 : (legal == 2) ? 0 : 3;
            int cost;
            x264pp_pred8c( cpred, 8, &n, p8 );
            cost = sad_block( csrc, e->fenc.stride[1], cpred, 8, 8 );
            if( cost < best_cost )
            {
                best_cost = cost;
                best = s;
            }
        }
        chroma_syn = fix8c( best, n.avail );
        chroma_pred = (chroma_syn == 0) ? 2 : (chroma_syn == 1) ? 1
                    : (chroma_syn == 2) ? 0 : 3;
    }

    for( plane = 1; plane <= 2; plane++ )
    {
        const int stride = e->fdec.stride[plane];
        const uint8_t *csrc = e->fenc.plane[plane]
            + (size_t)mby * 8 * e->fenc.stride[plane] + mbx * 8;
        uint8_t *cdst = e->fdec.plane[plane]
            + (size_t)mby * 8 * stride + mbx * 8;
        x264pp_neighbor_t cn;

        gather_neighbors( &e->fdec, plane, mbx, mby, 8, &cn );
        x264pp_pred8c( cpred, 8, &cn, chroma_pred );

        for( b = 0; b < 4; b++ )
        {
            const int bx = (b & 1) * 4, by = (b >> 1) * 4;
            for( y = 0; y < 4; y++ )
                for( x = 0; x < 4; x++ )
                    res[y * 4 + x] = (int16_t)
                        ((int)csrc[(size_t)(by + y) * e->fenc.stride[plane] + bx + x]
                         - (int)cpred[(by + y) * 8 + bx + x]);
            x264pp_dct4( res, cac[b] );
            cv[b] = cac[b][0];
            cac[b][0] = 0;                  /* AC blocks exclude the DC level */
        }

        /* Chroma 2x2 DC block (one per colour plane, coded before all AC). */
        x264pp_hadamard2( cv, cdcc );
        x264pp_quant2( &q, cdcc, cdcq );
        for( i = 0; i < 4; i++ )
            cdzl[plane - 1][i] = cdcq[x264pp_zigzag2[i]];
        if( restrict_levels( cdzl[plane - 1], 4 ) ) cbp_chroma |= 1;
        memset( cdcq, 0, sizeof( cdcq ) );
        for( i = 0; i < 4; i++ )
            cdcq[x264pp_zigzag2[i]] = cdzl[plane - 1][i];
        x264pp_dequant2( &q, cdcq, cdcdeq );
        x264pp_hadamard2_inv( cdcdeq, cdcback );

        for( b = 0; b < 4; b++ )
        {
            const int bx = (b & 1) * 4, by = (b >> 1) * 4;
            int nz;
            x264pp_quant4( &q, cac[b], cacq, 4 );
            for( i = 0; i < 15; i++ )
                cazl[plane - 1][b][i] = cacq[x264pp_zigzag4[i + 1]];
            nz = restrict_levels( cazl[plane - 1][b], 15 );
            if( nz ) cbp_chroma |= 2;
            e->nz_c[plane - 1][mbidx * 4 + b] = (uint8_t)nz;

            memset( cacq, 0, sizeof( cacq ) );
            for( i = 0; i < 15; i++ )
                cacq[x264pp_zigzag4[i + 1]] = cazl[plane - 1][b][i];
            cacq[0] = cdcback[b];
            x264pp_dequant4( &q, cacq, rec );
            x264pp_idct4_add( cdst + (size_t)by * stride + bx, stride, rec );
        }
    }

    /* ---- bitstream syntax, H.264 7.3.5 / 7.3.5.1 order ---- */
    x264pp_bs_ue( bs, 0 );                  /* mb_type = I_NxN (I_4x4) */
    for( b = 0; b < 16; b++ )
    {
        derived = derive_mode4( e, mbx, mby, b, modes );
        if( modes[b] == derived )
            x264pp_bs_write( bs, 1, 1 );    /* prev_intra4x4_pred_mode_flag */
        else
        {
            /* H.264 7.3.5.1: rem_intra4x4_pred_mode is the 3-bit mode number
             * with the inferred mode skipped, NOT the mode itself:
             *   mode = rem + (rem >= derived ? 1 : 0). */
            int rem = modes[b] < derived ? modes[b] : modes[b] - 1;
            x264pp_bs_write( bs, 0, 1 );
            x264pp_bs_write( bs, (unsigned)rem, 3 );
        }
    }
    x264pp_bs_ue( bs, (unsigned)chroma_syn );
    /* Safe subset: chroma DC is coded, chroma AC is omitted. */
    /* Safe subset: chroma DC is coded; chroma AC and luma residual are
     * omitted.  This keeps every CAVLC block at TotalCoeff<=1, so no
     * level_prefix/suffix or run_before is needed anywhere in the slice. */
    cbp_chroma = cbp_chroma > 1 ? 1 : cbp_chroma;
    cbp_luma = 0;
    x264pp_bs_ue( bs, (unsigned)x264pp_cavlc_cbp_code( cbp_chroma, cbp_luma ) );
    if( cbp_luma || cbp_chroma )
        x264pp_bs_se( bs, 0 );              /* mb_qp_delta */

    /* CodedBlockPatternLuma is a 4-bit mask over the 8x8 quadrants: a
     * residual block is present only for quadrants whose bit is set. */
    for( b = 0; b < 16; b++ )
    {
        if( !(cbp_luma & (1 << (b >> 2))) )
            continue;
        x264pp_cavlc_residual( bs, lz[b], 16,
            block_nz_context( e, mbx, mby, b, 4, 4, e->nz_y ) );
    }
    if( cbp_chroma >= 1 )
        for( plane = 0; plane < 2; plane++ )
            x264pp_cavlc_residual( bs, cdzl[plane], 4, -1 );
    if( cbp_chroma == 2 )
        for( plane = 0; plane < 2; plane++ )
            for( b = 0; b < 4; b++ )
                x264pp_cavlc_residual( bs, cazl[plane][b], 15,
                    block_nz_context( e, mbx, mby, b, 2, 2, e->nz_c[plane] ) );
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
    e->intra4_mode = (uint8_t *)calloc( (size_t)e->mb_count * 16, 1 );
    e->pred_buf = (uint8_t *)malloc( 16 * 16 );
    e->res_buf  = (int16_t *)malloc( 16 * 16 * sizeof( int16_t ) );
    if( !e->nz_y || !e->nz_c[0] || !e->nz_c[1] || !e->intra4_mode ||
        !e->pred_buf || !e->res_buf )
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
    free( e->intra4_mode );
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
    if( rc != X264PP_OK ) return rc;
    /* Every picture is an IDR in I1, so frame_num stays at zero. */
    *out_len = pos;
    return X264PP_OK;
}
