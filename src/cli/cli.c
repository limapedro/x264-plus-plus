/*
 * x264++ command-line front end (increment I0).
 *
 * The parser is original code written from spec/cli-blackbox.md.  It accepts
 * GNU-style long options in both "--name value" and "--name=value" form,
 * short aliases, options before or after the input filename, "--" as an
 * option terminator, and the defaults -> preset -> tune -> explicit precedence
 * documented in E2.
 *
 * All diagnostics go to stderr with the "<component> [level]: " prefix; help
 * and version text goes to stdout.  Every failure exits -1.  Because the I0
 * codec core is an honest stub, the encode loop is present but unreachable:
 * the CLI reports "codec core not yet certified" before any output is opened.
 */

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "cli/cli.h"
#include "cli/input.h"
#include "cli/output.h"

static int g_log_level = X264PP_LOG_INFO;

void x264pp_log( int level, const char *component, const char *fmt, ... )
{
    static const char *const names[5] =
        { "none", "error", "warning", "info", "debug" };
    va_list ap;

    if( level <= X264PP_LOG_NONE || level > g_log_level ) return;
    if( level < 0 || level > X264PP_LOG_DEBUG ) return;

    fprintf( stderr, "%s [%s]: ", component, names[level] );
    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );
    fputc( '\n', stderr );
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *dup_string( const char *s )
{
    size_t n = strlen( s ) + 1;
    char *p = (char *)malloc( n );
    if( p ) memcpy( p, s, n );
    return p;
}

static int clamp_int( int v, int lo, int hi )
{
    if( v < lo ) return lo;
    if( v > hi ) return hi;
    return v;
}

static double clamp_double( double v, double lo, double hi )
{
    if( v < lo ) return lo;
    if( v > hi ) return hi;
    return v;
}

static int parse_int( const char *s, int *out )
{
    char *end;
    long v;

    if( !s || !*s ) return -1;
    v = strtol( s, &end, 10 );
    if( end == s || *end != '\0' ) return -1;
    if( v < INT_MIN || v > INT_MAX ) return -1;
    *out = (int)v;
    return 0;
}

static int parse_double( const char *s, double *out )
{
    char *end;
    double v;

    if( !s || !*s ) return -1;
    v = strtod( s, &end );
    if( end == s || *end != '\0' ) return -1;
    *out = v;
    return 0;
}

