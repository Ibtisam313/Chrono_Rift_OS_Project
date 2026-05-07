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
#include <unistd.h>

using namespace std;

namespace {

volatile sig_atomic_t g_should_stop = 0;

void handle_sigint(int) { g_should_stop = 1; }

int init_shared_sync(cr_shared_state_t *state) {
    pthread_mutexattr_t mattr;
    if (pthread_mutexattr_init(&mattr) != 0) return -1;
    if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED) != 0) return -1;

    if (pthread_mutex_init(&state->state_mutex, &mattr) != 0) return -1;
    if (pthread_mutex_init(&state->artifact_mutex, &mattr) != 0) return -1;

    if (sem_init(&state->sem_player_action_ready, 1, 0) != 0) return -1;
    if (sem_init(&state->sem_npc_action_ready, 1, 0) != 0) return -1;
    if (sem_init(&state->sem_render_tick, 1, 0) != 0) return -1;

    pthread_mutexattr_destroy(&mattr);
    return 0;
}

}  // namespace

int main() {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

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

    memset(state, 0, sizeof(*state));
    state->abi_version = CR_SHARED_ABI_VERSION;
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
        munmap(state, sizeof(*state));
        shm_unlink(CR_SHM_NAME);
        return 1;
    }

    state->phase = CR_PHASE_RUNNING;
    puts("[arbiter] shared memory created and initialized.");
    puts("[arbiter] waiting for hip/asp attachment. Press Ctrl+C to stop.");

    while (!g_should_stop) {
        sleep(1);
    }

    state->shutdown_requested = 1;
    state->game_running = 0;
    state->phase = CR_PHASE_FINISHED;

    sem_destroy(&state->sem_player_action_ready);
    sem_destroy(&state->sem_npc_action_ready);
    sem_destroy(&state->sem_render_tick);
    pthread_mutex_destroy(&state->artifact_mutex);
    pthread_mutex_destroy(&state->state_mutex);

    munmap(state, sizeof(*state));
    if (shm_unlink(CR_SHM_NAME) != 0) {
        perror("[arbiter] shm_unlink warning");
    }
    puts("[arbiter] shutdown complete.");
    return 0;
}
