// NPC decision policy for ASP (no shared RNG mutation; uses turn/ids only).
 // Goals: focus fragile players, self-heal when hurt, pull weapons from storage (SWAP_IN),
// occasionally exhaust high-stamina threats, use equipped weapons when advantageous.
 
#include "npc_logic.h"

using namespace std;

namespace {

unsigned tactic_mix(const cr_shared_state_t *state, int actor_id) {
    return (unsigned)actor_id * 2654435761u ^ (unsigned)state->turn.turn_seq * 1597334677u;
}

int pick_lowest_hp_player_id(const cr_shared_state_t *state) {
    int best = -1;
    int best_hp = 2147483647;
    for (int id = 0; id < state->player_count; ++id) {
        const cr_entity_state_t *p = &state->entities[id];
        if (!p->alive) {
            continue;
        }
        if (p->hp < best_hp) {
            best_hp = p->hp;
            best = id;
        }
    }
    return best;
}

int pick_highest_hp_player_id(const cr_shared_state_t *state) {
    int best = -1;
    int best_hp = -1;
    for (int id = 0; id < state->player_count; ++id) {
        const cr_entity_state_t *p = &state->entities[id];
        if (!p->alive) {
            continue;
        }
        if (p->hp > best_hp) {
            best_hp = p->hp;
            best = id;
        }
    }
    return best;
}

int pick_highest_stamina_player_id(const cr_shared_state_t *state) {
    int best = -1;
    int best_st = -1;
    for (int id = 0; id < state->player_count; ++id) {
        const cr_entity_state_t *p = &state->entities[id];
        if (!p->alive) {
            continue;
        }
        if (p->stamina > best_st) {
            best_st = p->stamina;
            best = id;
        }
    }
    return best;
}

/*
 * Strike / weapon target: mostly focus lowest-HP player, but sometimes hit the tank or a random
 * alive player so both humans take damage in multi-player games.
 */
int pick_player_attack_focus(const cr_shared_state_t *state, int actor_id) {
    const int soft = pick_lowest_hp_player_id(state);
    if (soft < 0) {
        return soft;
    }
    if (state->player_count < 2) {
        return soft;
    }
    const unsigned mix = tactic_mix(state, actor_id);
    const unsigned mode = mix % 10u;
    if (mode >= 6u) {
        const int tank = pick_highest_hp_player_id(state);
        if (tank >= 0) {
            return tank;
        }
    } else if (mode == 5u) {
        const int start = (int)((mix >> 5) % (unsigned)state->player_count);
        for (int j = 0; j < state->player_count; ++j) {
            const int id = (start + j) % state->player_count;
            if (state->entities[id].alive) {
                return id;
            }
        }
    }
    return soft;
}

int npc_may_use_weapon_now(const cr_shared_state_t *state, int entity_id, cr_weapon_type_t w) {
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

cr_weapon_type_t pick_best_usable_weapon(const cr_shared_state_t *state, const cr_entity_state_t *npc,
                                         int actor_id) {
    cr_weapon_type_t best = CR_WEAPON_NONE;
    int best_dmg = -1;
    int i = 0;
    while (i < CR_MAX_INVENTORY_SLOTS) {
        const cr_weapon_type_t w = (cr_weapon_type_t)npc->inventory.slots[i];
        if (w == CR_WEAPON_NONE) {
            ++i;
            continue;
        }
        const int span = cr_weapon_slot_size(w);
        const int step = span > 0 ? span : 1;
        if (npc_may_use_weapon_now(state, actor_id, w)) {
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

/* Strongest weapon type sitting in long-term storage (for SWAP_IN choice). */
cr_weapon_type_t best_weapon_in_storage(const cr_inventory_t *inv) {
    cr_weapon_type_t best = CR_WEAPON_NONE;
    int best_dmg = -1;
    for (int s = 0; s < inv->storage_count; ++s) {
        const cr_weapon_type_t w = (cr_weapon_type_t)inv->storage[s];
        const int d = cr_weapon_damage(w);
        if (d > best_dmg) {
            best_dmg = d;
            best = w;
        }
    }
    return best;
}

void fill_ready_slot(cr_shared_state_t *state, cr_action_msg_t *slot, int actor_id, int turn_seq,
                     cr_action_type_t type,
                     int target_id, cr_weapon_type_t weapon) {
    slot->status = CR_ACTION_STATUS_READY;
    slot->type = type;
    slot->actor_entity_id = actor_id;
    slot->target_entity_id = target_id;
    slot->weapon = weapon;
    slot->requested_by_turn_seq = turn_seq;
    clock_gettime(CLOCK_REALTIME, &slot->created_at);
    if (state != nullptr) {
        sem_post(&state->sem_npc_action_ready);
    }
}

}  // namespace

void asp_submit_npc_action_for_slot(cr_shared_state_t *state, int npc_slot_index) {
    if (state->phase != CR_PHASE_RUNNING || !state->game_running) {
        return;
    }
    if (state->turn.action_committed || state->turn.active_is_player) {
        return;
    }
    if (npc_slot_index < 0 || npc_slot_index >= state->npc_count_current) {
        return;
    }

    const int actor_id = CR_MAX_PLAYERS + npc_slot_index;
    if (state->turn.active_entity_id != actor_id) {
        return;
    }

    cr_entity_state_t *actor = &state->entities[actor_id];
    if (!actor->alive || actor->stunned) {
        return;
    }

    cr_action_msg_t *slot = &state->npc_actions[npc_slot_index];
    if (slot->status == CR_ACTION_STATUS_READY && slot->requested_by_turn_seq == state->turn.turn_seq) {
        return;
    }

    const int focus = pick_player_attack_focus(state, actor_id);
    if (focus < 0) {
        fill_ready_slot(state, slot, actor_id, state->turn.turn_seq, CR_ACTION_SKIP, -1, CR_WEAPON_NONE);
        return;
    }

    const unsigned mix = tactic_mix(state, actor_id);

    if (actor->max_hp > 0) {
        const int hp_pct = (100 * actor->hp) / actor->max_hp;
        if (hp_pct < 35) {
            fill_ready_slot(state, slot, actor_id, state->turn.turn_seq, CR_ACTION_HEAL, focus,
                            CR_WEAPON_NONE);
            return;
        }
    }

    /*
     * Bring a better weapon from storage into primary inventory when current best equipped
     * is weak or empty. Arbiter may reject if no contiguous space (action fails safely).
     */
    const cr_weapon_type_t stored = best_weapon_in_storage(&actor->inventory);
    if (stored != CR_WEAPON_NONE && actor->inventory.storage_count > 0 && (mix % 11u) == 0u) {
        const cr_weapon_type_t equipped_best = pick_best_usable_weapon(state, actor, actor_id);
        const int eq_dmg = cr_weapon_damage(equipped_best);
        const int st_dmg = cr_weapon_damage(stored);
        if (st_dmg > eq_dmg) {
            fill_ready_slot(state, slot, actor_id, state->turn.turn_seq, CR_ACTION_SWAP_IN, focus, stored);
            return;
        }
    }

    const int high_stamina_target = pick_highest_stamina_player_id(state);
    if (high_stamina_target >= 0) {
        const cr_entity_state_t *t = &state->entities[high_stamina_target];
        if (t->max_stamina > 0 && t->stamina * 10 >= t->max_stamina * 7 && (mix % 7u) == 0u) {
            fill_ready_slot(state, slot, actor_id, state->turn.turn_seq, CR_ACTION_ATTACK_EXHAUST,
                            high_stamina_target, CR_WEAPON_NONE);
            return;
        }
    }

    const cr_weapon_type_t weapon = pick_best_usable_weapon(state, actor, actor_id);
    const int w_dmg = cr_weapon_damage(weapon);
    if (weapon != CR_WEAPON_NONE && w_dmg > actor->damage && (mix % 2u) == 0u) {
        fill_ready_slot(state, slot, actor_id, state->turn.turn_seq, CR_ACTION_USE_WEAPON, focus, weapon);
        return;
    }

    fill_ready_slot(state, slot, actor_id, state->turn.turn_seq, CR_ACTION_ATTACK_STRIKE, focus,
                    CR_WEAPON_NONE);
}

void asp_submit_npc_action(cr_shared_state_t *state) {
    const int actor_id = state->turn.active_entity_id;
    const int npc_index = actor_id - CR_MAX_PLAYERS;
    if (npc_index < 0 || npc_index >= CR_MAX_NPCS) {
        return;
    }
    asp_submit_npc_action_for_slot(state, npc_index);
}
