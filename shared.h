#ifndef CHRONO_RIFT_SHARED_H
#define CHRONO_RIFT_SHARED_H

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CR_SHM_NAME "/chrono_rift_shm"
#define CR_MAX_PLAYERS 4
#define CR_MAX_NPCS 9
#define CR_MAX_ENTITIES (CR_MAX_PLAYERS + CR_MAX_NPCS)
#define CR_MAX_LOG_LINES 64
#define CR_LOG_LINE_LEN 160
#define CR_MAX_INVENTORY_SLOTS 20
#define CR_MAX_WEAPONS_PER_STORAGE 32
#define CR_MAX_ARTIFACTS 3
#define CR_MAX_TARGETS CR_MAX_ENTITIES
#define CR_MAX_NAME_LEN 32

#define CR_STUN_SECONDS 3
#define CR_ULTIMATE_PAUSE_SECONDS 10
#define CR_NPC_TURN_TIMEOUT_SECONDS 3

#define CR_PLAYER_MAX_STAMINA 100
#define CR_NPC_MAX_STAMINA 150
#define CR_WIN_KILL_TARGET 10

#define CR_DEV_MODE 0

/* ---------- Role / lifecycle ---------- */
typedef enum cr_process_role_t {
    CR_ROLE_NONE = 0,
    CR_ROLE_ARBITER = 1,
    CR_ROLE_HIP = 2,
    CR_ROLE_ASP = 3
} cr_process_role_t;

typedef enum cr_game_phase_t {
    CR_PHASE_INIT = 0,
    CR_PHASE_RUNNING = 1,
    CR_PHASE_FINISHED = 2
} cr_game_phase_t;

typedef enum cr_end_reason_t {
    CR_END_NONE = 0,
    CR_END_PLAYER_WIN = 1,
    CR_END_PLAYER_LOSE = 2,
    CR_END_PLAYER_QUIT = 3
} cr_end_reason_t;

/* ---------- Weapons / artifacts ---------- */
typedef enum cr_weapon_type_t {
    CR_WEAPON_NONE = 0,
    CR_WEAPON_SOLAR_CORE = 1,
    CR_WEAPON_LUNAR_BLADE = 2,
    CR_WEAPON_IRON_HALBERD = 3,
    CR_WEAPON_VENOM_DAGGER = 4,
    CR_WEAPON_THUNDERSTAFF = 5,
    CR_WEAPON_OBSIDIAN_AXE = 6,
    CR_WEAPON_FROSTBOW = 7,
    CR_WEAPON_SPLINTER_STICK = 8,
    CR_WEAPON_ECLIPSE_RELIC = 9
} cr_weapon_type_t;

typedef enum cr_artifact_type_t {
    CR_ARTIFACT_SOLAR_CORE = 0,
    CR_ARTIFACT_LUNAR_BLADE = 1,
    CR_ARTIFACT_ECLIPSE_RELIC = 2
} cr_artifact_type_t;

/* ---------- Action protocol ---------- */
typedef enum cr_action_type_t {
    CR_ACTION_NONE = 0,
    CR_ACTION_ATTACK_STRIKE = 1,
    CR_ACTION_ATTACK_EXHAUST = 2,
    CR_ACTION_USE_WEAPON = 3,
    CR_ACTION_SWAP_IN = 4,
    CR_ACTION_HEAL = 5,
    CR_ACTION_SKIP = 6,
    CR_ACTION_QUIT = 7
} cr_action_type_t;

typedef enum cr_action_status_t {
    CR_ACTION_STATUS_EMPTY = 0,
    CR_ACTION_STATUS_READY = 1,
    CR_ACTION_STATUS_CONSUMED = 2,
    CR_ACTION_STATUS_TIMEOUT_SKIP = 3,
    CR_ACTION_STATUS_REJECTED = 4
} cr_action_status_t;

/* ---------- Core records ---------- */
typedef struct cr_weapon_def_t {
    cr_weapon_type_t type;
    char name[CR_MAX_NAME_LEN];
    int slot_size;
    int damage;
    int is_artifact;
} cr_weapon_def_t;

typedef struct cr_inventory_t {
    /* Primary contiguous inventory slots, each slot stores a weapon type id or NONE. */
    int slots[CR_MAX_INVENTORY_SLOTS];
    int occupied_slots;

    /* Long-term storage keeps weapon ids as stack/queue by game logic choice. */
    int storage[CR_MAX_WEAPONS_PER_STORAGE];
    int storage_count;
} cr_inventory_t;

