//Human Interfacing Process: shared memory + stdin-driven player_actions[].
 // input.cpp owns a stdin reader thread; player_threads.cpp runs one worker per player slot.
//Set CR_TEST_MODE=1 for headless automated actions (scripts only); omit for real keyboard play.

#include "../shared.h"
#include "hip_tui.h"
#include "input.h"
#include "player_threads.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

using namespace std;

namespace {
volatile sig_atomic_t g_should_stop = 0;
void handle_sigint(int) { g_should_stop = 1; }
}  // namespace

int main() {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    const int shm_fd = shm_open(CR_SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("[hip] shm_open failed (start arbiter first)");
        return 1;
    }

    auto *state = static_cast<cr_shared_state_t *>(
        mmap(nullptr, sizeof(cr_shared_state_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    close(shm_fd);
    if (state == MAP_FAILED) {
        perror("[hip] mmap failed");
        return 1;
    }

    if (state->abi_version != CR_SHARED_ABI_VERSION) {
        fprintf(stderr, "[hip] ABI mismatch. expected=%u got=%u\n", CR_SHARED_ABI_VERSION,
                state->abi_version);
        munmap(state, sizeof(*state));
        return 1;
    }

    pthread_mutex_lock(&state->state_mutex);
    state->hip_pid = getpid();
    pthread_mutex_unlock(&state->state_mutex);

    fputs("[hip] attached to shared memory.\n", stderr);
    const int hip_tui = hip_tui_init(state);
    if (!hip_tui) {
        hip_input_start();
        fputs("[hip] PID registered. Line mode: stdin choices; stderr shows menu on your turn.\n", stderr);
        fputs("[hip] Tip: omit CR_HIP_TUI=0 on a TTY for scrollable ncurses (this session is line mode).\n", stderr);
    } else {
        fputs("[hip] PID registered. ncurses HUD on (export CR_HIP_TUI=0 for plain line mode).\n", stderr);
    }

    int threads_ok = (hip_player_threads_start(state) == 0);
    if (!threads_ok) {
        fputs("[hip] error: player worker threads failed to start.\n", stderr);
        if (hip_tui) {
            hip_tui_shutdown();
        } else {
            hip_input_stop();
        }
        munmap(state, sizeof(*state));
        return 1;
    }

    while (!g_should_stop && state->game_running) {
        usleep(250 * 1000);
    }

    cr_end_reason_t final_reason = CR_END_NONE;
    pthread_mutex_lock(&state->state_mutex);
    final_reason = state->end_reason;
    pthread_mutex_unlock(&state->state_mutex);

    hip_player_threads_stop();
    if (hip_tui) {
        hip_tui_shutdown();
    } else {
        hip_input_stop();
    }

    switch (final_reason) {
        case CR_END_PLAYER_WIN:
            puts("[hip] Victory: players defeated required number of enemies.");
            break;
        case CR_END_PLAYER_LOSE:
            puts("[hip] Game over: all player characters defeated.");
            break;
        case CR_END_PLAYER_QUIT:
            puts("[hip] Game session ended (quit or client disconnect).");
            break;
        default:
            break;
    }

    pthread_mutex_lock(&state->state_mutex);
    state->hip_pid = 0;
    pthread_mutex_unlock(&state->state_mutex);
    munmap(state, sizeof(*state));
    puts("[hip] shutdown complete.");
    return 0;
}