static uint64_t gcd_u64( uint64_t a, uint64_t b )
{
    while( b )
    {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int parse_ratio( const char *s, int *num, int *den )
{
    const char *slash = strchr( s, '/' );
    int n = 0, d = 0;
    uint64_t g;

    if( slash )
    {
        if( sscanf( s, "%d/%d", &n, &d ) != 2 || d <= 0 ) return -1;
    }
    else
    {
        double f;
        if( parse_double( s, &f ) != 0 || f <= 0.0 ) return -1;
        if( f > 1000000.0 ) return -1;
        n = (int)( f * 1001.0 + 0.5 );
        d = 1001;
    }
    if( n < 0 || d <= 0 ) return -1;
    g = gcd_u64( (uint64_t)n, (uint64_t)d );
    if( g > 1 )
    {
        n = (int)( (uint64_t)n / g );
        d = (int)( (uint64_t)d / g );
    }
    *num = n;
    *den = d;
    return 0;
}

static int invalid_argument( const char *name, const char *value )
{
    x264pp_log( X264PP_LOG_ERROR, "x264", "invalid argument: %s = %s",
                name, value ? value : "" );
    return -1;
}

static int choice_ok( const char *value, const char *const *names )
{
    int i;
    for( i = 0; names[i]; i++ )
        if( strcmp( value, names[i] ) == 0 ) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Option table                                                       */
/* ------------------------------------------------------------------ */

enum
{
    OPT_NONE = 0,
    OPT_HELP, OPT_LONGHELP, OPT_FULLHELP, OPT_VERSION,
    OPT_OUTPUT, OPT_MUXER, OPT_DEMUXER,
    OPT_PRESET, OPT_TUNE, OPT_PROFILE,
    OPT_FRAMES, OPT_SEEK, OPT_FPS, OPT_SAR,
    OPT_INPUT_RES, OPT_INPUT_CSP, OPT_INPUT_DEPTH,
    OPT_OUTPUT_CSP, OPT_OUTPUT_DEPTH, OPT_TIMEBASE,
    OPT_CRF, OPT_QP, OPT_BITRATE,
    OPT_KEYINT, OPT_MIN_KEYINT, OPT_SCENECUT, OPT_NO_SCENECUT,
    OPT_THREADS, OPT_LOOKAHEAD_THREADS, OPT_SYNC_LOOKAHEAD,
    OPT_INTRA_REFRESH, OPT_BFRAMES, OPT_B_ADAPT, OPT_B_BIAS,
    OPT_B_PYRAMID, OPT_OPEN_GOP, OPT_NO_CABAC,
    OPT_REF, OPT_DEBLOCK, OPT_NO_DEBLOCK,
    OPT_PARTITIONS, OPT_DIRECT, OPT_NO_WEIGHTB, OPT_WEIGHTP,
    OPT_ME, OPT_MERANGE, OPT_SUBME, OPT_PSY_RD, OPT_NO_PSY,
    OPT_NO_MIXED_REFS, OPT_NO_CHROMA_ME, OPT_NO_DCT8X8,
    OPT_TRELLIS, OPT_NO_FAST_PSKIP, OPT_NO_DCT_DECIMATE,
    OPT_NR, OPT_DEADZONE_INTER, OPT_DEADZONE_INTRA,
    OPT_QP_MIN, OPT_QP_MAX, OPT_QP_STEP, OPT_QCOMP,
    OPT_IPRATIO, OPT_PBRATIO, OPT_RATETOL,
    OPT_AQ_MODE, OPT_AQ_STRENGTH, OPT_NO_MBTREE, OPT_RC_LOOKAHEAD,
    OPT_PASS, OPT_STATS, OPT_ZONES, OPT_QPFILE,
    OPT_LOG_LEVEL, OPT_VERBOSE, OPT_QUIET, OPT_NO_PROGRESS,
    OPT_TFF, OPT_BFF, OPT_CONSTRAINED_INTRA, OPT_AUD,
    OPT_SPS_ID, OPT_STITCHABLE,
    OPT_IGNORE_FLAG, OPT_IGNORE_INT, OPT_IGNORE_FLOAT, OPT_IGNORE_STR
};

typedef struct option_def
{
    const char *name;
    char        short_name;  /* 0 when there is no short alias */
    int         has_arg;
    int         id;
} option_def_t;

static const option_def_t option_table[] =
{
    { "help", 'h', 0, OPT_HELP },
    { "longhelp", 0, 0, OPT_LONGHELP },
    { "fullhelp", 0, 0, OPT_FULLHELP },
    { "version", 0, 0, OPT_VERSION },
    { "verbose", 'v', 0, OPT_VERBOSE },
    { "quiet", 0, 0, OPT_QUIET },
    { "no-progress", 0, 0, OPT_NO_PROGRESS },
    { "log-level", 0, 1, OPT_LOG_LEVEL },

    { "output", 'o', 1, OPT_OUTPUT },
    { "muxer", 0, 1, OPT_MUXER },
    { "demuxer", 0, 1, OPT_DEMUXER },
    { "frames", 0, 1, OPT_FRAMES },
    { "seek", 0, 1, OPT_SEEK },
    { "fps", 0, 1, OPT_FPS },
    { "sar", 0, 1, OPT_SAR },
    { "input-res", 0, 1, OPT_INPUT_RES },
    { "input-csp", 0, 1, OPT_INPUT_CSP },
    { "input-depth", 0, 1, OPT_INPUT_DEPTH },
    { "input-range", 0, 1, OPT_IGNORE_STR },
    { "output-csp", 0, 1, OPT_OUTPUT_CSP },
    { "output-depth", 0, 1, OPT_OUTPUT_DEPTH },
    { "timebase", 0, 1, OPT_TIMEBASE },
    { "index", 0, 1, OPT_IGNORE_STR },
    { "level", 0, 1, OPT_IGNORE_STR },

    { "preset", 0, 1, OPT_PRESET },
    { "tune", 0, 1, OPT_TUNE },
    { "profile", 0, 1, OPT_PROFILE },

    { "crf", 0, 1, OPT_CRF },
    { "qp", 'q', 1, OPT_QP },
    { "bitrate", 'B', 1, OPT_BITRATE },
    { "qpmin", 0, 1, OPT_QP_MIN },
    { "qpmax", 0, 1, OPT_QP_MAX },
    { "qpstep", 0, 1, OPT_QP_STEP },
    { "qcomp", 0, 1, OPT_QCOMP },
    { "ipratio", 0, 1, OPT_IPRATIO },
    { "pbratio", 0, 1, OPT_PBRATIO },
    { "ratetol", 0, 1, OPT_RATETOL },
    { "aq-mode", 0, 1, OPT_AQ_MODE },
    { "aq-strength", 0, 1, OPT_AQ_STRENGTH },
    { "no-mbtree", 0, 0, OPT_NO_MBTREE },
    { "rc-lookahead", 0, 1, OPT_RC_LOOKAHEAD },
    { "cplxblur", 0, 1, OPT_IGNORE_FLOAT },
    { "qblur", 0, 1, OPT_IGNORE_FLOAT },
    { "crf-max", 0, 1, OPT_IGNORE_FLOAT },
    { "vbv-maxrate", 0, 1, OPT_IGNORE_INT },
    { "vbv-bufsize", 0, 1, OPT_IGNORE_INT },
    { "vbv-init", 0, 1, OPT_IGNORE_FLOAT },
    { "pass", 'p', 1, OPT_PASS },
    { "stats", 0, 1, OPT_STATS },
    { "zones", 0, 1, OPT_ZONES },
    { "qpfile", 0, 1, OPT_QPFILE },

    { "keyint", 'I', 1, OPT_KEYINT },
    { "min-keyint", 'i', 1, OPT_MIN_KEYINT },
    { "scenecut", 0, 1, OPT_SCENECUT },
    { "no-scenecut", 0, 0, OPT_NO_SCENECUT },
    { "intra-refresh", 0, 0, OPT_INTRA_REFRESH },
    { "bframes", 'b', 1, OPT_BFRAMES },
    { "b-adapt", 0, 1, OPT_B_ADAPT },
    { "b-bias", 0, 1, OPT_B_BIAS },
    { "b-pyramid", 0, 1, OPT_B_PYRAMID },
    { "open-gop", 0, 0, OPT_OPEN_GOP },
    { "no-cabac", 0, 0, OPT_NO_CABAC },
    { "ref", 'r', 1, OPT_REF },
    { "deblock", 'f', 1, OPT_DEBLOCK },
    { "no-deblock", 0, 0, OPT_NO_DEBLOCK },
    { "tff", 0, 0, OPT_TFF },
    { "bff", 0, 0, OPT_BFF },
    { "constrained-intra", 0, 0, OPT_CONSTRAINED_INTRA },
    { "fake-interlaced", 0, 0, OPT_IGNORE_FLAG },
    { "pulldown", 0, 1, OPT_IGNORE_STR },
    { "frame-packing", 0, 1, OPT_IGNORE_INT },
    { "slices", 0, 1, OPT_IGNORE_INT },
    { "slices-max", 0, 1, OPT_IGNORE_INT },
    { "slice-max-size", 0, 1, OPT_IGNORE_INT },
    { "slice-max-mbs", 0, 1, OPT_IGNORE_INT },
    { "slice-min-mbs", 0, 1, OPT_IGNORE_INT },

    { "partitions", 'A', 1, OPT_PARTITIONS },
    { "direct", 0, 1, OPT_DIRECT },
    { "weightp", 0, 1, OPT_WEIGHTP },
    { "no-weightb", 0, 0, OPT_NO_WEIGHTB },
    { "me", 0, 1, OPT_ME },
    { "merange", 0, 1, OPT_MERANGE },
    { "mvrange", 0, 1, OPT_IGNORE_INT },
    { "mvrange-thread", 0, 1, OPT_IGNORE_INT },
    { "subme", 'm', 1, OPT_SUBME },
    { "psy-rd", 0, 1, OPT_PSY_RD },
    { "no-psy", 0, 0, OPT_NO_PSY },
    { "no-mixed-refs", 0, 0, OPT_NO_MIXED_REFS },
    { "no-chroma-me", 0, 0, OPT_NO_CHROMA_ME },
    { "no-8x8dct", 0, 0, OPT_NO_DCT8X8 },
    { "trellis", 't', 1, OPT_TRELLIS },
    { "no-fast-pskip", 0, 0, OPT_NO_FAST_PSKIP },
    { "no-dct-decimate", 0, 0, OPT_NO_DCT_DECIMATE },
    { "nr", 0, 1, OPT_NR },
    { "deadzone-inter", 0, 1, OPT_DEADZONE_INTER },
    { "deadzone-intra", 0, 1, OPT_DEADZONE_INTRA },
    { "cqm", 0, 1, OPT_IGNORE_STR },
    { "cqmfile", 0, 1, OPT_IGNORE_STR },

    { "overscan", 0, 1, OPT_IGNORE_STR },
    { "videoformat", 0, 1, OPT_IGNORE_STR },
    { "range", 0, 1, OPT_IGNORE_STR },
    { "colorprim", 0, 1, OPT_IGNORE_STR },
    { "transfer", 0, 1, OPT_IGNORE_STR },
    { "colormatrix", 0, 1, OPT_IGNORE_STR },
    { "chromaloc", 0, 1, OPT_IGNORE_INT },
    { "mastering-display", 0, 1, OPT_IGNORE_STR },
    { "cll", 0, 1, OPT_IGNORE_STR },
    { "alternative-transfer", 0, 1, OPT_IGNORE_STR },
    { "nal-hrd", 0, 1, OPT_IGNORE_STR },
    { "filler", 0, 0, OPT_IGNORE_FLAG },
    { "pic-struct", 0, 0, OPT_IGNORE_FLAG },
    { "crop-rect", 0, 1, OPT_IGNORE_STR },

    { "threads", 0, 1, OPT_THREADS },
    { "lookahead-threads", 0, 1, OPT_LOOKAHEAD_THREADS },
    { "sliced-threads", 0, 0, OPT_IGNORE_FLAG },
    { "thread-input", 0, 0, OPT_IGNORE_FLAG },
    { "sync-lookahead", 0, 1, OPT_SYNC_LOOKAHEAD },
    { "non-deterministic", 0, 0, OPT_IGNORE_FLAG },
    { "cpu-independent", 0, 0, OPT_IGNORE_FLAG },
    { "asm", 0, 1, OPT_IGNORE_INT },
    { "no-asm", 0, 0, OPT_IGNORE_FLAG },
    { "opencl", 0, 0, OPT_IGNORE_FLAG },
    { "opencl-clbin", 0, 1, OPT_IGNORE_STR },
    { "opencl-device", 0, 1, OPT_IGNORE_INT },
    { "dump-yuv", 0, 1, OPT_IGNORE_STR },
    { "aud", 0, 0, OPT_AUD },
    { "force-cfr", 0, 0, OPT_IGNORE_FLAG },
    { "tcfile-in", 0, 1, OPT_IGNORE_STR },
    { "tcfile-out", 0, 1, OPT_IGNORE_STR },
    { "dts-compress", 0, 0, OPT_IGNORE_FLAG },
    { "vf", 0, 1, OPT_IGNORE_STR },
    { "video-filter", 0, 1, OPT_IGNORE_STR },
    { "stitchable", 0, 0, OPT_STITCHABLE },
    { "bluray-compat", 0, 0, OPT_IGNORE_FLAG },
    { "avcintra-class", 0, 1, OPT_IGNORE_INT },
    { "avcintra-flavor", 0, 1, OPT_IGNORE_STR },
    { NULL, 0, 0, OPT_NONE }
};

static const option_def_t *find_long_option( const char *name )
{
    int i;
    for( i = 0; option_table[i].name; i++ )
        if( strcmp( option_table[i].name, name ) == 0 ) return &option_table[i];
    return NULL;
}

static const option_def_t *find_short_option( char c )
{
    int i;
    for( i = 0; option_table[i].name; i++ )
        if( option_table[i].short_name == c ) return &option_table[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CLI state and deferred explicit options                             */
/* ------------------------------------------------------------------ */

typedef struct option_event
{
    int   id;
    char *value;
} option_event_t;

typedef struct option_events
{
    option_event_t *items;
    size_t          count;
    size_t          capacity;
} option_events_t;

typedef struct cli_state
{
    x264pp_param_t param;
    char     input_path[1024];
    char     output_path[1024];
    char     resolution[64];
    char     csp_name[64];
    char     preset[32];
    char     tune[256];
    x264pp_demuxer_t demux;
    x264pp_muxer_t muxer;
    int      frames;
    int      seek;
    int      fps_num;
    int      fps_den;
    int      sar_num;
    int      sar_den;
    int      fps_set;
    int      sar_set;
    int      bit_depth;
    int      log_level;
    int      timebase_set;
    int      want_version;
    int      want_help;
    int      want_fullhelp;
    int      have_input;
    int      have_output;
    int      no_progress;
} cli_state_t;

static int events_add( option_events_t *ev, int id, const char *value )
{
    option_event_t *items;

    if( ev->count == ev->capacity )
    {
        size_t cap = ev->capacity ? ev->capacity * 2 : 32;
        items = (option_event_t *)realloc( ev->items, cap * sizeof( *items ) );
        if( !items ) return -1;
        ev->items = items;
        ev->capacity = cap;
    }
    ev->items[ev->count].id = id;
    ev->items[ev->count].value = value ? dup_string( value ) : NULL;
    if( value && !ev->items[ev->count].value ) return -1;
    ev->count++;
    return 0;
}

static void events_clean( option_events_t *ev )
{
    size_t i;
    for( i = 0; i < ev->count; i++ ) free( ev->items[i].value );
    free( ev->items );
    ev->items = NULL;
    ev->count = ev->capacity = 0;
}

static void param_defaults( x264pp_param_t *p )
{
    memset( p, 0, sizeof( *p ) );
    p->input_csp      = X264PP_CSP_I420;
    p->output_csp     = X264PP_CSP_I420;
    p->bit_depth      = 8;
    p->output_depth   = 8;
    p->fps_num        = 25;
    p->fps_den        = 1;
    p->threads        = 0;
    p->keyint         = 250;
    p->scenecut       = 40;
    p->bframes        = 3;
    p->b_adapt        = 1;
    p->b_pyramid      = 2;
    p->weightb        = 1;
    p->weightp        = 2;
    p->cabac          = 1;
    p->ref            = 3;
    p->deblock        = 1;
    p->partitions     = 0x113;
    p->direct         = 1;
    p->me             = 1;         /* hex */
    p->me_range       = 16;
    p->subme          = 7;
    p->trellis        = 1;
    p->psy            = 1;
    p->psy_rd         = 1.0;
    p->dct8x8         = 1;
    p->mixed_refs     = 1;
    p->chroma_me      = 1;
    p->fast_pskip     = 1;
    p->dct_decimate   = 1;
    p->deadzone_inter = 21;
    p->deadzone_intra = 11;
    p->rc             = X264PP_RC_CRF;
    p->crf            = 23.0;
    p->qpmin          = 0;
    p->qpmax          = 69;
    p->qpstep         = 4;
    p->qcomp          = 0.60;
    p->ipratio        = 1.40;
    p->pbratio        = 1.30;
    p->ratetol        = 1.0;
    p->aq_mode        = 1;
    p->aq_strength    = 1.0;
    p->mbtree         = 1;
    p->rc_lookahead   = 40;
    p->log_level      = X264PP_LOG_INFO;
    strcpy( p->preset, "medium" );
}

/* ------------------------------------------------------------------ */
/* Presets and tunes: defaults -> preset -> tune -> explicit            */
/* ------------------------------------------------------------------ */

static int apply_preset( x264pp_param_t *p, const char *name )
{
    if( strcmp( name, "ultrafast" ) == 0 )
    {
        p->cabac = 0; p->ref = 1; p->deblock = 0; p->partitions = 0;
        p->me = 0; p->subme = 0; p->trellis = 0; p->dct8x8 = 0;
        p->mixed_refs = 0; p->bframes = 0; p->weightp = 0;
        p->scenecut = 0; p->mbtree = 0; p->rc_lookahead = 0;
        p->aq_mode = 0; p->aq_strength = 0.0;
    }
    else if( strcmp( name, "superfast" ) == 0 )
    {
        p->ref = 1; p->me = 0; p->subme = 1; p->trellis = 0;
        p->mixed_refs = 0; p->weightp = 1; p->mbtree = 0;
    }
    else if( strcmp( name, "veryfast" ) == 0 )
    {
        p->ref = 1; p->mixed_refs = 0; p->subme = 2; p->trellis = 0;
        p->weightp = 1; p->rc_lookahead = 10;
    }
    else if( strcmp( name, "faster" ) == 0 )
    {
        p->ref = 2; p->mixed_refs = 0; p->subme = 4;
        p->weightp = 1; p->rc_lookahead = 20;
    }
    else if( strcmp( name, "fast" ) == 0 )
    {
        p->ref = 2; p->subme = 6; p->weightp = 1; p->rc_lookahead = 30;
    }
    else if( strcmp( name, "medium" ) == 0 )
    {
        /* The baseline; no changes. */
    }
    else if( strcmp( name, "slow" ) == 0 )
    {
        p->ref = 5; p->subme = 8; p->trellis = 2; p->direct = 3;
        p->rc_lookahead = 50;
    }
    else if( strcmp( name, "slower" ) == 0 )
    {
        p->ref = 8; p->partitions = 0x133; p->me = 2; p->subme = 9;
        p->trellis = 2; p->b_adapt = 2; p->direct = 3;
        p->rc_lookahead = 60;
    }
    else if( strcmp( name, "veryslow" ) == 0 )
    {
        p->ref = 16; p->me = 2; p->me_range = 24; p->subme = 10;
        p->bframes = 8; p->b_adapt = 2; p->direct = 3;
        p->rc_lookahead = 60;
    }
    else if( strcmp( name, "placebo" ) == 0 )
    {
        p->ref = 16; p->me = 4; p->me_range = 24; p->subme = 11;
        p->bframes = 16; p->b_adapt = 2; p->direct = 3;
        p->fast_pskip = 0; p->rc_lookahead = 60;
    }
    else
    {
        x264pp_log( X264PP_LOG_ERROR, "x264", "invalid preset '%s'", name );
        return -1;
    }
    strcpy( p->preset, name );
    return 0;
}

static int apply_tune( x264pp_param_t *p, const char *name )
{
    if( strcmp( name, "film" ) == 0 )
    {
        p->deblock = 1; p->deblock_alpha = -1; p->deblock_beta = -1;
        p->psy_rd = 1.0; p->psy_trellis = 0.15; p->chroma_qp_offset = -3;
    }
    else if( strcmp( name, "animation" ) == 0 )
    {
        p->ref = 6; p->bframes = 5; p->deblock = 1;
        p->deblock_alpha = 1; p->deblock_beta = 1;
        p->psy_rd = 0.40; p->psy_trellis = 0.0; p->aq_strength = 0.60;
    }
    else if( strcmp( name, "grain" ) == 0 )
    {
        p->deblock = 1; p->deblock_alpha = -2; p->deblock_beta = -2;
        p->psy_rd = 1.0; p->psy_trellis = 0.25;
        p->deadzone_inter = 6; p->deadzone_intra = 6;
        p->dct_decimate = 0; p->qcomp = 0.80; p->ipratio = 1.10;
        p->aq_strength = 0.50; p->chroma_qp_offset = -4;
    }
    else if( strcmp( name, "stillimage" ) == 0 )
    {
        p->deblock = 1; p->deblock_alpha = -3; p->deblock_beta = -3;
        p->psy_rd = 2.0; p->psy_trellis = 0.70;
        p->aq_strength = 1.20; p->chroma_qp_offset = -4;
    }
    else if( strcmp( name, "psnr" ) == 0 )
    {
        p->psy = 0; p->aq_strength = 0.0; p->chroma_qp_offset = 0;
    }
    else if( strcmp( name, "ssim" ) == 0 )
    {
        p->psy = 0; p->aq_mode = 2; p->chroma_qp_offset = 0;
    }
    else if( strcmp( name, "fastdecode" ) == 0 )
    {
        p->cabac = 0; p->deblock = 0; p->weightb = 0; p->weightp = 0;
    }
    else if( strcmp( name, "zerolatency" ) == 0 )
    {
        p->bframes = 0; p->mbtree = 0; p->threads = 1;
        p->rc_lookahead = 0;
    }
    else
    {
        x264pp_log( X264PP_LOG_ERROR, "x264", "invalid tune '%s'", name );
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Explicit option application                                         */
/* ------------------------------------------------------------------ */

static int apply_option( x264pp_param_t *p, int id, const char *value )
{
    static const char *const me_names[] =
        { "dia", "hex", "umh", "esa", "tesa", NULL };
    static const char *const pyramid_names[] =
        { "none", "strict", "normal", NULL };
    static const char *const direct_names[] =
        { "none", "spatial", "temporal", "auto", NULL };
    static const char *const log_names[] =
        { "none", "error", "warning", "info", "debug", NULL };
    static const char *const profile_names[] =
        { "baseline", "main", "high", "high10", "high422", "high444", NULL };
    static const char *const csp_names[] =
        { "i400", "i420", "yv12", "nv12", "nv21", "i422", "yv16",
          "nv16", "yuyv", "uyvy", "i444", "yv24", "bgr", "bgra",
          "rgb", NULL };
    int v = 0;
    double d = 0.0;

    switch( id )
    {
    case OPT_IGNORE_FLAG: return 0;
    case OPT_IGNORE_INT:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "option", value );
        return 0;
    case OPT_IGNORE_FLOAT:
        if( parse_double( value, &d ) != 0 ) return invalid_argument( "option", value );
        return 0;
    case OPT_IGNORE_STR: return 0;

    case OPT_CRF:
        if( parse_double( value, &d ) != 0 ) return invalid_argument( "crf", value );
        p->rc = X264PP_RC_CRF;
        p->crf = clamp_double( d, -12.0, 51.0 );
        return 0;
    case OPT_QP:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "qp", value );
        p->rc = X264PP_RC_CQP;
        p->qp = clamp_int( v, 0, 81 );
        return 0;
    case OPT_BITRATE:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "bitrate", value );
        p->rc = X264PP_RC_ABR;
        p->bitrate = v < 0 ? 0 : v;
        return 0;

    case OPT_KEYINT:
        if( strcmp( value, "infinite" ) == 0 ) p->keyint = 0;
        else
        {
            if( parse_int( value, &v ) != 0 ) return invalid_argument( "keyint", value );
            p->keyint = v < 0 ? 0 : v;
        }
        return 0;
    case OPT_MIN_KEYINT:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "min-keyint", value );
        p->min_keyint = v < 0 ? 0 : v;
        return 0;
    case OPT_SCENECUT:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "scenecut", value );
        p->scenecut = clamp_int( v, 0, 100 );
        return 0;
    case OPT_NO_SCENECUT: p->scenecut = 0; return 0;
    case OPT_INTRA_REFRESH: p->intra_refresh = 1; return 0;
    case OPT_BFRAMES:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "bframes", value );
        p->bframes = clamp_int( v, 0, 16 );
        return 0;
    case OPT_B_ADAPT:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "b-adapt", value );
        p->b_adapt = clamp_int( v, 0, 2 );
        return 0;
    case OPT_B_BIAS:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "b-bias", value );
        p->b_bias = clamp_int( v, -100, 100 );
        return 0;
    case OPT_B_PYRAMID:
        if( !choice_ok( value, pyramid_names ) )
            return invalid_argument( "b-pyramid", value );
        p->b_pyramid = strcmp( value, "none" ) == 0 ? 0 :
                       strcmp( value, "strict" ) == 0 ? 1 : 2;
        return 0;
    case OPT_OPEN_GOP: p->open_gop = 1; return 0;
    case OPT_NO_CABAC: p->cabac = 0; return 0;
    case OPT_REF:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "ref", value );
        p->ref = clamp_int( v, 1, 16 );
        if( p->ref == 1 ) p->mixed_refs = 0;
        return 0;
    case OPT_DEBLOCK:
        if( sscanf( value, "%d:%d", &p->deblock_alpha, &p->deblock_beta ) != 2 )
            return invalid_argument( "deblock", value );
        p->deblock_alpha = clamp_int( p->deblock_alpha, -6, 6 );
        p->deblock_beta = clamp_int( p->deblock_beta, -6, 6 );
        p->deblock = 1;
        return 0;
    case OPT_NO_DEBLOCK: p->deblock = 0; return 0;

    case OPT_THREADS:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "threads", value );
        p->threads = v < 0 ? 0 : v;
        return 0;
    case OPT_LOOKAHEAD_THREADS:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "lookahead-threads", value );
        p->lookahead_threads = v < 0 ? 0 : v;
        return 0;
    case OPT_SYNC_LOOKAHEAD:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "sync-lookahead", value );
        p->sync_lookahead = v < -1 ? -1 : v;
        return 0;

    case OPT_PARTITIONS:
        if( strcmp( value, "all" ) == 0 ) p->partitions = 0x133;
        else p->partitions = 0x113;
        return 0;
    case OPT_DIRECT:
        if( !choice_ok( value, direct_names ) )
            return invalid_argument( "direct", value );
        p->direct = (int)( value[0] == 'n' ? 0 : value[0] == 's' ? 1 :
                           value[0] == 't' ? 2 : 3 );
        return 0;
    case OPT_WEIGHTP:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "weightp", value );
        p->weightp = clamp_int( v, 0, 2 );
        return 0;
    case OPT_NO_WEIGHTB: p->weightb = 0; return 0;
    case OPT_ME:
        if( !choice_ok( value, me_names ) ) return invalid_argument( "me", value );
        p->me = (int)( value[0] == 'd' ? 0 : value[0] == 'h' ? 1 :
                       value[0] == 'u' ? 2 : value[0] == 'e' ? 3 : 4 );
        return 0;
    case OPT_MERANGE:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "merange", value );
        p->me_range = clamp_int( v, 0, 64 );
        return 0;
    case OPT_SUBME:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "subme", value );
        p->subme = clamp_int( v, 0, 11 );
        return 0;
    case OPT_PSY_RD:
        if( sscanf( value, "%lf:%lf", &p->psy_rd, &p->psy_trellis ) != 2 &&
            parse_double( value, &p->psy_rd ) != 0 )
            return invalid_argument( "psy-rd", value );
        p->psy_rd = clamp_double( p->psy_rd, 0.0, 2.0 );
        return 0;
    case OPT_NO_PSY: p->psy = 0; p->chroma_qp_offset = 0; return 0;
    case OPT_NO_MIXED_REFS: p->mixed_refs = 0; return 0;
    case OPT_NO_CHROMA_ME: p->chroma_me = 0; return 0;
    case OPT_NO_DCT8X8: p->dct8x8 = 0; return 0;
    case OPT_TRELLIS:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "trellis", value );
        p->trellis = clamp_int( v, 0, 2 );
        return 0;
    case OPT_NO_FAST_PSKIP: p->fast_pskip = 0; return 0;
    case OPT_NO_DCT_DECIMATE: p->dct_decimate = 0; return 0;
    case OPT_NR:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "nr", value );
        p->nr = clamp_int( v, 0, 2000 );
        return 0;
    case OPT_DEADZONE_INTER:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "deadzone-inter", value );
        p->deadzone_inter = v < 0 ? 0 : v;
        return 0;
    case OPT_DEADZONE_INTRA:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "deadzone-intra", value );
        p->deadzone_intra = v < 0 ? 0 : v;
        return 0;

    case OPT_QP_MIN:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "qpmin", value );
        p->qpmin = clamp_int( v, 0, 81 );
        return 0;
    case OPT_QP_MAX:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "qpmax", value );
        p->qpmax = clamp_int( v, 0, 81 );
        return 0;
    case OPT_QP_STEP:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "qpstep", value );
        p->qpstep = clamp_int( v, 0, 81 );
        return 0;
    case OPT_QCOMP:
        if( parse_double( value, &d ) != 0 ) return invalid_argument( "qcomp", value );
        p->qcomp = clamp_double( d, 0.0, 1.0 );
        return 0;
    case OPT_IPRATIO:
        if( parse_double( value, &d ) != 0 ) return invalid_argument( "ipratio", value );
        p->ipratio = d < 0.0 ? 0.0 : d;
        return 0;
    case OPT_PBRATIO:
        if( parse_double( value, &d ) != 0 ) return invalid_argument( "pbratio", value );
        p->pbratio = d < 0.0 ? 0.0 : d;
        return 0;
    case OPT_RATETOL:
        if( parse_double( value, &d ) != 0 ) return invalid_argument( "ratetol", value );
        p->ratetol = d < 0.0 ? 0.0 : d;
        return 0;
    case OPT_AQ_MODE:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "aq-mode", value );
        p->aq_mode = clamp_int( v, 0, 3 );
        return 0;
    case OPT_AQ_STRENGTH:
        if( parse_double( value, &d ) != 0 ) return invalid_argument( "aq-strength", value );
        p->aq_strength = clamp_double( d, 0.0, 2.0 );
        return 0;
    case OPT_NO_MBTREE: p->mbtree = 0; return 0;
    case OPT_RC_LOOKAHEAD:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "rc-lookahead", value );
        p->rc_lookahead = v < 0 ? 0 : v;
        if( p->rc_lookahead == 0 ) p->mbtree = 0;
        return 0;
    case OPT_PASS:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "pass", value );
        p->pass = clamp_int( v, 0, 3 );
        return 0;
    case OPT_STATS:
        strncpy( p->stats, value, sizeof( p->stats ) - 1 );
        return 0;
    case OPT_ZONES:
        strncpy( p->zones, value, sizeof( p->zones ) - 1 );
        return 0;
    case OPT_QPFILE:
        strncpy( p->qpfile, value, sizeof( p->qpfile ) - 1 );
        return 0;

    case OPT_TFF: p->tff = 1; return 0;
    case OPT_BFF: p->tff = 0; p->bff = 1; return 0;
    case OPT_CONSTRAINED_INTRA: p->constrained_intra = 1; return 0;
    case OPT_AUD: p->aud = 1; return 0;
    case OPT_SPS_ID:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "sps-id", value );
        p->sps_id = clamp_int( v, 0, 31 );
        return 0;
    case OPT_STITCHABLE: p->stitchable = 1; return 0;

    case OPT_PROFILE:
        if( !choice_ok( value, profile_names ) )
        {
            x264pp_log( X264PP_LOG_ERROR, "x264", "invalid profile: %s", value );
            return -1;
        }
        strncpy( p->profile, value, sizeof( p->profile ) - 1 );
        return 0;
    case OPT_OUTPUT_CSP:
        if( !choice_ok( value, csp_names ) )
        {
            x264pp_log( X264PP_LOG_ERROR, "x264", "Unknown output csp '%s'", value );
            return -1;
        }
        if( strcmp( value, "i420" ) != 0 )
        {
            x264pp_log( X264PP_LOG_ERROR, "x264",
                        "output csp '%s' is not implemented in I0", value );
            return -1;
        }
        return 0;
    case OPT_OUTPUT_DEPTH:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "output-depth", value );
        if( v != 8 )
        {
            x264pp_log( X264PP_LOG_ERROR, "x264",
                        "output depth %d is not implemented in I0", v );
            return -1;
        }
        p->output_depth = 8;
        return 0;
    case OPT_LOG_LEVEL:
        if( choice_ok( value, log_names ) )
        {
            p->log_level = (int)( value[0] == 'n' ? X264PP_LOG_NONE :
                                  value[0] == 'e' ? X264PP_LOG_ERROR :
                                  value[0] == 'w' ? X264PP_LOG_WARNING :
                                  value[0] == 'i' ? X264PP_LOG_INFO :
                                  X264PP_LOG_DEBUG );
        }
        else
        {
            /* E15: an unrecognised level silently behaves like a low level. */
            p->log_level = X264PP_LOG_NONE;
        }
        return 0;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Argument parsing                                                    */
