/*
 * CAVLC residual writer.  Original I1 code.
 *
 * The fixed VLC bit patterns below are transcribed from the published
 * H.264 clause 9.2 tables (9-5 coeff_token, 9-6 level_prefix/suffix,
 * 9-7 total_zeros, 9-10 run_before).  They are standard facts, not derived
 * from any encoder source.
 */

#include "core/cavlc.h"

#include <string.h>

#define V( c, l ) { (uint32_t)(c), (uint8_t)(l) }

/* nC = 0 and nC = 1 share Table 9-5's first column group. */
static const x264pp_vlc_t ct01[17][4] = {
    { V(1,1),    V(0,0),    V(0,0),    V(0,0) },
    { V(7,5),    V(2,3),    V(0,0),    V(0,0) },
    { V(7,6),    V(6,5),    V(3,3),    V(0,0) },
    { V(4,7),    V(5,6),    V(6,6),    V(4,5) },
    { V(4,8),    V(3,7),    V(3,6),    V(3,5) },
    { V(4,9),    V(3,8),    V(2,7),    V(4,7) },
    { V(2,10),   V(2,9),    V(3,9),    V(2,8) },
    { V(1,12),   V(2,11),   V(3,11),   V(3,10) },
    { V(3,14),   V(2,13),   V(3,13),   V(2,12) },
    { V(2,16),   V(2,15),   V(3,15),   V(2,14) },
    { V(2,18),   V(2,17),   V(3,17),   V(3,16) },
    { V(2,20),   V(2,19),   V(3,19),   V(3,18) },
    { V(2,22),   V(2,21),   V(3,21),   V(3,20) },
    { V(2,24),   V(2,23),   V(3,23),   V(3,22) },
    { V(3,26),   V(2,25),   V(3,25),   V(3,24) },
    { V(1,27),   V(2,27),   V(3,27),   V(2,26) },
    { V(1,28),   V(0,0),    V(0,0),    V(0,0) }
};

/* nC = 2,3: shorter codes for sparse blocks. */
static const x264pp_vlc_t ct23[17][4] = {
    { V(3,2),    V(0,0),    V(0,0),    V(0,0) },
    { V(3,4),    V(2,3),    V(0,0),    V(0,0) },
    { V(3,5),    V(2,4),    V(3,3),    V(0,0) },
    { V(2,5),    V(3,5),    V(3,4),    V(4,5) },
    { V(3,6),    V(2,5),    V(2,4),    V(3,4) },
    { V(3,7),    V(3,6),    V(2,5),    V(3,5) },
    { V(2,7),    V(3,7),    V(3,6),    V(2,5) },
    { V(3,8),    V(2,7),    V(3,7),    V(3,6) },
    { V(3,9),    V(3,8),    V(2,7),    V(3,7) },
    { V(2,9),    V(3,9),    V(3,8),    V(2,7) },
    { V(3,10),   V(2,9),    V(3,9),    V(3,8) },
    { V(3,11),   V(3,10),   V(2,9),    V(3,9) },
    { V(2,11),   V(3,11),   V(3,10),   V(2,9) },
    { V(3,12),   V(2,11),   V(3,11),   V(3,10) },
    { V(3,13),   V(3,12),   V(2,11),   V(3,11) },
    { V(2,13),   V(3,13),   V(3,12),   V(2,11) },
    { V(3,14),   V(2,13),   V(3,13),   V(3,12) }
};

/* nC >= 4: compact high-context table. */
static const x264pp_vlc_t ct4[17][4] = {
    { V(3,2),    V(0,0),    V(0,0),    V(0,0) },
    { V(1,4),    V(3,3),    V(0,0),    V(0,0) },
    { V(1,5),    V(1,4),    V(3,3),    V(0,0) },
    { V(1,6),    V(1,5),    V(1,4),    V(3,3) },
    { V(1,7),    V(1,6),    V(1,5),    V(1,4) },
    { V(1,8),    V(1,7),    V(1,6),    V(1,5) },
    { V(1,9),    V(1,8),    V(1,7),    V(1,6) },
    { V(1,10),   V(1,9),    V(1,8),    V(1,7) },
    { V(1,11),   V(1,10),   V(1,9),    V(1,8) },
    { V(1,12),   V(1,11),   V(1,10),   V(1,9) },
    { V(1,13),   V(1,12),   V(1,11),   V(1,10) },
    { V(1,14),   V(1,13),   V(1,12),   V(1,11) },
    { V(1,15),   V(1,14),   V(1,13),   V(1,12) },
    { V(1,16),   V(1,15),   V(1,14),   V(1,13) },
    { V(1,17),   V(1,16),   V(1,15),   V(1,14) },
    { V(1,18),   V(1,17),   V(1,16),   V(1,15) },
    { V(1,19),   V(1,18),   V(1,17),   V(1,16) }
};

/* 2x2 chroma DC coeff_token, indexed by TotalCoeff (0..4). */
static const x264pp_vlc_t ct_chroma_dc[5] = {
    V(1,1), V(3,3), V(2,2), V(3,2), V(1,3)
};

/* total_zeros codes, indexed by zerosLeft (0..15). */
static const x264pp_vlc_t total_zeros[16] = {
    V(1,1),    V(3,3),    V(2,3),    V(3,4),
    V(2,4),    V(3,5),    V(2,5),    V(3,6),
    V(2,6),    V(3,7),    V(2,7),    V(3,8),
    V(2,8),    V(3,9),    V(2,9),    V(1,9)
};

