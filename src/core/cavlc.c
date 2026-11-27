/*
 * CAVLC residual writer for x264++ (original clean-room code).
 *
 * Normative reference: ITU-T Rec. H.264, clause 9.2 and its subclauses:
 *   9.2.1 / Table 9-5   coeff_token (TotalCoeff, TrailingOnes) and the
 *                       all-zero token, for nC = 0..3 and nC = -1/-2
 *   9.2.2 / 9.2.2.1 / Table 9-6
 *                       trailing_ones_sign_flag, level_prefix/level_suffix,
 *                       levelCode reconstruction (including the +2 rule for
 *                       the first non-T1 level when T1 < 3) and the
 *                       suffixLength initialisation/update rules
 *   9.2.3 / Tables 9-7, 9-8, 9-9
 *                       total_zeros, indexed by tzVlcIndex = TotalCoeff
 *   9.2.3 / Table 9-10  run_before, indexed by zerosLeft
 *   9.2.4               combining levels and runs in scan order
 *
 * The code/length pairs below are transcribed from those published tables;
 * they are standard numeric facts, not derived from any encoder source.
 * The public interface is unchanged from core/cavlc.h.
 */

#include "core/cavlc.h"

#define V( c, l ) { (uint32_t)(c), (uint8_t)(l) }

/* Table 9-5 coeff_token: [cat][TotalCoeff][TrailingOnes]. cat 0..3 are the
 * standard 0<=nC<2, 2<=nC<4, 4<=nC<8, 8<=nC columns; cat 4/5 are nC=-1/-2. */
