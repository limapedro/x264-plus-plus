/*
 * Integer transforms and scans.  Original I1 code; all constants come from
 * H.264 clause 8.5 as described in docs/rebuild/common/dct.c.md.
 */

#include "core/dct.h"
#include <immintrin.h>

const uint8_t x264pp_zigzag4[16] = {
     0,  1,  4,  8,  5,  2,  3,  6,
     9, 12, 13, 10,  7, 11, 14, 15
};
/* (row,col) -> raster for the 4x4 scan:
 * (0,0)(0,1)(1,0)(2,0)(1,1)(0,2)(0,3)(1,2)(2,1)(3,0)(3,1)(2,2)(1,3)(2,3)(3,2)(3,3)
 * raster index = row*4 + col:
 * 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
 */

const uint8_t x264pp_zigzag2[4] = { 0, 1, 2, 3 };

static int clip_pixel( int v )
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* One forward 1-D pass, H.264 clause 8.5.1. */
static void fwd1( const int16_t *in, int16_t *out )
{
    int p = in[0] + in[3];
    int q = in[1] + in[2];
    int r = in[0] - in[3];
    int s = in[1] - in[2];
    out[0] = (int16_t)(p + q);
    out[1] = (int16_t)(2 * r + s);
    out[2] = (int16_t)(p - q);
    out[3] = (int16_t)(r - 2 * s);
}

void x264pp_dct4( const int16_t res[16], int16_t coef[16] )
{
    int16_t tmp[16];
    int i;
    for( i = 0; i < 4; i++ )
        fwd1( res + 4 * i, tmp + 4 * i );
    for( i = 0; i < 4; i++ )
    {
        int16_t col[4];
        col[0] = tmp[i];
        col[1] = tmp[4 + i];
        col[2] = tmp[8 + i];
        col[3] = tmp[12 + i];
        fwd1( col, col );
        coef[i]      = col[0];
        coef[4 + i]  = col[1];
        coef[8 + i]  = col[2];
        coef[12 + i] = col[3];
    }
}

/* One inverse 1-D pass (without the final scaling).  H.264 clause 8.5.2. */
static void inv1( int a, int b, int c, int d, int out[4] )
{
    int u = a + c;
    int v = a - c;
    int w = b + (d >> 1);
    int z = (b >> 1) - d;
    out[0] = u + w;
    out[1] = v + z;
    out[2] = v - z;
    out[3] = u - w;
}

/* AVX2 implementation of the scalar IDCT + add + clip.  The arithmetic is
 * identical to inv1() followed by ((t + 32) >> 6) and clip_pixel(). */
__attribute__((target("avx2")))
static void idct4_add_avx2( uint8_t *dst, int stride, const int16_t coef[16] )
{
    int tmp[16];
    int r, y;
    const __m128i c0 = _mm_cvtepi16_epi32( _mm_loadl_epi64( (__m128i *)( coef ) ) );
    const __m128i c1 = _mm_cvtepi16_epi32( _mm_loadl_epi64( (__m128i *)( coef + 4 ) ) );
    const __m128i c2 = _mm_cvtepi16_epi32( _mm_loadl_epi64( (__m128i *)( coef + 8 ) ) );
    const __m128i c3 = _mm_cvtepi16_epi32( _mm_loadl_epi64( (__m128i *)( coef + 12 ) ) );
    __m128i o0, o1, o2, o3;
    {
        __m128i u = _mm_add_epi32( c0, c2 );
        __m128i v = _mm_sub_epi32( c0, c2 );
        __m128i w = _mm_add_epi32( c1, _mm_srai_epi32( c3, 1 ) );
        __m128i z = _mm_sub_epi32( _mm_srai_epi32( c1, 1 ), c3 );
        o0 = _mm_add_epi32( u, w );
        o1 = _mm_add_epi32( v, z );
        o2 = _mm_sub_epi32( v, z );
        o3 = _mm_sub_epi32( u, w );
    }
    _mm_storeu_si128( (__m128i *)( tmp ),      o0 );
    _mm_storeu_si128( (__m128i *)( tmp + 4 ),  o1 );
    _mm_storeu_si128( (__m128i *)( tmp + 8 ),  o2 );
    _mm_storeu_si128( (__m128i *)( tmp + 12 ), o3 );
    for( y = 0; y < 4; y++ )
    {
        int t[4];
        inv1( tmp[4 * y], tmp[4 * y + 1], tmp[4 * y + 2], tmp[4 * y + 3], t );
        for( r = 0; r < 4; r++ )
        {
            int v = dst[r * stride + y] + ((t[r] + 32) >> 6);
            dst[r * stride + y] = (uint8_t)( v < 0 ? 0 : (v > 255 ? 255 : v) );
        }
    }
}

