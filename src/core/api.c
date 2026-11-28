/*
 * Encoder API implementation for I1.  Replaces the I0 stub without changing
 * the CLI-facing contract in core/api.h.
 *
 * Original code for x264++; written only from the rebuild specifications and
 * the public H.264 syntax description.
 */

#include "core/api.h"
#include "core/encode.h"
#include "core/params.h"

const char *x264pp_codec_status( void )
{
#if X264PP_INTER_P
    return "I2 CAVLC I/P-slice encoder";
#else
    return "I1 CAVLC I-slice encoder";
#endif
}

int x264pp_encoder_open( const x264pp_param_t *param, x264pp_encoder_t **enc )
{
    x264pp_param_t resolved;
    x264pp_encoder_t *e;

    if( enc )
        *enc = NULL;
    if( !param || !enc )
        return X264PP_ERR_INVALID;
    if( x264pp_params_validate( param, &resolved ) != X264PP_OK )
        return X264PP_ERR_INVALID;

    e = x264pp_encoder_create( &resolved );
    if( !e )
        return X264PP_ERR_INVALID;
    *enc = e;
    return X264PP_OK;
}

int x264pp_encoder_encode( x264pp_encoder_t *enc, const x264pp_picture_t *pic,
                           int force_type, uint8_t *out, int out_capacity,
                           int *out_len, int *frame_type )
{
    (void)force_type;   /* no frame delay; every picture is emitted at once */
    if( out_len )
        *out_len = 0;
    if( frame_type )
        *frame_type =
#if X264PP_INTER_P
            ( enc && enc->frame_num > 0 )
            ? X264PP_FRAME_P : X264PP_FRAME_IDR;
#else
            X264PP_FRAME_IDR;
#endif
    if( !enc )
        return X264PP_ERR_INVALID;
    if( !pic )
        return X264PP_OK;   /* flush: no delayed frames */

    if( !out || out_capacity <= 0 )
        return X264PP_ERR_INVALID;
    return x264pp_encoder_encode_frame( enc, pic, out, out_capacity, out_len );
}

int x264pp_encoder_close( x264pp_encoder_t *enc )
{
    x264pp_encoder_destroy( enc );
    return X264PP_OK;
}