static const x264pp_vlc_t coeff_token_tbl[6][17][4] = {
    { /* cat 0 */
        { V(1,1), V(0,0), V(0,0), V(0,0) },
        { V(5,6), V(1,2), V(0,0), V(0,0) },
        { V(7,8), V(4,6), V(1,3), V(0,0) },
        { V(7,9), V(6,8), V(5,7), V(3,5) },
        { V(7,10), V(6,9), V(5,8), V(3,6) },
        { V(7,11), V(6,10), V(5,9), V(4,7) },
        { V(15,13), V(6,11), V(5,10), V(4,8) },
        { V(11,13), V(14,13), V(5,11), V(4,9) },
        { V(8,13), V(10,13), V(13,13), V(4,10) },
        { V(15,14), V(14,14), V(9,13), V(4,11) },
        { V(11,14), V(10,14), V(13,14), V(12,13) },
        { V(15,15), V(14,15), V(9,14), V(12,14) },
        { V(11,15), V(10,15), V(13,15), V(8,14) },
        { V(15,16), V(1,15), V(9,15), V(12,15) },
        { V(11,16), V(14,16), V(13,16), V(8,15) },
        { V(7,16), V(10,16), V(9,16), V(12,16) },
        { V(4,16), V(6,16), V(5,16), V(8,16) },
    },
    { /* cat 1 */
        { V(3,2), V(0,0), V(0,0), V(0,0) },
        { V(11,6), V(2,2), V(0,0), V(0,0) },
        { V(7,6), V(7,5), V(3,3), V(0,0) },
        { V(7,7), V(10,6), V(9,6), V(5,4) },
        { V(7,8), V(6,6), V(5,6), V(4,4) },
        { V(4,8), V(6,7), V(5,7), V(6,5) },
        { V(7,9), V(6,8), V(5,8), V(8,6) },
        { V(15,11), V(6,9), V(5,9), V(4,6) },
        { V(11,11), V(14,11), V(13,11), V(4,7) },
        { V(15,12), V(10,11), V(9,11), V(4,9) },
        { V(11,12), V(14,12), V(13,12), V(12,11) },
        { V(8,12), V(10,12), V(9,12), V(8,11) },
        { V(15,13), V(14,13), V(13,13), V(12,12) },
        { V(11,13), V(10,13), V(9,13), V(12,13) },
        { V(7,13), V(11,14), V(6,13), V(8,13) },
        { V(9,14), V(8,14), V(10,14), V(1,13) },
        { V(7,14), V(6,14), V(5,14), V(4,14) },
    },
    { /* cat 2 */
        { V(15,4), V(0,0), V(0,0), V(0,0) },
        { V(15,6), V(14,4), V(0,0), V(0,0) },
        { V(11,6), V(15,5), V(13,4), V(0,0) },
        { V(8,6), V(12,5), V(14,5), V(12,4) },
        { V(15,7), V(10,5), V(11,5), V(11,4) },
        { V(11,7), V(8,5), V(9,5), V(10,4) },
        { V(9,7), V(14,6), V(13,6), V(9,4) },
        { V(8,7), V(10,6), V(9,6), V(8,4) },
        { V(15,8), V(14,7), V(13,7), V(13,5) },
        { V(11,8), V(14,8), V(10,7), V(12,6) },
        { V(15,9), V(10,8), V(13,8), V(12,7) },
        { V(11,9), V(14,9), V(9,8), V(12,8) },
        { V(8,9), V(10,9), V(13,9), V(8,8) },
        { V(13,10), V(7,9), V(9,9), V(12,9) },
        { V(9,10), V(12,10), V(11,10), V(10,10) },
        { V(5,10), V(8,10), V(7,10), V(6,10) },
        { V(1,10), V(4,10), V(3,10), V(2,10) },
    },
    { /* cat 3 */
        { V(3,6), V(0,0), V(0,0), V(0,0) },
        { V(0,6), V(1,6), V(0,0), V(0,0) },
        { V(4,6), V(5,6), V(6,6), V(0,0) },
        { V(8,6), V(9,6), V(10,6), V(11,6) },
        { V(12,6), V(13,6), V(14,6), V(15,6) },
        { V(16,6), V(17,6), V(18,6), V(19,6) },
        { V(20,6), V(21,6), V(22,6), V(23,6) },
        { V(24,6), V(25,6), V(26,6), V(27,6) },
        { V(28,6), V(29,6), V(30,6), V(31,6) },
        { V(32,6), V(33,6), V(34,6), V(35,6) },
        { V(36,6), V(37,6), V(38,6), V(39,6) },
        { V(40,6), V(41,6), V(42,6), V(43,6) },
        { V(44,6), V(45,6), V(46,6), V(47,6) },
        { V(48,6), V(49,6), V(50,6), V(51,6) },
        { V(52,6), V(53,6), V(54,6), V(55,6) },
        { V(56,6), V(57,6), V(58,6), V(59,6) },
        { V(60,6), V(61,6), V(62,6), V(63,6) },
    },
    { /* cat 4: nC = -1, ChromaDC 2x2 (4:2:0) */
        { V(1,2), V(0,0), V(0,0), V(0,0) },
        { V(7,6), V(1,1), V(0,0), V(0,0) },
        { V(4,6), V(6,6), V(1,3), V(0,0) },
        { V(3,6), V(3,7), V(2,7), V(5,6) },
        { V(2,6), V(3,8), V(2,8), V(0,7) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
    },
    { /* cat 5: nC = -2, ChromaDC 2x4 (4:2:2) */
        { V(1,1), V(0,0), V(0,0), V(0,0) },
        { V(15,7), V(1,2), V(0,0), V(0,0) },
        { V(14,7), V(13,7), V(1,3), V(0,0) },
        { V(7,9), V(12,7), V(11,7), V(1,5) },
        { V(6,9), V(5,9), V(10,7), V(1,6) },
        { V(7,10), V(6,10), V(4,9), V(9,7) },
        { V(7,11), V(6,11), V(5,10), V(8,7) },
        { V(7,12), V(6,12), V(5,11), V(4,10) },
        { V(7,13), V(5,12), V(4,12), V(4,11) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
        { V(0,0), V(0,0), V(0,0), V(0,0) },
    },
};

/* Tables 9-7/9-8 total_zeros: [tzVlcIndex-1][total_zeros]. */
static const x264pp_vlc_t total_zeros_tbl[15][16] = {
    { V(1,1), V(3,3), V(2,3), V(3,4), V(2,4), V(3,5), V(2,5), V(3,6), V(2,6), V(3,7), V(2,7), V(3,8), V(2,8), V(3,9), V(2,9), V(1,9) },
    { V(7,3), V(6,3), V(5,3), V(4,3), V(3,3), V(5,4), V(4,4), V(3,4), V(2,4), V(3,5), V(2,5), V(3,6), V(2,6), V(1,6), V(0,6), V(0,0) },
    { V(5,4), V(7,3), V(6,3), V(5,3), V(4,4), V(3,4), V(4,3), V(3,3), V(2,4), V(3,5), V(2,5), V(1,6), V(1,5), V(0,6), V(0,0), V(0,0) },
    { V(3,5), V(7,3), V(5,4), V(4,4), V(6,3), V(5,3), V(4,3), V(3,4), V(3,3), V(2,4), V(2,5), V(1,5), V(0,5), V(0,0), V(0,0), V(0,0) },
    { V(5,4), V(4,4), V(3,4), V(7,3), V(6,3), V(5,3), V(4,3), V(3,3), V(2,4), V(1,5), V(1,4), V(0,5), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,6), V(1,5), V(7,3), V(6,3), V(5,3), V(4,3), V(3,3), V(2,3), V(1,4), V(1,3), V(0,6), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,6), V(1,5), V(5,3), V(4,3), V(3,3), V(3,2), V(2,3), V(1,4), V(1,3), V(0,6), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,6), V(1,4), V(1,5), V(3,3), V(3,2), V(2,2), V(2,3), V(1,3), V(0,6), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,6), V(0,6), V(1,4), V(3,2), V(2,2), V(1,3), V(1,2), V(1,5), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,5), V(0,5), V(1,3), V(3,2), V(2,2), V(1,2), V(1,4), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(0,4), V(1,4), V(1,3), V(2,3), V(1,1), V(3,3), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(0,4), V(1,4), V(1,2), V(1,1), V(1,3), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(0,3), V(1,3), V(1,1), V(1,2), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(0,2), V(1,2), V(1,1), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(0,1), V(1,1), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
};

