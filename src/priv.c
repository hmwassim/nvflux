#define _GNU_SOURCE
#include "priv.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* A setuid program can be started with fds 0-2 closed. Fill them so nothing
 * we open later is mistaken for stdout or stderr. The descriptors are kept
 * open on purpose, which GCC's analyzer reports as a leak. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
void priv_init(void)
{
    int fd;
    while ((fd = open("/dev/null", O_RDWR)) >= 0 && fd <= STDERR_FILENO)
        ;
    if (fd > STDERR_FILENO)
        close(fd);
}

int priv_drop(void)
{
    gid_t gid = getgid();
    uid_t uid = getuid();
    if (setresgid(gid, gid, gid) != 0 || setresuid(uid, uid, uid) != 0)
        return -1;
    return 0;
}

int priv_elevate(void)
{
    if (geteuid() != 0)
        return -1;
    if (setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0)
        return -1;

    /* Nothing from the calling user's environment reaches root code. */
    if (clearenv() != 0 || setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1) != 0)
        return -1;
    umask(022);
    return 0;
}
