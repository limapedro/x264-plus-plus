#ifndef X264PP_CORE_ENCODE_H
#define X264PP_CORE_ENCODE_H

/*
 * I-slice / IDR encoder core for I1.  Original code for x264++.
 */

#include "core/api.h"
#include "core/frame.h"
#include "core/params.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x264pp_encoder
{
    x264pp_param_t param;
    x264pp_sps_t   sps;
    x264pp_pps_t   pps;

    x264pp_frame_t fenc;       /* padded source */
    x264pp_frame_t fdec;       /* padded reconstruction */

    int   mb_width, mb_height, mb_count;
    int   frame_num;
    int   wrote_headers;
    int   qp;

    uint8_t *nz_y;             /* mb_count * 16 nonzero counts (luma 4x4) */
    uint8_t *nz_c[2];          /* mb_count * 4 nonzero counts (chroma 4x4) */

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

/* Encode one picture.  On the first call the output contains SPS, PPS and
 * the IDR slice; later calls contain one IDR slice each. */
int x264pp_encoder_encode_frame( x264pp_encoder_t *enc,
                                 const x264pp_picture_t *pic,
                                 uint8_t *out, int out_cap, int *out_len );

void x264pp_encoder_destroy( x264pp_encoder_t *enc );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_ENCODE_H */
