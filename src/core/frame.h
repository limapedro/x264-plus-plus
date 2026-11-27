#ifndef X264PP_CORE_FRAME_H
#define X264PP_CORE_FRAME_H

/*
 * 8-bit I420 pictures with padding for intra prediction and reconstruction.
 * Original code for x264++ (increment I1).
 */

#include "core/api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Planes are indexed 0=Y, 1=U, 2=V.  `plane[i]` points at sample (0,0) of
 * the coded picture; `stride[i]` is the distance between consecutive rows.
 * Padding of 16 luma / 8 chroma samples exists on every side so that a
 * predictor may read the top and left neighbours of any block with negative
 * offsets.  Edge samples are replicated into the padding. */
typedef struct x264pp_frame
{
    int       width;      /* coded width  (multiple of 16) */
    int       height;     /* coded height (multiple of 16) */
    int       stride[3];
    uint8_t  *plane[3];
    uint8_t  *base;       /* single allocation, may be NULL */
} x264pp_frame_t;

int  x264pp_frame_alloc( x264pp_frame_t *f, int width, int height );
void x264pp_frame_free( x264pp_frame_t *f );

/* Fill every coded and padded sample with `value`. */
void x264pp_frame_fill( x264pp_frame_t *f, int value );

/* Copy an I420 CLI picture into the coded area and replicate edge samples
 * into the padding.  Returns X264PP_ERR_INVALID for an unsupported format. */
int  x264pp_frame_load( x264pp_frame_t *f, const x264pp_picture_t *pic );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_FRAME_H */
