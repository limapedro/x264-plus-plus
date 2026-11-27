#ifndef X264PP_CORE_CAVLC_H
#define X264PP_CORE_CAVLC_H

/*
 * CAVLC residual syntax for I1 (4x4 and 2x2 chroma-DC blocks).
 * Original code for x264++ derived from spec/h264-syntax.md section 7.1,
 * docs/rebuild/encoder/cavlc.c.md and the published H.264 clause 9.2 tables.
 */

#include "core/bs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x264pp_vlc
{
    uint32_t code;
    uint8_t  len;
} x264pp_vlc_t;

/* Write one residual block in zigzag order.  `levels` holds `n` levels,
 * with zeros already interleaved; `nC` is the CAVLC neighbour context
 * (0..16 for 4x4 blocks, -1 for a 2x2 chroma DC block, -2 for a 2x4
 * chroma DC block). */
void x264pp_cavlc_residual( x264pp_bs_t *bs, const int16_t *levels,
                            int n, int nC );

/* H.264 Table 9-4 mapping from (cbp_chroma, cbp_luma) to codeNum for I
 * slices with 4:2:0 chroma.  `cbp_luma` is the 4-bit mask. */
int x264pp_cavlc_cbp_code( int cbp_chroma, int cbp_luma );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_CAVLC_H */
