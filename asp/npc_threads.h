#ifndef CHRONO_RIFT_NPC_THREADS_H
#define CHRONO_RIFT_NPC_THREADS_H

#include "../shared.h"

/*
 * Spawn CR_MAX_NPCS pthread workers; each periodically locks state_mutex and runs
 * asp_submit_npc_action_for_slot for its index (no-op when not that NPC’s turn).
 * Returns 0 on success, non-zero if thread creation fails (caller should stop any started threads).
 */
int asp_npc_threads_start(cr_shared_state_t *state);

/* Signal workers to exit and pthread_join all of them. Safe to call once. */
void asp_npc_threads_stop();

#endif /* CHRONO_RIFT_NPC_THREADS_H */
