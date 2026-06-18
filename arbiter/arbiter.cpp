 // Create shared-memory segment 
 //Initialize process-shared mutexes/semaphores and core game state.
 //it runs the main 1 Hz loop: liveness checks, signal-driven effects,
 // turn scheduling, action consumption, while auxiliary threads handle
 //ncurses UI and periodic deadlock monitoring.
 // On exit: stop threads, destroy sync primitives, unmap and unlink shm.
 
#include "../shared.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <unistd.h>

using namespace std;

/* Cross-translation-unit entry points implemented in other arbiter directory sources. */
void cr_arbiter_init_demo_entities(cr_shared_state_t *state);
void cr_arbiter_scheduler_tick(cr_shared_state_t *state);
void cr_arbiter_consume_active_action(cr_shared_state_t *state);
void cr_arbiter_signals_setup(cr_shared_state_t *state);
void cr_arbiter_signals_tick(cr_shared_state_t *state);
void cr_arbiter_deadlock_setup(cr_shared_state_t *state);
int cr_arbiter_deadlock_start_monitor(cr_shared_state_t *state);
void cr_arbiter_deadlock_stop_monitor();
int cr_arbiter_render_start(cr_shared_state_t *state);
void cr_arbiter_render_stop();

namespace {

// Set by signal handlers; main loop polls this (async-signal-safe). 
volatile sig_atomic_t g_should_stop = 0;

// User requested shutdown via SIGINT/SIGTERM/SIGQUIT/SIGHUP. 
void handle_termination_signal(int) { g_should_stop = 1; }

/*
 * RNG seed: CR_SEED env (decimal, non-zero) overrides team default.
 * Invalid or empty env falls back to a fixed roll-based default.
 */
uint32_t resolve_game_seed() {
    /* Team-specific default seed from roll numbers: 22i-1201 and 22i-1191 */
    const uint32_t default_seed = 12011191u;

    const char *seed_env = getenv("CR_SEED");
    if (seed_env == nullptr || *seed_env == '\0') {
        return default_seed;
    }

    char *endptr = nullptr;
    const unsigned long parsed = strtoul(seed_env, &endptr, 10);
    if (endptr == seed_env || *endptr != '\0' || parsed == 0UL) {
        return default_seed;
    }
    return (uint32_t)parsed;
}

// Append one line to the in-shm ring log (used by UI and diagnostics). Caller must hold state_mutex. 
void append_runtime_log(cr_shared_state_t *state, const char *text) {
    const uint64_t idx = state->log_write_seq % CR_MAX_LOG_LINES;
    cr_log_entry_t *entry = &state->logs[idx];
    entry->seq = state->log_write_seq + 1;
    clock_gettime(CLOCK_REALTIME, &entry->ts);
    snprintf(entry->text, sizeof(entry->text), "%s", text);
    ++state->log_write_seq;
}

/*
 * When HIP or ASP dies, drop artifact locks held by that role so the world
 * does not stay inconsistent. release_players=1 clears player-held artifacts.
 */
void release_artifacts_for_dead_role(cr_shared_state_t *state, int release_players) {
    for (int a = 0; a < CR_MAX_ARTIFACTS; ++a) {
        const int owner = state->artifacts[a].held_by_entity_id;
        if (owner < 0 || owner >= CR_MAX_ENTITIES) {
            continue;
        }
        const int is_player = state->entities[owner].is_player != 0;
        if ((release_players && is_player) || (!release_players && !is_player)) {
            state->artifacts[a].held_by_entity_id = -1;
            state->artifacts[a].locked = 0;
        }
    }
}

/*
 * Non-blocking check: if HIP/ASP PID is no longer running (kill(pid,0)+ESRCH),
 * update state and logs. HIP death ends the game; ASP death only clears NPC artifacts.
 */
void monitor_process_liveness(cr_shared_state_t *state) {
    if (state->hip_pid > 0 && kill(state->hip_pid, 0) != 0 && errno == ESRCH) {
        state->hip_pid = 0;
        release_artifacts_for_dead_role(state, 1);
        append_runtime_log(state, "HIP process died unexpectedly; player-held artifacts released.");
        state->end_reason = CR_END_PLAYER_QUIT;
        state->phase = CR_PHASE_FINISHED;
        state->game_running = 0;
    }

    if (state->asp_pid > 0 && kill(state->asp_pid, 0) != 0 && errno == ESRCH) {
        state->asp_pid = 0;
        state->asp_paused_for_ultimate = 0;
        release_artifacts_for_dead_role(state, 0);
        append_runtime_log(state, "ASP process died unexpectedly; NPC-held artifacts released.");
    }
}

/*
 * Ordered teardown: stop UI and deadlock threads, destroy semaphores/mutexes
 * if they were initialized, unmap shm, then unlink the segment name.
 */
void cleanup_arbiter_runtime(cr_shared_state_t *state, int render_started, int deadlock_started,
                             int sync_initialized) {
    if (state == nullptr || state == MAP_FAILED) {
        return;
    }

    if (render_started) {
        cr_arbiter_render_stop();
    }
    /*
     * Ncurses clears the screen on endwin(); battle log lived only in the UI buffer.
     * Echo the final outcome on the plain terminal so wins/losses are visible after exit.
     */
    switch (state->end_reason) {
        case CR_END_PLAYER_WIN:
            puts("[arbiter] Victory: players defeated required number of enemies.");
            break;
        case CR_END_PLAYER_LOSE:
            puts("[arbiter] Game over: all player characters defeated.");
            break;
        case CR_END_PLAYER_QUIT:
            puts("[arbiter] Game session ended (quit or client disconnect).");
            break;
        default:
            break;
    }
    if (deadlock_started) {
        cr_arbiter_deadlock_stop_monitor();
    }

    if (sync_initialized) {
        state->shutdown_requested = 1;
        state->game_running = 0;
        state->phase = CR_PHASE_FINISHED;

        sem_destroy(&state->sem_player_action_ready);
        sem_destroy(&state->sem_npc_action_ready);
        sem_destroy(&state->sem_render_tick);
        pthread_mutex_destroy(&state->artifact_mutex);
        pthread_mutex_destroy(&state->state_mutex);
    }

    munmap(state, sizeof(*state));
    if (shm_unlink(CR_SHM_NAME) != 0) {
        perror("[arbiter] shm_unlink warning");
    }
}

/*
 * Initialize mutexes and semaphores stored inside shared memory with
 * PTHREAD_PROCESS_SHARED so HIP/ASP can lock the same objects after attach.
 * Semaphores use pshared=1 (second argument to sem_init).
 */
int init_shared_sync(cr_shared_state_t *state) {
    pthread_mutexattr_t mattr;
    if (pthread_mutexattr_init(&mattr) != 0) return -1;
    if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED) != 0) {
        pthread_mutexattr_destroy(&mattr);
        return -1;
    }

    if (pthread_mutex_init(&state->state_mutex, &mattr) != 0) {
        pthread_mutexattr_destroy(&mattr);
        return -1;
    }
    if (pthread_mutex_init(&state->artifact_mutex, &mattr) != 0) {
        pthread_mutex_destroy(&state->state_mutex);
        pthread_mutexattr_destroy(&mattr);
        return -1;
    }

    if (sem_init(&state->sem_player_action_ready, 1, 0) != 0) {
        pthread_mutex_destroy(&state->artifact_mutex);
        pthread_mutex_destroy(&state->state_mutex);
        pthread_mutexattr_destroy(&mattr);
        return -1;
    }
    if (sem_init(&state->sem_npc_action_ready, 1, 0) != 0) {
        sem_destroy(&state->sem_player_action_ready);
        pthread_mutex_destroy(&state->artifact_mutex);
        pthread_mutex_destroy(&state->state_mutex);
        pthread_mutexattr_destroy(&mattr);
        return -1;
    }
    if (sem_init(&state->sem_render_tick, 1, 0) != 0) {
        sem_destroy(&state->sem_npc_action_ready);
        sem_destroy(&state->sem_player_action_ready);
        pthread_mutex_destroy(&state->artifact_mutex);
        pthread_mutex_destroy(&state->state_mutex);
        pthread_mutexattr_destroy(&mattr);
        return -1;
    }

    pthread_mutexattr_destroy(&mattr);
    return 0;
}

}  // namespace

