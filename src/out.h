#ifndef NVFLUX_OUT_H
#define NVFLUX_OUT_H

#include <stdio.h>

/*
 * All output is short and plain ASCII: results on stdout, one line each;
 * problems on stderr as "nvflux: <what>; <how to fix>".
 */

__attribute__((format(printf, 1, 2))) void out_line(const char *fmt, ...);  /* stdout */
__attribute__((format(printf, 1, 2))) void out_error(const char *fmt, ...); /* "nvflux: ..." */
__attribute__((format(printf, 1, 2))) void out_warn(const char *fmt, ...);  /* "nvflux: warning: ..." */

void out_json_str(FILE *f, const char *s);

#endif
