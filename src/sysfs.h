#ifndef NVFLUX_SYSFS_H
#define NVFLUX_SYSFS_H

/*
 * Facts nvflux reads from the kernel without touching the GPU. Reading these
 * never wakes a sleeping laptop GPU, unlike any NVML call.
 */

#define SYSFS_BUS_ID_LEN 32
#define SYSFS_MAX_GPUS 16

struct pci_gpu {
    char bus_id[SYSFS_BUS_ID_LEN]; /* 0000:01:00.0 */
    int asleep;                    /* runtime_status is "suspended" */
    int runtime_pm;                /* power/control is "auto": the GPU may power off */
};

/* Lists NVIDIA display/3D controllers. Returns how many were found. */
int sysfs_nvidia_gpus(struct pci_gpu *out, int max);

/* Fills one GPU's power facts; returns 0, or -1 if the device isn't found. */
int sysfs_gpu(const char *bus_id, struct pci_gpu *out);

/* Whether the kernel command line contains this exact word. */
int sysfs_cmdline_has(const char *word);

/* The kernel module's version from /proc/driver/nvidia/version; 0 or -1. */
int sysfs_kernel_driver_version(char *buf, unsigned len);

/* 1 if a battery is present, and whether the system runs on it right now. */
int sysfs_on_battery(int *has_battery);

#endif
