#ifndef X264PP_CORE_QUANT_H
#define X264PP_CORE_QUANT_H

/*
 * Quantisation / dequantisation for I1 (4x4 transform only).
 * Original code for x264++ derived from docs/rebuild/common/quant.c.md and
 * H.264 clauses 8.5.9 / 8.5.12.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x264pp_quant
{
    int qp;           /* 0..51, clamped by the caller */
    int q6;           /* qp % 6 */
    int level_scale;  /* H.264 levelScale(q6): {10,11,13,14,16,18} */
    int deadzone;     /* 0..32; larger means a wider dead zone */
    int64_t num, den, bias;  /* precomputed by quant_init */
} x264pp_quant_t;

void x264pp_quant_init( x264pp_quant_t *q, int qp, int deadzone );

/* Forward quantisation of a 4x4 coefficient block.  `factor` is 4 for normal
 * 4x4 residual blocks and 2 for the Hadamard DC blocks, reflecting the
 * different decoder normalisation of those transforms.  Returns 1 if any
 * level is nonzero. */
int  x264pp_quant4( const x264pp_quant_t *q, const int16_t in[16],
                    int16_t levels[16], int factor );
int  x264pp_quant2( const x264pp_quant_t *q, const int16_t in[4],
                    int16_t levels[4] );

/* Inverse quantisation: reconstruct transform coefficients from levels. */
void x264pp_dequant4( const x264pp_quant_t *q, const int16_t levels[16],
                      int16_t coef[16] );
void x264pp_dequant2( const x264pp_quant_t *q, const int16_t levels[4],
                      int16_t coef[4] );

/* Map luma QP to chroma QP (H.264 clause 8.5.9, Table 8-15 style mapping). */
int  x264pp_chroma_qp( int luma_qp, int chroma_qp_offset );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_QUANT_H */
