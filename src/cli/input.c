/*
 * Raw I420 and YUV4MPEG2 input readers.
 *
 * Both readers share a small buffered byte source so header probing works on
 * stdin as well as on seekable files: bytes consumed while probing are pushed
 * back and served again by the next read.  One frame-sized buffer is allocated
 * at open time and reused for every frame; picture plane pointers point into
 * that buffer and are valid until the next read or close.
 *
 * All behaviour here is derived from spec/cli-blackbox.md (sections 2, 3 and
 * evidence E6-E9) and from the public YUV4MPEG2 format description.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <io.h>

#include "cli/cli.h"
#include "cli/input.h"

#define X264PP_IO_BUFFER 8192
#define X264PP_LINE_MAX  512
#define X264PP_PUSH_MAX  16

typedef struct x264pp_byte_source
{
    FILE         *fh;
    unsigned char buf[X264PP_IO_BUFFER];
    size_t        pos;
    size_t        len;
    unsigned char push[X264PP_PUSH_MAX];
    size_t        push_pos;
    size_t        push_len;
    int           eof;
} byte_source_t;

struct x264pp_input
{
    byte_source_t io;
    int           owns_file;
    int           regular_file;
    x264pp_demuxer_t demux;
    int           width;
    int           height;
    int           interlaced;
    int64_t       frame_size;
    int64_t       next_frame;
    int64_t       num_frames;
    uint8_t      *frame;
};

typedef struct csp_name_entry
{
    const char   *name;
    x264pp_csp_t  csp;
} csp_name_entry_t;

static const csp_name_entry_t csp_names[] =
{
    { "i400", X264PP_CSP_I400 }, { "i420", X264PP_CSP_I420 },
    { "yv12", X264PP_CSP_YV12 }, { "nv12", X264PP_CSP_NV12 },
    { "nv21", X264PP_CSP_NV21 }, { "i422", X264PP_CSP_I422 },
    { "yv16", X264PP_CSP_YV16 }, { "nv16", X264PP_CSP_NV16 },
    { "yuyv", X264PP_CSP_YUYV }, { "uyvy", X264PP_CSP_UYVY },
    { "i444", X264PP_CSP_I444 }, { "yv24", X264PP_CSP_YV24 },
    { "bgr",  X264PP_CSP_BGR  }, { "bgra", X264PP_CSP_BGRA },
    { "rgb",  X264PP_CSP_RGB  }, { NULL,   X264PP_CSP_NONE }
};

static int same_name( const char *a, const char *b )
{
    while( *a && *b )
    {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if( ca >= 'A' && ca <= 'Z' ) ca = (unsigned char)(ca - 'A' + 'a');
        if( cb >= 'A' && cb <= 'Z' ) cb = (unsigned char)(cb - 'A' + 'a');
        if( ca != cb ) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static size_t min_size( size_t a, size_t b )
{
    return a < b ? a : b;
}

static size_t io_read( byte_source_t *io, void *dst, size_t count )
{
    unsigned char *out = (unsigned char *)dst;
    size_t done = 0;

    while( done < count )
    {
        size_t chunk;
        if( io->push_pos < io->push_len )
        {
            chunk = min_size( io->push_len - io->push_pos, count - done );
            memcpy( out + done, io->push + io->push_pos, chunk );
            io->push_pos += chunk;
            done += chunk;
            continue;
        }
        if( io->pos < io->len )
        {
            chunk = min_size( io->len - io->pos, count - done );
            memcpy( out + done, io->buf + io->pos, chunk );
            io->pos += chunk;
            done += chunk;
            continue;
        }
        if( io->eof ) break;
        io->len = fread( io->buf, 1, sizeof( io->buf ), io->fh );
        io->pos = 0;
        if( io->len == 0 )
        {
            io->eof = 1;
            break;
        }
    }
    return done;
}

static int io_getc( byte_source_t *io )
{
    unsigned char c;
    if( io_read( io, &c, 1 ) != 1 ) return -1;
    return (int)c;
}

/* Bulk (frame-sized) read.  The probe buffer above is deliberately small;
 * pushing multi-kilobyte frames through it would cost one memcpy per frame.
 * When the whole frame is already buffered we hand back a borrowed pointer
 * into that buffer instead (zero-copy); otherwise we drain the few remaining
 * buffered bytes and issue one fread() directly into the destination.  The
 * borrowed pointer stays valid until the next read from this source, which is
 * exactly the lifetime the encoder needs for a picture. */
