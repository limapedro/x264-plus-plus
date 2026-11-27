#ifndef X264PP_X86_KERNELS_H
#define X264PP_X86_KERNELS_H

/* Original clean-room kernel ABI. Implemented entirely in hand-written NASM. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* cpu.c (C shim) publishes feature bits: 0=SSE2 1=SSE3 2=SSSE3 3=SSE4.1
 * 4=AVX 5=AVX2. Kernels are selected by the dispatcher in dispatch.asm. */
void  x264pp_x86_init( int flags );
int   x264pp_x86_flags( void );

/* ---- distortion metrics. Return unsigned sum, zero-extended in EAX. ---- */
int x264pp_sad4x4_avx2  ( const uint8_t *a, intptr_t sa, const uint8_t *b, intptr_t sb );
int x264pp_sad8x8_avx2  ( const uint8_t *a, intptr_t sa, const uint8_t *b, intptr_t sb );
int x264pp_sad16x16_avx2( const uint8_t *a, intptr_t sa, const uint8_t *b, intptr_t sb );
int x264pp_satd4x4_avx2 ( const uint8_t *a, intptr_t sa, const uint8_t *b, intptr_t sb );
int x264pp_satd8x8_avx2 ( const uint8_t *a, intptr_t sa, const uint8_t *b, intptr_t sb );
int x264pp_satd16x16_avx2( const uint8_t *a, intptr_t sa, const uint8_t *b, intptr_t sb );

/* ---- 4x4 integer transform. src/dst are 16 signed 16-bit values,
 * row-major. Hadamard operates on residuals for DC-only blocks. ---- */
void x264pp_dct4x4_avx2     ( const int16_t *src, int16_t *dst );
void x264pp_idct4x4_avx2    ( const int16_t *src, int16_t *dst );
void x264pp_hadamard4x4_avx2( const int16_t *src, int16_t *dst );

/* ---- intra prediction. above[] has 9 usable samples, left[] has 9.
 * dst is the 4x4 or 16x16 block at the given stride. mode follows
 * H.264 intra prediction mode numbering. ---- */
void x264pp_intra4x4_avx2 ( uint8_t *dst, intptr_t stride, const uint8_t *above, const uint8_t *left, int mode );
void x264pp_intra16x16_avx2( uint8_t *dst, intptr_t stride, const uint8_t *above, const uint8_t *left, int mode );

/* ---- scalar quantisation. qp_table is the per-QP multiply/shift table.
 * zigzag maps natural -> scan order. Returns count of non-zero coefficients. ---- */
int  x264pp_quant4x4_avx2( const int16_t *coef, int16_t *qcoef, int16_t *dqcoef,
                           const uint16_t *qp_table, int qp, const uint8_t *zigzag );
void x264pp_dequant4x4_avx2( const int16_t *qcoef, int16_t *dqcoef,
                             const uint16_t *qp_table, int qp, const uint8_t *zigzag );

/* ---- deblocking: one horizontal or vertical edge, luma (8 samples) or
 * chroma (4 samples). bS is the boundary strength array. ---- */
void x264pp_deblock_edge_avx2( uint8_t *pix, intptr_t stride, int alpha, int beta,
                               const int8_t *bS, int chroma, int vertical );

#ifdef __cplusplus
}
#endif
#endif
