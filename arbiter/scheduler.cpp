// Arbiter game rules: entity init, stamina scheduling, action application,
//win/lose, inventory allocation, NPC respawn, and combat logging.
//Scheduling model: each tick advances stamina for all eligible entities by the minimum
//whole seconds needed until someone reaches max_stamina; the ready actor with the
//lowest entity id wins ties. Actions are read from per-player / per-npc slots in shm.
#include "../shared.h"
#include "arbiter_ui.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

using namespace std;

int cr_arbiter_deadlock_try_acquire_artifact(cr_shared_state_t *state, int entity_id, int artifact_id);
void cr_arbiter_deadlock_release_artifacts(cr_shared_state_t *state, int entity_id);
void cr_arbiter_trigger_stun(cr_shared_state_t *state, int target_id);
void cr_arbiter_try_trigger_ultimate_pause_for_entity(cr_shared_state_t *state, int entity_id);

namespace {

void append_log(cr_shared_state_t *state, const char *text);
void handle_weapon_drop_on_npc_death(cr_shared_state_t *state, cr_entity_state_t *killer,
                                     cr_entity_state_t *dead_npc);
uint32_t next_rand(cr_shared_state_t *state);

/* Reset one entity record to sane defaults and copy the display name (NUL-terminated). */
void init_entity(cr_entity_state_t *entity, int id, int is_player, const char *name, int hp, int damage,
                 int speed, int max_stamina) {
    memset(entity, 0, sizeof(*entity));
    entity->id = id;
    entity->is_player = is_player;
    entity->alive = 1;
    entity->hp = hp;
    entity->max_hp = hp;
    entity->damage = damage;
    entity->speed = speed;
    entity->stamina = 0;
    entity->max_stamina = max_stamina;
    strncpy(entity->name, name, CR_MAX_NAME_LEN - 1);
    entity->name[CR_MAX_NAME_LEN - 1] = '\0';
}

/* Uniform integer in [lo, hi] using next_rand (inclusive endpoints). */
int random_range_inclusive(cr_shared_state_t *state, int lo, int hi) {
    if (hi < lo) {
        return lo;
    }
    const uint32_t span = (uint32_t)(hi - lo + 1);
    return lo + (int)(next_rand(state) % span);
}

/* Linear congruential step on state->rng_seed; same algorithm across the match for reproducibility. */
uint32_t next_rand(cr_shared_state_t *state) {
    if (state->rng_seed == 0) {
        state->rng_seed = 0xA341316Cu; /* fallback deterministic seed until roll seed is provided */
    }
    state->rng_seed = (1103515245u * state->rng_seed + 12345u);
    return state->rng_seed;
}

/*
 * For each dead NPC slot in the current encounter table, roll new stats and revive.
 * Stops when win kill count already reached (no endless respawn after victory).
 */
void respawn_npc_if_needed(cr_shared_state_t *state) {
    static int spawn_serial = 0;

    if (state->phase != CR_PHASE_RUNNING) {
        return;
    }
    if (state->total_npc_kills >= CR_WIN_KILL_TARGET) {
        return;
    }

    for (int i = 0; i < state->npc_count_current; ++i) {
        const int entity_id = CR_MAX_PLAYERS + i;
        cr_entity_state_t *npc = &state->entities[entity_id];
        if (npc->alive) {
            continue;
        }

        const int hp = 120 + (int)(next_rand(state) % 90);
        const int damage = 12 + (int)(next_rand(state) % 7);
        const int speed = 10 + (int)(next_rand(state) % 21); /* 10..30 */

        char npc_name[CR_MAX_NAME_LEN];
        snprintf(npc_name, sizeof(npc_name), "NPC-%d", entity_id);
        init_entity(npc, entity_id, 0, npc_name, hp, damage, speed, CR_NPC_MAX_STAMINA);

        char log_text[CR_LOG_LINE_LEN];
        snprintf(log_text, sizeof(log_text), "NPC respawned: %s hp=%d dmg=%d spd=%d",
                 npc->name, npc->hp, npc->damage, npc->speed);
        append_log(state, log_text);
        ++spawn_serial;
    }
}

/* Ring-buffer log line; indices use log_write_seq % CR_MAX_LOG_LINES. */
void append_log(cr_shared_state_t *state, const char *text) {
    const uint64_t idx = state->log_write_seq % CR_MAX_LOG_LINES;
    cr_log_entry_t *entry = &state->logs[idx];
    entry->seq = state->log_write_seq + 1;
    clock_gettime(CLOCK_REALTIME, &entry->ts);
    snprintf(entry->text, sizeof(entry->text), "%s", text);
    ++state->log_write_seq;
}

/* Target must be within active player or active NPC id ranges and alive. */
int is_valid_target(const cr_shared_state_t *state, int target_id) {
    if (target_id < 0 || target_id >= CR_MAX_ENTITIES) {
        return 0;
    }

    const int is_player_id = (target_id >= 0 && target_id < state->player_count);
    const int is_npc_id = (target_id >= CR_MAX_PLAYERS &&
                           target_id < CR_MAX_PLAYERS + state->npc_count_current);
    return (is_player_id || is_npc_id) && state->entities[target_id].alive;
}

/* Combat actions must target the opposite side (no friendly fire). */
int is_valid_combat_target(const cr_shared_state_t *state, const cr_entity_state_t *actor,
                           int target_id) {
    if (!is_valid_target(state, target_id)) {
        return 0;
    }
    const cr_entity_state_t *target = &state->entities[target_id];
    return actor->is_player != target->is_player;
}

int entity_has_equipped_weapon(const cr_entity_state_t *actor, cr_weapon_type_t weapon) {
    if (weapon == CR_WEAPON_NONE) {
        return 0;
    }
    int i = 0;
    while (i < CR_MAX_INVENTORY_SLOTS) {
        const cr_weapon_type_t w = (cr_weapon_type_t)actor->inventory.slots[i];
        if (w == CR_WEAPON_NONE) {
            ++i;
            continue;
        }
        const int span = cr_weapon_slot_size(w);
        const int step = span > 0 ? span : 1;
        if (w == weapon) {
            return 1;
        }
        i += step;
    }
    return 0;
}

/* After a full action resolves: reset actor stamina to 0 and advance turn sequence. */
void commit_action(cr_shared_state_t *state, cr_entity_state_t *actor) {
    actor->stamina = 0;
    state->turn.action_committed = 1;
    ++state->turn.turn_seq;
}

/* Basic attack: actor->damage to target HP; on NPC death count kill and maybe drop weapon. */
void apply_strike_action(cr_shared_state_t *state, cr_entity_state_t *actor, const cr_action_msg_t *action) {
    char log_text[CR_LOG_LINE_LEN];
    if (!is_valid_combat_target(state, actor, action->target_entity_id)) {
        snprintf(log_text, sizeof(log_text), "Turn %d rejected: invalid enemy target by %s",
                 state->turn.turn_seq, actor->name);
        append_log(state, log_text);
        return;
    }
    cr_entity_state_t *target = &state->entities[action->target_entity_id];
    target->hp -= actor->damage;
    if (target->hp < 0) {
        target->hp = 0;
    }
    if (target->hp == 0 && target->alive) {
        target->alive = 0;
        if (!target->is_player) {
            ++state->total_npc_kills;
            handle_weapon_drop_on_npc_death(state, actor, target);
        }
        cr_arbiter_deadlock_release_artifacts(state, target->id);
    }
    snprintf(log_text, sizeof(log_text), "Turn %d %s STRIKE %s for %d", state->turn.turn_seq,
             actor->name, target->name, actor->damage);
    append_log(state, log_text);
}

/* All players dead => lose; enough NPC kills => win; sets phase FINISHED and game_running 0. */
void check_end_conditions(cr_shared_state_t *state) {
    if (state->phase != CR_PHASE_RUNNING) {
        return;
    }

    if (!state->artifacts[CR_ARTIFACT_ECLIPSE_RELIC].exists_in_world &&
        state->total_npc_kills >= (CR_WIN_KILL_TARGET / 2)) {
        state->artifacts[CR_ARTIFACT_ECLIPSE_RELIC].exists_in_world = 1;
        state->artifacts[CR_ARTIFACT_ECLIPSE_RELIC].held_by_entity_id = -1;
        state->artifacts[CR_ARTIFACT_ECLIPSE_RELIC].locked = 0;
        append_log(state, "Eclipse Relic manifested in the world.");
    }

    int alive_players = 0;
    for (int i = 0; i < state->player_count; ++i) {
        if (state->entities[i].alive) {
            ++alive_players;
        }
    }

    if (alive_players == 0) {
        append_log(state, "Game over: all player characters defeated.");
        state->end_reason = CR_END_PLAYER_LOSE;
        state->phase = CR_PHASE_FINISHED;
        state->game_running = 0;
        return;
    }

    if (state->total_npc_kills >= CR_WIN_KILL_TARGET) {
        append_log(state, "Victory: players defeated required number of enemies.");
        state->end_reason = CR_END_PLAYER_WIN;
        state->phase = CR_PHASE_FINISHED;
        state->game_running = 0;
    }
}

/* Map equipped weapon enum to parallel artifact slot (or -1 if not an artifact weapon). */
int artifact_id_from_weapon(cr_weapon_type_t weapon) {
    switch (weapon) {
        case CR_WEAPON_SOLAR_CORE:
            return CR_ARTIFACT_SOLAR_CORE;
        case CR_WEAPON_LUNAR_BLADE:
            return CR_ARTIFACT_LUNAR_BLADE;
        case CR_WEAPON_ECLIPSE_RELIC:
            return CR_ARTIFACT_ECLIPSE_RELIC;
        default:
            return -1;
    }
}

/* First starting index of `required_slots` consecutive CR_WEAPON_NONE cells, or -1. */
int find_contiguous_free_block(const cr_inventory_t *inv, int required_slots) {
    if (required_slots <= 0 || required_slots > CR_MAX_INVENTORY_SLOTS) {
        return -1;
    }
    int run = 0;
    int start = -1;
    for (int i = 0; i < CR_MAX_INVENTORY_SLOTS; ++i) {
        if (inv->slots[i] == CR_WEAPON_NONE) {
            if (run == 0) {
                start = i;
            }
            ++run;
            if (run >= required_slots) {
                return start;
            }
        } else {
            run = 0;
            start = -1;
        }
    }
    return -1;
}

/* Fill [start, start+required_slots) with the same weapon id and bump occupied_slots. */
void place_weapon_into_slots(cr_inventory_t *inv, cr_weapon_type_t weapon, int start, int required_slots) {
    for (int i = start; i < start + required_slots; ++i) {
        inv->slots[i] = weapon;
    }
    inv->occupied_slots += required_slots;
}

/* Append one weapon to long-term storage if capacity remains. */
int push_storage(cr_inventory_t *inv, cr_weapon_type_t weapon) {
    if (inv->storage_count >= CR_MAX_WEAPONS_PER_STORAGE) {
        return 0;
    }
    inv->storage[inv->storage_count++] = (int)weapon;
    return 1;
}

/*
 * Move one contiguous equipped run [seg_start, seg_end] into storage and clear those slots.
 * Used when freeing space for a larger weapon footprint.
 */
void remove_segment_and_store(cr_inventory_t *inv, int seg_start, int seg_end) {
    if (seg_start < 0 || seg_end < seg_start || seg_end >= CR_MAX_INVENTORY_SLOTS) {
        return;
    }
    const cr_weapon_type_t weapon = (cr_weapon_type_t)inv->slots[seg_start];
    if (weapon == CR_WEAPON_NONE) {
        return;
    }
    if (!push_storage(inv, weapon)) {
        return;
    }
    for (int i = seg_start; i <= seg_end; ++i) {
        inv->slots[i] = CR_WEAPON_NONE;
    }
    inv->occupied_slots -= (seg_end - seg_start + 1);
    if (inv->occupied_slots < 0) {
        inv->occupied_slots = 0;
    }
}

/* Remove first matching weapon from storage array (compacts tail). Returns 1 if found. */
int pop_storage_weapon(cr_inventory_t *inv, cr_weapon_type_t weapon) {
    for (int i = 0; i < inv->storage_count; ++i) {
        if (inv->storage[i] == weapon) {
            for (int j = i; j < inv->storage_count - 1; ++j) {
                inv->storage[j] = inv->storage[j + 1];
            }
            --inv->storage_count;
            return 1;
        }
    }
    return 0;
}

/*
 * Return a valid start index to place `weapon`, or -1 if impossible.
 * If no contiguous hole exists, evict equipped segments left-to-right into storage until it fits.
 */
int ensure_space_for_weapon(cr_inventory_t *inv, cr_weapon_type_t weapon) {
    const int required_slots = cr_weapon_slot_size(weapon);
    if (required_slots <= 0) {
        return -1;
    }

    int start = find_contiguous_free_block(inv, required_slots);
    if (start >= 0) {
        return start;
    }

    /*
     * Minimal swap-out strategy (heuristic):
     * remove left-to-right occupied segments until enough contiguous space appears.
     */
    int i = 0;
    while (i < CR_MAX_INVENTORY_SLOTS) {
        if (inv->slots[i] == CR_WEAPON_NONE) {
            ++i;
            continue;
        }
        const int seg_start = i;
        const int weapon_id = inv->slots[i];
        while (i + 1 < CR_MAX_INVENTORY_SLOTS && inv->slots[i + 1] == weapon_id) {
            ++i;
        }
        const int seg_end = i;
        remove_segment_and_store(inv, seg_start, seg_end);
        start = find_contiguous_free_block(inv, required_slots);
        if (start >= 0) {
            return start;
        }
        ++i;
    }
    return -1;
}

/* High-level: ensure_space + place_weapon for one entity's primary inventory. */
int equip_weapon_to_entity(cr_entity_state_t *entity, cr_weapon_type_t weapon) {
    const int required_slots = cr_weapon_slot_size(weapon);
    if (required_slots <= 0) {
        return 0;
    }
    const int place_start = ensure_space_for_weapon(&entity->inventory, weapon);
    if (place_start < 0) {
        return 0;
    }
    place_weapon_into_slots(&entity->inventory, weapon, place_start, required_slots);
    return 1;
}

/* Lowest entity id among currently alive NPCs in the active encounter (or -1). */
int pick_alive_npc_id(const cr_shared_state_t *state) {
    for (int i = 0; i < state->npc_count_current; ++i) {
        const int id = CR_MAX_PLAYERS + i;
        if (state->entities[id].alive) {
            return id;
        }
    }
    return -1;
}

/* Reward table when an NPC dies: random non-NONE weapon from the design pool. */
cr_weapon_type_t random_dropped_weapon(cr_shared_state_t *state) {
    static const cr_weapon_type_t pool[] = {
        CR_WEAPON_SOLAR_CORE,  CR_WEAPON_LUNAR_BLADE,  CR_WEAPON_IRON_HALBERD,
        CR_WEAPON_VENOM_DAGGER, CR_WEAPON_THUNDERSTAFF, CR_WEAPON_OBSIDIAN_AXE,
        CR_WEAPON_FROSTBOW,     CR_WEAPON_SPLINTER_STICK
    };
    const int idx = (int)(next_rand(state) % (sizeof(pool) / sizeof(pool[0])));
    return pool[idx];
}

/*
 * Spec simplification: killing player may auto-accept/decline pickup; if not equipped,
 * first alive NPC tries to take the drop. Logs each branch for tests/UI.
 */
void handle_weapon_drop_on_npc_death(cr_shared_state_t *state, cr_entity_state_t *killer,
                                     cr_entity_state_t *dead_npc) {
    char log_text[CR_LOG_LINE_LEN];
    const cr_weapon_type_t dropped = random_dropped_weapon(state);
    snprintf(log_text, sizeof(log_text), "NPC %s defeated; dropped weapon=%d", dead_npc->name, dropped);
    append_log(state, log_text);

    if (killer->is_player) {
        const int player_picks = (next_rand(state) % 2u) == 0u; /* simulated player choice */
        if (player_picks) {
            if (equip_weapon_to_entity(killer, dropped)) {
                snprintf(log_text, sizeof(log_text), "Player %s picked up weapon=%d", killer->name, dropped);
                append_log(state, log_text);
                return;
            }
            snprintf(log_text, sizeof(log_text),
                     "Player %s wanted weapon=%d but had no allocatable space", killer->name, dropped);
            append_log(state, log_text);
        } else {
            snprintf(log_text, sizeof(log_text), "Player %s declined weapon=%d", killer->name, dropped);
            append_log(state, log_text);
        }
    }

    const int npc_picker = pick_alive_npc_id(state);
    if (npc_picker >= 0) {
        cr_entity_state_t *npc = &state->entities[npc_picker];
        if (equip_weapon_to_entity(npc, dropped)) {
            snprintf(log_text, sizeof(log_text), "NPC %s picked up dropped weapon=%d", npc->name, dropped);
            append_log(state, log_text);
            return;
        }
        snprintf(log_text, sizeof(log_text), "NPC %s could not allocate dropped weapon=%d", npc->name, dropped);
        append_log(state, log_text);
        return;
    }

    append_log(state, "No alive NPC available to pick dropped weapon.");
}

}  // namespace

