/*
 * 8-bit I420 frame planes with padding.  Original code for x264++ (I1).
 */

#include "core/frame.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Optional AVX2 fast path for frame loading.
 *
 * Loading a picture copies one strided row at a time and then replicates the
 * border of every plane; both happen for each plane of every frame, so they
 * are worth vectorising.  The project builds with plain `gcc -O2` and we may
 * not change the Makefile, so the AVX2 helpers below are compiled with GCC's
 * per-function `target("avx2")` attribute and are only reached after a
 * runtime CPUID check.  A compiler/target without that attribute keeps the
 * original scalar code, so this file stays portable.
 *
 * Byte identity is preserved deliberately:
 *   - the row copy transfers exactly `w` bytes (tail done with memcpy);
 *   - the edge fill writes only the `padx` padding bytes on each side, with
 *     the same value the scalar loop would store there;
 *   - the vertical pass copies exactly `pw + 2*padx` bytes from the already
 *     padded source row, in the same order (horizontal first, then vertical).
 * No padding byte outside those ranges is ever read or written.
 * ---------------------------------------------------------------------- */

#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
#define X264PP_FRAME_HAVE_AVX2_TARGET 1
#include <immintrin.h>
#else
#define X264PP_FRAME_HAVE_AVX2_TARGET 0
#endif

#define PAD_Y 8
#define PAD_X 16

static int align16( int x )
{
    return (x + 15) & ~15;
}

static int frame_avx2_available( void )
{
#if X264PP_FRAME_HAVE_AVX2_TARGET
    static int cached = -1;
    if( cached < 0 )
    {
        __builtin_cpu_init();
        cached = __builtin_cpu_supports( "avx2" ) ? 1 : 0;
    }
    return cached;
#else
    return 0;
#endif
}

/* --- Scalar reference path ------------------------------------------------ */

static void copy_rows_scalar( uint8_t *dst, int ds, const uint8_t *src,
                              int ss, int w, int h )
{
    int y;
    for( y = 0; y < h; y++ )
        memcpy( dst + (size_t)y * ds, src + (size_t)y * ss, (size_t)w );
}

/* Replicate the border of the coded area into the padding of one plane. */
static void pad_plane_scalar( uint8_t *plane, int stride, int w, int h, int shift )
{
    int x, y;
    int pw = w >> shift;
    int ph = h >> shift;
    int padx = PAD_X >> shift;
    int pady = PAD_Y >> shift;

    for( y = 0; y < ph; y++ )
    {
        uint8_t *row = plane + (size_t)y * stride;
        for( x = 1; x <= padx; x++ )
        {
            row[-x] = row[0];
            row[pw - 1 + x] = row[pw - 1];
        }
    }
    for( y = 1; y <= pady; y++ )
    {
        uint8_t *dst = plane - (size_t)y * stride;
        uint8_t *src = plane;
        uint8_t *dst2 = plane + (size_t)(ph - 1 + y) * stride;
        uint8_t *src2 = plane + (size_t)(ph - 1) * stride;
        memcpy( dst, src, (size_t)(pw + 2 * padx) );
        memcpy( dst2, src2, (size_t)(pw + 2 * padx) );
    }
}

/* --- AVX2 path (compiled per-function, dispatched at runtime) ------------- */

#if X264PP_FRAME_HAVE_AVX2_TARGET

/* Copy exactly n bytes with 32-byte vector moves plus a byte-exact tail. */
__attribute__((target("avx2")))
static void copy_bytes_avx2( uint8_t *dst, const uint8_t *src, int n )
{
    int i = 0;
    for( ; i + 32 <= n; i += 32 )
    {
        __m256i v = _mm256_loadu_si256( (const __m256i *)(src + i) );
        _mm256_storeu_si256( (__m256i *)(dst + i), v );
    }
    if( i < n )
        memcpy( dst + i, src + i, (size_t)(n - i) );
}

/* Fill exactly n bytes with one byte value.  Only called with the small
 * constant padx (16 luma / 8 chroma), but written generically. */
__attribute__((target("avx2")))
static void fill_bytes_avx2( uint8_t *dst, int value, int n )
{
    __m256i v = _mm256_set1_epi8( (char)(unsigned char)value );
    __m128i lo = _mm256_castsi256_si128( v );
    int i = 0;
    for( ; i + 32 <= n; i += 32 )
        _mm256_storeu_si256( (__m256i *)(dst + i), v );
    if( n - i >= 16 )
    {
        _mm_storeu_si128( (__m128i *)(dst + i), lo );
        i += 16;
    }
    if( n - i >= 8 )
    {
        _mm_storel_epi64( (__m128i *)(dst + i), lo );
        i += 8;
    }
    if( n - i >= 4 )
    {
        uint32_t w = 0x01010101u * (unsigned int)(unsigned char)value;
        memcpy( dst + i, &w, 4 );
        i += 4;
    }
    for( ; i < n; i++ )
        dst[i] = (uint8_t)value;
}

