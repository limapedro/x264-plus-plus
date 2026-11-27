/*
 * RBSP bit writer and Annex B NAL encapsulation.  Original I1 code.
 */

#include "core/bs.h"

#include <string.h>

void x264pp_bs_init( x264pp_bs_t *bs, uint8_t *buf, int cap )
{
    bs->buf = buf;
    bs->cap = cap;
    bs->bitpos = 0;
    bs->overflow = 0;
    if( buf && cap > 0 )
        memset( buf, 0, (size_t)cap );
}

void x264pp_bs_write( x264pp_bs_t *bs, uint32_t code, int n )
{
    int i;
    if( n <= 0 )
        return;
    for( i = n - 1; i >= 0; i-- )
    {
        int bit = (int)( (code >> i) & 1u );
        int byte = bs->bitpos >> 3;
        int offset = 7 - (bs->bitpos & 7);
        if( byte < 0 || byte >= bs->cap )
        {
            bs->overflow = 1;
            return;
        }
        if( bit )
            bs->buf[byte] |= (uint8_t)(1u << offset);
        bs->bitpos++;
    }
}

void x264pp_bs_u( x264pp_bs_t *bs, unsigned value, int n )
{
    x264pp_bs_write( bs, (uint32_t)value, n );
}

void x264pp_bs_ue( x264pp_bs_t *bs, unsigned code_num )
{
    unsigned v = code_num + 1u;
    int n = 0;
    while( v >> (n + 1) )
        n++;
    x264pp_bs_write( bs, 0, n );             /* n leading zero bits */
    x264pp_bs_write( bs, v, n + 1 );          /* terminating 1 + n suffix bits */
}

void x264pp_bs_se( x264pp_bs_t *bs, int value )
{
    unsigned code_num;
    if( value <= 0 )
        code_num = (unsigned)( -2 * (int64_t)value );
    else
        code_num = (unsigned)( 2 * (int64_t)value - 1 );
    x264pp_bs_ue( bs, code_num );
}

int x264pp_bs_bytes( const x264pp_bs_t *bs )
{
    return (bs->bitpos + 7) >> 3;
}

int x264pp_bs_trailing( x264pp_bs_t *bs )
{
    int bytes;
    x264pp_bs_write( bs, 1, 1 );             /* rbsp_stop_one_bit */
    while( bs->bitpos & 7 )
        x264pp_bs_write( bs, 0, 1 );          /* rbsp_alignment_zero_bit */
    if( bs->overflow )
        return 0;
    bytes = bs->bitpos >> 3;
    return bytes;
}

int x264pp_bs_nal( const uint8_t *rbsp, int len, int ref_idc, int type,
                   int long_start, uint8_t *out, int out_cap )
{
    int pos = 0;
    int i, zero_run = 0;

    if( !out || out_cap < 1 || len < 0 )
        return -1;

    if( long_start )
    {
        if( pos + 4 > out_cap ) return -1;
        out[pos++] = 0;
        out[pos++] = 0;
        out[pos++] = 0;
        out[pos++] = 1;
    }
    else
    {
        if( pos + 3 > out_cap ) return -1;
        out[pos++] = 0;
        out[pos++] = 0;
        out[pos++] = 1;
    }

    /* forbidden_zero_bit = 0, nal_ref_idc in bits 6..5, type in 4..0. */
    out[pos++] = (uint8_t)( (ref_idc & 3) << 5 | (type & 31) );

    for( i = 0; i < len; i++ )
    {
        uint8_t b = rbsp[i];
        if( zero_run >= 2 && b <= 3 )
        {
            if( pos + 1 > out_cap ) return -1;
            out[pos++] = 3;             /* emulation_prevention_three_byte */
            zero_run = 0;               /* the 0x03 breaks the zero run */
        }
        if( pos + 1 > out_cap ) return -1;
        out[pos++] = b;
        zero_run = (b == 0) ? zero_run + 1 : 0;
    }
    return pos;
}