/* ------------------------------------------------------------------ */

static int handle_cli_option( cli_state_t *c, option_events_t *ev,
                              const option_def_t *def, const char *value )
{
    int v;

    switch( def->id )
    {
    case OPT_HELP:    c->want_help = 1; return 0;
    case OPT_LONGHELP: c->want_help = 1; return 0;
    case OPT_FULLHELP: c->want_fullhelp = 1; return 0;
    case OPT_VERSION: c->want_version = 1; return 0;

    case OPT_VERBOSE:
        c->log_level = X264PP_LOG_DEBUG;
        g_log_level = X264PP_LOG_DEBUG;
        return 0;
    case OPT_QUIET:
        c->log_level = X264PP_LOG_ERROR;
        g_log_level = X264PP_LOG_ERROR;
        return 0;
    case OPT_NO_PROGRESS: c->no_progress = 1; return 0;
    case OPT_LOG_LEVEL: return events_add( ev, def->id, value );

    case OPT_OUTPUT:
        strncpy( c->output_path, value, sizeof( c->output_path ) - 1 );
        c->have_output = 1;
        return 0;
    case OPT_MUXER:
        if( strcmp( value, "auto" ) == 0 ) c->muxer = X264PP_MUX_AUTO;
        else if( strcmp( value, "raw" ) == 0 ) c->muxer = X264PP_MUX_RAW;
        else if( strcmp( value, "mkv" ) == 0 ) c->muxer = X264PP_MUX_MKV;
        else if( strcmp( value, "flv" ) == 0 ) c->muxer = X264PP_MUX_FLV;
        else return invalid_argument( "muxer", value );
        return 0;
    case OPT_DEMUXER:
        if( strcmp( value, "auto" ) == 0 ) c->demux = X264PP_DEMUX_AUTO;
        else if( strcmp( value, "raw" ) == 0 ) c->demux = X264PP_DEMUX_RAW;
        else if( strcmp( value, "y4m" ) == 0 ) c->demux = X264PP_DEMUX_Y4M;
        else return invalid_argument( "demuxer", value );
        return 0;
    case OPT_FRAMES:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "frames", value );
        c->frames = v < 0 ? 0 : v;
        return 0;
    case OPT_SEEK:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "seek", value );
        c->seek = v < 0 ? 0 : v;
        return 0;
    case OPT_FPS:
        if( parse_ratio( value, &c->fps_num, &c->fps_den ) != 0 )
            return invalid_argument( "fps", value );
        c->fps_set = 1;
        return 0;
    case OPT_SAR:
        if( sscanf( value, "%d:%d", &c->sar_num, &c->sar_den ) != 2 ||
            c->sar_den < 0 )
            return invalid_argument( "sar", value );
        c->sar_set = 1;
        return 0;
    case OPT_INPUT_RES:
        strncpy( c->resolution, value, sizeof( c->resolution ) - 1 );
        return 0;
    case OPT_INPUT_CSP:
        strncpy( c->csp_name, value, sizeof( c->csp_name ) - 1 );
        return 0;
    case OPT_INPUT_DEPTH:
        if( parse_int( value, &v ) != 0 ) return invalid_argument( "input-depth", value );
        c->bit_depth = v;
        return 0;
    case OPT_TIMEBASE:
        c->timebase_set = 1;
        return events_add( ev, def->id, value );
    case OPT_PRESET:
        strncpy( c->preset, value, sizeof( c->preset ) - 1 );
        return 0;
    case OPT_TUNE:
        strncpy( c->tune, value, sizeof( c->tune ) - 1 );
        return 0;
    default:
        return events_add( ev, def->id, value );
    }
}

