//Arbiter-side OS signal integration for timed stun and "ultimate" ASP pause.
//Stun uses wall-clock expiry in entity->stun_end_epoch. Ultimate uses SIGSTOP/SIGCONT
//on the ASP process plus SIGALRM to resume after CR_ULTIMATE_PAUSE_SECONDS.
#include "../shared.h"
#include "arbiter_ui.h"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <unistd.h>

using namespace std;

namespace {

cr_shared_state_t *g_state = nullptr;

/* SIGALRM: resume ASP and clear asp_paused_for_ultimate (must be async-signal-safe enough here). */
void handle_ultimate_alarm(int) {
    if (g_state == nullptr) {
        return;
    }
    if (g_state->asp_pid > 0) {
        kill(g_state->asp_pid, SIGCONT);
    }
    g_state->asp_paused_for_ultimate = 0;
    if (!cr_arbiter_ncurses_ui_active()) {
        printf("[arbiter][signals] ultimate pause ended, ASP resumed.\n");
    }
}

}  // namespace

/* Register SIGALRM handler and remember shared state for the alarm callback. */
void cr_arbiter_signals_setup(cr_shared_state_t *state) {
    g_state = state;
    signal(SIGALRM, handle_ultimate_alarm);
}

/* Called every arbiter loop iteration: clear stun flags when wall clock passes stun_end_epoch. */
void cr_arbiter_signals_tick(cr_shared_state_t *state) {
    const time_t now = time(nullptr);
    for (int i = 0; i < CR_MAX_ENTITIES; ++i) {
        cr_entity_state_t *entity = &state->entities[i];
        if (entity->stunned && entity->stun_end_epoch > 0 && now >= entity->stun_end_epoch) {
            entity->stunned = 0;
            entity->stun_end_epoch = 0;
            if (entity->blocked_from_turn) {
                entity->blocked_from_turn = 0;
                if (!cr_arbiter_ncurses_ui_active()) {
                    printf("[arbiter][signals] %s(%d) stun ended; previously skipped turn now unblocked.\n",
                           entity->name, entity->id);
                }
            }
            if (!cr_arbiter_ncurses_ui_active()) {
                printf("[arbiter][signals] stun expired for %s(%d)\n", entity->name, entity->id);
            }
        }
    }
}

/*
 * Apply stun for CR_STUN_SECONDS. If the victim is currently waiting to act, burn this turn
 * immediately (turn_seq++) so the scheduler can move on; otherwise mark blocked_from_turn if
 * they were already at max stamina so they do not sneak in before stun wears off.
 */
void cr_arbiter_trigger_stun(cr_shared_state_t *state, int target_id) {
    if (target_id < 0 || target_id >= CR_MAX_ENTITIES) {
        return;
    }
    cr_entity_state_t *target = &state->entities[target_id];
    if (!target->alive) {
        return;
    }
    target->stunned = 1;
    target->stun_end_epoch = time(nullptr) + CR_STUN_SECONDS;

    /*
     * Stun skip semantics:
     * if target was currently active and still waiting to act, force-skip that turn now.
     * Stamina is preserved (scheduler excludes stunned entities), so target can be
     * considered again only after stun expiry.
     */
    if (state->turn.active_entity_id == target_id && !state->turn.action_committed) {
        target->blocked_from_turn = 1;
        state->turn.action_committed = 1;
        ++state->turn.turn_seq;
        if (!cr_arbiter_ncurses_ui_active()) {
            printf("[arbiter][signals] active stunned entity %s(%d) forced to skip current turn.\n",
                   target->name, target->id);
        }
    } else if (target->stamina >= target->max_stamina) {
        target->blocked_from_turn = 1;
    }

    if (!cr_arbiter_ncurses_ui_active()) {
        printf("[arbiter][signals] %s(%d) stunned for %d seconds.\n", target->name, target->id,
               CR_STUN_SECONDS);
    }
}

/*
 * If a living player holds BOTH Solar Core and Lunar Blade artifacts, pause ASP with SIGSTOP,
 * start a one-shot alarm for CR_ULTIMATE_PAUSE_SECONDS, then SIGCONT resumes ASP in the handler.
 */
void cr_arbiter_try_trigger_ultimate_pause_for_entity(cr_shared_state_t *state, int entity_id) {
    if (state->asp_paused_for_ultimate) {
        return;
    }
    if (state->asp_pid <= 0) {
        return;
    }
    if (entity_id < 0 || entity_id >= CR_MAX_ENTITIES) {
        return;
    }
    if (!state->entities[entity_id].is_player || !state->entities[entity_id].alive) {
        return;
    }

    const int holds_solar =
        state->artifacts[CR_ARTIFACT_SOLAR_CORE].held_by_entity_id == entity_id;
    const int holds_lunar =
        state->artifacts[CR_ARTIFACT_LUNAR_BLADE].held_by_entity_id == entity_id;
    if (!holds_solar || !holds_lunar) {
        return;
    }

    if (kill(state->asp_pid, SIGSTOP) == 0) {
        state->asp_paused_for_ultimate = 1;
        alarm(CR_ULTIMATE_PAUSE_SECONDS);
        if (!cr_arbiter_ncurses_ui_active()) {
            printf("[arbiter][signals] ultimate pause started for %d seconds by %s(%d).\n",
                   CR_ULTIMATE_PAUSE_SECONDS, state->entities[entity_id].name, entity_id);
        }
    }
}
