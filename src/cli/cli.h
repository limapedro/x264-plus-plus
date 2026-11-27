#ifndef X264PP_CLI_H
#define X264PP_CLI_H

/*
 * Shared CLI vocabulary.  The logging entry point is defined in cli.c and is
 * used by the input and output modules so every diagnostic has the documented
 * "<component> [level]: " prefix on stderr.
 */

#include "core/api.h"

#define X264PP_MAX_RESOLUTION 16384

enum
{
    X264PP_LOG_NONE    = 0,
    X264PP_LOG_ERROR   = 1,
    X264PP_LOG_WARNING = 2,
    X264PP_LOG_INFO    = 3,
    X264PP_LOG_DEBUG   = 4
};

/* Print a level-tagged diagnostic on stderr.  Messages at or below the active
 * log level are emitted; WARNING/INFO/DEBUG are suppressed by --quiet and by
 * lower --log-level settings. */
void x264pp_log( int level, const char *component, const char *fmt, ... );

/* Program entry point, kept out of main() so the translation unit that owns
 * main() can stay trivial. */
int x264pp_cli_main( int argc, char **argv );

#endif /* X264PP_CLI_H */
