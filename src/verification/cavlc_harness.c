/*
 * Standalone CAVLC round-trip harness (scratch, outside the repo).
 *
 * Calls our own x264pp_cavlc_residual() on a handmade coefficient vector and
 * writes a complete H.264 Annex B stream containing one 16x16 I picture with
 * a single macroblock.  Two macroblock flavours:
 *   i4    : I_NxN (mb_type ue(0)), CBP = luma block 0 only, one 4x4 residual
 *   i16dc : I_16x16 (mb_type ue(1)), no CBP, only the 16-coeff DC residual
 *
 * Usage:
 *   harness.exe out.264 vectors.txt [i4|i16dc]
 *   harness.exe --bits vectors.txt [i4|i16dc]     (print emitted bit strings)
 *
 * vectors.txt: one vector per line:  n nc v0 v1 ... v(n-1)
 */
#include "core/bs.h"
#include "core/cavlc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 64

static void write_sps( x264pp_bs_t *bs )
{
    /* Constrained Baseline, level 1.0, 16x16, 4:2:0, 8 bit. */
    x264pp_bs_u( bs, 66, 8 );                 /* profile_idc */
    x264pp_bs_u( bs, 0, 6 );                  /* constraint flags */
    x264pp_bs_u( bs, 0, 2 );                  /* reserved */
    x264pp_bs_u( bs, 10, 8 );                 /* level_idc */
    x264pp_bs_ue( bs, 0 );                    /* sps_id */
    x264pp_bs_ue( bs, 0 );                    /* log2_max_frame_num_minus4 */
    x264pp_bs_ue( bs, 0 );                    /* pic_order_cnt_type */
    x264pp_bs_ue( bs, 0 );                    /* log2_max_poc_lsb_minus4 */
    x264pp_bs_ue( bs, 1 );                    /* max_num_ref_frames */
    x264pp_bs_u( bs, 0, 1 );                  /* gaps_allowed */
    x264pp_bs_ue( bs, 0 );                    /* width_in_mbs_minus1 = 0 */
    x264pp_bs_ue( bs, 0 );                    /* height_in_map_units_minus1 = 0 */
    x264pp_bs_u( bs, 1, 1 );                  /* frame_mbs_only */
    x264pp_bs_u( bs, 1, 1 );                  /* direct_8x8_inference */
    x264pp_bs_u( bs, 0, 1 );                  /* crop flag */
    x264pp_bs_u( bs, 0, 1 );                  /* VUI flag */
    x264pp_bs_trailing( bs );
}

static void write_pps( x264pp_bs_t *bs )
{
    x264pp_bs_ue( bs, 0 );                    /* pps_id */
    x264pp_bs_ue( bs, 0 );                    /* sps_id */
    x264pp_bs_u( bs, 0, 1 );                  /* entropy = CAVLC */
    x264pp_bs_u( bs, 0, 1 );                  /* bottom field POC */
    x264pp_bs_ue( bs, 0 );                    /* num_slice_groups_minus1 */
    x264pp_bs_ue( bs, 0 );                    /* num_ref_idx_l0_default_active_minus1 */
    x264pp_bs_ue( bs, 0 );                    /* num_ref_idx_l1_default_active_minus1 */
    x264pp_bs_u( bs, 0, 1 );                  /* weighted_pred */
    x264pp_bs_u( bs, 0, 2 );                  /* weighted_bipred_idc */
    x264pp_bs_se( bs, 0 );                    /* pic_init_qp_minus26 */
    x264pp_bs_se( bs, 0 );                    /* pic_init_qs_minus26 */
    x264pp_bs_se( bs, 0 );                    /* chroma_qp_index_offset */
    x264pp_bs_u( bs, 1, 1 );                  /* deblocking_control_present */
    x264pp_bs_u( bs, 0, 1 );                  /* constrained_intra */
    x264pp_bs_u( bs, 0, 1 );                  /* redundant_pic_cnt_present */
    x264pp_bs_trailing( bs );
}

static void write_slice_header( x264pp_bs_t *bs )
{
    x264pp_bs_ue( bs, 0 );                    /* first_mb_in_slice */
    x264pp_bs_ue( bs, 7 );                    /* slice_type = I */
    x264pp_bs_ue( bs, 0 );                    /* pps_id */
    x264pp_bs_u( bs, 0, 4 );                  /* frame_num */
    x264pp_bs_ue( bs, 0 );                    /* idr_pic_id */
    x264pp_bs_u( bs, 0, 4 );                  /* pic_order_cnt_lsb */
    x264pp_bs_u( bs, 0, 1 );                  /* no_output_of_prior_pics */
    x264pp_bs_u( bs, 0, 1 );                  /* long_term_reference */
    x264pp_bs_se( bs, 0 );                    /* slice_qp_delta */
    x264pp_bs_ue( bs, 1 );                    /* disable_deblocking_filter_idc */
}

static void print_bits( const x264pp_bs_t *bs )
{
    int i;
    for( i = 0; i < bs->bitpos; i++ )
    {
        int byte = i >> 3, bit = 7 - (i & 7);
        putchar( ( bs->buf[byte] >> bit ) & 1 ? '1' : '0' );
    }
    putchar( '\n' );
}

static int append_nal( uint8_t *out, int cap, int *pos,
                       const uint8_t *rbsp, int len, int ref, int type )
{
    int n = x264pp_bs_nal( rbsp, len, ref, type, 1, out + *pos, cap - *pos );
    if( n < 0 ) return -1;
    *pos += n;
    return 0;
}