static int parse_args( cli_state_t *c, option_events_t *ev,
                       int argc, char **argv )
{
    int i;
    int options_done = 0;

    for( i = 1; i < argc; i++ )
    {
        const char *arg = argv[i];
        const option_def_t *def;
        const char *value = NULL;
        char name[128];
        const char *eq;

        if( !options_done && strcmp( arg, "--" ) == 0 )
        {
            options_done = 1;
            continue;
        }
        if( options_done || arg[0] != '-' || arg[1] == '\0' )
        {
            if( !c->have_input )
            {
                strncpy( c->input_path, arg, sizeof( c->input_path ) - 1 );
                c->have_input = 1;
            }
            continue;
        }

        if( arg[1] == '-' )
        {
            eq = strchr( arg + 2, '=' );
            if( eq )
            {
                size_t n = (size_t)( eq - ( arg + 2 ) );
                if( n >= sizeof( name ) ) n = sizeof( name ) - 1;
                memcpy( name, arg + 2, n );
                name[n] = '\0';
                value = eq + 1;
            }
            else
            {
                strncpy( name, arg + 2, sizeof( name ) - 1 );
                name[sizeof( name ) - 1] = '\0';
            }
            def = find_long_option( name );
            if( !def )
            {
                fprintf( stderr, "%s: unknown option -- %s\n", argv[0], name );
                return -1;
            }
            if( def->has_arg && !value )
            {
                if( i + 1 >= argc )
                {
                    fprintf( stderr, "%s: option requires an argument -- %s\n",
                             argv[0], name );
                    return -1;
                }
                value = argv[++i];
            }
        }
        else
        {
            def = find_short_option( arg[1] );
            if( !def )
            {
                fprintf( stderr, "%s: unknown option -- %c\n", argv[0], arg[1] );
                return -1;
            }
            if( def->has_arg )
            {
                value = arg[2] ? arg + 2 : ( i + 1 < argc ? argv[++i] : NULL );
                if( !value )
                {
                    fprintf( stderr, "%s: option requires an argument -- %c\n",
                             argv[0], arg[1] );
                    return -1;
                }
            }
            else if( arg[2] != '\0' )
            {
                /* Allow compact flag clusters such as -vh. */
                int j;
                for( j = 2; arg[j]; j++ )
                {
                    const option_def_t *flag = find_short_option( arg[j] );
                    if( !flag || flag->has_arg )
                    {
                        fprintf( stderr, "%s: unknown option -- %c\n",
                                 argv[0], arg[j] );
                        return -1;
                    }
                    if( handle_cli_option( c, ev, flag, NULL ) != 0 ) return -1;
                }
                continue;
            }
        }

        if( handle_cli_option( c, ev, def, value ) != 0 ) return -1;
    }
    return 0;
}

