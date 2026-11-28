/*
 * Scalar quantisation with a configurable dead zone, plus an AVX2 fast path.
 * Original code for x264++; the AVX2 path is an exact vectorisation of the
 * scalar formulas and falls back to scalar on CPUs without AVX2.
 */

#include "core/quant.h"

#include <stdlib.h>
#include <immintrin.h>

static const int level_scale6[6] = { 10, 11, 13, 14, 16, 18 };

void x264pp_quant_init( x264pp_quant_t *q, int qp, int deadzone )
{
    q->qp = qp < 0 ? 0 : (qp > 51 ? 51 : qp);
    q->q6 = q->qp % 6;
    q->level_scale = level_scale6[q->q6];
    q->deadzone = deadzone < 0 ? 0 : (deadzone > 32 ? 32 : deadzone);
    {
        int s = q->qp / 6 - 4;
        if( s >= 0 ) { q->num = (int64_t)q->level_scale << s; q->den = 1; }
        else { q->num = q->level_scale; q->den = (int64_t)1 << (-s); }
        q->bias = q->num / 2;
        q->bias -= (q->bias * q->deadzone) / 64;
        if( q->bias < 0 ) q->bias = 0;
    }
}

static int quant_one( const x264pp_quant_t *q, int coef, int factor )
{
    int64_t val;
    int m = coef < 0 ? -coef : coef;
    int sign = coef < 0 ? -1 : 1;
    if( m == 0 )
        return 0;
    val = ( (int64_t)factor * m * q->den + q->bias ) / q->num;
    if( val > 32767 ) val = 32767;
    return sign * (int)val;
}

__attribute__((target("avx2")))
static void quant4_avx2( const x264pp_quant_t *q, const int16_t in[16],
                         int16_t levels[16], int factor )
{
    const int64_t mul = (int64_t)factor * q->den;
    const int64_t add = q->bias;
    const double div = (double)q->num;
    const __m256i vmul = _mm256_set1_epi32( (int)mul );
    const __m256i vadd = _mm256_set1_epi32( (int)add );
    const __m256d vdiv = _mm256_set1_pd( div );
    int tmp[16];
    int half;

    for( half = 0; half < 2; half++ )
    {
        __m256i x = _mm256_cvtepi16_epi32(
            _mm_loadu_si128( (const __m128i *)( in + half * 8 ) ) );
        __m256i s = _mm256_srai_epi32( x, 31 );
        __m256i a = _mm256_sub_epi32( _mm256_xor_si256( x, s ), s );
        __m256i p = _mm256_add_epi32( _mm256_mullo_epi32( a, vmul ), vadd );
        __m128i lo = _mm256_cvtpd_epi32( _mm256_div_pd( _mm256_cvtepi32_pd(
            _mm256_castsi256_si128( p ) ), vdiv ) );
        __m128i hi = _mm256_cvtpd_epi32( _mm256_div_pd( _mm256_cvtepi32_pd(
            _mm256_extracti128_si256( p, 1 ) ), vdiv ) );
        __m128i sl = _mm256_castsi256_si128( s );
        __m128i sh = _mm256_extracti128_si256( s, 1 );
        const __m128i zero128 = _mm_setzero_si128();
        const __m128i lim128 = _mm_set1_epi32( 32767 );
        lo = _mm_min_epi32( _mm_max_epi32( lo, zero128 ), lim128 );
        hi = _mm_min_epi32( _mm_max_epi32( hi, zero128 ), lim128 );
        lo = _mm_sub_epi32( _mm_xor_si128( lo, sl ), sl );
        hi = _mm_sub_epi32( _mm_xor_si128( hi, sh ), sh );
        _mm256_storeu_si256( (__m256i *)( tmp + half * 8 ),
                             _mm256_inserti128_si256( _mm256_castsi128_si256( lo ), hi, 1 ) );
    }
    for( half = 0; half < 16; half++ )
        levels[half] = (int16_t)tmp[half];
}

int x264pp_quant4( const x264pp_quant_t *q, const int16_t in[16],
                   int16_t levels[16], int factor )
{
    int i, nz = 0;
    if( __builtin_cpu_supports( "avx2" ) )
    {
        quant4_avx2( q, in, levels, factor );
        for( i = 0; i < 16; i++ )
            nz |= levels[i] != 0;
        return nz;
    }
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

static int dequant_one( const x264pp_quant_t *q, int level )
{
    int shift = q->qp / 6 - 4;
    int64_t v = (int64_t)level * q->level_scale;
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