static size_t io_read_bulk( byte_source_t *io, unsigned char *dst, size_t count,
                            unsigned char **borrowed )
{
    size_t done = 0;

    *borrowed = NULL;
    if( io->push_pos >= io->push_len && count <= io->len - io->pos )
    {
        *borrowed = io->buf + io->pos;
        io->pos += count;
        return count;
    }

    /* Consume pushed-back and buffered bytes first so stream order is kept. */
    while( done < count )
    {
        if( io->push_pos < io->push_len )
        {
            size_t chunk = min_size( io->push_len - io->push_pos, count - done );
            memcpy( dst + done, io->push + io->push_pos, chunk );
            io->push_pos += chunk;
            done += chunk;
            continue;
        }
        if( io->pos < io->len )
        {
            size_t chunk = min_size( io->len - io->pos, count - done );
            memcpy( dst + done, io->buf + io->pos, chunk );
            io->pos += chunk;
            done += chunk;
            continue;
        }
        break;
    }

    /* Read the remainder straight from the FILE into the picture buffer.
     * A pipe may return a short read without being at EOF, so keep reading
     * until the frame is complete (or fread actually returns zero). */
    while( done < count && !io->eof )
    {
        size_t need = count - done;
        size_t n = fread( dst + done, 1, need, io->fh );
        done += n;
        if( n == 0 ) io->eof = 1;
    }
    return done;
}

static void io_unread( byte_source_t *io, const unsigned char *data, size_t count )
{
    if( count > X264PP_PUSH_MAX ) count = X264PP_PUSH_MAX;
    memcpy( io->push, data, count );
    io->push_len = count;
    io->push_pos = 0;
}

static void io_reset( byte_source_t *io )
{
    io->pos = 0;
    io->len = 0;
    io->push_pos = 0;
    io->push_len = 0;
    io->eof = 0;
}

/* Returns 1 for a line (without its newline), 0 at EOF before any character,
 * -1 when the line is longer than cap. */
static int io_read_line( byte_source_t *io, char *dst, size_t cap )
{
    size_t len = 0;
    int c;

    while( (c = io_getc( io )) >= 0 )
    {
        if( c == '\n' ) break;
        if( len + 1 >= cap ) return -1;
        dst[len++] = (char)c;
    }
    if( len > 0 && dst[len - 1] == '\r' ) len--;
    dst[len] = '\0';
    if( c < 0 && len == 0 ) return 0;
    return 1;
}

static int64_t io_count_frames( byte_source_t *io, int regular, int64_t frame_size )
{
    long end;

    if( !regular || frame_size <= 0 ) return -1;
    if( fseek( io->fh, 0, SEEK_END ) != 0 ) return -1;
    end = ftell( io->fh );
    if( end < 0 ) return -1;
    if( fseek( io->fh, 0, SEEK_SET ) != 0 ) return -1;
    io_reset( io );
    return (int64_t)end / frame_size;
}

static int64_t i420_frame_size( int width, int height )
{
    return (int64_t)width * (int64_t)height * 3 / 2;
}

static int valid_geometry( int width, int height )
{
    return width > 0 && height > 0 &&
           width <= X264PP_MAX_RESOLUTION && height <= X264PP_MAX_RESOLUTION &&
           (width & 1) == 0 && (height & 1) == 0;
}