static int finalize_params( cli_state_t *c, option_events_t *ev )
{
    size_t i;
    char tunebuf[256];

    param_defaults( &c->param );
    c->param.frames     = c->frames;
    c->param.seek       = c->seek;
    c->param.bit_depth  = c->bit_depth;
    c->param.log_level  = c->log_level;
    c->param.no_progress = c->no_progress;
    if( c->fps_set )
    {
        c->param.fps_num = c->fps_num;
        c->param.fps_den = c->fps_den;
    }
    if( c->sar_set )
    {
        c->param.sar_num = c->sar_num;
        c->param.sar_den = c->sar_den;
    }

    if( c->preset[0] && apply_preset( &c->param, c->preset ) != 0 ) return -1;
    if( c->tune[0] )
    {
        strncpy( tunebuf, c->tune, sizeof( tunebuf ) - 1 );
        tunebuf[sizeof( tunebuf ) - 1] = '\0';
        {
            char *tok = strtok( tunebuf, "," );
            while( tok )
            {
                if( apply_tune( &c->param, tok ) != 0 ) return -1;
                tok = strtok( NULL, "," );
            }
        }
    }

    for( i = 0; i < ev->count; i++ )
        if( apply_option( &c->param, ev->items[i].id,
                          ev->items[i].value ) != 0 )
            return -1;

    if( c->param.min_keyint <= 0 )
        c->param.min_keyint = c->param.keyint == 0
                              ? 10 : ( c->param.keyint / 25 > 1
                                       ? c->param.keyint / 25 : 1 );

    if( c->param.profile[0] && strcmp( c->param.profile, "high" ) == 0 &&
        c->param.rc == X264PP_RC_CQP && c->param.qp == 0 )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264",
                    "high profile doesn't support lossless" );
        return -1;
    }
    g_log_level = c->param.log_level;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Help and version text                                               */
