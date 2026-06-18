// One pthread per possible player slot; submits player_actions[i] when
 //the arbiter grants that entity a turn. Human lines come from input.cpp; CR_TEST_MODE keeps
 // automated tests working without a TTY.

#include "player_threads.h"

#include "hip_tui.h"
#include "input.h"

#include "../shared.h"

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <pthread.h>
#include <unistd.h>

using namespace std;

namespace {

cr_shared_state_t *g_state = nullptr;
volatile sig_atomic_t g_workers_stop = 0;
pthread_t g_threads[CR_MAX_PLAYERS];
int g_threads_started = 0;

int g_last_menu_seq[CR_MAX_PLAYERS];

int pick_first_alive_npc(const cr_shared_state_t *state) {
    for (int i = 0; i < state->npc_count_current; ++i) {
        const int id = CR_MAX_PLAYERS + i;
        if (state->entities[id].alive) {
            return id;
        }
    }
    return -1;
}

int pick_lowest_hp_npc_id(const cr_shared_state_t *state) {
    int best = -1;
    int best_hp = 2147483647;
    for (int i = 0; i < state->npc_count_current; ++i) {
        const int id = CR_MAX_PLAYERS + i;
        const cr_entity_state_t *n = &state->entities[id];
        if (!n->alive) {
            continue;
        }
        if (n->hp < best_hp) {
            best_hp = n->hp;
            best = id;
        }
    }
    return best;
}

int pick_highest_stamina_npc_id(const cr_shared_state_t *state) {
    int best = -1;
    int best_st = -1;
    for (int i = 0; i < state->npc_count_current; ++i) {
        const int id = CR_MAX_PLAYERS + i;
        const cr_entity_state_t *n = &state->entities[id];
        if (!n->alive) {
            continue;
        }
        if (n->stamina > best_st) {
            best_st = n->stamina;
            best = id;
        }
    }
    return best;
}

int is_alive_npc_target(const cr_shared_state_t *state, int target_id) {
    if (target_id < CR_MAX_PLAYERS || target_id >= CR_MAX_PLAYERS + state->npc_count_current) {
        return 0;
    }
    return state->entities[target_id].alive != 0;
}

int player_may_use_weapon_now(const cr_shared_state_t *state, int entity_id, cr_weapon_type_t w) {
    if (w == CR_WEAPON_NONE) {
        return 0;
    }
    if (!cr_is_artifact_weapon(w)) {
        return 1;
    }
    switch (w) {
        case CR_WEAPON_SOLAR_CORE:
            return state->artifacts[CR_ARTIFACT_SOLAR_CORE].held_by_entity_id == entity_id;
        case CR_WEAPON_LUNAR_BLADE:
            return state->artifacts[CR_ARTIFACT_LUNAR_BLADE].held_by_entity_id == entity_id;
        case CR_WEAPON_ECLIPSE_RELIC:
            return state->artifacts[CR_ARTIFACT_ECLIPSE_RELIC].exists_in_world != 0 &&
                   state->artifacts[CR_ARTIFACT_ECLIPSE_RELIC].held_by_entity_id == entity_id;
        default:
            return 0;
    }
}

cr_weapon_type_t pick_best_usable_weapon(const cr_shared_state_t *state, const cr_entity_state_t *pl,
                                         int entity_id) {
    cr_weapon_type_t best = CR_WEAPON_NONE;
    int best_dmg = -1;
    int i = 0;
    while (i < CR_MAX_INVENTORY_SLOTS) {
        const cr_weapon_type_t w = (cr_weapon_type_t)pl->inventory.slots[i];
        if (w == CR_WEAPON_NONE) {
            ++i;
            continue;
        }
        const int span = cr_weapon_slot_size(w);
        const int step = span > 0 ? span : 1;
        if (player_may_use_weapon_now(state, entity_id, w)) {
            const int d = cr_weapon_damage(w);
            if (d > best_dmg) {
                best_dmg = d;
                best = w;
            }
        }
        i += step;
    }
    return best;
}

cr_weapon_type_t first_weapon_in_storage(const cr_inventory_t *inv) {
    if (inv->storage_count <= 0) {
        return CR_WEAPON_NONE;
    }
    return (cr_weapon_type_t)inv->storage[0];
}

void fill_slot_ready(cr_action_msg_t *slot, int actor_id, int turn_seq, cr_action_type_t type,
                     int target_id, cr_weapon_type_t weapon) {
    slot->status = CR_ACTION_STATUS_READY;
    slot->type = type;
    slot->actor_entity_id = actor_id;
    slot->target_entity_id = target_id;
    slot->weapon = weapon;
    slot->requested_by_turn_seq = turn_seq;
    clock_gettime(CLOCK_REALTIME, &slot->created_at);
    if (g_state != nullptr) {
        sem_post(&g_state->sem_player_action_ready);
    }
}

/* Fills a synthetic action when CR_TEST_MODE=1 (headless CI); not used in normal play. */
void apply_test_mode_defaults(cr_shared_state_t *state, int actor_id, cr_action_msg_t *slot) {
    slot->status = CR_ACTION_STATUS_READY;
    slot->type = CR_ACTION_ATTACK_STRIKE;
    slot->actor_entity_id = actor_id;
    slot->target_entity_id = pick_first_alive_npc(state);
    slot->weapon = CR_WEAPON_NONE;
    if (actor_id == 0 && (state->turn.turn_seq % 3) == 1) {
        slot->type = CR_ACTION_USE_WEAPON;
        slot->weapon = CR_WEAPON_SOLAR_CORE;
    }
    slot->requested_by_turn_seq = state->turn.turn_seq;
    clock_gettime(CLOCK_REALTIME, &slot->created_at);
}

void print_turn_menu(cr_shared_state_t *state, int player_id) {
    const cr_entity_state_t *e = &state->entities[player_id];
    fprintf(stderr,
            "\n=== YOUR TURN: %s (id=%d)  seq=%d ===\n"
            "  1 STRIKE   2 EXHAUST   3 USE_WEAPON   4 HEAL   5 SKIP   6 SWAP_IN   7 QUIT\n"
            "  Optional: second number = target NPC id only (default: softest NPC for attack).\n"
            "  For 3/6 add weapon type id (see shared.h CR_WEAPON_*). Defaults: best equipped / first stored.\n"
            "Enter choice: ",
            e->name, player_id, state->turn.turn_seq);
    fflush(stderr);
}

/*
 * Parse a queued human line. Returns 1 if slot was filled.
 */
int try_parse_player_line(cr_shared_state_t *state, int player_id, const char *line,
                          cr_action_msg_t *slot) {
    if (line == nullptr || line[0] == '\0') {
        return 0;
    }

    char buf[256];
    snprintf(buf, sizeof buf, "%s", line);
    char *save = nullptr;
    char *tok = strtok_r(buf, " \t", &save);
    if (tok == nullptr) {
        return 0;
    }

    const int cmd = atoi(tok);
    char *tok2 = strtok_r(nullptr, " \t", &save);
    char *tok3 = strtok_r(nullptr, " \t", &save);

    const cr_entity_state_t *pl = &state->entities[player_id];

    switch (cmd) {
        case 1: { /* STRIKE */
            int target = tok2 != nullptr ? atoi(tok2) : pick_lowest_hp_npc_id(state);
            if (!is_alive_npc_target(state, target)) {
                fprintf(stderr, "[hip] STRIKE: target must be an alive NPC id.\n");
                return 0;
            }
            fill_slot_ready(slot, player_id, state->turn.turn_seq, CR_ACTION_ATTACK_STRIKE, target,
                            CR_WEAPON_NONE);
            return 1;
        }
        case 2: { /* EXHAUST */
            int target = tok2 != nullptr ? atoi(tok2) : pick_highest_stamina_npc_id(state);
            if (!is_alive_npc_target(state, target)) {
                fprintf(stderr, "[hip] EXHAUST: target must be an alive NPC id.\n");
                return 0;
            }
            fill_slot_ready(slot, player_id, state->turn.turn_seq, CR_ACTION_ATTACK_EXHAUST, target,
                            CR_WEAPON_NONE);
            return 1;
        }
        case 3: { /* USE_WEAPON — optional: "3 [target] [weapon]" */
            int t = pick_lowest_hp_npc_id(state);
            cr_weapon_type_t w = pick_best_usable_weapon(state, pl, player_id);
            if (tok2 != nullptr && tok3 != nullptr) {
                t = atoi(tok2);
                w = (cr_weapon_type_t)atoi(tok3);
            } else if (tok2 != nullptr) {
                t = atoi(tok2);
            }
            if (!is_alive_npc_target(state, t) || w == CR_WEAPON_NONE) {
                fprintf(stderr, "[hip] USE_WEAPON: need alive NPC target and equipped weapon.\n");
                return 0;
            }
            fill_slot_ready(slot, player_id, state->turn.turn_seq, CR_ACTION_USE_WEAPON, t, w);
            return 1;
        }
        case 4:
            fill_slot_ready(slot, player_id, state->turn.turn_seq, CR_ACTION_HEAL, player_id,
                            CR_WEAPON_NONE);
            return 1;
        case 5:
            fill_slot_ready(slot, player_id, state->turn.turn_seq, CR_ACTION_SKIP, -1,
                            CR_WEAPON_NONE);
            return 1;
        case 6: { /* SWAP_IN */
            cr_weapon_type_t w = CR_WEAPON_NONE;
            if (tok2 != nullptr) {
                w = (cr_weapon_type_t)atoi(tok2);
            }
            if (w == CR_WEAPON_NONE) {
                w = first_weapon_in_storage(&pl->inventory);
            }
            if (w == CR_WEAPON_NONE) {
                fprintf(stderr, "[hip] no weapon in storage to swap in.\n");
                return 0;
            }
            fill_slot_ready(slot, player_id, state->turn.turn_seq, CR_ACTION_SWAP_IN, -1, w);
            return 1;
        }
        case 7:
            fill_slot_ready(slot, player_id, state->turn.turn_seq, CR_ACTION_QUIT, -1,
                            CR_WEAPON_NONE);
            return 1;
        default:
            fprintf(stderr, "[hip] unknown command %d — try 1-7.\n", cmd);
            return 0;
    }
}

void try_submit_for_player(cr_shared_state_t *state, int player_id, const char *inject_line) {
    if (state->phase != CR_PHASE_RUNNING || !state->game_running) {
        return;
    }
    if (state->turn.action_committed || !state->turn.active_is_player) {
        return;
    }
    if (player_id < 0 || player_id >= state->player_count) {
        return;
    }
    if (state->turn.active_entity_id != player_id) {
        return;
    }

    cr_action_msg_t *slot = &state->player_actions[player_id];
    if (slot->status == CR_ACTION_STATUS_READY &&
        slot->requested_by_turn_seq == state->turn.turn_seq) {
        return;
    }

    if (inject_line != nullptr) {
        if (inject_line[0] != '\0' && try_parse_player_line(state, player_id, inject_line, slot)) {
            if (!hip_tui_is_enabled()) {
                fprintf(stderr, "[hip] submitted action for %s\n", state->entities[player_id].name);
            }
        }
        return;
    }

    const char *test_env = getenv("CR_TEST_MODE");
    const int auto_test = (test_env != nullptr && strcmp(test_env, "1") == 0);
    if (!auto_test) {
        if (state->turn.turn_seq != g_last_menu_seq[player_id]) {
            g_last_menu_seq[player_id] = state->turn.turn_seq;
            print_turn_menu(state, player_id);
        }
    } else {
        g_last_menu_seq[player_id] = state->turn.turn_seq;
    }

    char line[256];
    if (hip_input_poll_line(line, sizeof line)) {
        if (try_parse_player_line(state, player_id, line, slot)) {
            fprintf(stderr, "[hip] submitted action for %s\n", state->entities[player_id].name);
        }
        return;
    }

    if (auto_test) {
        apply_test_mode_defaults(state, player_id, slot);
    }
}

void *player_worker_main(void *opaque) {
    const intptr_t idx = reinterpret_cast<intptr_t>(opaque);
    while (!g_workers_stop) {
        cr_shared_state_t *st = g_state;
        if (st == nullptr) {
            break;
        }

        const char *test_env = getenv("CR_TEST_MODE");
        const int auto_test = (test_env != nullptr && strcmp(test_env, "1") == 0);

        pthread_mutex_lock(&st->state_mutex);
        const int running = st->game_running ? 1 : 0;
        if (!running) {
            pthread_mutex_unlock(&st->state_mutex);
            break;
        }

        if (hip_tui_is_enabled() && !auto_test) {
            const int pid = static_cast<int>(idx);
            const int our_turn =
                st->phase == CR_PHASE_RUNNING && st->turn.active_is_player != 0 &&
                st->turn.active_entity_id == pid && st->turn.action_committed == 0;
            cr_action_msg_t *slot = &st->player_actions[pid];
            const int already = slot->status == CR_ACTION_STATUS_READY &&
                                slot->requested_by_turn_seq == st->turn.turn_seq;
            pthread_mutex_unlock(&st->state_mutex);

            if (our_turn && !already) {
                char line[256];
                line[0] = '\0';
                hip_tui_read_player_line(st, pid, line, sizeof line);
                pthread_mutex_lock(&st->state_mutex);
                if (st->game_running) {
                    try_submit_for_player(st, pid, line);
                }
                pthread_mutex_unlock(&st->state_mutex);
            }
        } else {
            try_submit_for_player(st, static_cast<int>(idx), nullptr);
            pthread_mutex_unlock(&st->state_mutex);
        }

        usleep(80 * 1000);
    }
    return nullptr;
}

}  // namespace

int hip_player_threads_start(cr_shared_state_t *state) {
    if (state == nullptr || g_threads_started != 0) {
        return -1;
    }
    g_state = state;
    g_workers_stop = 0;
    for (int i = 0; i < CR_MAX_PLAYERS; ++i) {
        g_last_menu_seq[i] = -1;
    }

    for (int i = 0; i < CR_MAX_PLAYERS; ++i) {
        const int rc =
            pthread_create(&g_threads[i], nullptr, player_worker_main,
                           reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        if (rc != 0) {
            g_workers_stop = 1;
            for (int j = 0; j < i; ++j) {
                pthread_join(g_threads[j], nullptr);
            }
            g_state = nullptr;
            g_threads_started = 0;
            return rc;
        }
    }
    g_threads_started = CR_MAX_PLAYERS;
    return 0;
}

void hip_player_threads_stop() {
    if (g_threads_started == 0) {
        return;
    }
    g_workers_stop = 1;
    for (int i = 0; i < CR_MAX_PLAYERS; ++i) {
        pthread_join(g_threads[i], nullptr);
    }
    g_threads_started = 0;
    g_state = nullptr;
}