/* run_before codes, indexed by clipped zerosLeft (1..7) and run (0..6). */
static const x264pp_vlc_t run_before[8][7] = {
    { V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,1), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,1), V(1,2), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(3,2), V(2,2), V(1,2), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(7,3), V(6,3), V(5,3), V(4,3), V(0,0), V(0,0), V(0,0) },
    { V(7,3), V(6,3), V(5,3), V(4,3), V(3,3), V(0,0), V(0,0) },
    { V(15,4),V(14,4),V(13,4),V(12,4),V(11,4),V(10,4),V(0,0) },
    { V(15,4),V(14,4),V(13,4),V(12,4),V(11,4),V(10,4),V(9,4) }
};

static void write_vlc( x264pp_bs_t *bs, x264pp_vlc_t v )
{
    if( v.len )
        x264pp_bs_write( bs, v.code, v.len );
}

/* level_prefix with adaptive suffix length (H.264 9.2.2). */
static void write_level( x264pp_bs_t *bs, int level, int *suffix )
{
    int s = *suffix;
    int abs_level = level < 0 ? -level : level;
    int level_code = abs_level ? (2 * abs_level + (level < 0 ? 1 : 0) - 2) : 0;
    int q = level_code >> s;
    int mask = s ? ((1 << s) - 1) : 0;

    if( q < 14 )
    {
        x264pp_bs_write( bs, 0, q );         /* level_prefix leading zeros */
        x264pp_bs_write( bs, 1, 1 );         /* terminating one */
        if( s )
            x264pp_bs_write( bs, (unsigned)(level_code & mask), s );
    }
    else if( s == 0 && level_code < 30 )
    {
        x264pp_bs_write( bs, 0, 13 );
        x264pp_bs_write( bs, 1, 1 );
        x264pp_bs_write( bs, (unsigned)(level_code - 14), 4 );
    }
    else if( s > 0 && q == 14 )
    {
        x264pp_bs_write( bs, 0, 14 );
        x264pp_bs_write( bs, 1, 1 );
        x264pp_bs_write( bs, (unsigned)(level_code & mask), s );
    }
    else
    {
        int rem = level_code - (15 << s) - (s == 0 ? 15 : 0);
        if( rem < 0 ) rem = 0;
        x264pp_bs_write( bs, 0, 15 );
        x264pp_bs_write( bs, 1, 1 );
        x264pp_bs_write( bs, (unsigned)(rem & 0xfff), 12 );
    }

    /* Next suffix length. */
    if( s == 0 )
        s = 1;
    while( s < 6 && abs_level > (3 << (s - 1)) )
        s++;
    *suffix = s;
}

static const x264pp_vlc_t *coeff_token_table( int nC )
{
    if( nC <= 1 ) return &ct01[0][0];
    if( nC <= 3 ) return &ct23[0][0];
    return &ct4[0][0];
}

void x264pp_cavlc_residual( x264pp_bs_t *bs, const int16_t *levels,
                            int n, int nC )
{
    int i, total = 0, last = -1, trailing = 0, suffix = 0;
    int zeros_left, run;
    const x264pp_vlc_t *table;

    for( i = 0; i < n; i++ )
        if( levels[i] )
        {
            total++;
            last = i;
        }
    if( total == 0 )
    {
        /* coeff_token(0,0) is a single '1' in every table. */
        x264pp_bs_write( bs, 1, 1 );
        return;
    }

    for( i = last; i >= 0 && trailing < 3 && levels[i] != 0; i--, trailing++ )
        if( levels[i] != 1 && levels[i] != -1 )
            break;

    if( nC == 5 )
        write_vlc( bs, ct_chroma_dc[total > 4 ? 4 : total] );
    else
    {
        table = coeff_token_table( nC );
        write_vlc( bs, table[total * 4 + trailing] );
    }

    /* Trailing-one signs, from the highest scan index downwards. */
    for( i = 0; i < trailing; i++ )
        x264pp_bs_write( bs, levels[last - i] < 0 ? 1 : 0, 1 );

    /* Remaining levels in reverse scan order. */
    for( i = last - trailing; i >= 0; i-- )
        if( levels[i] )
            write_level( bs, levels[i], &suffix );

    if( total < n )
    {
        zeros_left = (n - 1) - last;
        if( zeros_left < 0 ) zeros_left = 0;
        if( zeros_left > 15 ) zeros_left = 15;
        write_vlc( bs, total_zeros[zeros_left] );

        run = 0;
        for( i = last - 1; i >= 0; i-- )
        {
            if( levels[i] == 0 )
                run++;
            else
            {
                int z = zeros_left > 7 ? 7 : zeros_left;
                int r = run > 6 ? 6 : run;
                write_vlc( bs, run_before[z][r] );
                zeros_left -= run;
                if( zeros_left < 0 ) zeros_left = 0;
                run = 0;
            }
        }
    }
}

int x264pp_cavlc_cbp_code( int cbp_chroma, int cbp_luma )
{
    /* Intra 4:2:0 CBP mapping.  The first four combined patterns use the
     * compact code numbers; the remaining luma masks follow in raster order. */
    if( cbp_luma == 0 )
        return cbp_chroma;                   /* 0,1,2 */
    if( cbp_luma == 15 )
        return 3 + cbp_chroma;               /* 3,4,5 */
    if( cbp_chroma == 0 )
    {
        static const int idx[16] = { 0, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20 };
        return idx[cbp_luma & 15];
    }
    return 6 + cbp_chroma * 15 + (cbp_luma & 15);
}