static int open_raw( x264pp_input_t *in, const char *path,
                     const char *resolution, const char *csp_name,
                     int bit_depth, x264pp_input_info_t *info )
{
    int width = 0, height = 0;
    int i;

    if( !resolution || !*resolution )
    {
        x264pp_log( X264PP_LOG_ERROR, "raw", "raw input requires a resolution." );
        return X264PP_INPUT_ERR_RESOLUTION;
    }
    if( sscanf( resolution, "%dx%d", &width, &height ) != 2 ||
        !valid_geometry( width, height ) )
    {
        x264pp_log( X264PP_LOG_ERROR, "raw", "invalid resolution '%s'", resolution );
        return X264PP_INPUT_ERR_IO;
    }
    if( csp_name && !same_name( csp_name, "i420" ) )
    {
        int known = 0;
        for( i = 0; csp_names[i].name; i++ )
            if( same_name( csp_name, csp_names[i].name ) ) known = 1;
        x264pp_log( X264PP_LOG_ERROR, "raw",
                    known ? "input csp '%s' is not supported by the I0 reader"
                          : "unknown input csp '%s'", csp_name );
        return X264PP_INPUT_ERR_UNSUPPORTED;
    }
    if( bit_depth != 8 )
    {
        x264pp_log( X264PP_LOG_ERROR, "raw",
                    "input depth %d is not supported by the I0 reader", bit_depth );
        return X264PP_INPUT_ERR_UNSUPPORTED;
    }

    in->demux      = X264PP_DEMUX_RAW;
    in->width      = width;
    in->height     = height;
    in->frame_size = i420_frame_size( width, height );
    in->num_frames = io_count_frames( &in->io, in->regular_file, in->frame_size );
    in->frame = (uint8_t *)malloc( (size_t)in->frame_size );
    if( !in->frame )
    {
        x264pp_log( X264PP_LOG_ERROR, "raw", "out of memory" );
        return X264PP_INPUT_ERR_IO;
    }

    info->width      = width;
    info->height     = height;
    info->interlaced = 0;
    info->csp        = X264PP_CSP_I420;
    info->bit_depth  = 8;
    info->fps_num    = 25;
    info->fps_den    = 1;
    info->sar_num    = 0;             /* undefined for raw, per E8 */
    info->sar_den    = 0;
    info->num_frames = in->num_frames;
    info->module     = "raw";
    (void)path;
    return X264PP_INPUT_OK;
}

static int y4m_chroma_is_i420( const char *c )
{
    return same_name( c, "420" ) || same_name( c, "420jpeg" ) ||
           same_name( c, "420paldv" ) || same_name( c, "420mpeg2" );
}

static int open_y4m( x264pp_input_t *in, x264pp_input_info_t *info )
{
    char line[X264PP_LINE_MAX];
    char chroma[64] = "420";
    int width = 0, height = 0;
    int fps_num = 25, fps_den = 1;
    int sar_num = 1, sar_den = 1;
    int interlaced = 0;
    int rc = io_read_line( &in->io, line, sizeof( line ) );
    char *tok;

    if( rc < 0 )
    {
        x264pp_log( X264PP_LOG_ERROR, "y4m", "sequence header is too long" );
        return X264PP_INPUT_ERR_IO;
    }
    if( rc == 0 )
    {
        x264pp_log( X264PP_LOG_ERROR, "y4m", "bad sequence header" );
        return X264PP_INPUT_ERR_IO;
    }

    for( tok = strtok( line, " \t" ); tok; tok = strtok( NULL, " \t" ) )
    {
        int value = 0;
        switch( tok[0] )
        {
        case 'W':
            if( sscanf( tok + 1, "%d", &value ) == 1 ) width = value;
            break;
        case 'H':
            if( sscanf( tok + 1, "%d", &value ) == 1 ) height = value;
            break;
        case 'F':
            if( sscanf( tok + 1, "%d:%d", &fps_num, &fps_den ) != 2 )
                fps_num = fps_den = 0;
            break;
        case 'A':
            if( sscanf( tok + 1, "%d:%d", &sar_num, &sar_den ) != 2 )
                sar_num = sar_den = 0;
            break;
        case 'I':
            interlaced = (tok[1] == 't' || tok[1] == 'b');
            break;
        case 'C':
            strncpy( chroma, tok + 1, sizeof( chroma ) - 1 );
            chroma[sizeof( chroma ) - 1] = '\0';
            break;
        default:
            break;  /* X extensions and unknown tokens are ignored in I0 */
        }
    }

    if( !valid_geometry( width, height ) )
    {
        x264pp_log( X264PP_LOG_ERROR, "y4m", "invalid sequence header" );
        return X264PP_INPUT_ERR_IO;
    }
    if( !y4m_chroma_is_i420( chroma ) )
    {
        x264pp_log( X264PP_LOG_ERROR, "y4m",
                    "unsupported chroma format '%s'", chroma );
        return X264PP_INPUT_ERR_UNSUPPORTED;
    }
    if( fps_num <= 0 || fps_den <= 0 )
    {
        fps_num = 25;
        fps_den = 1;
    }

    in->demux      = X264PP_DEMUX_Y4M;
    in->width      = width;
    in->height     = height;
    in->interlaced = interlaced;
    in->frame_size = i420_frame_size( width, height );
    in->frame = (uint8_t *)malloc( (size_t)in->frame_size );
    if( !in->frame )
    {
        x264pp_log( X264PP_LOG_ERROR, "y4m", "out of memory" );
        return X264PP_INPUT_ERR_IO;
    }

    info->width      = width;
    info->height     = height;
    info->interlaced = interlaced;
    info->csp        = X264PP_CSP_I420;
    info->bit_depth  = 8;
    info->fps_num    = fps_num;
    info->fps_den    = fps_den;
    info->sar_num    = sar_num;
    info->sar_den    = sar_den;
    info->num_frames = -1;            /* Y4M has no frame count in its header */
    info->module     = "y4m";
    return X264PP_INPUT_OK;
}

