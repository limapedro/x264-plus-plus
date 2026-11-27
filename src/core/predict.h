#ifndef X264PP_CORE_PREDICT_H
#define X264PP_CORE_PREDICT_H

/*
 * Intra prediction sample generation for I1.  Original code for x264++
 * derived from docs/rebuild/common/predict.c.md and H.264 clause 8.3.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Neighbour availability flags. */
enum
{
    X264PP_NEIGH_LEFT      = 1,
    X264PP_NEIGH_TOP       = 2,
    X264PP_NEIGH_TOPRIGHT  = 4,
    X264PP_NEIGH_TOPLEFT   = 8
};

/* `t[0..15]` are the samples immediately above the block (left to right),
 * `l[0..15]` the samples immediately to its left (top to bottom), `tl` the
 * top-left corner.  Unavailable neighbours are filled with 128 by the caller. */
typedef struct x264pp_neighbor
{
    uint8_t t[17];
    uint8_t l[17];
    uint8_t tl;
    unsigned avail;
} x264pp_neighbor_t;

/* 16x16 luma modes 0=V, 1=H, 2=DC, 3=plane. */
void x264pp_pred16( uint8_t *dst, int stride, const x264pp_neighbor_t *n, int mode );
/* 4x4 luma modes 0..8. */
void x264pp_pred4( uint8_t *dst, int stride, const x264pp_neighbor_t *n, int mode );
/* 4:2:0 chroma 8x8 modes 0=V, 1=H, 2=DC, 3=plane. */
void x264pp_pred8c( uint8_t *dst, int stride, const x264pp_neighbor_t *n, int mode );

/* Prepare the 8x8 luma edge array.  Indices 7..14 hold the eight left
 * samples bottom-to-top, index 15 the top-left corner, 16..31 the sixteen
 * top samples, and index 32 a top-right extension. */
void x264pp_pred8_edge( const x264pp_neighbor_t *n, uint8_t edge[36],
                        unsigned filters );
/* 8x8 luma modes 0..8, using the prepared edge array. */
void x264pp_pred8( uint8_t *dst, int stride, const uint8_t edge[36], int mode );

/* Return a legal mode when a requested mode lacks its neighbours. */
int x264pp_pred_fix4( int mode, unsigned avail );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_PREDICT_H */
