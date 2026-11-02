#include "out.h"

#include <stdarg.h>

static void emit(FILE *f, const char *prefix, const char *fmt, va_list ap)
{
    fflush(stdout); /* keep stdout and stderr in order on a terminal */
    fputs(prefix, f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
}

#define EMIT(f, prefix)               \
    do {                              \
        va_list ap;                   \
        va_start(ap, fmt);            \
        emit(f, prefix, fmt, ap);     \
        va_end(ap);                   \
    } while (0)

void out_line(const char *fmt, ...) { EMIT(stdout, ""); }
void out_error(const char *fmt, ...) { EMIT(stderr, "nvflux: "); }
void out_warn(const char *fmt, ...) { EMIT(stderr, "nvflux: warning: "); }

void out_json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(f, "\\%c", c);
        else if (c < 0x20)
            fprintf(f, "\\u%04x", c);
        else
            fputc(c, f);
    }
    fputc('"', f);
}