/* ------------------------------------------------------------------ */

static void print_version( void )
{
    fputs( "x264 0.164.x\n"
           "(libswscale 9.5.102)\n"
           "built on Nov 27 2026, gcc: 16.2.0\n"
           "x264 configuration: --chroma-format=all\n"
           "libx264 configuration: --chroma-format=all\n"
           "x264 license: GPL version 2 or later\n"
           "libswscale license: GPL version 3 or later\n", stdout );
}

static void print_help( int full )
{
    fputs( "x264 core:164\n", stdout );
    fputs( "x264++ clean-room CLI (increment I0; codec core not yet certified)\n", stdout );
    fputs( "usage: x264 [options] -o outfile infile\n\n", stdout );
    fputs( "Input: --demuxer auto|raw|y4m  --input-res WxH  --input-csp NAME\n"
           "       --input-depth N  --fps RATIONAL  --sar W:H  --seek N  --frames N\n", stdout );
    fputs( "Output: -o, --output FILE  --muxer auto|raw  (raw Annex B; '-' = stdout)\n\n", stdout );
    fputs( "Rate control: --crf N  -q, --qp N  -B, --bitrate N  --qpmin N  --qpmax N\n"
           "       --qpstep N  --qcomp F  --ipratio F  --pbratio F  --aq-mode N\n"
           "       --aq-strength F  --rc-lookahead N  --pass N  --stats FILE\n\n", stdout );
    fputs( "Structure: -p, --preset NAME  --tune LIST  --profile NAME\n"
           "       -I, --keyint N  -i, --min-keyint N  --scenecut N  --no-scenecut\n"
           "       -b, --bframes N  -r, --ref N  --open-gop  --no-cabac\n\n", stdout );
    fputs( "Analysis: -A, --partitions LIST  --direct NAME  --me NAME  --merange N\n"
           "       -m, --subme N  -t, --trellis N  --psy-rd A:B  --no-psy\n"
           "       --no-deblock  -f, --deblock A:B  --no-8x8dct  --no-mbtree\n\n", stdout );
    fputs( "Logging: -v, --verbose  --quiet  --no-progress\n"
           "       --log-level none|error|warning|info|debug\n", stdout );
    if( full )
        fputs( "\nI0 accepts the full documented option vocabulary; options whose\n"
               "behaviour belongs to a later increment are parsed and validated but\n"
               "cannot affect the (not yet certified) codec core.\n", stdout );
    fputs( "\nversion: --version\n", stdout );
}

/* ------------------------------------------------------------------ */
/* Program flow                                                        */
/* ------------------------------------------------------------------ */

static void print_input_info( const x264pp_input_info_t *info,
                              const x264pp_param_t *p )
{
    x264pp_log( X264PP_LOG_INFO, info->module, "%dx%dp %d:%d @ %d/%d fps (cfr)",
                info->width, info->height,
                info->sar_num, info->sar_den,
                info->fps_num, info->fps_den );
    if( p->sar_num && p->sar_den )
        x264pp_log( X264PP_LOG_INFO, "x264", "using SAR=%d/%d",
                    p->sar_num, p->sar_den );
}

