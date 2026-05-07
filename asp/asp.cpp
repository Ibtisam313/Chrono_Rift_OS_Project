#include "../shared.h"

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

    puts("[asp] attached to shared memory.");
    puts("[asp] PID registered. Press Ctrl+C to stop.");

    while (!g_should_stop && state->game_running) {
        sleep(1);
    }

    pthread_mutex_lock(&state->state_mutex);
    state->asp_pid = 0;
    pthread_mutex_unlock(&state->state_mutex);
    munmap(state, sizeof(*state));
    puts("[asp] shutdown complete.");
    return 0;
}