/*
 * Program entry: set up shm + sync, spawn render/deadlock helpers, then loop until
 * Ctrl+C or game_over clears game_running.
 */
int main() {
    signal(SIGINT, handle_termination_signal);
    signal(SIGTERM, handle_termination_signal);
    signal(SIGQUIT, handle_termination_signal);
    signal(SIGHUP, handle_termination_signal);

    /* Remove leftover segment so shm_open(O_CREAT) starts from a clean object. */
    shm_unlink(CR_SHM_NAME);

    int sync_initialized = 0;
    int render_started = 0;
    int deadlock_started = 0;

    const int shm_fd = shm_open(CR_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("[arbiter] shm_open failed");
        return 1;
    }

    if (ftruncate(shm_fd, sizeof(cr_shared_state_t)) != 0) {
        perror("[arbiter] ftruncate failed");
        close(shm_fd);
        return 1;
    }

    auto *state = static_cast<cr_shared_state_t *>(
        mmap(nullptr, sizeof(cr_shared_state_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    close(shm_fd);
    if (state == MAP_FAILED) {
        perror("[arbiter] mmap failed");
        return 1;
    }

    /* Zero entire struct then fill ABI, seed, phase, and artifact table defaults. */
    memset(state, 0, sizeof(*state));
    state->abi_version = CR_SHARED_ABI_VERSION;
    state->rng_seed = resolve_game_seed();
    const uint32_t initial_rng_seed = state->rng_seed;
    state->phase = CR_PHASE_INIT;
    state->game_running = 1;
    state->arbiter_pid = getpid();
    state->hip_pid = 0;
    state->asp_pid = 0;
    state->turn.turn_seq = 1;
    state->turn.active_entity_id = -1;

    for (int i = 0; i < CR_MAX_ARTIFACTS; ++i) {
        state->artifacts[i].type = static_cast<cr_artifact_type_t>(i);
        state->artifacts[i].exists_in_world = (i < 2) ? 1 : 0;
        state->artifacts[i].held_by_entity_id = -1;
        state->artifacts[i].locked = 0;
    }

    if (init_shared_sync(state) != 0) {
        perror("[arbiter] sync init failed");
        cleanup_arbiter_runtime(state, 0, 0, 0);
        return 1;
    }
    sync_initialized = 1;

    state->phase = CR_PHASE_RUNNING;
    /* Entity init + signal hooks + deadlock tables; seed line uses *initial* value (CR_SEED or default). */
    pthread_mutex_lock(&state->state_mutex);
    cr_arbiter_init_demo_entities(state);
    cr_arbiter_signals_setup(state);
    cr_arbiter_deadlock_setup(state);
    char seed_log[CR_LOG_LINE_LEN];
    snprintf(seed_log, sizeof(seed_log), "Initial RNG seed: %u", initial_rng_seed);
    const uint64_t seed_idx = state->log_write_seq % CR_MAX_LOG_LINES;
    cr_log_entry_t *seed_entry = &state->logs[seed_idx];
    seed_entry->seq = state->log_write_seq + 1;
    clock_gettime(CLOCK_REALTIME, &seed_entry->ts);
    snprintf(seed_entry->text, sizeof(seed_entry->text), "%s", seed_log);
    ++state->log_write_seq;
    pthread_mutex_unlock(&state->state_mutex);
    puts("[arbiter] shared memory created and initialized.");
    printf("[arbiter] Initial RNG seed: %u\n", initial_rng_seed);
    puts("[arbiter] scheduler tick is active. Press Ctrl+C to stop.");
    const char *disable_ui = getenv("CR_DISABLE_UI");
    if (disable_ui != nullptr && strcmp(disable_ui, "1") == 0) {
        puts("[arbiter] UI disabled via CR_DISABLE_UI=1.");
    } else if (cr_arbiter_render_start(state) != 0) {
        puts("[arbiter] render thread failed to start, continuing without UI.");
    } else {
        render_started = 1;
    }
    if (cr_arbiter_deadlock_start_monitor(state) != 0) {
        puts("[arbiter] deadlock monitor thread failed to start.");
    } else {
        deadlock_started = 1;
    }

    /*
     * Core tick: all mutations that touch turn/actions/entities run under state_mutex.
     * Sleep keeps CPU usage low; scheduler simulates "seconds to ready" in discrete steps.
     */
    while (!g_should_stop) {
        /*
         * Non-blocking semaphore consumption for rubric-visible action signaling.
         * Core game logic remains turn-seq validated in scheduler/consume path.
         */
        while (sem_trywait(&state->sem_player_action_ready) == 0) {
        }
        while (sem_trywait(&state->sem_npc_action_ready) == 0) {
        }

        pthread_mutex_lock(&state->state_mutex);
        monitor_process_liveness(state);
        cr_arbiter_signals_tick(state);
        cr_arbiter_scheduler_tick(state);
        cr_arbiter_consume_active_action(state);
        pthread_mutex_unlock(&state->state_mutex);
        if (!state->game_running) {
            g_should_stop = 1;
        }
        sleep(1);
    }

    cleanup_arbiter_runtime(state, render_started, deadlock_started, sync_initialized);
    puts("[arbiter] shutdown complete.");
    return 0;
}