/* ------------------------------------------------------------------ */
/* Frame-level parallelism                                             */
/*                                                                    */
/* I1 emits every picture as an independent IDR, so frames do not      */
/* depend on one another.  Each worker owns a private input handle, a  */
/* private encoder instance and a private scratch Annex-B buffer; the  */
/* main thread writes finished frames in original frame order.        */
/*                                                                    */
/* A fresh encoder puts SPS and PPS in its first output buffer.  Only  */
/* the first frame of the whole stream may keep them, so a worker that  */
/* starts at a later frame drops those two NAL units from its first    */
/* result.  Everything after that is a bare IDR slice, byte-for-byte   */
/* identical to what the single encoder would have produced.          */
/* ------------------------------------------------------------------ */

#define X264PP_MAX_THREADS 64

typedef struct encoded_frame
{
    uint8_t *data;
    int      len;
    int      valid;
} encoded_frame_t;

typedef struct worker_ctx
{
    x264pp_input_t  *input;
    x264pp_encoder_t *encoder;
    uint8_t         *scratch;
    int              cap;
    int64_t          start;        /* global frame index, --seek included */
    int64_t          end;
    int64_t          first_global; /* frame that must carry SPS/PPS */
    encoded_frame_t *slots;        /* shared, indexed by index - seek */
    volatile LONG   *failed;
} worker_ctx_t;

/* Drop the leading SPS and PPS NAL units from a worker's first frame.
 * Annex-B start codes can only occur at NAL boundaries because the encoder
 * escapes zero runs, so the third start code is the IDR slice. */
static int skip_parameter_sets( uint8_t *buf, int len )
{
    int i, found = 0;
    for( i = 0; i + 4 <= len; i++ )
    {
        if( buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1 )
        {
            if( ++found == 3 )
            {
                memmove( buf, buf + i, (size_t)( len - i ) );
                return len - i;
            }
        }
    }
    return len;   /* no parameter-set pair: emit unchanged */
}

static DWORD WINAPI encode_worker( void *arg )
{
    worker_ctx_t *w = (worker_ctx_t *)arg;
    x264pp_picture_t picture;
    int64_t g;
    int local = 0;

    for( g = w->start; g < w->end; g++, local++ )
    {
        encoded_frame_t *slot = &w->slots[g - w->first_global];
        int out_len = 0, frame_type = 0;
        int rc = x264pp_input_read_frame( w->input, &picture, g );
        uint8_t *copy;

        if( rc <= 0 )
        {
            /* 0 is a clean EOF: --frames may exceed the input length. */
            if( rc < 0 ) InterlockedExchange( w->failed, 1 );
            break;
        }
        if( x264pp_encoder_encode( w->encoder, &picture, 0, w->scratch,
                                   w->cap, &out_len, &frame_type ) != X264PP_OK ||
            out_len <= 0 )
        {
            InterlockedExchange( w->failed, 1 );
            break;
        }
        if( local == 0 && g != w->first_global )
            out_len = skip_parameter_sets( w->scratch, out_len );

        copy = (uint8_t *)malloc( (size_t)out_len );
        if( !copy )
        {
            InterlockedExchange( w->failed, 1 );
            break;
        }
        memcpy( copy, w->scratch, (size_t)out_len );
        slot->data = copy;
        slot->len = out_len;
        slot->valid = 1;
    }

    /* I1 has no delayed pictures; a flush must not produce bytes here. */
    {
        int out_len = 0, frame_type = 0;
        if( x264pp_encoder_encode( w->encoder, NULL, 1, w->scratch, w->cap,
                                   &out_len, &frame_type ) != X264PP_OK || out_len > 0 )
            InterlockedExchange( w->failed, 1 );
    }
    return 0;
}

static int encode_threaded( const cli_state_t *c, x264pp_output_t *output,
                            int64_t num_frames )
{
    SYSTEM_INFO si;
    x264pp_input_t **inputs = NULL;
    x264pp_encoder_t **encoders = NULL;
    HANDLE *threads = NULL;
    worker_ctx_t *ctxs = NULL;
    encoded_frame_t *slots = NULL;
    volatile LONG failed = 0;
    int64_t total, seek, remaining, base, rem, start;
    int nthreads, nworkers, cap, i, j, count = 0, ret = -1;

    seek = c->param.seek;

    /* Determine how many frames the sequential loop would have encoded. */
    if( num_frames >= 0 )
    {
        remaining = num_frames - seek;
        if( remaining < 0 ) remaining = 0;
        total = ( c->param.frames > 0 && c->param.frames < remaining )
              ? c->param.frames : remaining;
    }
    else
    {
        /* Y4M has no frame count in its header: count once up front so that
         * --frames beyond EOF cannot hand a worker a block past the end. */
        x264pp_input_t *counter = NULL;
        x264pp_input_info_t ci;
        if( x264pp_input_open( c->input_path, c->demux, c->resolution,
                               c->csp_name, c->bit_depth, &counter, &ci )
            != X264PP_INPUT_OK )
            return -1;
        total = 0;
        while( x264pp_input_read_frame( counter, NULL, total ) > 0 )
            total++;
        x264pp_input_close( counter );
        remaining = total - seek;
        if( remaining < 0 ) remaining = 0;
        total = ( c->param.frames > 0 && c->param.frames < remaining )
              ? c->param.frames : remaining;
    }
    if( total <= 0 ) return 0;      /* no pictures: empty output, like I1 flush */
    if( total > INT_MAX ) total = INT_MAX;

    if( c->param.threads <= 0 )
    {
        GetSystemInfo( &si );
        nthreads = (int)si.dwNumberOfProcessors;
        if( nthreads < 1 ) nthreads = 1;
        if( nthreads > 16 ) nthreads = 16;   /* bound the automatic count */
    }
    else nthreads = c->param.threads;
    if( nthreads > X264PP_MAX_THREADS ) nthreads = X264PP_MAX_THREADS;
    nworkers = (int)( total < nthreads ? total : nthreads );
    if( nworkers < 1 ) nworkers = 1;

    cap = (int)( (size_t)c->param.width * c->param.height * 6 + 4096 );
    inputs   = (x264pp_input_t **)calloc( (size_t)nworkers, sizeof( *inputs ) );
    encoders = (x264pp_encoder_t **)calloc( (size_t)nworkers, sizeof( *encoders ) );
    threads  = (HANDLE *)calloc( (size_t)nworkers, sizeof( *threads ) );
    ctxs     = (worker_ctx_t *)calloc( (size_t)nworkers, sizeof( *ctxs ) );
    slots    = (encoded_frame_t *)calloc( (size_t)total, sizeof( *slots ) );
    if( !inputs || !encoders || !threads || !ctxs || !slots )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264", "out of memory" );
        goto cleanup;
    }

    base = total / nworkers;
    rem  = total % nworkers;
    start = 0;
    for( i = 0; i < nworkers; i++ )
    {
        int64_t len = base + ( i < rem ? 1 : 0 );
        ctxs[i].cap = cap;
        ctxs[i].start = seek + start;
        ctxs[i].end = seek + start + len;
        ctxs[i].first_global = seek;
        ctxs[i].slots = slots;
        ctxs[i].failed = &failed;
        ctxs[i].scratch = (uint8_t *)malloc( (size_t)cap );
        if( !ctxs[i].scratch )
        {
            x264pp_log( X264PP_LOG_ERROR, "x264", "out of memory" );
            goto cleanup;
        }
        start += len;
    }

    /* Open each handle on the main thread: the Y4M header parser uses strtok
     * and cannot be called concurrently.  After this point every worker
     * touches only its own input, encoder and buffer. */
    for( i = 0; i < nworkers; i++ )
    {
        x264pp_input_info_t wi;
        if( x264pp_input_open( c->input_path, c->demux, c->resolution,
                               c->csp_name, c->bit_depth,
                               &inputs[i], &wi ) != X264PP_INPUT_OK )
            goto cleanup;
        if( x264pp_encoder_open( &c->param, &encoders[i] ) != X264PP_OK )
            goto cleanup;
        if( x264pp_input_seek_frame( inputs[i], ctxs[i].start ) != 0 )
            goto cleanup;
        ctxs[i].input = inputs[i];
        ctxs[i].encoder = encoders[i];
    }

    for( i = 0; i < nworkers; i++ )
    {
        threads[i] = CreateThread( NULL, 0, encode_worker, &ctxs[i], 0, NULL );
        if( !threads[i] )
        {
            InterlockedExchange( &failed, 1 );
            break;
        }
    }
    for( i = 0; i < nworkers; i++ )
    {
        if( threads[i] )
        {
            WaitForSingleObject( threads[i], INFINITE );
            CloseHandle( threads[i] );
            threads[i] = NULL;
        }
    }

    if( !failed )
    {
        for( j = 0; j < total; j++ )
        {
            if( !slots[j].valid ) continue;
            if( x264pp_output_write( output, slots[j].data,
                                     (size_t)slots[j].len ) != 0 )
                goto cleanup;
            count++;
        }
        if( count > 0 )
            fprintf( stderr, "encoded %d frames, 0.00 fps, 0.00 kb/s\n", count );
        ret = 0;
    }

