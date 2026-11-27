/*
 * I0 encoder core stub.
 *
 * This is an honest placeholder, not a codec.  It implements the API contract
 * in core/api.h but deliberately emits no bitstream: the clean-room encoder
 * core has not been specified, reviewed or certified yet.  Increments I1..I5
 * replace this translation unit with the real implementation; the CLI and the
 * input/output modules do not change.
 */

#include "core/api.h"

const char *x264pp_codec_status( void )
{
    return "codec core not yet certified";
}

int x264pp_encoder_open( const x264pp_param_t *param, x264pp_encoder_t **enc )
{
    (void)param;

    if( enc )
        *enc = NULL;
    return X264PP_ERR_NOT_CERTIFIED;
}

int x264pp_encoder_encode( x264pp_encoder_t *enc, const x264pp_picture_t *pic,
                           int force_type, uint8_t *out, int out_capacity,
                           int *out_len, int *frame_type )
{
    (void)enc;
    (void)pic;
    (void)force_type;
    (void)out;
    (void)out_capacity;

    if( out_len )
        *out_len = 0;
    if( frame_type )
        *frame_type = X264PP_FRAME_IDR;
    return X264PP_ERR_NOT_CERTIFIED;
}

int x264pp_encoder_close( x264pp_encoder_t *enc )
{
    (void)enc;
    return X264PP_OK;
}
