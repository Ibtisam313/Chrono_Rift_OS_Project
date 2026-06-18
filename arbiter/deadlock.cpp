

 //cr_arbiter_deadlock_try_acquire_artifact records which artifact an entity waits on when
// busy. cr_arbiter_deadlock_tick looks for 2-cycles (A holds X, waits Y; B holds Y, waits X)
//and breaks them by stripping one victim's artifacts. A background thread calls tick periodically.
 
#include "../shared.h"

#include <cstdio>
#include <pthread.h>
#include <unistd.h>

using namespace std;

void cr_arbiter_deadlock_tick(cr_shared_state_t *state);

namespace {

/* requesting_artifact[entity_id] = artifact id being waited on, or -1 */
int requesting_artifact[CR_MAX_ENTITIES];
#if CR_DEV_MODE
int demo_injected = 0;
#endif
pthread_t deadlock_thread;
int deadlock_thread_running = 0;
cr_shared_state_t *g_deadlock_state = nullptr;

/* Entity must be an active player index or an active NPC slot (by current counts). */
int is_valid_entity_for_artifacts(const cr_shared_state_t *state, int entity_id) {
    if (entity_id < 0 || entity_id >= CR_MAX_ENTITIES) {
        return 0;
    }
    const int is_player = (entity_id >= 0 && entity_id < state->player_count);
    const int is_npc =
        (entity_id >= CR_MAX_PLAYERS && entity_id < CR_MAX_PLAYERS + state->npc_count_current);
    return is_player || is_npc;
}

/* Clear this entity's outgoing wait edge in the resource graph. */
void clear_entity_request(int entity_id) {
    if (entity_id >= 0 && entity_id < CR_MAX_ENTITIES) {
        requesting_artifact[entity_id] = -1;
    }
}

/* Force-drop every artifact record owned by entity_id (death or deadlock resolution). */
void release_all_artifacts_held_by(cr_shared_state_t *state, int entity_id) {
    for (int a = 0; a < CR_MAX_ARTIFACTS; ++a) {
        if (state->artifacts[a].held_by_entity_id == entity_id) {
            state->artifacts[a].held_by_entity_id = -1;
            state->artifacts[a].locked = 0;
        }
    }
}

/* Background loop: lock state_mutex, run detection, sleep ~200ms (coarse but responsive). */
void *deadlock_monitor_main(void *) {
    while (deadlock_thread_running) {
        if (g_deadlock_state != nullptr) {
            pthread_mutex_lock(&g_deadlock_state->state_mutex);
            /* keep existing detection logic centralized */
            cr_arbiter_deadlock_tick(g_deadlock_state);
            pthread_mutex_unlock(&g_deadlock_state->state_mutex);
        }
        usleep(200000); /* 200ms monitor interval */
    }
    return nullptr;
}

}  // namespace

/* Reset per-entity request table at match start. */
void cr_arbiter_deadlock_setup(cr_shared_state_t *state) {
    (void)state;
    for (int i = 0; i < CR_MAX_ENTITIES; ++i) {
        requesting_artifact[i] = -1;
    }
}

/*
 * Non-blocking acquire: if free, grant; if already owner, succeed; else record wait on
 * requesting_artifact[entity_id] and return 0 so the action can retry next tick.
 */
int cr_arbiter_deadlock_try_acquire_artifact(cr_shared_state_t *state, int entity_id, int artifact_id) {
    if (!is_valid_entity_for_artifacts(state, entity_id)) {
        return 0;
    }
    if (artifact_id < 0 || artifact_id >= CR_MAX_ARTIFACTS) {
        return 0;
    }

    cr_artifact_state_t *artifact = &state->artifacts[artifact_id];
    if (artifact->held_by_entity_id == entity_id) {
        clear_entity_request(entity_id);
        return 1;
    }
    if (artifact->held_by_entity_id == -1) {
        artifact->held_by_entity_id = entity_id;
        artifact->locked = 1;
        clear_entity_request(entity_id);
        return 1;
    }

    requesting_artifact[entity_id] = artifact_id;
    return 0;
}