typedef struct cr_entity_state_t {
    int id;                 /* 0..CR_MAX_ENTITIES-1 */
    int is_player;          /* 1 player, 0 npc */
    int alive;              /* 1 alive, 0 dead */
    int hp;
    int max_hp;
    int damage;
    int speed;
    int stamina;
    int max_stamina;
    int stunned;            /* 1 while stunned */
    time_t stun_end_epoch;  /* absolute epoch when stun expires */
    int blocked_from_turn;  /* set by arbiter for immediate skip conditions */

    cr_inventory_t inventory;
    char name[CR_MAX_NAME_LEN];
} cr_entity_state_t;

typedef struct cr_artifact_state_t {
    cr_artifact_type_t type;
    int exists_in_world;      /* Eclipse starts as 0 and becomes 1 once introduced */
    int held_by_entity_id;    /* -1 if free */
    int locked;               /* 1 when held/locked */
} cr_artifact_state_t;

typedef struct cr_action_msg_t {
    cr_action_status_t status;
    cr_action_type_t type;
    int actor_entity_id;
    int target_entity_id;      /* -1 when no direct target */
    cr_weapon_type_t weapon;   /* for use/swap actions */
    int requested_by_turn_seq; /* arbiter turn sequence for stale-action rejection */
    struct timespec created_at;
} cr_action_msg_t;

typedef struct cr_turn_state_t {
    int turn_seq;
    int active_entity_id;      /* entity allowed to act now */
    int active_is_player;      /* 1 player turn, 0 npc turn */
    int action_committed;      /* toggled by arbiter after valid action */
} cr_turn_state_t;

typedef struct cr_log_entry_t {
    uint64_t seq;
    struct timespec ts;
    char text[CR_LOG_LINE_LEN];
} cr_log_entry_t;

/* ---------- Shared memory root ---------- */
typedef struct cr_shared_state_t {
    uint32_t abi_version;      /* bump only on intentional breaking changes */
    uint32_t rng_seed;         /* roll-number based seed */

    volatile sig_atomic_t game_running;
    volatile sig_atomic_t shutdown_requested;
    volatile sig_atomic_t asp_paused_for_ultimate;

    pid_t arbiter_pid;
    pid_t hip_pid;
    pid_t asp_pid;

    int player_count;          /* 1..4 */
    int npc_count_current;     /* 2..9 at runtime */
    int total_npc_kills;       /* win at CR_WIN_KILL_TARGET */

    cr_game_phase_t phase;
    cr_end_reason_t end_reason;

    cr_turn_state_t turn;
    cr_entity_state_t entities[CR_MAX_ENTITIES];
    cr_artifact_state_t artifacts[CR_MAX_ARTIFACTS];

    /* hip -> arbiter (one slot per player thread) */
    cr_action_msg_t player_actions[CR_MAX_PLAYERS];

    /* asp -> arbiter (one slot per npc thread) */
    cr_action_msg_t npc_actions[CR_MAX_NPCS];

    /* Read-mostly UI/event log ring buffer */
    uint64_t log_write_seq;
    cr_log_entry_t logs[CR_MAX_LOG_LINES];

    /* ---------- Synchronization primitives in shared memory ---------- */
    /* Guards most of shared state (set pshared attr before init). */
    pthread_mutex_t state_mutex;

    /* Protects artifact table and deadlock-related ownership transitions. */
    pthread_mutex_t artifact_mutex;

    /* Enables arbiter to wait for hip/asp action availability without busy spin. */
    sem_t sem_player_action_ready;
    sem_t sem_npc_action_ready;

    /* Optional wake-up semaphore for renderer thread / process. */
    sem_t sem_render_tick;
} cr_shared_state_t;

/* ---------- ABI and helper declarations ---------- */
#define CR_SHARED_ABI_VERSION 1U

/*
 * Static weapon table in fixed index order.
 * Implement this function in exactly one .cpp file and expose read-only pointer.
 * Returns the full table; if count_out is non-NULL, writes the number of entries.
 */
const cr_weapon_def_t *cr_get_weapon_defs(int *count_out);

/*
 * Utility mapping helpers (implemented in shared.cpp once; linked into every executable).
 * slot_size: inventory footprint; damage: USE_WEAPON base; is_artifact: ties to artifact system.
 */
int cr_weapon_slot_size(cr_weapon_type_t weapon);
int cr_weapon_damage(cr_weapon_type_t weapon);
int cr_is_artifact_weapon(cr_weapon_type_t weapon);

#ifdef __cplusplus
}
#endif

#endif /* CHRONO_RIFT_SHARED_H */