void x264pp_idct4_add( uint8_t *dst, int stride, const int16_t coef[16] )
{
    if( __builtin_cpu_supports( "avx2" ) )
    {
        idct4_add_avx2( dst, stride, coef );
        return;
    }
    {
    int16_t tmp[16];
    int x, y;
    for( x = 0; x < 4; x++ )
    {
        int t[4];
        inv1( coef[x], coef[4 + x], coef[8 + x], coef[12 + x], t );
        tmp[x]      = (int16_t)t[0];
        tmp[4 + x]  = (int16_t)t[1];
        tmp[8 + x]  = (int16_t)t[2];
        tmp[12 + x] = (int16_t)t[3];
    }
    for( y = 0; y < 4; y++ )
    {
        int t[4];
        inv1( tmp[4 * y], tmp[4 * y + 1], tmp[4 * y + 2], tmp[4 * y + 3], t );
        dst[0 * stride + y] = (uint8_t)clip_pixel( dst[0 * stride + y] + ((t[0] + 32) >> 6) );
        dst[1 * stride + y] = (uint8_t)clip_pixel( dst[1 * stride + y] + ((t[1] + 32) >> 6) );
        dst[2 * stride + y] = (uint8_t)clip_pixel( dst[2 * stride + y] + ((t[2] + 32) >> 6) );
        dst[3 * stride + y] = (uint8_t)clip_pixel( dst[3 * stride + y] + ((t[3] + 32) >> 6) );
    }
    }
}

void x264pp_hadamard2( const int16_t in[4], int16_t out[4] )
{
    int s0 = in[0] + in[1], s1 = in[2] + in[3];
    int d0 = in[0] - in[1], d1 = in[2] - in[3];
    out[0] = (int16_t)(s0 + s1);
    out[1] = (int16_t)(s0 - s1);
    out[2] = (int16_t)(d0 + d1);
    out[3] = (int16_t)(d0 - d1);
}

void x264pp_hadamard2_inv( const int16_t in[4], int16_t out[4] )
{
    int16_t t[4];
    x264pp_hadamard2( in, t );
    out[0] = (int16_t)((t[0] + 1) >> 1);
    out[1] = (int16_t)((t[1] + 1) >> 1);
    out[2] = (int16_t)((t[2] + 1) >> 1);
    out[3] = (int16_t)((t[3] + 1) >> 1);
}

static void had4_1d( const int16_t *in, int16_t *out )
{
    int a = in[0] + in[3], b = in[1] + in[2];
    int c = in[0] - in[3], d = in[1] - in[2];
    out[0] = (int16_t)(a + b);
    out[1] = (int16_t)(a - b);
    out[2] = (int16_t)(c + d);
    out[3] = (int16_t)(c - d);
}

void x264pp_hadamard4( const int16_t in[16], int16_t out[16] )
{
    int16_t tmp[16];
    int i;
    for( i = 0; i < 4; i++ )
        had4_1d( in + 4 * i, tmp + 4 * i );
    for( i = 0; i < 4; i++ )
    {
        int16_t col[4];
        col[0] = tmp[i];
        col[1] = tmp[4 + i];
        col[2] = tmp[8 + i];
        col[3] = tmp[12 + i];
        had4_1d( col, col );
        out[i]      = (int16_t)((col[0] + 1) >> 1);
        out[4 + i]  = (int16_t)((col[1] + 1) >> 1);
        out[8 + i]  = (int16_t)((col[2] + 1) >> 1);
        out[12 + i] = (int16_t)((col[3] + 1) >> 1);
    }
}

void x264pp_hadamard4_inv( const int16_t in[16], int16_t out[16] )
{
    int16_t tmp[16];
    int i;
    for( i = 0; i < 4; i++ )
        had4_1d( in + 4 * i, tmp + 4 * i );
    for( i = 0; i < 4; i++ )
    {
        int16_t col[4];
        col[0] = tmp[i];
        col[1] = tmp[4 + i];
        col[2] = tmp[8 + i];
        col[3] = tmp[12 + i];
        had4_1d( col, col );
        out[i]      = (int16_t)((col[0] + 2) >> 2);
        out[4 + i]  = (int16_t)((col[1] + 2) >> 2);
        out[8 + i]  = (int16_t)((col[2] + 2) >> 2);
        out[12 + i] = (int16_t)((col[3] + 2) >> 2);
    }
}

int x264pp_last_nonzero( const int16_t *coef, int n )
{
    int i;
    for( i = n - 1; i >= 0; i-- )
        if( coef[i] != 0 )
            return i;
    return -1;
}