/* Death or forced resolution: entity drops every artifact and clears its wait edge. */
void cr_arbiter_deadlock_release_artifacts(cr_shared_state_t *state, int entity_id) {
    if (!is_valid_entity_for_artifacts(state, entity_id)) {
        return;
    }
    release_all_artifacts_held_by(state, entity_id);
    clear_entity_request(entity_id);
}

/*
 * Optional dev injection + detection pass. Victim selection uses max(owner_a, owner_b) as a
 * deterministic tie-break for who loses all held artifacts.
 */
void cr_arbiter_deadlock_tick(cr_shared_state_t *state) {
#if CR_DEV_MODE
    /*
     * Deterministic demo trigger (one-shot):
     * creates a 2-way circular wait so deadlock detection can be observed in logs.
     */
    if (!demo_injected && state->turn.turn_seq >= 8 && state->player_count > 0 && state->npc_count_current > 0) {
        const int entity_a = 0;
        int entity_b = -1;
        for (int i = 0; i < state->npc_count_current; ++i) {
            const int candidate = CR_MAX_PLAYERS + i;
            if (state->entities[candidate].alive) {
                entity_b = candidate;
                break;
            }
        }

        if (entity_b >= 0 && is_valid_entity_for_artifacts(state, entity_a) &&
            is_valid_entity_for_artifacts(state, entity_b) &&
            state->entities[entity_a].alive && state->entities[entity_b].alive) {
            state->artifacts[CR_ARTIFACT_SOLAR_CORE].held_by_entity_id = entity_a;
            state->artifacts[CR_ARTIFACT_SOLAR_CORE].locked = 1;
            state->artifacts[CR_ARTIFACT_LUNAR_BLADE].held_by_entity_id = entity_b;
            state->artifacts[CR_ARTIFACT_LUNAR_BLADE].locked = 1;
            requesting_artifact[entity_a] = CR_ARTIFACT_LUNAR_BLADE;
            requesting_artifact[entity_b] = CR_ARTIFACT_SOLAR_CORE;
            demo_injected = 1;
            printf("[arbiter][deadlock-demo] injected circular wait: %d<->%d\n", entity_a, entity_b);
        }
    }
#endif

    for (int a = 0; a < CR_MAX_ARTIFACTS; ++a) {
        const int owner_a = state->artifacts[a].held_by_entity_id;
        if (!is_valid_entity_for_artifacts(state, owner_a)) {
            continue;
        }

        const int wait_artifact_a = requesting_artifact[owner_a];
        if (wait_artifact_a < 0 || wait_artifact_a >= CR_MAX_ARTIFACTS) {
            continue;
        }

        const int owner_b = state->artifacts[wait_artifact_a].held_by_entity_id;
        if (!is_valid_entity_for_artifacts(state, owner_b) || owner_b == owner_a) {
            continue;
        }

        const int wait_artifact_b = requesting_artifact[owner_b];
        if (wait_artifact_b < 0 || wait_artifact_b >= CR_MAX_ARTIFACTS) {
            continue;
        }

        const int owner_of_b_wait = state->artifacts[wait_artifact_b].held_by_entity_id;
        if (owner_of_b_wait != owner_a) {
            continue;
        }

        /* 2-way circular wait detected: owner_a <-> owner_b */
        const int victim = (owner_a > owner_b) ? owner_a : owner_b;
        printf("[arbiter][deadlock] circular wait detected between %d and %d; forcing release by %d\n",
               owner_a, owner_b, victim);

        release_all_artifacts_held_by(state, victim);
        clear_entity_request(victim);
    }
}

/* Spawn deadlock_monitor_main once; idempotent if already running. */
int cr_arbiter_deadlock_start_monitor(cr_shared_state_t *state) {
    if (deadlock_thread_running) {
        return 0;
    }
    g_deadlock_state = state;
    deadlock_thread_running = 1;
    return pthread_create(&deadlock_thread, nullptr, deadlock_monitor_main, nullptr);
}

/* Cooperative shutdown flag + join (called from arbiter cleanup). */
void cr_arbiter_deadlock_stop_monitor() {
    if (!deadlock_thread_running) {
        return;
    }
    deadlock_thread_running = 0;
    pthread_join(deadlock_thread, nullptr);
}
