#ifndef X264PP_CORE_ENCODE_H
#define X264PP_CORE_ENCODE_H

/*
 * I/P-slice CAVLC encoder core.  The I2 inter path is compile-time gated:
 * the default build is the known-good I1 all-IDR encoder, and a build with
 * -DX264PP_INTER_P=1 enables the P-slice implementation.  Original code for
 * x264++.
 */

#include "core/api.h"
#include "core/frame.h"
#include "core/params.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default OFF: certified speed is better without motion estimation. */
#ifndef X264PP_INTER_P
#define X264PP_INTER_P 0
#endif

/* I2 searches whole-pel displacements in [-8,+8] on each axis.  Motion
 * vectors are stored in quarter-luma units, so every value is a multiple of
 * four and no interpolation filter is needed on luma. */
#define X264PP_INTER_MV_RANGE 8

#if X264PP_INTER_P
/* Per-macroblock decision for a P picture.  Analysis (motion search, motion
 * compensation, transform, quantisation and reconstruction) runs before the
 * slice is written so that CAVLC mb_skip_run counts can be emitted in one
 * pass; the stored arrays are the exact levels the entropy writer emits. */
typedef struct x264pp_mb_inter
{
    uint8_t skip;                      /* P_Skip: inferred type, no syntax */
    uint8_t cbp_luma;                  /* four bits, one per 8x8 quadrant */
    uint8_t cbp_chroma;                /* 0 none, 1 DC only, 2 DC+AC */
    int16_t mv[2];                     /* chosen MV, quarter-luma units */
    int16_t mvp[2];                    /* predictor used for mvd_l0 */
    int16_t luma_q[16][16];            /* natural-order levels, 16 per 4x4 */
    int16_t chroma_dcq[2][4];          /* 2x2 chroma DC levels */
    int16_t chroma_acq[2][4][16];      /* chroma 4x4 AC levels */
} x264pp_mb_inter_t;
#endif

typedef struct x264pp_encoder
{
    x264pp_param_t param;
    x264pp_sps_t   sps;
    x264pp_pps_t   pps;

    x264pp_frame_t fenc;       /* padded source */
    x264pp_frame_t fdec;       /* padded reconstruction (current picture) */
#if X264PP_INTER_P
    x264pp_frame_t ref;        /* previous reconstructed picture, padded */
#endif

    int   mb_width, mb_height, mb_count;
    int   frame_num;           /* only advanced when X264PP_INTER_P */
    int   wrote_headers;
    int   qp;

    uint8_t *nz_y;             /* mb_count * 16 nonzero counts (luma 4x4) */
    uint8_t *nz_c[2];          /* mb_count * 4 nonzero counts (chroma 4x4) */
    uint8_t *i4_mode;          /* mb_count * 16 Intra_4x4 prediction modes */

#if X264PP_INTER_P
    int16_t (*mv_cache)[2];    /* mb_count MVs for the next picture's MVP */
    x264pp_mb_inter_t *inter_mbs;
#endif

    uint8_t *sps_rbsp;
    uint8_t *pps_rbsp;
    int      sps_len, pps_len;

    uint8_t *slice_rbsp;
    int      slice_cap;

    uint8_t *pred_buf;         /* 16x16 scratch prediction */
    int16_t *res_buf;          /* 16x16 scratch residual */
} x264pp_encoder_t;

/* Create an encoder from a fully validated parameter block. */
x264pp_encoder_t *x264pp_encoder_create( const x264pp_param_t *param );

/* Encode one picture.  The first call always contains SPS, PPS and an IDR
 * slice.  With X264PP_INTER_P=0 every later call is another IDR (I1
 * behaviour); with X264PP_INTER_P=1 it is a non-IDR P slice referencing the
 * previous reconstruction. */
int x264pp_encoder_encode_frame( x264pp_encoder_t *enc,
                                 const x264pp_picture_t *pic,
                                 uint8_t *out, int out_cap, int *out_len );

void x264pp_encoder_destroy( x264pp_encoder_t *enc );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_ENCODE_H */