__attribute__((target("avx2")))
static void copy_rows_avx2( uint8_t *dst, int ds, const uint8_t *src,
                            int ss, int w, int h )
{
    int y;
    for( y = 0; y < h; y++ )
        copy_bytes_avx2( dst + (size_t)y * ds, src + (size_t)y * ss, w );
}

__attribute__((target("avx2")))
static void pad_plane_avx2( uint8_t *plane, int stride, int w, int h, int shift )
{
    int y;
    int pw = w >> shift;
    int ph = h >> shift;
    int padx = PAD_X >> shift;
    int pady = PAD_Y >> shift;

    for( y = 0; y < ph; y++ )
    {
        uint8_t *row = plane + (size_t)y * stride;
        /* Same bytes as `row[-x] = row[0]` / `row[pw-1+x] = row[pw-1]`. */
        fill_bytes_avx2( row - padx, row[0], padx );
        fill_bytes_avx2( row + pw, row[pw - 1], padx );
    }
    for( y = 1; y <= pady; y++ )
    {
        uint8_t *dst  = plane - (size_t)y * stride;
        uint8_t *src  = plane;
        uint8_t *dst2 = plane + (size_t)(ph - 1 + y) * stride;
        uint8_t *src2 = plane + (size_t)(ph - 1) * stride;
        copy_bytes_avx2( dst, src, pw + 2 * padx );
        copy_bytes_avx2( dst2, src2, pw + 2 * padx );
    }
}

#endif /* X264PP_FRAME_HAVE_AVX2_TARGET */

/* --- Public API ----------------------------------------------------------- */

int x264pp_frame_alloc( x264pp_frame_t *f, int width, int height )
{
    int stride_y, stride_c, size_y, size_c, total;
    uint8_t *ptr;

    if( !f || width <= 0 || height <= 0 )
        return X264PP_ERR_INVALID;

    memset( f, 0, sizeof( *f ) );
    f->width  = align16( width );
    f->height = align16( height );

    stride_y = align16( f->width + 2 * PAD_X );
    stride_c = align16( f->width / 2 + 2 * PAD_X );
    f->stride[0] = stride_y;
    f->stride[1] = stride_c;
    f->stride[2] = stride_c;

    size_y = stride_y * (f->height + 2 * PAD_Y);
    size_c = stride_c * (f->height / 2 + 2 * PAD_Y);
    total  = size_y + 2 * size_c;
    f->base = (uint8_t *)malloc( (size_t)total + 32 );
    if( !f->base )
        return X264PP_ERR_INVALID;
    memset( f->base, 0, (size_t)total + 32 );

    ptr = f->base;
    f->plane[0] = ptr + PAD_Y * stride_y + PAD_X;
    ptr += size_y;
    f->plane[1] = ptr + PAD_Y * stride_c + PAD_X;
    ptr += size_c;
    f->plane[2] = ptr + PAD_Y * stride_c + PAD_X;
    return X264PP_OK;
}

void x264pp_frame_free( x264pp_frame_t *f )
{
    if( !f )
        return;
    free( f->base );
    memset( f, 0, sizeof( *f ) );
}

void x264pp_frame_fill( x264pp_frame_t *f, int value )
{
    int p;
    uint8_t v = (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
    if( !f || !f->base )
        return;
    for( p = 0; p < 3; p++ )
    {
        int h = (p == 0) ? f->height : f->height / 2;
        int row;
        uint8_t *base = f->plane[p] - PAD_Y * f->stride[p] - PAD_X;
        for( row = 0; row < h + 2 * PAD_Y; row++ )
            memset( base + (size_t)row * f->stride[p], v,
                    (size_t)f->stride[p] );
    }
}

int x264pp_frame_load( x264pp_frame_t *f, const x264pp_picture_t *pic )
{
    int p;
#if X264PP_FRAME_HAVE_AVX2_TARGET
    int avx2;
#endif
    if( !f || !f->base || !pic )
        return X264PP_ERR_INVALID;
    if( pic->csp != X264PP_CSP_I420 && pic->csp != X264PP_CSP_YV12 )
        return X264PP_ERR_INVALID;
    if( pic->width <= 0 || pic->height <= 0 ||
        pic->width > f->width || pic->height > f->height )
        return X264PP_ERR_INVALID;

#if X264PP_FRAME_HAVE_AVX2_TARGET
    avx2 = frame_avx2_available();
#endif

    for( p = 0; p < 3; p++ )
    {
        int shift = (p == 0) ? 0 : 1;
        int w = pic->width >> shift;
        int h = pic->height >> shift;
#if X264PP_FRAME_HAVE_AVX2_TARGET
        if( avx2 )
        {
            copy_rows_avx2( f->plane[p], f->stride[p],
                            pic->plane[p], pic->stride[p], w, h );
            pad_plane_avx2( f->plane[p], f->stride[p],
                            f->width, f->height, shift );
            continue;
        }
#endif
        copy_rows_scalar( f->plane[p], f->stride[p],
                          pic->plane[p], pic->stride[p], w, h );
        pad_plane_scalar( f->plane[p], f->stride[p],
                          f->width, f->height, shift );
    }
    return X264PP_OK;
}