/*
 * Populate players, random NPC count, and per-entity stats from roll-derived formulas.
 * If CR_TEST_MODE=1 in the environment, seeds Solar+Lunar onto player 0 (automation / smoke tests).
 */
void cr_arbiter_init_demo_entities(cr_shared_state_t *state) {
    /* Using primary roll digits from 22i-1201 for stat formulas. */
    const int roll_number = 1201;
    const int roll_last_digit = roll_number % 10;               /* 1 */
    const int roll_second_last_digit = (roll_number / 10) % 10; /* 0 */
    const int roll_last_two = roll_number % 100;                /* 1 */

    state->player_count = 2;
    state->npc_count_current = 2 + (int)(next_rand(state) % 8); /* 2..9 */

    const int player_speed = 100 / state->player_count;
    const int player_damage = roll_last_digit + 10;
    const int player_hp_divisor_1 = random_range_inclusive(state, 3, 4);
    const int player_hp_divisor_2 = random_range_inclusive(state, 3, 4);
    init_entity(&state->entities[0], 0, 1, "Player-1", roll_number / player_hp_divisor_1,
                player_damage, player_speed, CR_PLAYER_MAX_STAMINA);
    init_entity(&state->entities[1], 1, 1, "Player-2", roll_number / player_hp_divisor_2,
                player_damage, player_speed, CR_PLAYER_MAX_STAMINA);

    for (int i = 0; i < state->npc_count_current; ++i) {
        const int id = CR_MAX_PLAYERS + i;
        char npc_name[CR_MAX_NAME_LEN];
        snprintf(npc_name, sizeof(npc_name), "NPC-%d", id);
        const int hp = roll_last_two + random_range_inclusive(state, 50, 200);
        const int damage = roll_second_last_digit + 10;
        const int speed = random_range_inclusive(state, 10, 30);
        init_entity(&state->entities[id], id, 0, npc_name, hp, damage, speed, CR_NPC_MAX_STAMINA);
    }

    char config_log[CR_LOG_LINE_LEN];
    snprintf(config_log, sizeof(config_log), "Initial concurrent NPC count: %d", state->npc_count_current);
    append_log(state, config_log);
    printf("[arbiter][init] %s\n", config_log);

    const char *test_mode = getenv("CR_TEST_MODE");
    if (test_mode != nullptr && strcmp(test_mode, "1") == 0) {
        state->artifacts[CR_ARTIFACT_SOLAR_CORE].held_by_entity_id = 0;
        state->artifacts[CR_ARTIFACT_SOLAR_CORE].locked = 1;
        state->artifacts[CR_ARTIFACT_LUNAR_BLADE].held_by_entity_id = 0;
        state->artifacts[CR_ARTIFACT_LUNAR_BLADE].locked = 1;
        append_log(state, "CR_TEST_MODE: Player-1 seeded with Solar+Lunar artifacts.");
    }

    char stat_log[CR_LOG_LINE_LEN];
    for (int i = 0; i < state->player_count; ++i) {
        const cr_entity_state_t *p = &state->entities[i];
        snprintf(stat_log, sizeof(stat_log),
                 "Init Player %s: hp=%d dmg=%d spd=%d maxSt=%d",
                 p->name, p->hp, p->damage, p->speed, p->max_stamina);
        append_log(state, stat_log);
        printf("[arbiter][init] %s\n", stat_log);
    }
    for (int i = 0; i < state->npc_count_current; ++i) {
        const cr_entity_state_t *n = &state->entities[CR_MAX_PLAYERS + i];
        snprintf(stat_log, sizeof(stat_log),
                 "Init NPC %s: hp=%d dmg=%d spd=%d maxSt=%d",
                 n->name, n->hp, n->damage, n->speed, n->max_stamina);
        append_log(state, stat_log);
        printf("[arbiter][init] %s\n", stat_log);
    }
}

