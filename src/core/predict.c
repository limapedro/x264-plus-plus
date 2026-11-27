/*
 * Intra predictors.  Original I1 code written from the published H.264
 * intra prediction equations as summarised in docs/rebuild/common/predict.c.md.
 */

#include "core/predict.h"

#define CLIP(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))
#define F1(a,b) (((a) + (b) + 1) >> 1)
#define F2(a,b,c) (((a) + 2 * (b) + (c) + 2) >> 2)

int x264pp_pred_fix4( int mode, unsigned avail )
{
    if( mode < 0 || mode > 8 )
        return 2;
    switch( mode )
    {
    case 0: return (avail & X264PP_NEIGH_TOP) ? 0 : 2;    /* V */
    case 1: return (avail & X264PP_NEIGH_LEFT) ? 1 : 2;   /* H */
    case 3: case 7:                                      /* DDL, VL */
        return (avail & X264PP_NEIGH_TOP) ? mode : 2;
    case 4: case 5: case 6:                              /* DDR, VR, HD */
        return ((avail & X264PP_NEIGH_LEFT) &&
                (avail & X264PP_NEIGH_TOP)) ? mode : 2;
    case 8:                                              /* HU */
        return (avail & X264PP_NEIGH_LEFT) ? 8 : 2;
    default: return 2;
    }
}

void x264pp_pred16( uint8_t *dst, int stride, const x264pp_neighbor_t *n, int mode )
{
    int x, y;
    const uint8_t *t = n->t;
    const uint8_t *l = n->l;

    switch( mode )
    {
    case 0: /* V */
        for( y = 0; y < 16; y++ )
            for( x = 0; x < 16; x++ )
                dst[y * stride + x] = t[x];
        break;
    case 1: /* H */
        for( y = 0; y < 16; y++ )
            for( x = 0; x < 16; x++ )
                dst[y * stride + x] = l[y];
        break;
    case 2: /* DC */
    default:
    {
        int sum = 0;
        int have_t = !!(n->avail & X264PP_NEIGH_TOP);
        int have_l = !!(n->avail & X264PP_NEIGH_LEFT);
        if( have_t ) for( x = 0; x < 16; x++ ) sum += t[x];
        if( have_l ) for( y = 0; y < 16; y++ ) sum += l[y];
        if( have_t && have_l ) sum = (sum + 16) >> 5;
        else if( have_t || have_l ) sum = (sum + 8) >> 4;
        else sum = 128;
        for( y = 0; y < 16; y++ )
            for( x = 0; x < 16; x++ )
                dst[y * stride + x] = (uint8_t)sum;
        break;
    }
    case 3: /* plane */
    {
        int h = 0, v = 0, a, b, c, i;
        for( i = 0; i < 8; i++ )
        {
            h += (i + 1) * ((int)t[8 + i] - (int)t[6 - i]);
            v += (i + 1) * ((int)l[8 + i] - (int)l[6 - i]);
        }
        a = 16 * ((int)l[15] + (int)t[15]);
        b = (5 * h + 32) >> 6;
        c = (5 * v + 32) >> 6;
        for( y = 0; y < 16; y++ )
            for( x = 0; x < 16; x++ )
                dst[y * stride + x] =
                    (uint8_t)CLIP((a - 7 * b - 7 * c + 16 + x * b + y * c) >> 5);
        break;
    }
    }
}

