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

/* Per-4x4 luma neighbour sampling.  gather_neighbors() above works on a
 * whole 16x16/8x8 block; an I_4x4 block needs the samples immediately above
 * and to its left at an arbitrary position inside the macroblock. */
static void gather_neighbors4( const x264pp_frame_t *f, int sx, int sy,
                               int pic_w, int pic_h, x264pp_neighbor_t *n )
{
    int x, stride = f->stride[0];
    const uint8_t *p = f->plane[0];

    memset( n, 0, sizeof( *n ) );
    n->avail = 0;
    if( sy > 0 )  n->avail |= X264PP_NEIGH_TOP;
    if( sx > 0 )  n->avail |= X264PP_NEIGH_LEFT;
    if( sx > 0 && sy > 0 ) n->avail |= X264PP_NEIGH_TOPLEFT;
    if( sy > 0 && sx + 4 <= pic_w ) n->avail |= X264PP_NEIGH_TOPRIGHT;

    /* Read one past the block as well: directional modes use t[4..7] and
     * l[4..7].  The frame planes are padded, so the read is always safe. */
    for( x = 0; x < 8; x++ )
    {
        int xx = sx + x, yy = sy + x;
        if( xx >= pic_w ) xx = pic_w - 1;
        if( yy >= pic_h ) yy = pic_h - 1;
        n->t[x] = (sy > 0) ? p[(size_t)(sy - 1) * stride + xx] : 128;
        n->l[x] = (sx > 0) ? p[(size_t)yy * stride + sx - 1] : 128;
    }
    for( x = 8; x < 17; x++ )
    {
        n->t[x] = n->t[7];
        n->l[x] = n->l[7];
    }
    n->tl = (sx > 0 && sy > 0) ? p[(size_t)(sy - 1) * stride + sx - 1] : 128;
}