/*
 * If no turn is waiting on an action, advance simulated time so stamina catches up,
 * then grant the turn to the ready entity with smallest id among those at max stamina.
 */
void cr_arbiter_scheduler_tick(cr_shared_state_t *state) {
    respawn_npc_if_needed(state);

    if (state->turn.active_entity_id >= 0 && !state->turn.action_committed) {
        return;
    }

    int selected_id = -1;
    int active_ids[CR_MAX_ENTITIES];
    int active_count = 0;

    for (int i = 0; i < state->player_count; ++i) {
        active_ids[active_count++] = i;
    }
    for (int i = 0; i < state->npc_count_current; ++i) {
        active_ids[active_count++] = CR_MAX_PLAYERS + i;
    }

    if (active_count <= 0) {
        return;
    }

    int min_seconds_to_ready = -1;
    for (int i = 0; i < active_count; ++i) {
        cr_entity_state_t *entity = &state->entities[active_ids[i]];
        if (!entity->alive || entity->stunned || entity->speed <= 0) {
            continue;
        }

        if (entity->stamina >= entity->max_stamina) {
            min_seconds_to_ready = 0;
            break;
        }

        const int deficit = entity->max_stamina - entity->stamina;
        const int seconds = (deficit + entity->speed - 1) / entity->speed; /* ceil(deficit/speed) */
        if (min_seconds_to_ready == -1 || seconds < min_seconds_to_ready) {
            min_seconds_to_ready = seconds;
        }
    }

    if (min_seconds_to_ready < 0) {
        return;
    }

    if (min_seconds_to_ready > 0) {
        for (int i = 0; i < active_count; ++i) {
            cr_entity_state_t *entity = &state->entities[active_ids[i]];
            if (!entity->alive || entity->stunned || entity->speed <= 0) {
                continue;
            }
            entity->stamina += entity->speed * min_seconds_to_ready;
            if (entity->stamina > entity->max_stamina) {
                entity->stamina = entity->max_stamina;
            }
        }
    }

    int ready_count = 0;
    for (int i = 0; i < active_count; ++i) {
        const int entity_id = active_ids[i];
        cr_entity_state_t *entity = &state->entities[entity_id];
        if (!entity->alive || entity->stunned) {
            continue;
        }
        if (entity->stamina >= entity->max_stamina) {
            ++ready_count;
            if (selected_id == -1 || entity_id < selected_id) {
                /*
                 * Tie policy (deterministic):
                 * if multiple entities are ready at same computed arrival window,
                 * pick lowest entity id to keep behavior reproducible.
                 */
                selected_id = entity_id;
            }
        }
    }

    if (selected_id == -1) {
        return;
    }

    cr_entity_state_t *actor = &state->entities[selected_id];
    state->turn.active_entity_id = selected_id;
    state->turn.active_is_player = actor->is_player;
    state->turn.action_committed = 0;

    char log_text[CR_LOG_LINE_LEN];
    snprintf(log_text, sizeof(log_text), "Turn %d ready: actor=%s(%d), dt=%ds, ready=%d",
             state->turn.turn_seq, actor->name, actor->id, min_seconds_to_ready, ready_count);
    append_log(state, log_text);
    if (!cr_arbiter_ncurses_ui_active()) {
        printf("[arbiter][scheduler] %s\n", log_text);
    }
}

