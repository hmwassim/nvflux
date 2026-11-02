#ifndef NVFLUX_DOCTOR_H
#define NVFLUX_DOCTOR_H

/* Checks the whole setup and prints one line per check:
 *   ok    <what>
 *   warn  <what>; <fix>
 *   FAIL  <what>; <fix>
 * Returns EXIT_OK, or EXIT_FAIL if any check failed. Runs without root and
 * never wakes a sleeping GPU. */
int doctor_run(void);

#endif