int main( int argc, char **argv )
{
    const char *vecfile, *outname = NULL;
    int bits_mode = 0, i4 = 1;
    FILE *vf, *out = NULL;
    char line[1024];
    uint8_t *sps, *pps, *slice, *ebsp;
    x264pp_bs_t bs;
    int lineno = 0, failures = 0;
    int sps_len, pps_len, pos = 0, cap = 1 << 20;

    if( argc < 3 )
    {
        fprintf( stderr, "usage: harness [--bits] <out.264|-> <vectors.txt> [i4|i16dc]\n" );
        return 2;
    }
    if( !strcmp( argv[1], "--bits" ) )
    {
        bits_mode = 1;
        vecfile = argv[2];
        outname = NULL;
        if( argc > 3 && !strcmp( argv[3], "i16dc" ) ) i4 = 0;
    }
    else
    {
        outname = argv[1];
        vecfile = argv[2];
        if( argc > 3 && !strcmp( argv[3], "i16dc" ) ) i4 = 0;
    }

    vf = fopen( vecfile, "r" );
    if( !vf ) { perror( "vectors" ); return 2; }
    if( !bits_mode )
    {
        out = fopen( outname, "wb" );
        if( !out ) { perror( "output" ); return 2; }
    }

    sps = (uint8_t *)malloc( cap );
    pps = (uint8_t *)malloc( cap );
    slice = (uint8_t *)malloc( cap );
    ebsp = (uint8_t *)malloc( cap );
    if( !sps || !pps || !slice || !ebsp ) return 2;

    x264pp_bs_init( &bs, sps, cap ); write_sps( &bs );
    sps_len = x264pp_bs_bytes( &bs );
    x264pp_bs_init( &bs, pps, cap ); write_pps( &bs );
    pps_len = x264pp_bs_bytes( &bs );

    while( fgets( line, sizeof( line ), vf ) )
    {
        int n = 0, nc = 0, k, parsed = 0;
        int16_t v[MAXN];
        char *p = line;
        while( *p == ' ' || *p == '\t' ) p++;
        if( !*p || *p == '#' ) continue;
        for( k = 0; k < MAXN + 2; k++ )
        {
            int val;
            if( !*p ) break;
            val = (int)strtol( p, &p, 10 );
            if( k == 0 ) n = val;
            else if( k == 1 ) nc = val;
            else v[k - 2] = (int16_t)val;
            parsed++;
            while( *p == ' ' || *p == '\t' ) p++;
        }
        if( n < 1 || n > MAXN || parsed < n + 2 ) continue;
        lineno++;

        if( bits_mode )
        {
            x264pp_bs_init( &bs, slice, cap );
            x264pp_cavlc_residual( &bs, v, n, nc );
            printf( "%d ", bs.bitpos );
            print_bits( &bs );
            if( bs.overflow ) { fprintf( stderr, "overflow line %d\n", lineno ); failures++; }
            continue;
        }

        pos = 0;
        if( append_nal( ebsp, cap, &pos, sps, sps_len, 3, 7 ) ||
            append_nal( ebsp, cap, &pos, pps, pps_len, 3, 8 ) )
        { fprintf( stderr, "nal fail\n" ); failures++; continue; }

        x264pp_bs_init( &bs, slice, cap );
        write_slice_header( &bs );
        if( i4 )
        {
            int j;
            x264pp_bs_ue( &bs, 0 );                 /* mb_type = I_NxN */
            for( j = 0; j < 16; j++ )
                x264pp_bs_u( &bs, 1, 1 );           /* prev_intra4x4_pred_mode_flag = DC */
            x264pp_bs_ue( &bs, 0 );                 /* intra_chroma_pred_mode */
            x264pp_bs_ue( &bs, 2 );                 /* CBP codeNum 2 -> CBP=15 (all 16 luma blocks) */
        }
        else
        {
            x264pp_bs_ue( &bs, 3 );                 /* mb_type = I_16x16 DC pred, CBP 0 */
            x264pp_bs_ue( &bs, 0 );                 /* intra_chroma_pred_mode */
        }
        x264pp_bs_se( &bs, 0 );                     /* mb_qp_delta */
        x264pp_cavlc_residual( &bs, v, n, nc );     /* luma block 0 = test vector */
        if( i4 )
        {
            /* CodedBlockPatternLuma bit 0 codes the four 4x4 blocks 0..3. */
            int16_t zero[16] = { 0 };
            int j, tc0 = 0;
            for( j = 0; j < n; j++ ) if( v[j] ) tc0++;
            /* Blocks 1..3 have only their left neighbour available; block 1
             * sees block 0's nonzero count, the rest see zero. */
            for( j = 1; j < 16; j++ )
                x264pp_cavlc_residual( &bs, zero, 16,
                    (j == 1 || j == 4) ? tc0 : 0 );
        }
        if( bs.overflow ) { fprintf( stderr, "overflow line %d\n", lineno ); failures++; continue; }
        {
            int len = x264pp_bs_trailing( &bs );
            if( append_nal( ebsp, cap, &pos, slice, len, 3, 5 ) )
            { fprintf( stderr, "slice nal fail\n" ); failures++; continue; }
        }
        fwrite( ebsp, 1, (size_t)pos, out );
    }

    fprintf( stderr, "harness: %d vectors, %d failures\n", lineno, failures );
    if( out ) fclose( out );
    fclose( vf );
    free( sps ); free( pps ); free( slice ); free( ebsp );
    return failures ? 1 : 0;
}