x264pp_input_status_t x264pp_input_open( const char *path,
                                         x264pp_demuxer_t demux,
                                         const char *resolution,
                                         const char *csp_name,
                                         int bit_depth,
                                         x264pp_input_t **input,
                                         x264pp_input_info_t *info )
{
    x264pp_input_t *in;
    int from_stdin;

    if( input ) *input = NULL;
    if( !input || !info ) return X264PP_INPUT_ERR_IO;

    in = (x264pp_input_t *)calloc( 1, sizeof( *in ) );
    if( !in ) return X264PP_INPUT_ERR_IO;

    from_stdin = path && strcmp( path, "-" ) == 0;
    if( from_stdin )
    {
        /* Windows maps stdin to text mode by default: 0x1A would be treated
         * as EOF and CRLF translated, both of which corrupt raw video. */
        _setmode( _fileno( stdin ), _O_BINARY );
        in->io.fh = stdin;
    }
    else in->io.fh = fopen( path, "rb" );
    if( !in->io.fh )
    {
        free( in );
        return X264PP_INPUT_ERR_IO;
    }
    in->owns_file = !from_stdin;
    in->regular_file = !from_stdin;
    in->num_frames = -1;

    if( demux == X264PP_DEMUX_AUTO && !from_stdin )
    {
        unsigned char magic[9];
        size_t got = io_read( &in->io, magic, sizeof( magic ) );
        if( got == sizeof( magic ) && memcmp( magic, "YUV4MPEG2", 9 ) == 0 )
        {
            io_unread( &in->io, magic, got );
            demux = X264PP_DEMUX_Y4M;
        }
        else
        {
            io_unread( &in->io, magic, got );
            demux = X264PP_DEMUX_RAW;
        }
    }
    else if( demux == X264PP_DEMUX_AUTO )
    {
        /* stdin is never autodetected: it is raw unless --demuxer y4m is given. */
        demux = X264PP_DEMUX_RAW;
    }

    if( demux == X264PP_DEMUX_Y4M )
    {
        unsigned char magic[9];
        size_t got = io_read( &in->io, magic, sizeof( magic ) );
        /* In auto mode the magic was consumed above and pushed back, so this
         * read sees it again.  In explicit mode it is the first read. */
        if( got != sizeof( magic ) || memcmp( magic, "YUV4MPEG2", 9 ) != 0 )
        {
            x264pp_log( X264PP_LOG_ERROR, "y4m", "bad sequence header magic" );
            x264pp_input_close( in );
            return X264PP_INPUT_ERR_MAGIC;
        }
    }

    {
        x264pp_input_status_t st;
        if( demux == X264PP_DEMUX_Y4M )
            st = open_y4m( in, info );
        else
            st = open_raw( in, path, resolution, csp_name, bit_depth, info );
        if( st != X264PP_INPUT_OK )
        {
            x264pp_input_close( in );
            return st;
        }
    }

    *input = in;
    return X264PP_INPUT_OK;
}

