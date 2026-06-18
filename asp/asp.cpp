//Automated Strategic Process: attaches to shared memory and drives npc_actions[].
//npc_logic.cpp implements decisions per NPC slot; npc_threads.cpp runs one pthread per slot
//so the active NPC is serviced quickly (arbiter NPC turn timeout is only a few seconds).
#include "../shared.h"
#include "asp_tui.h"
#include "npc_logic.h"
#include "npc_threads.h"

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
        perror("[asp] shm_open failed (start arbiter first)");
        return 1;
    }

    auto *state = static_cast<cr_shared_state_t *>(
        mmap(nullptr, sizeof(cr_shared_state_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    close(shm_fd);
    if (state == MAP_FAILED) {
        perror("[asp] mmap failed");
        return 1;
    }

    if (state->abi_version != CR_SHARED_ABI_VERSION) {
        fprintf(stderr, "[asp] ABI mismatch. expected=%u got=%u\n", CR_SHARED_ABI_VERSION,
                state->abi_version);
        munmap(state, sizeof(*state));
        return 1;
    }

    pthread_mutex_lock(&state->state_mutex);
    state->asp_pid = getpid();
    pthread_mutex_unlock(&state->state_mutex);

    fputs("[asp] attached to shared memory.\n", stderr);
    const int asp_tui = asp_tui_init(state);
    if (!asp_tui) {
        fputs("[asp] PID registered (set CR_ASP_TUI=0 only to disable TTY HUD).\n", stderr);
    } else {
        fputs("[asp] PID registered. ncurses NPC HUD on (CR_ASP_TUI=0 to disable).\n", stderr);
    }

    int threads_ok = (asp_npc_threads_start(state) == 0);
    if (threads_ok) {
        fputs("[asp] NPC worker threads running (one pthread per NPC slot).\n", stderr);
    } else {
        fputs("[asp] warning: pthread workers failed; using single-thread poll fallback.\n", stderr);
    }

    while (!g_should_stop && state->game_running) {
        if (!threads_ok) {
            pthread_mutex_lock(&state->state_mutex);
            asp_submit_npc_action(state);
            pthread_mutex_unlock(&state->state_mutex);
        }
        usleep(250 * 1000);
    }

    if (threads_ok) {
        asp_npc_threads_stop();
    }

    if (asp_tui) {
        asp_tui_shutdown();
    }

    pthread_mutex_lock(&state->state_mutex);
    state->asp_pid = 0;
    pthread_mutex_unlock(&state->state_mutex);
    munmap(state, sizeof(*state));
    fputs("[asp] shutdown complete.\n", stderr);
    return 0;
}
