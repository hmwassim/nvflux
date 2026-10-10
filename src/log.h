#ifndef NVFLUX_LOG_H
#define NVFLUX_LOG_H

__attribute__((format(printf, 1, 2))) void log_error(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void log_warn(const char *fmt, ...);

#endif
