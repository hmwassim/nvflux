#define _GNU_SOURCE
#include "sysfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Overridable so the tests can use a fake /sys and /proc. */
#ifndef SYSFS_ROOT
#define SYSFS_ROOT "/sys"
#endif
#ifndef PROC_ROOT
#define PROC_ROOT "/proc"
#endif

#define PCI_DIR SYSFS_ROOT "/bus/pci/devices"
#define SUPPLY_DIR SYSFS_ROOT "/class/power_supply"

/* Reads a small text file into buf without its trailing newline. */
static int read_text(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static int read_attr(const char *dir, const char *name, const char *attr, char *buf, size_t len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", dir, name, attr);
    return read_text(path, buf, len);
}

int sysfs_gpu(const char *bus_id, struct pci_gpu *out)
{
    char buf[64];
    memset(out, 0, sizeof(*out));
    snprintf(out->bus_id, sizeof(out->bus_id), "%s", bus_id);
    if (read_attr(PCI_DIR, bus_id, "vendor", buf, sizeof(buf)) != 0)
        return -1;
    if (read_attr(PCI_DIR, bus_id, "power/runtime_status", buf, sizeof(buf)) == 0)
        out->asleep = strcmp(buf, "suspended") == 0;
    if (read_attr(PCI_DIR, bus_id, "power/control", buf, sizeof(buf)) == 0)
        out->runtime_pm = strcmp(buf, "auto") == 0;
    return 0;
}

int sysfs_nvidia_gpus(struct pci_gpu *out, int max)
{
    DIR *d = opendir(PCI_DIR);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        char vendor[16], class[16];
        if (e->d_name[0] == '.')
            continue;
        if (read_attr(PCI_DIR, e->d_name, "vendor", vendor, sizeof(vendor)) != 0 ||
            read_attr(PCI_DIR, e->d_name, "class", class, sizeof(class)) != 0)
            continue;
        /* VGA (0x0300xx) and 3D (0x0302xx) controllers, not the HDMI audio
         * function on the same card. */
        if (strcmp(vendor, "0x10de") != 0 ||
            (strncmp(class, "0x0300", 6) != 0 && strncmp(class, "0x0302", 6) != 0))
            continue;
        if (sysfs_gpu(e->d_name, &out[n]) == 0)
            n++;
    }
    closedir(d);
    return n;
}

int sysfs_cmdline_has(const char *word)
{
    char buf[4096];
    if (read_text(PROC_ROOT "/cmdline", buf, sizeof(buf)) != 0)
        return 0;
    size_t len = strlen(word);
    for (char *p = buf; (p = strstr(p, word)); p += len) {
        int start = p == buf || p[-1] == ' ';
        int end = p[len] == '\0' || p[len] == ' ';
        if (start && end)
            return 1;
    }
    return 0;
}

int sysfs_on_battery(int *has_battery)
{
    DIR *d = opendir(SUPPLY_DIR);
    int battery = 0, mains_online = 0, battery_discharging = 0;
    if (d) {
        struct dirent *e;
        char buf[32];
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' || read_attr(SUPPLY_DIR, e->d_name, "type", buf, sizeof(buf)) != 0)
                continue;
            if (strcmp(buf, "Battery") == 0) {
                /* Peripheral batteries (mice, headsets) say scope=Device. */
                char scope[32];
                if (read_attr(SUPPLY_DIR, e->d_name, "scope", scope, sizeof(scope)) == 0 &&
                    strcmp(scope, "Device") == 0)
                    continue;
                battery = 1;
                if (read_attr(SUPPLY_DIR, e->d_name, "status", buf, sizeof(buf)) == 0 &&
                    strcmp(buf, "Discharging") == 0)
                    battery_discharging = 1;
            } else if (strcmp(buf, "Mains") == 0 || strcmp(buf, "USB") == 0) {
                if (read_attr(SUPPLY_DIR, e->d_name, "online", buf, sizeof(buf)) == 0 &&
                    strcmp(buf, "1") == 0)
                    mains_online = 1;
            }
        }
        closedir(d);
    }
    if (has_battery)
        *has_battery = battery;
    return battery && battery_discharging && !mains_online;
}

int sysfs_kernel_driver_version(char *buf, unsigned len)
{
    char text[512];
    buf[0] = '\0';
    if (read_text(PROC_ROOT "/driver/nvidia/version", text, sizeof(text)) != 0)
        return -1;
    /* "NVRM version: NVIDIA UNIX ... Kernel Module ...  615.71.10  Release Build ..."
     * The version is the first word made of digits and dots. */
    for (char *p = text; *p; p++) {
        if (*p >= '0' && *p <= '9' && (p == text || p[-1] == ' ')) {
            size_t n = strspn(p, "0123456789.");
            if (n >= 3 && memchr(p, '.', n) && (p[n] == ' ' || p[n] == '\0')) {
                snprintf(buf, len, "%.*s", (int)n, p);
                return 0;
            }
        }
    }
    return -1;
}