void x264pp_pred4( uint8_t *dst, int stride, const x264pp_neighbor_t *n, int mode )
{
    int x, y;
    const uint8_t *t = n->t;
    const uint8_t *l = n->l;
    int lt = n->tl;

    switch( mode )
    {
    case 0: /* V */
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
                dst[y * stride + x] = t[x];
        break;
    case 1: /* H */
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
                dst[y * stride + x] = l[y];
        break;
    case 2: /* DC */
    default:
    {
        int sum = 0, nt = 0, nl = 0;
        if( n->avail & X264PP_NEIGH_TOP )
            for( x = 0; x < 4; x++ ) { sum += t[x]; nt++; }
        if( n->avail & X264PP_NEIGH_LEFT )
            for( y = 0; y < 4; y++ ) { sum += l[y]; nl++; }
        if( nt + nl == 0 ) sum = 128;
        else sum = (sum + ((nt + nl) >> 1)) / (nt + nl);
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
                dst[y * stride + x] = (uint8_t)sum;
        break;
    }
    case 3: /* diagonal down-left */
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
            {
                int s = x + y;
                int a = t[s < 7 ? s : 7];
                int b = t[s + 1 < 7 ? s + 1 : 7];
                int c = t[s + 2 < 7 ? s + 2 : 7];
                dst[y * stride + x] = (uint8_t)F2(a, b, c);
            }
        break;
    case 4: /* diagonal down-right */
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
            {
                int s = x - y;
                int val;
                if( s > 0 )
                    val = F2(t[s + 1 < 7 ? s + 1 : 7], t[s], t[s - 1]);
                else if( s == 0 )
                    val = F2(t[1], t[0], lt);
                else
                {
                    int k = -s;
                    val = F2(l[k + 1 < 7 ? k + 1 : 7], l[k < 7 ? k : 7],
                             l[k - 1 < 0 ? 0 : k - 1]);
                }
                dst[y * stride + x] = (uint8_t)val;
            }
        break;
    case 5: /* vertical-right */
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
            {
                int s = x - 2 * y;
                int val;
                if( s >= 0 )
                    val = F2(t[s + 1 < 7 ? s + 1 : 7], t[s < 7 ? s : 7],
                             t[s - 1 < 0 ? 0 : s - 1]);
                else
                {
                    int k = -s;
                    val = F2(l[k + 1 < 7 ? k + 1 : 7], l[k < 7 ? k : 7],
                             l[k - 1 < 0 ? 0 : k - 1]);
                }
                dst[y * stride + x] = (uint8_t)val;
            }
        break;
    case 6: /* horizontal-down */
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
            {
                int s = y - 2 * x;
                int val;
                if( s >= 0 )
                    val = F2(l[s + 1 < 7 ? s + 1 : 7], l[s < 7 ? s : 7],
                             l[s - 1 < 0 ? 0 : s - 1]);
                else
                {
                    int k = -s;
                    val = F2(t[k + 1 < 7 ? k + 1 : 7], t[k < 7 ? k : 7],
                             t[k - 1 < 0 ? 0 : k - 1]);
                }
                dst[y * stride + x] = (uint8_t)val;
            }
        break;
    case 7: /* vertical-left */
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
            {
                int s = x + 2 * y;
                dst[y * stride + x] = (uint8_t)
                    F2(t[s < 7 ? s : 7], t[s + 1 < 7 ? s + 1 : 7],
                       t[s + 2 < 7 ? s + 2 : 7]);
            }
        break;
    case 8: /* horizontal-up */
        for( y = 0; y < 4; y++ )
            for( x = 0; x < 4; x++ )
            {
                int s = y - (x >> 1) - 1;
                if( s < 0 ) s = 0;
                if( s > 6 ) s = 6;
                dst[y * stride + x] = (uint8_t)F2(l[s], l[s < 6 ? s + 1 : 6],
                                                  l[s < 5 ? s + 2 : 6]);
            }
        break;
    }
}

void x264pp_pred8c( uint8_t *dst, int stride, const x264pp_neighbor_t *n, int mode )
{
    int x, y;
    const uint8_t *t = n->t;
    const uint8_t *l = n->l;

    switch( mode )
    {
    case 0: /* V */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
                dst[y * stride + x] = t[x];
        break;
    case 1: /* H */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
                dst[y * stride + x] = l[y];
        break;
    case 2: /* DC with four quadrants */
    default:
    {
        int s0 = 0, s1 = 0, s2 = 0, s3 = 0, i;
        int have_t = !!(n->avail & X264PP_NEIGH_TOP);
        int have_l = !!(n->avail & X264PP_NEIGH_LEFT);
        if( have_t )
        {
            for( i = 0; i < 4; i++ ) s0 += t[i];
            for( i = 4; i < 8; i++ ) s1 += t[i];
        }
        if( have_l )
        {
            for( i = 0; i < 4; i++ ) s2 += l[i];
            for( i = 4; i < 8; i++ ) s3 += l[i];
        }
        {
            int dc00, dc01, dc10, dc11;
            if( !have_t && !have_l )
                dc00 = dc01 = dc10 = dc11 = 128;
            else if( !have_t )
            {
                dc00 = dc10 = (s2 + 2) >> 2;  /* left half only */
                dc01 = dc11 = (s3 + 2) >> 2;
            }
            else if( !have_l )
            {
                dc00 = dc01 = (s0 + 2) >> 2;  /* top half only */
                dc10 = dc11 = (s1 + 2) >> 2;
            }
            else
            {
                dc00 = (s0 + s2 + 4) >> 3;
                dc01 = (s1 + 2) >> 2;
                dc10 = (s3 + 2) >> 2;
                dc11 = (s1 + s3 + 4) >> 3;
            }
            for( y = 0; y < 4; y++ )
                for( x = 0; x < 4; x++ )
                    dst[y * stride + x] = (uint8_t)dc00;
            for( y = 0; y < 4; y++ )
                for( x = 4; x < 8; x++ )
                    dst[y * stride + x] = (uint8_t)dc01;
            for( y = 4; y < 8; y++ )
                for( x = 0; x < 4; x++ )
                    dst[y * stride + x] = (uint8_t)dc10;
            for( y = 4; y < 8; y++ )
                for( x = 4; x < 8; x++ )
                    dst[y * stride + x] = (uint8_t)dc11;
        }
        break;
    }
    case 3: /* plane */
    {
        int h = 0, v = 0, i, a, b, c;
        for( i = 0; i < 4; i++ )
        {
            h += (i + 1) * ((int)t[4 + i] - (int)t[2 - i]);
            v += (i + 1) * ((int)l[4 + i] - (int)l[2 - i]);
        }
        a = 16 * ((int)l[7] + (int)t[7]);
        b = (17 * h + 16) >> 5;
        c = (17 * v + 16) >> 5;
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
                dst[y * stride + x] =
                    (uint8_t)CLIP((a - 3 * b - 3 * c + 16 + x * b + y * c) >> 5);
        break;
    }
    }
}

