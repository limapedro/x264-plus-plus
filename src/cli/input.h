#ifndef X264PP_CLI_INPUT_H
#define X264PP_CLI_INPUT_H

/*
 * Input (demuxer) contract for the CLI.  The I0 readers accept raw planar I420
 * and YUV4MPEG2 version 2 files.  Frames are delivered through the shared
 * x264pp_picture_t from core/api.h; plane memory is owned by the input handle.
 */

#include <stdint.h>

#include "core/api.h"

typedef enum x264pp_demuxer
{
    X264PP_DEMUX_AUTO = 0,
    X264PP_DEMUX_RAW,
    X264PP_DEMUX_Y4M
} x264pp_demuxer_t;

/* Negative statuses are failures; the module logs its own detailed diagnostic
 * before returning, so the caller only adds the generic "could not open"
 * line.  ERR_RESOLUTION additionally selects the "via any method!" suffix
 * documented for raw input. */
typedef enum x264pp_input_status
{
    X264PP_INPUT_OK           = 0,
    X264PP_INPUT_ERR_RESOLUTION = -1,
    X264PP_INPUT_ERR_MAGIC      = -2,
    X264PP_INPUT_ERR_IO         = -3,
    X264PP_INPUT_ERR_UNSUPPORTED = -4
} x264pp_input_status_t;

typedef struct x264pp_input_info
{
    int      width;
    int      height;
    int      interlaced;
    x264pp_csp_t csp;
    int      bit_depth;
    int      fps_num;
    int      fps_den;
    int      sar_num;
    int      sar_den;
    int64_t  num_frames;   /* -1 when unknown (stdin/pipe) */
    const char *module;    /* "raw" or "y4m", used in log lines */
} x264pp_input_info_t;

typedef struct x264pp_input x264pp_input_t;

/* resolution is a "WxH" string (required for raw input); csp_name is the
 * --input-csp name or NULL for the I420 default. */
x264pp_input_status_t x264pp_input_open( const char *path,
                                         x264pp_demuxer_t demux,
                                         const char *resolution,
                                         const char *csp_name,
                                         int bit_depth,
                                         x264pp_input_t **input,
                                         x264pp_input_info_t *info );

/* Read frame number index (0-based).  Returns 1 when a frame was delivered,
 * 0 at end of stream, -1 on malformed/truncated input.  Seeking forward is
 * implemented by reading and discarding intermediate frames, which also works
 * for stdin. */
int x264pp_input_read_frame( x264pp_input_t *input, x264pp_picture_t *pic,
                             int64_t index );

void x264pp_input_close( x264pp_input_t *input );

#endif /* X264PP_CLI_INPUT_H */
