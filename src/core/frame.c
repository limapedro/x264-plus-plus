/*
 * 8-bit I420 frame planes with padding.  Original code for x264++ (I1).
 */

#include "core/frame.h"

#include <stdlib.h>
#include <string.h>

#define PAD_Y 8
#define PAD_X 16

static int align16( int x )
{
    return (x + 15) & ~15;
}

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

/* Replicate the border of the coded area into the padding of one plane. */
static void pad_plane( uint8_t *plane, int stride, int w, int h, int shift )
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

int x264pp_frame_load( x264pp_frame_t *f, const x264pp_picture_t *pic )
{
    int p;
    if( !f || !f->base || !pic )
        return X264PP_ERR_INVALID;
    if( pic->csp != X264PP_CSP_I420 && pic->csp != X264PP_CSP_YV12 )
        return X264PP_ERR_INVALID;
    if( pic->width <= 0 || pic->height <= 0 ||
        pic->width > f->width || pic->height > f->height )
        return X264PP_ERR_INVALID;

    for( p = 0; p < 3; p++ )
    {
        int shift = (p == 0) ? 0 : 1;
        int w = pic->width >> shift;
        int h = pic->height >> shift;
        int y;
        for( y = 0; y < h; y++ )
            memcpy( f->plane[p] + (size_t)y * f->stride[p],
                    pic->plane[p] + (size_t)y * pic->stride[p],
                    (size_t)w );
        pad_plane( f->plane[p], f->stride[p], f->width, f->height, shift );
    }
    return X264PP_OK;
}
