#ifndef X264PP_CLI_OUTPUT_H
#define X264PP_CLI_OUTPUT_H

/*
 * Output (muxer) contract for the CLI.  I0 implements the raw Annex B path:
 * bytes from the encoder are written verbatim, to a file or to stdout.
 * Container muxers are named and validated but are not implemented yet.
 */

#include <stddef.h>
#include <stdint.h>

typedef enum x264pp_muxer
{
    X264PP_MUX_AUTO = 0,
    X264PP_MUX_RAW,
    X264PP_MUX_MKV,
    X264PP_MUX_FLV
} x264pp_muxer_t;

typedef struct x264pp_output x264pp_output_t;

/* On failure a diagnostic has already been logged and *output is left NULL. */
int x264pp_output_open( const char *path, x264pp_muxer_t muxer,
                        x264pp_output_t **output );

/* Write exactly count bytes.  Returns 0 on success, -1 on a short write. */
int x264pp_output_write( x264pp_output_t *output, const uint8_t *data,
                         size_t count );

void x264pp_output_close( x264pp_output_t *output );

#endif /* X264PP_CLI_OUTPUT_H */