/*
 * If the active entity posted a READY action matching this turn_seq, apply it.
 * Otherwise track elapsed time and auto-skip NPC turns after CR_NPC_TURN_TIMEOUT_SECONDS.
 * Side effects: combat, artifacts, stun, ultimate pause trigger, end-game checks.
 */
void cr_arbiter_consume_active_action(cr_shared_state_t *state) {
    static int waiting_turn_seq = -1;
    static struct timespec wait_started_at = {0, 0};

    if (state->turn.active_entity_id < 0 || state->turn.action_committed) {
        waiting_turn_seq = -1;
        wait_started_at = {0, 0};
        return;
    }

    /* Resolve the correct action slot: one per human player id and one per NPC index. */
    cr_entity_state_t *actor = &state->entities[state->turn.active_entity_id];
    cr_action_msg_t *slot = nullptr;
    if (state->turn.active_is_player) {
        if (actor->id >= 0 && actor->id < CR_MAX_PLAYERS) {
            slot = &state->player_actions[actor->id];
        }
    } else {
        const int npc_index = actor->id - CR_MAX_PLAYERS;
        if (npc_index >= 0 && npc_index < CR_MAX_NPCS) {
            slot = &state->npc_actions[npc_index];
        }
    }

    /*
     * Stale or missing action: start or continue wait timer. NPCs get a hard timeout
     * so the match cannot stall if ASP stops submitting.
     */
    if (slot == nullptr || slot->status != CR_ACTION_STATUS_READY ||
        slot->requested_by_turn_seq != state->turn.turn_seq || slot->actor_entity_id != actor->id) {
        if (waiting_turn_seq != state->turn.turn_seq) {
            waiting_turn_seq = state->turn.turn_seq;
            clock_gettime(CLOCK_MONOTONIC, &wait_started_at);
        }

        struct timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long sec_diff = now.tv_sec - wait_started_at.tv_sec;
        const long nsec_diff = now.tv_nsec - wait_started_at.tv_nsec;
        const long elapsed_ms = sec_diff * 1000 + nsec_diff / 1000000;

        const int is_npc_turn = (state->turn.active_is_player == 0);
        if (is_npc_turn && elapsed_ms >= (CR_NPC_TURN_TIMEOUT_SECONDS * 1000L)) {
            char timeout_log[CR_LOG_LINE_LEN];
            actor->stamina = actor->max_stamina / 2;
            state->turn.action_committed = 1;
            ++state->turn.turn_seq;
            snprintf(timeout_log, sizeof(timeout_log), "NPC turn timeout: %s auto SKIP", actor->name);
            append_log(state, timeout_log);
            waiting_turn_seq = -1;
            wait_started_at = {0, 0};
        }
        return;
    }

    /* Valid action: dispatch by type, then mark slot consumed and re-evaluate win/lose. */
    char log_text[CR_LOG_LINE_LEN];
    switch (slot->type) {
        case CR_ACTION_ATTACK_STRIKE:
            apply_strike_action(state, actor, slot);
            commit_action(state, actor);
            break;
        case CR_ACTION_HEAL: {
            const int heal_amount = actor->max_hp / 10;
            actor->hp += heal_amount;
            if (actor->hp > actor->max_hp) actor->hp = actor->max_hp;
            snprintf(log_text, sizeof(log_text), "Turn %d %s HEAL +%d", state->turn.turn_seq, actor->name,
                     heal_amount);
            append_log(state, log_text);
            commit_action(state, actor);
            break;
        }
        case CR_ACTION_USE_WEAPON: {
            /* Artifact weapons may block here until acquire succeeds (see deadlock module). */
            if (!is_valid_combat_target(state, actor, slot->target_entity_id)) {
                snprintf(log_text, sizeof(log_text),
                         "Turn %d rejected weapon action: invalid enemy target by %s",
                         state->turn.turn_seq, actor->name);
                append_log(state, log_text);
                break;
            }
            if (!entity_has_equipped_weapon(actor, slot->weapon)) {
                snprintf(log_text, sizeof(log_text),
                         "Turn %d rejected weapon action: %s does not have weapon=%d equipped",
                         state->turn.turn_seq, actor->name, slot->weapon);
                append_log(state, log_text);
                break;
            }

            const int artifact_id = artifact_id_from_weapon(slot->weapon);
            if (artifact_id >= 0) {
                const int acquired =
                    cr_arbiter_deadlock_try_acquire_artifact(state, actor->id, artifact_id);
                if (!acquired) {
                    snprintf(log_text, sizeof(log_text),
                             "Turn %d %s waiting for artifact %d", state->turn.turn_seq, actor->name,
                             artifact_id);
                    append_log(state, log_text);
                    return;
                }
            }

            cr_entity_state_t *target = &state->entities[slot->target_entity_id];
            int damage = cr_weapon_damage(slot->weapon);
            if (damage <= 0) {
                damage = actor->damage;
            }

            target->hp -= damage;
            if (target->hp < 0) {
                target->hp = 0;
            }
            if (target->hp == 0 && target->alive) {
                target->alive = 0;
                if (!target->is_player) {
                    ++state->total_npc_kills;
                    handle_weapon_drop_on_npc_death(state, actor, target);
                }
                cr_arbiter_deadlock_release_artifacts(state, target->id);
            }
            snprintf(log_text, sizeof(log_text), "Turn %d %s USE_WEAPON(%d) on %s for %d",
                     state->turn.turn_seq, actor->name, slot->weapon, target->name, damage);
            append_log(state, log_text);
            if (slot->weapon == CR_WEAPON_SOLAR_CORE || slot->weapon == CR_WEAPON_ECLIPSE_RELIC) {
                cr_arbiter_trigger_stun(state, target->id);
            }
            cr_arbiter_try_trigger_ultimate_pause_for_entity(state, actor->id);
            commit_action(state, actor);
            break;
        }
        case CR_ACTION_ATTACK_EXHAUST: {
            if (!is_valid_combat_target(state, actor, slot->target_entity_id)) {
                snprintf(log_text, sizeof(log_text),
                         "Turn %d rejected exhaust action: invalid enemy target by %s",
                         state->turn.turn_seq, actor->name);
                append_log(state, log_text);
                break;
            }
            cr_entity_state_t *target = &state->entities[slot->target_entity_id];
            target->stamina -= actor->damage;
            if (target->stamina < 0) {
                target->stamina = 0;
            }
            snprintf(log_text, sizeof(log_text), "Turn %d %s EXHAUST %s by %d stamina",
                     state->turn.turn_seq, actor->name, target->name, actor->damage);
            append_log(state, log_text);
            commit_action(state, actor);
            break;
        }
        case CR_ACTION_SKIP:
            /* Skip partially refills stamina without a full "ready" cycle. */
            actor->stamina = actor->max_stamina / 2;
            state->turn.action_committed = 1;
            ++state->turn.turn_seq;
            snprintf(log_text, sizeof(log_text), "Turn %d %s SKIP", state->turn.turn_seq - 1, actor->name);
            append_log(state, log_text);
            break;
        case CR_ACTION_SWAP_IN: {
            cr_inventory_t *inv = &actor->inventory;
            const cr_weapon_type_t weapon = slot->weapon;
            if (weapon == CR_WEAPON_NONE || !pop_storage_weapon(inv, weapon)) {
                snprintf(log_text, sizeof(log_text), "Turn %d %s SWAP_IN failed: weapon missing in storage",
                         state->turn.turn_seq, actor->name);
                append_log(state, log_text);
                break;
            }

            const int place_start = ensure_space_for_weapon(inv, weapon);
            if (place_start < 0) {
                /* rollback to storage if placement failed */
                push_storage(inv, weapon);
                snprintf(log_text, sizeof(log_text), "Turn %d %s SWAP_IN failed: no allocatable space",
                         state->turn.turn_seq, actor->name);
                append_log(state, log_text);
                break;
            }

            const int required_slots = cr_weapon_slot_size(weapon);
            place_weapon_into_slots(inv, weapon, place_start, required_slots);
            snprintf(log_text, sizeof(log_text), "Turn %d %s SWAP_IN weapon=%d slots=%d@%d",
                     state->turn.turn_seq, actor->name, weapon, required_slots, place_start);
            append_log(state, log_text);
            commit_action(state, actor); /* costs complete turn */
            break;
        }
        case CR_ACTION_QUIT:
            state->end_reason = CR_END_PLAYER_QUIT;
            state->phase = CR_PHASE_FINISHED;
            state->game_running = 0;
            append_log(state, "Player requested quit.");
            commit_action(state, actor);
            break;
        default:
            snprintf(log_text, sizeof(log_text), "Turn %d rejected unsupported action from %s",
                     state->turn.turn_seq, actor->name);
            append_log(state, log_text);
            break;
    }

    slot->status = CR_ACTION_STATUS_CONSUMED;
    check_end_conditions(state);
    waiting_turn_seq = -1;
    wait_started_at = {0, 0};
}
