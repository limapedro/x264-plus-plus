/*
 * Scalar quantisation with a configurable dead zone.  Original I1 code.
 */

#include "core/quant.h"

#include <stdlib.h>

static const int level_scale6[6] = { 10, 11, 13, 14, 16, 18 };

void x264pp_quant_init( x264pp_quant_t *q, int qp, int deadzone )
{
    q->qp = qp < 0 ? 0 : (qp > 51 ? 51 : qp);
    q->q6 = q->qp % 6;
    q->level_scale = level_scale6[q->q6];
    q->deadzone = deadzone < 0 ? 0 : (deadzone > 32 ? 32 : deadzone);
}

/* level = round( factor * |coef| * qstep_den / (levelScale<<shift) ), with a
 * dead-zone bias that shrinks as `deadzone` grows.  Using int64 keeps the
 * arithmetic well defined for every QP including the q/6 == 4 boundary. */
static int quant_one( const x264pp_quant_t *q, int coef, int factor )
{
    int shift = q->qp / 6 - 4;
    int64_t num, den, bias, val;
    int m = coef < 0 ? -coef : coef;
    int sign = coef < 0 ? -1 : 1;

    if( m == 0 )
        return 0;
    if( shift >= 0 )
    {
        num = (int64_t)q->level_scale << shift;
        den = 1;
    }
    else
    {
        num = q->level_scale;
        den = (int64_t)1 << ( -shift );
    }
    /* H.264 8.5.13: levelScale[q%6] << (q/6), with the 4x4 forward transform
     * scaled so that AC coefficients use the /64 IDCT normalisation and the
     * Hadamard DC path uses /2.  The decoder's dequantisation multiplies by
     * levelScale << (q/6) and stores with >>6, an effective gain of 16 times
     * the plain levelScale<<(q/6-4) value; match that gain here. */
    num *= 4;                        /* forward: divide by 4 extra */
    /* Half-unit bias for round-to-nearest; the dead zone narrows it. */
    bias = num / 2;
    bias -= ( bias * q->deadzone ) / 64;
    if( bias < 0 )
        bias = 0;

    val = ( (int64_t)factor * m * den + bias ) / num;
    if( val > 32767 )
        val = 32767;
    return sign * (int)val;
}

int x264pp_quant4( const x264pp_quant_t *q, const int16_t in[16],
                   int16_t levels[16], int factor )
{
    int i, nz = 0;
    for( i = 0; i < 16; i++ )
    {
        levels[i] = (int16_t)quant_one( q, in[i], factor );
        nz |= levels[i] != 0;
    }
    return nz;
}

int x264pp_quant2( const x264pp_quant_t *q, const int16_t in[4],
                   int16_t levels[4] )
{
    int i, nz = 0;
    for( i = 0; i < 4; i++ )
    {
        levels[i] = (int16_t)quant_one( q, in[i], 2 );
        nz |= levels[i] != 0;
    }
    return nz;
}

/* W' = level * levelScale << (qp/6 - 4), with round-to-nearest when the
 * shift is negative.  This is the decoder's dequantisation rule (8.5.12). */
static int dequant_one( const x264pp_quant_t *q, int level )
{
    int shift = q->qp / 6 - 4;
    /* Match the decoder's dequant4_coeff (levelScale * 16 << (q/6+2), then
     * >>6 on store), i.e. an effective 16x gain over plain levelScale. */
    int64_t v = (int64_t)level * q->level_scale * 16;
    if( shift >= 0 )
        v <<= shift;
    else
    {
        int s = -shift;
        v = (v + ((int64_t)1 << (s - 1))) >> s;
    }
    if( v > 32767 ) v = 32767;
    if( v < -32768 ) v = -32768;
    return (int)v;
}

void x264pp_dequant4( const x264pp_quant_t *q, const int16_t levels[16],
                      int16_t coef[16] )
{
    int i;
    for( i = 0; i < 16; i++ )
        coef[i] = (int16_t)dequant_one( q, levels[i] );
}

void x264pp_dequant2( const x264pp_quant_t *q, const int16_t levels[4],
                      int16_t coef[4] )
{
    int i;
    for( i = 0; i < 4; i++ )
        coef[i] = (int16_t)dequant_one( q, levels[i] );
}

int x264pp_chroma_qp( int luma_qp, int chroma_qp_offset )
{
    static const int table[52] = {
         0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,
        16,17,18,19,20,21,22,23,24,25,26,27,28,29,
        29,30,31,31,32,32,33,33,34,34,35,35,36,36,
        37,37,38,38,39,39,40,40
    };
    int qp = luma_qp + chroma_qp_offset;
    if( qp < 0 ) qp = 0;
    if( qp > 51 ) qp = 51;
    return table[qp];
}
