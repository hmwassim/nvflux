#include "log.h"

#include <stdarg.h>
#include <stdio.h>

static void vlog(const char *level, const char *fmt, va_list ap)
{
    fflush(stdout); /* keep stdout and stderr in order when both are a terminal */
    fprintf(stderr, "nvflux: %s: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("error", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("warning", fmt, ap);
    va_end(ap);
}