void x264pp_pred8_edge( const x264pp_neighbor_t *n, uint8_t edge[36],
                        unsigned filters )
{
    int i;
    for( i = 0; i < 36; i++ )
        edge[i] = 128;
    for( i = 0; i < 8; i++ )
        edge[7 - i] = n->l[i];        /* left, bottom-to-top */
    edge[15] = n->tl;
    for( i = 0; i < 16; i++ )
        edge[16 + i] = n->t[i];
    edge[32] = n->t[15];

    if( filters & X264PP_NEIGH_LEFT )
    {
        int l[8];
        for( i = 0; i < 8; i++ ) l[i] = edge[7 - i];
        for( i = 0; i < 8; i++ )
        {
            int above = i == 0 ? l[0] : ((int)l[i - 1] + l[i] + 1) >> 1;
            int below = i == 7 ? l[7] : ((int)l[i] + l[i + 1] + 1) >> 1;
            edge[7 - i] = (uint8_t)((above + 2 * l[i] + below + 2) >> 2);
        }
    }
    if( filters & X264PP_NEIGH_TOP )
    {
        int t[16];
        for( i = 0; i < 16; i++ ) t[i] = edge[16 + i];
        for( i = 0; i < 16; i++ )
        {
            int left  = i == 0 ? (int)n->tl : t[i - 1];
            int right = i == 15 ? t[15] : t[i + 1];
            edge[16 + i] = (uint8_t)((left + 2 * t[i] + right + 2) >> 2);
        }
        edge[32] = edge[31];
    }
}

void x264pp_pred8( uint8_t *dst, int stride, const uint8_t edge[36], int mode )
{
    int x, y;
    const uint8_t *t = edge + 16;
    const uint8_t *l = edge + 7; /* l[i] = edge[7-i] */

    if( mode < 0 || mode > 8 )
        mode = 2;
    switch( mode )
    {
    case 0: /* V */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
                dst[y * stride + x] = t[x];
        break;
    case 1: /* H */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
                dst[y * stride + x] = l[-y];
        break;
    case 2: /* DC */
    default:
    {
        int sum = 0;
        for( x = 0; x < 8; x++ ) sum += l[-x];
        for( y = 0; y < 8; y++ ) sum += t[y];
        sum = (sum + 8) >> 4;
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
                dst[y * stride + x] = (uint8_t)sum;
        break;
    }
    case 3: /* DDL */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
            {
                int s = x + y;
                dst[y * stride + x] = (uint8_t)F2(t[s < 15 ? s : 15],
                                                  t[s + 1 < 16 ? s + 1 : 16],
                                                  t[s + 2 < 16 ? s + 2 : 16]);
            }
        break;
    case 7: /* VL */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
            {
                int s = x + 2 * y;
                dst[y * stride + x] = (uint8_t)F2(t[s < 16 ? s : 16],
                                                  t[s + 1 < 16 ? s + 1 : 16],
                                                  t[s + 2 < 16 ? s + 2 : 16]);
            }
        break;
    case 4: /* DDR */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
            {
                int s = x - y;
                int k = s < 0 ? -s : s;
                if( k > 7 ) k = 7;
                dst[y * stride + x] = (uint8_t)
                    (s >= 0 ? F2(t[k < 16 ? k + 1 : 16], t[k], t[k ? k - 1 : 0])
                            : F2(l[-k], l[-(k < 7 ? k + 1 : 7)],
                                 l[-(k < 6 ? k + 2 : 7)]));
            }
        break;
    case 5: /* VR */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
            {
                int s = x - 2 * y;
                int k = s < 0 ? -s : s;
                if( k > 7 ) k = 7;
                dst[y * stride + x] = (uint8_t)
                    (s >= 0 ? F2(t[k < 16 ? k + 1 : 16], t[k], t[k ? k - 1 : 0])
                            : F2(l[-k], l[-(k < 7 ? k + 1 : 7)],
                                 l[-(k < 6 ? k + 2 : 7)]));
            }
        break;
    case 6: /* HD */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
            {
                int s = y - 2 * x;
                int k = s < 0 ? -s : s;
                if( k > 7 ) k = 7;
                dst[y * stride + x] = (uint8_t)
                    (s >= 0 ? F2(l[-k], l[-(k < 7 ? k + 1 : 7)],
                                 l[-(k < 6 ? k + 2 : 7)])
                            : F2(t[k < 16 ? k + 1 : 16], t[k], t[k ? k - 1 : 0]));
            }
        break;
    case 8: /* HU */
        for( y = 0; y < 8; y++ )
            for( x = 0; x < 8; x++ )
            {
                int k = y - (x >> 1) - 1;
                if( k < 0 ) k = 0;
                if( k > 6 ) k = 6;
                dst[y * stride + x] = (uint8_t)
                    F2(l[-k], l[-(k < 7 ? k + 1 : 7)], l[-(k < 6 ? k + 2 : 7)]);
            }
        break;
    }
}
