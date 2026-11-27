#ifndef X264PP_CORE_DCT_H
#define X264PP_CORE_DCT_H

/*
 * H.264 integer 4x4 forward/inverse transform and Hadamard DC transforms.
 * Original code for x264++ (increment I1), derived from
 * docs/rebuild/common/dct.c.md and H.264 clause 8.5.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zigzag scan position -> raster index, for a 4x4 coefficient block. */
extern const uint8_t x264pp_zigzag4[16];
/* Scan position -> raster index for a 2x2 chroma DC block. */
extern const uint8_t x264pp_zigzag2[4];

/* Forward 4x4 transform of a raster-order residual block.  The result is
 * stored in raster order (row*4 + column); no shift or rounding is applied. */
void x264pp_dct4( const int16_t res[16], int16_t coef[16] );

/* Inverse 4x4 transform of a dequantised coefficient block.  The result is
 * added to the samples at `dst` and clipped to [0,255]. */
void x264pp_idct4_add( uint8_t *dst, int stride, const int16_t coef[16] );

/* Forward 2x2 Hadamard over the four 4x4 DC values of a 4:2:0 chroma block. */
void x264pp_hadamard2( const int16_t in[4], int16_t out[4] );
/* Inverse 2x2 Hadamard with the decoder's one-bit right shift. */
void x264pp_hadamard2_inv( const int16_t in[4], int16_t out[4] );

/* Forward 4x4 Hadamard over the sixteen 4x4 DC values of a luma 16x16 MB;
 * the second one-dimensional pass rounds and shifts right by one. */
void x264pp_hadamard4( const int16_t in[16], int16_t out[16] );
/* Inverse 4x4 Hadamard with the decoder's two-bit right shift. */
void x264pp_hadamard4_inv( const int16_t in[16], int16_t out[16] );

/* Highest index i with coef[i] != 0, or -1 for an all-zero block. */
int x264pp_last_nonzero( const int16_t *coef, int n );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_DCT_H */