/* Table 9-9(a) total_zeros for 2x2 chroma DC: [TotalCoeff-1][total_zeros]. */
static const x264pp_vlc_t total_zeros_2x2[3][4] = {
    { V(1,1), V(1,2), V(1,3), V(0,3) },
    { V(1,1), V(1,2), V(0,2), V(0,0) },
    { V(1,1), V(0,1), V(0,0), V(0,0) },
};

/* Table 9-9(b) total_zeros for 2x4 chroma DC: [TotalCoeff-1][total_zeros]. */
static const x264pp_vlc_t total_zeros_2x4[7][8] = {
    { V(1,1), V(2,3), V(3,3), V(2,4), V(3,4), V(1,4), V(1,5), V(0,5) },
    { V(0,3), V(1,2), V(1,3), V(4,3), V(5,3), V(6,3), V(7,3), V(0,0) },
    { V(0,3), V(1,3), V(1,2), V(2,2), V(6,3), V(7,3), V(0,0), V(0,0) },
    { V(6,3), V(0,2), V(1,2), V(2,2), V(7,3), V(0,0), V(0,0), V(0,0) },
    { V(0,2), V(1,2), V(2,2), V(3,2), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(0,2), V(1,2), V(1,1), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(0,1), V(1,1), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
};

/* Table 9-10 run_before: [zerosLeft category 1..6,7 for >6][run_before]. */
static const x264pp_vlc_t run_before_tbl[8][15] = {
    { V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,1), V(0,1), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(1,1), V(1,2), V(0,2), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(3,2), V(2,2), V(1,2), V(0,2), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(3,2), V(2,2), V(1,2), V(1,3), V(0,3), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(3,2), V(2,2), V(3,3), V(2,3), V(1,3), V(0,3), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(3,2), V(0,3), V(1,3), V(3,3), V(2,3), V(5,3), V(4,3), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0), V(0,0) },
    { V(7,3), V(6,3), V(5,3), V(4,3), V(3,3), V(2,3), V(1,3), V(1,4), V(1,5), V(1,6), V(1,7), V(1,8), V(1,9), V(1,10), V(1,11) },
};

static void write_vlc( x264pp_bs_t *bs, x264pp_vlc_t v )
{
    if( v.len )
        x264pp_bs_write( bs, v.code, v.len );
}

/* Map the caller's nC to a Table 9-5 category.  Non-negative nC is the
 * standard neighbour count; -1/-2 are the dedicated chroma-DC contexts.
 * The I1 convention uses nC = 5 for a 2x2 chroma-DC block. */
static int coeff_token_category( int nC )
{
    if( nC == -2 || nC == 6 ) return 5;
    if( nC == -1 || nC == 5 ) return 4;
    if( nC < 2 ) return 0;
    if( nC < 4 ) return 1;
    if( nC < 8 ) return 2;
    return 3;
}

/* Inverse of clause 9.2.2 steps 1-6: encode one levelCode as
 * level_prefix (leading zeros, then a one) plus level_suffix. */
static void write_level( x264pp_bs_t *bs, int level_code, int *suffix )
{
    int s = *suffix;
    int lp, lss;
    int64_t suffix_val;

    if( level_code < 0 )
        level_code = 0;                 /* defensive: never emit a negative code */

    if( s == 0 )
    {
        /* level_prefix 14 uses a 4-bit suffix; 15 and above use 12 bits. */
        if( level_code < 14 )
        {
            lp = level_code;
            lss = 0;
            suffix_val = 0;
        }
        else if( level_code < 30 )
        {
            lp = 14;
            lss = 4;
            suffix_val = level_code - 14;
        }
        else
        {
            int64_t rem = (int64_t)level_code - 30;
            lp = 15;
            while( lp < 31 )
            {
                int64_t next = ((int64_t)1 << (lp - 2)) - 4096;
                if( rem < next )
                    break;
                lp++;
            }
            lss = lp - 3;
            suffix_val = rem - (((int64_t)1 << (lp - 3)) - 4096);
        }
    }
    else
    {
        int q = level_code >> s;
        int mask = (1 << s) - 1;
        if( q <= 14 )
        {
            lp = q;
            lss = s;
            suffix_val = level_code & mask;
        }
        else
        {
            int64_t rem = (int64_t)level_code - ((int64_t)15 << s);
            lp = 15;
            while( lp < 31 )
            {
                int64_t next = ((int64_t)1 << (lp - 2)) - 4096;
                if( rem < next )
                    break;
                lp++;
            }
            lss = lp - 3;
            suffix_val = rem - (((int64_t)1 << (lp - 3)) - 4096);
        }
    }

    x264pp_bs_write( bs, 0, lp );        /* leadingZeroBits zeros */
    x264pp_bs_write( bs, 1, 1 );         /* terminating one */
    if( lss > 0 )
        x264pp_bs_write( bs, (uint32_t)(suffix_val & ((1u << lss) - 1)), lss );
}