/* Deliver or discard one frame.  A return of 0 means clean end of stream. */
static int read_one( x264pp_input_t *in, x264pp_picture_t *pic )
{
    const char *module = in->demux == X264PP_DEMUX_Y4M ? "y4m" : "raw";
    unsigned char *borrowed = NULL;
    unsigned char *frame_data;
    size_t got;
    int64_t y_size;

    if( in->demux == X264PP_DEMUX_Y4M )
    {
        char line[X264PP_LINE_MAX];
        int rc = io_read_line( &in->io, line, sizeof( line ) );
        if( rc <= 0 ) return rc;
        if( strncmp( line, "FRAME", 5 ) != 0 ||
            (line[5] != '\0' && line[5] != ' ') )
        {
            x264pp_log( X264PP_LOG_ERROR, "y4m", "bad frame header" );
            return -1;
        }
    }

    got = io_read_bulk( &in->io, in->frame, (size_t)in->frame_size, &borrowed );
    frame_data = borrowed ? borrowed : in->frame;
    if( got == 0 ) return 0;
    if( got != (size_t)in->frame_size )
    {
        x264pp_log( X264PP_LOG_ERROR, module, "truncated final frame" );
        return -1;
    }

    if( pic )
    {
        y_size = (int64_t)in->width * (int64_t)in->height;
        memset( pic, 0, sizeof( *pic ) );
        pic->csp       = X264PP_CSP_I420;
        pic->width     = in->width;
        pic->height    = in->height;
        pic->bit_depth = 8;
        /* Planes may point straight into the byte source's buffer; the caller
         * encodes the picture before asking for the next frame. */
        pic->plane[0]  = frame_data;
        pic->plane[1]  = frame_data + y_size;
        pic->plane[2]  = frame_data + y_size + y_size / 4;
        pic->stride[0] = in->width;
        pic->stride[1] = in->width / 2;
        pic->stride[2] = in->width / 2;
        pic->pts       = in->next_frame;
        pic->duration  = 1;
    }
    in->next_frame++;
    return 1;
}

int x264pp_input_read_frame( x264pp_input_t *in, x264pp_picture_t *pic,
                             int64_t index )
{
    if( !in || index < in->next_frame ) return -1;

    while( in->next_frame < index )
    {
        int rc = read_one( in, NULL );
        if( rc <= 0 ) return rc;
    }
    return read_one( in, pic );
}

int x264pp_input_seek_frame( x264pp_input_t *in, int64_t index )
{
    if( !in || index < in->next_frame ) return -1;
    if( index == in->next_frame ) return 0;

    /* Raw files have fixed-size frames, so a regular file can be positioned
     * exactly.  Y4M (and stdin) have variable byte offsets per frame, so
     * discard frames until the requested index is reached. */
    if( in->regular_file && in->demux == X264PP_DEMUX_RAW && in->frame_size > 0 )
    {
        if( _fseeki64( in->io.fh, index * in->frame_size, SEEK_SET ) != 0 )
            return -1;
        io_reset( &in->io );
        in->next_frame = index;
        return 0;
    }

    while( in->next_frame < index )
    {
        int rc = read_one( in, NULL );
        if( rc <= 0 ) return -1;
    }
    return 0;
}

void x264pp_input_close( x264pp_input_t *in )
{
    if( !in ) return;
    free( in->frame );
    if( in->owns_file && in->io.fh ) fclose( in->io.fh );
    free( in );
}
