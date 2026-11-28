/*
 * CAVLC residual writer for x264++ (clean-room implementation).
 *
 * This writer is the exact inverse of the independent syntax reader in
 * cert/bitstream_diag.py, which was validated by parsing a real ffmpeg/x264
 * CAVLC stream with zero errors.  The bit-string tables below are transcribed
 * from the published ITU-T Rec. H.264 clause 9.2 tables (9-5, 9-7..9-10) in
 * the same order the reader uses:
 *
 *   CT_*   coeff_token      [TotalCoeff, TrailingOnes] / nC category
 *   TZ4_*  total_zeros      [TotalCoeff] for 4x4 blocks
 *   TZC_*  total_zeros      [TotalCoeff] for 2x2 chroma DC blocks
 *   RB_*   run_before       [zerosLeft] for each run
 *
 * Emission order mirrors the reader's consumption order exactly:
 * coeff_token, trailing-one signs, remaining levels (level_prefix/suffix),
 * total_zeros keyed by TotalCoeff, then run_before with zerosLeft bookkeeping.
 * A zero-valued scan position is NOT emitted as a level; it is described only
 * by total_zeros/run_before, which is what the reader expects.
 */

#include "core/cavlc.h"

/* ---- published VLC codewords (MSB-first bit strings) ------------------ */

