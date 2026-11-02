#ifndef NVFLUX_STATUS_H
#define NVFLUX_STATUS_H

/* Read-only commands. They run without root and never wake a sleeping GPU. */
int status_show(int json);
int status_info(int json);

#endif
