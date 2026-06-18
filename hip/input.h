#ifndef CHRONO_RIFT_HIP_INPUT_H
#define CHRONO_RIFT_HIP_INPUT_H

#include <stddef.h>

/*
 * Background stdin reader: lines are queued for the player-action layer.
 * Safe to call poll only from one consumer (e.g. one player worker at a time).
 */
void hip_input_start();
void hip_input_stop();

/* Pop one line (without newline). Returns 1 if buf filled, 0 if queue empty. */
int hip_input_poll_line(char *buf, size_t buflen);

#endif /* CHRONO_RIFT_HIP_INPUT_H */