static const char *const CT_STR[6][62] = {
    { "1", "000101", "01", "00000111", "000100", "001", "000000111", "00000110", "0000101", "00011", "0000000111", "000000110", "00000101", "000011", "00000000111", "0000000110", "000000101", "0000100", "0000000001111", "00000000110", "0000000101", "00000100", "0000000001011", "0000000001110", "00000000101", "000000100", "0000000001000", "0000000001010", "0000000001101", "0000000100", "00000000001111", "00000000001110", "0000000001001", "00000000100", "00000000001011", "00000000001010", "00000000001101", "0000000001100", "000000000001111", "000000000001110", "00000000001001", "00000000001100", "000000000001011", "000000000001010", "000000000001101", "00000000001000", "0000000000001111", "000000000000001", "000000000001001", "000000000001100", "0000000000001011", "0000000000001110", "0000000000001101", "000000000001000", "0000000000000111", "0000000000001010", "0000000000001001", "0000000000001100", "0000000000000100", "0000000000000110", "0000000000000101", "0000000000001000" }, /* nC 0..1 */
    { "11", "001011", "10", "000111", "00111", "011", "0000111", "001010", "001001", "0101", "00000111", "000110", "000101", "0100", "00000100", "0000110", "0000101", "00110", "000000111", "00000110", "00000101", "001000", "00000001111", "000000110", "000000101", "000100", "00000001011", "00000001110", "00000001101", "0000100", "000000001111", "00000001010", "00000001001", "000000100", "000000001011", "000000001110", "000000001101", "00000001100", "000000001000", "000000001010", "000000001001", "00000001000", "0000000001111", "0000000001110", "0000000001101", "000000001100", "0000000001011", "0000000001010", "0000000001001", "0000000001100", "0000000000111", "00000000001011", "0000000000110", "0000000001000", "00000000001001", "00000000001000", "00000000001010", "0000000000001", "00000000000111", "00000000000110", "00000000000101", "00000000000100" }, /* nC 2..3 */
    { "1111", "001111", "1110", "001011", "01111", "1101", "001000", "01100", "01110", "1100", "0001111", "01010", "01011", "1011", "0001011", "01000", "01001", "1010", "0001001", "001110", "001101", "1001", "0001000", "001010", "001001", "1000", "00001111", "0001110", "0001101", "01101", "00001011", "00001110", "0001010", "001100", "000001111", "00001010", "00001101", "0001100", "000001011", "000001110", "00001001", "00001100", "000001000", "000001010", "000001101", "00001000", "0000001101", "000000111", "000001001", "000001100", "0000001001", "0000001100", "0000001011", "0000001010", "0000000101", "0000001000", "0000000111", "0000000110", "0000000001", "0000000100", "0000000011", "0000000010" }, /* nC 4..7 */
    { "000011", "000000", "000001", "000100", "000101", "000110", "001000", "001001", "001010", "001011", "001100", "001101", "001110", "001111", "010000", "010001", "010010", "010011", "010100", "010101", "010110", "010111", "011000", "011001", "011010", "011011", "011100", "011101", "011110", "011111", "100000", "100001", "100010", "100011", "100100", "100101", "100110", "100111", "101000", "101001", "101010", "101011", "101100", "101101", "101110", "101111", "110000", "110001", "110010", "110011", "110100", "110101", "110110", "110111", "111000", "111001", "111010", "111011", "111100", "111101", "111110", "111111" }, /* nC >= 8 */
    { "01", "000111", "1", "000100", "000110", "001", "000011", "0000011", "0000010", "000101", "000010", "00000011", "00000010", "0000000", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" }, /* nC = -1, chroma DC 2x2 */
    { "1", "0001111", "01", "0001110", "0001101", "001", "000000111", "0001100", "0001011", "00001", "000000110", "000000101", "0001010", "000001", "0000000111", "0000000110", "000000100", "0001001", "00000000111", "00000000110", "0000000101", "0001000", "000000000111", "000000000110", "00000000101", "0000000100", "0000000000111", "000000000101", "000000000100", "00000000100", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" }, /* nC = -2, chroma DC 2x4 */
};

static const char *const TZ4_1[16]  = { "1", "011", "010", "0011", "0010", "00011", "00010", "000011", "000010", "0000011", "0000010", "00000011", "00000010", "000000011", "000000010", "000000001" };
static const char *const TZ4_2[15]  = { "111", "110", "101", "100", "011", "0101", "0100", "0011", "0010", "00011", "00010", "000011", "000010", "000001", "000000" };
static const char *const TZ4_3[14]  = { "0101", "111", "110", "101", "0100", "0011", "100", "011", "0010", "00011", "00010", "000001", "00001", "000000" };
static const char *const TZ4_4[13]  = { "00011", "111", "0101", "0100", "110", "101", "100", "0011", "011", "0010", "00010", "00001", "00000" };
static const char *const TZ4_5[12]  = { "0101", "0100", "0011", "111", "110", "101", "100", "011", "0010", "00001", "0001", "00000" };
static const char *const TZ4_6[11]  = { "000001", "00001", "111", "110", "101", "100", "011", "010", "0001", "001", "000000" };
static const char *const TZ4_7[10]  = { "000001", "00001", "101", "100", "011", "11", "010", "0001", "001", "000000" };
static const char *const TZ4_8[9]   = { "000001", "0001", "00001", "011", "11", "10", "010", "001", "000000" };
static const char *const TZ4_9[8]   = { "000001", "000000", "0001", "11", "10", "001", "01", "00001" };
static const char *const TZ4_10[7]  = { "00001", "00000", "001", "11", "10", "01", "0001" };
static const char *const TZ4_11[6]  = { "0000", "0001", "001", "010", "1", "011" };
static const char *const TZ4_12[5]  = { "0000", "0001", "01", "1", "001" };
static const char *const TZ4_13[4]  = { "000", "001", "1", "01" };
static const char *const TZ4_14[3]  = { "00", "01", "1" };
static const char *const TZ4_15[2]  = { "0", "1" };

static const char *const TZC_1[4] = { "1", "01", "001", "000" };
static const char *const TZC_2[3] = { "1", "01", "00" };
static const char *const TZC_3[2] = { "1", "0" };

static const char *const RB_1[2]  = { "1", "0" };
static const char *const RB_2[3]  = { "1", "01", "00" };
static const char *const RB_3[4]  = { "11", "10", "01", "00" };
static const char *const RB_4[5]  = { "11", "10", "01", "001", "000" };
static const char *const RB_5[6]  = { "11", "10", "011", "010", "001", "000" };
static const char *const RB_6[7]  = { "11", "000", "001", "011", "010", "101", "100" };
static const char *const RB_7[15] = { "111", "110", "101", "100", "011", "010", "001", "0001", "00001", "000001", "0000001", "00000001", "000000001", "0000000001", "00000000001" };

static const char *const *const CT[6] = { CT_STR[0], CT_STR[1], CT_STR[2], CT_STR[3], CT_STR[4], CT_STR[5] };
static const char *const *const TZ4[15] = { TZ4_1,TZ4_2,TZ4_3,TZ4_4,TZ4_5,TZ4_6,TZ4_7,TZ4_8,TZ4_9,TZ4_10,TZ4_11,TZ4_12,TZ4_13,TZ4_14,TZ4_15 };
static const int TZ4_LEN[15] = { 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2 };
static const char *const *const TZC[3] = { TZC_1, TZC_2, TZC_3 };
static const int TZC_LEN[3] = { 4,3,2 };
static const char *const *const RB[7] = { RB_1,RB_2,RB_3,RB_4,RB_5,RB_6,RB_7 };
static const int RB_LEN[7] = { 2,3,4,5,6,7,15 };

/* ---- helpers ---------------------------------------------------------- */

static x264pp_vlc_t vlc_of( const char *bits )
{
    x264pp_vlc_t v;
    uint32_t code = 0;
    int len = 0;

    v.code = 0;
    v.len = 0;
    if( !bits ) return v;
    while( *bits )
    {
        if( *bits != '0' && *bits != '1' ) { v.code = 0; v.len = 0; return v; }
        code = ( code << 1 ) | (uint32_t)( *bits - '0' );
        bits++;
        len++;
    }
    v.code = code;
    v.len = (uint8_t)len;
    return v;
}

static void write_vlc( x264pp_bs_t *bs, x264pp_vlc_t v )
{
    if( v.len )
        x264pp_bs_write( bs, v.code, v.len );
}

/* Table 9-5 category from the caller's nC.  -1/-2 are the dedicated chroma-DC
 * contexts; positive values 5 and 6 are ordinary neighbour counts and must
 * stay in the 4<=nC<8 column. */
static int token_category( int nC )
{
    if( nC == -1 ) return 4;
    if( nC == -2 ) return 5;
    if( nC < 2 ) return 0;
    if( nC < 4 ) return 1;
    if( nC < 8 ) return 2;
    return 3;
}

/* Flat list layout used by the reader: TotalCoeff=0 has only T1=0,
 * TotalCoeff=1 has T1=0,1, and TotalCoeff>=2 has all four T1 values
 * (the impossible (2,3) slot is present but unused). */
static int ct_index( int total, int t1 )
{
    if( total <= 0 ) return 0;
    if( total == 1 ) return 1 + t1;
    /* The published list omits the impossible (TotalCoeff=2,
     * TrailingOnes=3) pair, so categories from TotalCoeff=3 onwards are
     * shifted by one relative to 4*TotalCoeff. */
    if( total == 2 ) return 3 + t1;
    return 4 * total - 6 + t1;
}

/* Inverse of the level_prefix/level_suffix reconstruction in clause 9.2.2,
 * steps 1-6, exactly as the diagnostic reader implements it. */
static void write_level( x264pp_bs_t *bs, int level_code, int *suffix_length )
{
    int s = *suffix_length;
    int lp, lss;
    int64_t rem = 0, suffix_val = 0;

    if( level_code < 0 ) level_code = 0;

    if( s == 0 )
    {
        if( level_code < 14 )
        {
            lp = level_code;
            lss = 0;
        }
        else if( level_code < 30 )
        {
            lp = 14;
            lss = 4;
            suffix_val = level_code - 14;
        }
        else
        {
            rem = (int64_t)level_code - 30;
            lp = 15;
            while( lp < 31 )
            {
                int64_t next = ( (int64_t)1 << ( lp - 2 ) ) - 4096;
                if( rem < next ) break;
                lp++;
            }
            lss = lp - 3;
            suffix_val = rem - ( ( (int64_t)1 << ( lp - 3 ) ) - 4096 );
        }
    }
    else
    {
        int q = level_code >> s;
        if( q <= 14 )
        {
            lp = q;
            lss = s;
            suffix_val = level_code & ( ( 1 << s ) - 1 );
        }
        else
        {
            rem = (int64_t)level_code - ( (int64_t)15 << s );
            lp = 15;
            while( lp < 31 )
            {
                int64_t next = ( (int64_t)1 << ( lp - 2 ) ) - 4096;
                if( rem < next ) break;
                lp++;
            }
            lss = lp - 3;
            suffix_val = rem - ( ( (int64_t)1 << ( lp - 3 ) ) - 4096 );
        }
    }

    x264pp_bs_write( bs, 0, lp );       /* leadingZeroBits */
    x264pp_bs_write( bs, 1, 1 );        /* terminating one */
    if( lss > 0 )
        x264pp_bs_write( bs, (uint32_t)( suffix_val & ( (1u << lss) - 1u ) ), lss );
}

/* Clause 9.2.2 steps 9-10, identical to the reader's suffix update. */
static void update_suffix( int abs_level, int *suffix_length )
{
    int s = *suffix_length;
    if( s == 0 ) s = 1;
    if( s < 6 && abs_level > ( 3 << ( s - 1 ) ) ) s++;
    *suffix_length = s;
}

/* ---- public writer ----------------------------------------------------- */

void x264pp_cavlc_residual( x264pp_bs_t *bs, const int16_t *levels,
                            int n, int nC )
{
    int i, total = 0, last = -1, trailing = 0;
    int cat, suffix, first_non_t1 = 1;
    int zeros_left, run;

    if( !bs || !levels || n <= 0 ) return;

    for( i = 0; i < n; i++ )
        if( levels[i] )
        {
            total++;
            last = i;
        }

    cat = token_category( nC );

    if( total == 0 )
    {
        write_vlc( bs, vlc_of( CT[cat][0] ) );      /* coeff0_token */
        return;
    }
    if( total > 16 ) total = 16;                     /* defensive */

    /* TrailingOnes are the highest scan indices whose magnitude is exactly 1. */
    for( i = last; i >= 0 && trailing < 3 && levels[i]; i-- )
    {
        int abs_level = levels[i] < 0 ? -levels[i] : levels[i];
        if( abs_level != 1 ) break;
        trailing++;
    }

    write_vlc( bs, vlc_of( CT[cat][ct_index( total, trailing )] ) );

    /* trailing_ones_sign_flag, high index first: 0 -> +1, 1 -> -1. */
    for( i = 0; i < trailing; i++ )
        x264pp_bs_write( bs, levels[last - i] < 0 ? 1 : 0, 1 );

    suffix = ( total > 10 && trailing < 3 ) ? 1 : 0;

    /* Remaining levels, high scan index first.  Zero levels are not coded:
     * their positions come from total_zeros / run_before. */
    for( i = last - trailing; i >= 0; i-- )
    {
        int level = levels[i];
        int abs_level, level_code;
        if( level == 0 ) continue;
        abs_level = level < 0 ? -level : level;
        level_code = 2 * abs_level - 2 + ( level < 0 ? 1 : 0 );
        if( first_non_t1 && trailing < 3 )
            level_code -= 2;                        /* inverse of 9.2.2 step 7 */
        first_non_t1 = 0;
        write_level( bs, level_code, &suffix );
        update_suffix( abs_level, &suffix );
    }

    /* The reader infers zerosLeft = 0 when TotalCoeff == maxNumCoeff. */
    if( total >= n ) return;

    zeros_left = last - ( total - 1 );              /* all zeros below `last` */
    if( zeros_left < 0 ) zeros_left = 0;

    /* total_zeros is keyed by TotalCoeff, not by the zero count itself. */
    if( n == 4 )
    {
        if( total <= 3 && zeros_left < TZC_LEN[total - 1] )
            write_vlc( bs, vlc_of( TZC[total - 1][zeros_left] ) );
    }
    else if( total <= 15 && zeros_left < TZ4_LEN[total - 1] )
        write_vlc( bs, vlc_of( TZ4[total - 1][zeros_left] ) );

    /* run_before for each non-zero coefficient below the last one, high scan
     * index first.  The reader reads exactly total-1 such values and then
     * appends the remaining zerosLeft for the lowest non-zero coefficient. */
    run = 0;
    for( i = last - 1; i >= 0; i-- )
    {
        if( levels[i] == 0 )
        {
            run++;
        }
        else
        {
            int zc = zeros_left > 6 ? 7 : ( zeros_left < 1 ? 1 : zeros_left );
            int maxr = RB_LEN[zc - 1] - 1;
            int r = run;
            /* Table 9-10 only defines run_before <= zerosLeft, and the row
             * selected by zerosLeft has no entry beyond that.  Clamp before
             * emitting so the decoder always reads a defined codeword.  The
             * lowest coefficient's leftover zeros are inferred, not coded. */
            if( r > zeros_left ) r = zeros_left;
            if( r > maxr ) r = maxr;
            if( r < 0 ) r = 0;
            if( zeros_left > 0 )
                write_vlc( bs, vlc_of( RB[zc - 1][r] ) );
            zeros_left -= run;
            if( zeros_left < 0 ) zeros_left = 0;
            run = 0;
        }
    }
}

int x264pp_cavlc_cbp_code( int cbp_chroma, int cbp_luma )
{
    /* Inverse of H.264 Table 9-4 for I slices, 4:2:0.  The table is indexed
     * by codeNum, which is what me(v) writes; the reader maps it back to
     * (CodedBlockPatternChroma << 4) | CodedBlockPatternLuma. */
    static const int cbp_from_num[48] = {
        47, 31, 15,  0, 23, 27, 29, 30,  7, 11, 13, 14, 39, 43, 45, 46,
        16,  3,  5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44,  1,  2,  4,
         8, 17, 18, 20, 24,  6,  9, 22, 25, 32, 33, 34, 36, 40, 38, 41
    };
    int cbp = ( ( cbp_chroma & 3 ) << 4 ) | ( cbp_luma & 15 );
    int i;
    for( i = 0; i < 48; i++ )
        if( cbp_from_num[i] == cbp )
            return i;
    return 0;                                /* safest fallback: CBP = 47 */
}
