#ifndef CHRONO_RIFT_HIP_PLAYER_THREADS_H
#define CHRONO_RIFT_HIP_PLAYER_THREADS_H

#include "../shared.h"

/* Spawn CR_MAX_PLAYERS workers; each submits player_actions[i] when it is player i’s turn. */
int hip_player_threads_start(cr_shared_state_t *state);
void hip_player_threads_stop();

#endif /* CHRONO_RIFT_HIP_PLAYER_THREADS_H */
