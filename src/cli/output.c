/*
 * Raw Annex B output.  The I0 writer is intentionally simple: it performs no
 * buffering of its own beyond stdio and hands every encoder byte to the file
 * unchanged.  A filename of "-" selects stdout; logs always stay on stderr.
 *
 * Behaviour is derived from spec/cli-blackbox.md sections 2 and 4 (evidence
 * E5 and E26).  The .mp4 diagnostic is part of the documented CLI surface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/output.h"

struct x264pp_output
{
    FILE *fh;
    int   is_stdout;
};

static const char *extension_of( const char *path )
{
    const char *dot = strrchr( path, '.' );
    const char *slash;

    if( !dot ) return "";
    /* Only look at the final path component. */
    slash = strrchr( path, '\\' );
    if( slash && slash > dot ) return "";
    slash = strrchr( path, '/' );
    if( slash && slash > dot ) return "";
    return dot + 1;
}

static int extension_is( const char *ext, const char *name )
{
    while( *ext && *name )
    {
        char a = *ext, b = *name;
        if( a >= 'A' && a <= 'Z' ) a = (char)(a - 'A' + 'a');
        if( b >= 'A' && b <= 'Z' ) b = (char)(b - 'A' + 'a');
        if( a != b ) return 0;
        ext++;
        name++;
    }
    return *ext == '\0' && *name == '\0';
}

int x264pp_output_open( const char *path, x264pp_muxer_t muxer,
                        x264pp_output_t **output )
{
    x264pp_output_t *out;
    int to_stdout = path && strcmp( path, "-" ) == 0;
    const char *ext = to_stdout ? "" : extension_of( path );

    if( output ) *output = NULL;
    if( !output || !path || !*path ) return -1;

    if( muxer == X264PP_MUX_AUTO )
    {
        if( extension_is( ext, "mp4" ) )
        {
            x264pp_log( X264PP_LOG_ERROR, "x264",
                        "not compiled with MP4 output support" );
            return -1;
        }
        if( extension_is( ext, "mkv" ) ) muxer = X264PP_MUX_MKV;
        else if( extension_is( ext, "flv" ) ) muxer = X264PP_MUX_FLV;
        else muxer = X264PP_MUX_RAW;
    }

    if( muxer != X264PP_MUX_RAW )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264",
                    "muxer %s is not implemented in I0",
                    muxer == X264PP_MUX_MKV ? "mkv" : "flv" );
        return -1;
    }

    out = (x264pp_output_t *)calloc( 1, sizeof( *out ) );
    if( !out ) return -1;

    out->fh = to_stdout ? stdout : fopen( path, "wb" );
    if( !out->fh )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264",
                    "could not open output file '%s'", path );
        free( out );
        return -1;
    }
    out->is_stdout = to_stdout;
    *output = out;
    return 0;
}

int x264pp_output_write( x264pp_output_t *output, const uint8_t *data,
                         size_t count )
{
    if( !output || !output->fh ) return -1;
    if( count == 0 ) return 0;
    return fwrite( data, 1, count, output->fh ) == count ? 0 : -1;
}

void x264pp_output_close( x264pp_output_t *output )
{
    if( !output ) return;
    if( output->is_stdout ) fflush( output->fh );
    else if( output->fh ) fclose( output->fh );
    free( output );
}
