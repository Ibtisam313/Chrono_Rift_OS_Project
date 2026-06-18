/*
 * npc_threads.cpp — One pthread per NPC slot (0 .. CR_MAX_NPCS-1) in the ASP process.
 *
 * Matches course expectation of pthread-based concurrency inside ASP: each worker only
 * submits to npc_actions[slot] when the arbiter’s active_entity_id is that NPC.
 */
#include "npc_threads.h"

#include "npc_logic.h"

#include <pthread.h>
#include <unistd.h>

using namespace std;

namespace {

cr_shared_state_t *g_worker_state = nullptr;
volatile sig_atomic_t g_workers_stop = 0;

pthread_t g_threads[CR_MAX_NPCS];
int g_threads_started = 0;

void *npc_worker_main(void *opaque) {
    const intptr_t slot = reinterpret_cast<intptr_t>(opaque);
    while (!g_workers_stop) {
        cr_shared_state_t *st = g_worker_state;
        if (st == nullptr) {
            break;
        }

        pthread_mutex_lock(&st->state_mutex);
        const int running = st->game_running ? 1 : 0;
        if (running && st->phase == CR_PHASE_RUNNING) {
            asp_submit_npc_action_for_slot(st, static_cast<int>(slot));
        }
        pthread_mutex_unlock(&st->state_mutex);

        if (!running) {
            break;
        }

        /* Short sleep: react before NPC turn timeout (CR_NPC_TURN_TIMEOUT_SECONDS). */
        usleep(80 * 1000);
    }
    return nullptr;
}

}  // namespace

int asp_npc_threads_start(cr_shared_state_t *state) {
    if (state == nullptr || g_threads_started != 0) {
        return -1;
    }
    g_worker_state = state;
    g_workers_stop = 0;

    for (int i = 0; i < CR_MAX_NPCS; ++i) {
        const int rc =
            pthread_create(&g_threads[i], nullptr, npc_worker_main,
                           reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        if (rc != 0) {
            g_workers_stop = 1;
            for (int j = 0; j < i; ++j) {
                pthread_join(g_threads[j], nullptr);
            }
            g_worker_state = nullptr;
            g_threads_started = 0;
            return rc;
        }
    }
    g_threads_started = CR_MAX_NPCS;
    return 0;
}

void asp_npc_threads_stop() {
    if (g_threads_started == 0) {
        return;
    }
    g_workers_stop = 1;
    for (int i = 0; i < CR_MAX_NPCS; ++i) {
        pthread_join(g_threads[i], nullptr);
    }
    g_threads_started = 0;
    g_worker_state = nullptr;
}