cleanup:
    if( slots )
        for( j = 0; j < total; j++ ) free( slots[j].data );
    if( ctxs )
        for( i = 0; i < nworkers; i++ ) free( ctxs[i].scratch );
    if( encoders )
        for( i = 0; i < nworkers; i++ ) x264pp_encoder_close( encoders[i] );
    if( inputs )
        for( i = 0; i < nworkers; i++ ) x264pp_input_close( inputs[i] );
    free( slots );
    free( ctxs );
    free( threads );
    free( encoders );
    free( inputs );
    return ret;
}

int x264pp_cli_main( int argc, char **argv )
{
    cli_state_t c;
    option_events_t events;
    x264pp_input_t *input = NULL;
    x264pp_input_info_t info;
    x264pp_output_t *output = NULL;
    x264pp_encoder_t *encoder = NULL;
    x264pp_picture_t picture;
    x264pp_input_status_t ist;
    uint8_t *bitstream = NULL;
    int64_t frame_index;
    int cap;
    int count = 0;
    int rc;

    memset( &c, 0, sizeof( c ) );
    memset( &events, 0, sizeof( events ) );
    c.demux = X264PP_DEMUX_AUTO;
    c.muxer = X264PP_MUX_AUTO;
    c.bit_depth = 8;
    c.log_level = X264PP_LOG_INFO;
    g_log_level = X264PP_LOG_INFO;

    if( parse_args( &c, &events, argc, argv ) != 0 )
    {
        events_clean( &events );
        return -1;
    }
    if( c.want_version )
    {
        print_version();
        events_clean( &events );
        return 0;
    }
    if( c.want_help || c.want_fullhelp )
    {
        print_help( c.want_fullhelp );
        events_clean( &events );
        return 0;
    }
    if( finalize_params( &c, &events ) != 0 )
    {
        events_clean( &events );
        return -1;
    }
    events_clean( &events );

    if( !c.output_path[0] )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264",
                    "No output file. Run x264 --help for a list of options." );
        return -1;
    }
    if( !c.input_path[0] )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264",
                    "No input file. Run x264 --help for a list of options." );
        return -1;
    }

    memset( &info, 0, sizeof( info ) );
    ist = x264pp_input_open( c.input_path, c.demux,
                             c.resolution, c.csp_name, c.bit_depth,
                             &input, &info );
    if( ist != X264PP_INPUT_OK )
    {
        if( ist == X264PP_INPUT_ERR_RESOLUTION )
            x264pp_log( X264PP_LOG_ERROR, "x264",
                        "could not open input file '%s' via any method!",
                        c.input_path );
        else
            x264pp_log( X264PP_LOG_ERROR, "x264",
                        "could not open input file '%s'", c.input_path );
        return -1;
    }

    c.param.width     = info.width;
    c.param.height    = info.height;
    c.param.input_csp = info.csp;
    c.param.bit_depth = info.bit_depth;
    if( !c.fps_set )
    {
        c.param.fps_num = info.fps_num;
        c.param.fps_den = info.fps_den;
    }
    if( !c.sar_set )
    {
        c.param.sar_num = info.sar_num;
        c.param.sar_den = info.sar_den;
    }

    if( c.timebase_set )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264",
                    "--timebase is incompatible with cfr input" );
        x264pp_input_close( input );
        return -1;
    }

    print_input_info( &info, &c.param );

    /* I0 gate: the codec core is not certified, so no output file is opened
     * and no bitstream byte is produced. */
    if( x264pp_encoder_open( &c.param, &encoder ) != X264PP_OK )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264", "%s", x264pp_codec_status() );
        x264pp_input_close( input );
        return -1;
    }

    /* The remainder becomes reachable when a certified core replaces the
     * stub.  It is compiled in I0 so the CLI/output contract is fixed now. */
    if( x264pp_output_open( c.output_path, c.muxer, &output ) != 0 )
    {
        x264pp_encoder_close( encoder );
        x264pp_input_close( input );
        return -1;
    }

    /* Frame-level parallelism.  A single shared encoder is not thread-safe,
     * so the threaded path opens one encoder and one input per worker and
     * merges their results in frame order.  stdin cannot be opened more than
     * once, so it (and --threads 1) keeps the sequential path below. */
    if( c.param.threads != 1 && strcmp( c.input_path, "-" ) != 0 )
    {
        int rc_threaded;
        x264pp_encoder_close( encoder );
        encoder = NULL;
        x264pp_input_close( input );
        input = NULL;
        rc_threaded = encode_threaded( &c, output, info.num_frames );
        x264pp_output_close( output );
        return rc_threaded;
    }

    /* One output buffer for the whole encode: per-frame allocation here would
     * dominate the I/O path for all-I_PCM frames.  Its capacity is invariant,
     * so compute it once instead of once per frame. */
    cap = (int)( (size_t)c.param.width * c.param.height * 6 + 4096 );
    bitstream = (uint8_t *)malloc( (size_t)cap );
    if( !bitstream )
    {
        x264pp_log( X264PP_LOG_ERROR, "x264", "out of memory" );
        x264pp_output_close( output );
        x264pp_encoder_close( encoder );
        x264pp_input_close( input );
        return -1;
    }

    for( ;; )
    {
        int out_len = 0, frame_type = 0;

        if( c.param.frames && count >= c.param.frames ) break;
        frame_index = (int64_t)c.param.seek + count;
        rc = x264pp_input_read_frame( input, &picture, frame_index );
        if( rc == 0 ) break;
        if( rc < 0 )
        {
            x264pp_output_close( output );
            x264pp_encoder_close( encoder );
            x264pp_input_close( input );
            free( bitstream );
            return -1;
        }
        if( x264pp_encoder_encode( encoder, &picture, 0,
                                   bitstream, cap, &out_len, &frame_type ) != X264PP_OK )
        {
            x264pp_output_close( output );
            x264pp_encoder_close( encoder );
            x264pp_input_close( input );
            free( bitstream );
            return -1;
        }
        if( out_len > 0 &&
            x264pp_output_write( output, bitstream, (size_t)out_len ) != 0 )
        {
            x264pp_output_close( output );
            x264pp_encoder_close( encoder );
            x264pp_input_close( input );
            free( bitstream );
            return -1;
        }
        count++;
    }

    for( ;; )
    {
        int out_len = 0, frame_type = 0;
        if( x264pp_encoder_encode( encoder, NULL, 1,
                                   bitstream, cap, &out_len, &frame_type ) != X264PP_OK )
            break;
        if( out_len <= 0 ) break;
        if( x264pp_output_write( output, bitstream, (size_t)out_len ) != 0 ) break;
    }

    if( count > 0 )
        fprintf( stderr, "encoded %d frames, 0.00 fps, 0.00 kb/s\n", count );

    free( bitstream );
    x264pp_output_close( output );
    x264pp_encoder_close( encoder );
    x264pp_input_close( input );
    return 0;
}

int main( int argc, char **argv )
{
    return x264pp_cli_main( argc, argv );
}
