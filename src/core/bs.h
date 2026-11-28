#ifndef X264PP_CORE_BS_H
#define X264PP_CORE_BS_H

/*
 * RBSP bit writer and Annex B NAL encapsulation.
 * Original code for x264++ (increment I1), derived from
 * spec/h264-syntax.md sections 1 and 2 and docs/rebuild/common/bitstream.c.md.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct x264pp_bs
{
    uint8_t *buf;      /* RBSP destination */
    int      cap;      /* capacity in bytes */
    int      bitpos;   /* bits written so far */
    int      overflow; /* set on the first out-of-bounds write */
} x264pp_bs_t;

void x264pp_bs_init( x264pp_bs_t *bs, uint8_t *buf, int cap );

/* Write n MSB-first bits from `code` (only the low n bits are used). */
void x264pp_bs_write( x264pp_bs_t *bs, uint32_t code, int n );
void x264pp_bs_u( x264pp_bs_t *bs, unsigned value, int n );
void x264pp_bs_ue( x264pp_bs_t *bs, unsigned code_num );
void x264pp_bs_se( x264pp_bs_t *bs, int value );

/* Bulk-append whole bytes.  The writer must be byte-aligned (bitpos % 8 == 0),
 * as it is for I_PCM samples after pcm_alignment_zero_bit. */
void x264pp_bs_write_bytes( x264pp_bs_t *bs, const uint8_t *src, int n );

/* Append rbsp_stop_one_bit and zero padding to the next byte boundary.
 * Returns the number of bytes occupied by the RBSP (0 on overflow). */
int  x264pp_bs_trailing( x264pp_bs_t *bs );

/* Number of whole bytes needed for the bits written so far (no trailing). */
int  x264pp_bs_bytes( const x264pp_bs_t *bs );

/* Escape an RBSP payload into an Annex B NAL unit.
 * `ref_idc` and `type` form the NAL header; `long_start` requests a 4-byte
 * start code.  Returns the total number of bytes written to `out`, or -1 if
 * the output buffer is too small. */
int  x264pp_bs_nal( const uint8_t *rbsp, int len, int ref_idc, int type,
                    int long_start, uint8_t *out, int out_cap );

#ifdef __cplusplus
}
#endif

#endif /* X264PP_CORE_BS_H */
