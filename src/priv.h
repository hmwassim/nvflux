#ifndef NVFLUX_PRIV_H
#define NVFLUX_PRIV_H

/*
 * nvflux is installed setuid root. Every run starts with priv_init(), then
 * either gives root up for good (priv_drop) or, only for commands that change
 * clocks, becomes plain root with a clean environment (priv_elevate).
 */

void priv_init(void);
int priv_drop(void);
int priv_elevate(void);

#endif
