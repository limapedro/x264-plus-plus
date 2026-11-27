#ifndef X264PP_CORE_API_H
#define X264PP_CORE_API_H

/*
 * Minimal encoder API contract for x264++.
 *
 * This header is original code written from spec/cli-blackbox.md and the
 * public H.264 surface.  It deliberately describes only the boundary the CLI
 * needs today; the encoder internals are added increment by increment (I1..I5)
 * behind this same contract.  I0 supplies an honest stub implementation in
 * api_stub.c: every entry point reports that the codec core is not certified.
 */

#include <stdint.h>

#define X264PP_API_VERSION 0

/* Status codes returned by the API.  Negative means failure. */
enum
{
    X264PP_OK               = 0,
    X264PP_ERR_NOT_CERTIFIED = -1,
    X264PP_ERR_INVALID       = -2
};

/* Colourspace identifiers used at the CLI/API boundary.
 * The names mirror the documented raw --input-csp vocabulary; only I420 is
 * actually decoded by the I0 input readers. */
typedef enum x264pp_csp
{
    X264PP_CSP_NONE = 0,
    X264PP_CSP_I400,
    X264PP_CSP_I420,
    X264PP_CSP_YV12,
    X264PP_CSP_NV12,
    X264PP_CSP_NV21,
    X264PP_CSP_I422,
    X264PP_CSP_YV16,
    X264PP_CSP_NV16,
    X264PP_CSP_YUYV,
    X264PP_CSP_UYVY,
    X264PP_CSP_I444,
    X264PP_CSP_YV24,
    X264PP_CSP_BGR,
    X264PP_CSP_BGRA,
    X264PP_CSP_RGB
} x264pp_csp_t;

/* Rate-control selection.  The defaults and interactions are specified in
 * spec/cli-blackbox.md, section 6.4. */
typedef enum x264pp_rc
{
    X264PP_RC_CRF = 0,
    X264PP_RC_CQP,
    X264PP_RC_ABR
} x264pp_rc_t;

/* Picture handed from the input module to the encoder.  Plane pointers are
 * owned by the input module and remain valid only until the next read or the
 * input is closed. */
typedef struct x264pp_picture
{
    x264pp_csp_t csp;
    int          width;
    int          height;
    int          bit_depth;
    uint8_t     *plane[4];
    int          stride[4];
    int64_t      pts;
    int64_t      duration;
} x264pp_picture_t;

/* Frame types the encoder may emit.  Values are API-local, not H.264 slice
 * type numbers. */
typedef enum x264pp_frame_type
{
    X264PP_FRAME_IDR = 0,
    X264PP_FRAME_I,
    X264PP_FRAME_P,
    X264PP_FRAME_B
} x264pp_frame_type_t;

/*
 * Encoder parameters.  Everything the CLI can set is collected here so that
 * later increments can refine the encoder without changing the front end.
 * A value of 0 in a "count" field means the documented automatic behaviour
 * (e.g. --threads 0, --frames 0), not "no threads" or "no frames".
 */
typedef struct x264pp_param
{
    /* Source / VUI geometry */
    int          width;
    int          height;
    x264pp_csp_t input_csp;
    x264pp_csp_t output_csp;
    int          bit_depth;
    int          output_depth;
    int          fps_num;
    int          fps_den;
    int          sar_num;
    int          sar_den;
    int          fullrange;

    /* Threading and scheduling */
    int          threads;
    int          lookahead_threads;
    int          sliced_threads;
    int          sync_lookahead;

    /* GOP / frame structure */
    int          keyint;          /* 0 == infinite */
    int          min_keyint;      /* 0 == automatic */
    int          scenecut;
    int          intra_refresh;
    int          bframes;
    int          b_adapt;
    int          b_bias;
    int          b_pyramid;
    int          open_gop;
    int          tff;
    int          bff;
    int          constrained_intra;
    int          fake_interlaced;

    /* Analysis / entropy */
    int          cabac;
    int          ref;
    int          deblock;         /* enabled flag */
    int          deblock_alpha;
    int          deblock_beta;
    int          partitions;
    int          direct;
    int          weightb;
    int          weightp;
    int          me;
    int          me_range;
    int          subme;
    int          trellis;
    int          psy;
    double       psy_rd;
    double       psy_trellis;
    int          dct8x8;
    int          mixed_refs;
    int          chroma_me;
    int          fast_pskip;
    int          dct_decimate;
    int          chroma_qp_offset;
    int          nr;
    int          deadzone_inter;
    int          deadzone_intra;
    int          cqm;

    /* Slice control (accepted by the parser, not used in I0) */
    int          slices;
    int          slices_max;
    int          slice_max_size;
    int          slice_max_mbs;
    int          slice_min_mbs;

    /* Rate control */
    x264pp_rc_t  rc;
    double       crf;
    int          qp;
    int          bitrate;
    int          qpmin;
    int          qpmax;
    int          qpstep;
    double       qcomp;
    double       ipratio;
    double       pbratio;
    double       ratetol;
    int          aq_mode;
    double       aq_strength;
    int          mbtree;
    int          rc_lookahead;
    int          pass;
    int          vbv_maxrate;
    int          vbv_bufsize;
    double       vbv_init;

    /* CLI-side controls */
    int          frames;         /* 0 == all frames */
    int          seek;
    int          log_level;
    int          verbose;
    int          no_progress;
    int          quiet;
    int          aud;
    int          sps_id;
    int          stitchable;

    char         preset[32];
    char         tune[128];
    char         profile[32];
    char         stats[260];
    char         zones[512];
    char         qpfile[260];
} x264pp_param_t;

/* Opaque encoder instance.  Its layout belongs to the implementation that
 * certifies the codec core; the CLI never looks inside it. */
typedef struct x264pp_encoder x264pp_encoder_t;

/* Open an encoder from a fully resolved parameter block.
 * Returns X264PP_OK and sets *enc on success.  I0 always returns
 * X264PP_ERR_NOT_CERTIFIED and sets *enc to NULL. */
int x264pp_encoder_open( const x264pp_param_t *param, x264pp_encoder_t **enc );

/* Encode one input picture into an Annex B byte buffer.
 * out_capacity bounds the write; on success *out_len is the number of bytes
 * produced and *frame_type receives the emitted frame type.  The stub writes
 * no bytes and returns X264PP_ERR_NOT_CERTIFIED. */
int x264pp_encoder_encode( x264pp_encoder_t *enc, const x264pp_picture_t *pic,
                           int force_type, uint8_t *out, int out_capacity,
                           int *out_len, int *frame_type );

/* Flush delayed pictures, then release all encoder state. */
int x264pp_encoder_close( x264pp_encoder_t *enc );

/* Short, human-readable readiness string used by the CLI for its honest
 * "not yet certified" diagnostic. */
const char *x264pp_codec_status( void );

#endif /* X264PP_CORE_API_H */