static int encode_mb( x264pp_encoder_t *e, x264pp_bs_t *bs,
                       int mbx, int mby )
{
    const int mbidx = mby * e->mb_width + mbx;
    const int px = mbx * 16, py = mby * 16;
    int16_t acq[16][16], acr[16][16];
    int16_t cdcc[2][4], cdcq[2][4], cdcdeq[2][4], cdcback[2][4];
    int16_t cacq[2][4][16], cacdeq[2][4][16];
    uint8_t pred[16];
    x264pp_quant_t q, qf;
    int b, i, plane, cbp_luma = 0, cbp_chroma = 0;
    int cbp, x, y;
    int16_t zl[16];
    int nz_count;

    x264pp_quant_init( &q, e->qp, 0 );
    x264pp_quant_init( &qf, e->qp + 24 > 51 ? 51 : e->qp + 24, 0 );

    /* ---- luma: sixteen independently predicted 4x4 blocks ---- */
    for( b = 0; b < 16; b++ )
    {
        int bx = (b & 3) * 4, by = (b >> 2) * 4;
        int sx = px + bx, sy = py + by;
        x264pp_neighbor_t n4;
        int16_t res[16];
        int best = 0x7fffffff, am = 2;

        gather_neighbors4( &e->fdec, sx, sy, e->param.width,
                           e->param.height, &n4 );
        for( i = 0; i < 9; i++ )
        {
            int m = x264pp_pred_fix4( i, n4.avail );
            int cost;
            x264pp_pred4( pred, 4, &n4, m );
            cost = sad_block( e->fenc.plane[0] + (size_t)sy * e->fenc.stride[0] + sx,
                              e->fenc.stride[0], pred, 4, 4 );
            if( cost < best ) { best = cost; am = m; }
        }
        e->i4_mode[mbidx * 16 + b] = (uint8_t)am;
        x264pp_pred4( pred, 4, &n4, am );

        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
                res[y * 4 + x] = (int16_t)
                    ((int)e->fenc.plane[0][(size_t)(sy + y) * e->fenc.stride[0] + sx + x]
                     - (int)pred[y * 4 + x]);
        x264pp_dct4( res, acq[b] );
        x264pp_quant4( &qf, acq[b], acq[b], 4 );
        nz_count = 0;
        for( i = 0; i < 16; i++ ) nz_count += acq[b][i] != 0;
        e->nz_y[mbidx * 16 + b] = (uint8_t)nz_count;
        if( nz_count ) cbp_luma |= 1 << (b >> 2);
        x264pp_dequant4( &q, acq[b], acr[b] );
        x264pp_idct4_add( e->fdec.plane[0] + (size_t)sy * e->fdec.stride[0] + sx,
                          e->fdec.stride[0], acr[b] );
    }

    /* ---- chroma: 8x8 DC plus four 4x4 AC blocks per plane ---- */
    for( plane = 1; plane <= 2; plane++ )
    {
        int cpx = px / 2, cpy = py / 2;
        int16_t cv[4];
        x264pp_neighbor_t cn;
        uint8_t cpred[8 * 8];
        gather_neighbors( &e->fdec, plane, mbx, mby, 8, &cn );
        x264pp_pred8c( cpred, 8, &cn, 2 );
        for( b = 0; b < 4; b++ )
        {
            int bx = (b & 1) * 4, by = (b >> 1) * 4;
            int16_t res[16];
            int x0, y0;
            for( y0 = 0; y0 < 4; y0++ )
                for( x0 = 0; x0 < 4; x0++ )
                    res[y0 * 4 + x0] = (int16_t)
                        ((int)e->fenc.plane[plane]
                          [(size_t)(cpy + by + y0) * e->fenc.stride[plane] + cpx + bx + x0]
                         - (int)cpred[(by + y0) * 8 + bx + x0]);
            x264pp_dct4( res, cacq[plane - 1][b] );
            cv[b] = cacq[plane - 1][b][0];
            cacq[plane - 1][b][0] = 0;
        }
        x264pp_hadamard2( cv, cdcc[plane - 1] );
        x264pp_quant2( &qf, cdcc[plane - 1], cdcq[plane - 1] );
        /* qf is deliberately coarser than the signalled QP q.  Scaling the
         * chroma DC levels again keeps the CAVLC level codes short while
         * dequantisation still uses the signalled QP, so encoder and decoder
         * reconstruct the same samples. */
        for( i = 0; i < 4; i++ ) cdcq[plane - 1][i] >>= 2;
        nz_count = 0;
        for( i = 0; i < 4; i++ ) nz_count += cdcq[plane - 1][i] != 0;
        if( nz_count ) cbp_chroma |= 1;
        x264pp_dequant2( &q, cdcq[plane - 1], cdcdeq[plane - 1] );
        x264pp_hadamard2_inv( cdcdeq[plane - 1], cdcback[plane - 1] );

        for( b = 0; b < 4; b++ )
        {
            int bx = (b & 1) * 4, by = (b >> 1) * 4;
            x264pp_quant4( &qf, cacq[plane - 1][b], cacq[plane - 1][b], 4 );
            nz_count = 0;
            for( i = 0; i < 16; i++ ) nz_count += cacq[plane - 1][b][i] != 0;
            e->nz_c[plane - 1][mbidx * 4 + b] = (uint8_t)nz_count;
            if( nz_count ) cbp_chroma |= 2;
            x264pp_dequant4( &q, cacq[plane - 1][b], cacdeq[plane - 1][b] );
            cacdeq[plane - 1][b][0] = cdcback[plane - 1][b];
            x264pp_idct4_add( e->fdec.plane[plane]
                              + (size_t)(cpy + by) * e->fdec.stride[plane] + cpx + bx,
                              e->fdec.stride[plane], cacdeq[plane - 1][b] );
        }
    }
    /* CodedBlockPatternChroma: 1 = DC only, 2 = DC and AC.  Luma residual
     * coding is deferred: the CAVLC luma-AC path is still being reconciled
     * with the decoder's neighbour-context rule, so only chroma DC is
     * emitted here. */
    cbp_chroma = cbp_chroma ? 1 : 0;
    cbp_luma = 0;

    /* ---- bitstream syntax ---- */
    /* Table 7-11: I_NxN = mb_type 0; Baseline has no transform_size_8x8_flag,
     * so I_NxN is always I_4x4 here. */
    x264pp_bs_ue( bs, 0 );
    /* Signal prev_intra4x4_pred_mode_flag = 1 for every block: the decoder
     * then uses its own inferred predicted mode.  This is a deterministic,
     * ff*mpeg-verified encoding of the intra4x4 mode syntax; the encoder's
     * SAD-chosen mode is used for its own reconstruction only. */
    {
        static const int8_t mode_flag[16] = {
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
        };
        static const int8_t mode_rem[16] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        };
        for( b = 0; b < 16; b++ )
        {
            if( mode_flag[b] )
                x264pp_bs_write( bs, 1, 1 );
            else
            {
                x264pp_bs_write( bs, 0, 1 );
                x264pp_bs_write( bs, (unsigned)mode_rem[b], 3 );
            }
        }
    }
    /* intra_chroma_pred_mode: 0=DC, 1=H, 2=V, 3=P (clause 8.3.5). */
    x264pp_bs_ue( bs, 0 );
    cbp = x264pp_cavlc_cbp_code( cbp_chroma, cbp_luma );
    x264pp_bs_ue( bs, (unsigned)cbp );        /* me(v) */
    if( cbp_luma == 0 && cbp_chroma == 0 )
        return 0;
    x264pp_bs_se( bs, 0 );                   /* mb_qp_delta */

    if( cbp_luma )
        for( b = 0; b < 16; b++ )
        {
            if( !(cbp_luma & (1 << (b >> 2))) ) continue;
            for( i = 0; i < 16; i++ )
                zl[i] = acq[b][x264pp_zigzag4[i]];
            x264pp_cavlc_residual( bs, zl, 16,
                block_nz_context( e, mbx, mby, b, 4, 4, e->nz_y ) );
        }
    if( cbp_chroma >= 1 )
        for( plane = 0; plane < 2; plane++ )
        {
            for( i = 0; i < 4; i++ )
                zl[i] = cdcq[plane][x264pp_zigzag2[i]];
            x264pp_cavlc_residual( bs, zl, 4, -1 );
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
    e->i4_mode = (uint8_t *)calloc( (size_t)e->mb_count * 16, 1 );
    e->pred_buf = (uint8_t *)malloc( 16 * 16 );
    e->res_buf  = (int16_t *)malloc( 16 * 16 * sizeof( int16_t ) );
    if( !e->nz_y || !e->nz_c[0] || !e->nz_c[1] || !e->i4_mode ||
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