/* Clause 9.2.2 steps 9-10. */
static void update_suffix( int abs_level, int *suffix )
{
    int s = *suffix;
    if( s == 0 )
        s = 1;
    if( s < 6 && abs_level > (3 << (s - 1)) )
        s++;
    *suffix = s;
}

void x264pp_cavlc_residual( x264pp_bs_t *bs, const int16_t *levels,
                             int n, int nC )
{
    int i, total = 0, last = -1, trailing = 0;
    int cat, suffix = 0, first_non_t1 = 1;
    int zeros_left, run;

    if( !bs || !levels || n <= 0 )
        return;

    for( i = 0; i < n; i++ )
        if( levels[i] )
        {
            total++;
            last = i;
        }

    cat = coeff_token_category( nC );

    if( total == 0 )
    {
        write_vlc( bs, coeff_token_tbl[cat][0][0] );  /* coeff0_token */
        return;
    }

    /* TrailingOnes: at most three ±1 levels at the highest scan indices. */
    for( i = last; i >= 0 && trailing < 3 && levels[i]; i-- )
    {
        int abs_level = levels[i] < 0 ? -levels[i] : levels[i];
        if( abs_level != 1 )
            break;
        trailing++;
    }
    if( total > 16 ) total = 16;
    write_vlc( bs, coeff_token_tbl[cat][total][trailing] );

    /* Clause 9.2.2: T1 signs directly after coeff_token, high index first. */
    for( i = 0; i < trailing; i++ )
        x264pp_bs_write( bs, levels[last - i] < 0 ? 1 : 0, 1 );

    suffix = (total > 10 && trailing < 3) ? 1 : 0;

    /* Remaining levels, reverse scan order. */
    for( i = last - trailing; i >= 0; i-- )
    {
        int level = levels[i];
        int abs_level = level < 0 ? -level : level;
        int level_code = 2 * abs_level - 2 + (level < 0 ? 1 : 0);
        if( first_non_t1 && trailing < 3 )
            level_code -= 2;            /* inverse of 9.2.2 step 7 */
        first_non_t1 = 0;
        write_level( bs, level_code, &suffix );
        update_suffix( abs_level, &suffix );
    }

    if( total < n )
    {
        /* Zeros between the lowest and the highest non-zero coefficient. */
        zeros_left = last - (total - 1);
        if( zeros_left < 0 )
            zeros_left = 0;

        /* total_zeros is indexed by tzVlcIndex = TotalCoeff, not just by
         * the number of zeros left (clauses 9.2.3, Tables 9-7/9-8/9-9). */
        if( total <= 16 )
        {
            int idx = total - 1;
            if( n <= 4 && idx < 3 )
                write_vlc( bs, total_zeros_2x2[idx][zeros_left] );
            else if( n <= 8 && (cat == 4 || cat == 5) && idx < 7 )
                write_vlc( bs, total_zeros_2x4[idx][zeros_left] );
            else if( idx < 15 )
                write_vlc( bs, total_zeros_tbl[idx][zeros_left] );
        }

        /* run_before for every gap above a non-zero level, high index first. */
        run = 0;
        for( i = last - 1; i >= 0; i-- )
        {
            if( levels[i] == 0 )
                run++;
            else
            {
                int zc = zeros_left > 6 ? 7 : (zeros_left < 1 ? 1 : zeros_left);
                int r = run > 14 ? 14 : run;
                if( zeros_left > 0 )
                    write_vlc( bs, run_before_tbl[zc][r] );
                zeros_left -= run;
                if( zeros_left < 0 )
                    zeros_left = 0;
                run = 0;
            }
        }
    }
}

int x264pp_cavlc_cbp_code( int cbp_chroma, int cbp_luma )
{
    /* Intra 4:2:0 CBP mapping (H.264 Table 9-4).  The first four combined
     * patterns use the compact code numbers; the remaining luma masks
     * follow in raster order. */
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
