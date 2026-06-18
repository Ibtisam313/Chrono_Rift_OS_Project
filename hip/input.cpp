/*
 * input.cpp — Dedicated pthread reading stdin so HIP never blocks the game mutex on fgets.
 * When stdin closes (EOF, non-interactive runs), the reader idles and tests use CR_TEST_MODE.
 */
#include "input.h"

#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

using namespace std;

namespace {

const int k_queue_depth = 16;
const int k_line_max = 256;

pthread_mutex_t g_q_mutex = PTHREAD_MUTEX_INITIALIZER;
char g_queue[k_queue_depth][k_line_max];
int g_head = 0;
int g_tail = 0;
int g_count = 0;

pthread_t g_reader_tid;
volatile int g_reader_run = 0;

void *stdin_reader_main(void *) {
    while (g_reader_run) {
        char line[k_line_max];
        if (fgets(line, sizeof line, stdin) == nullptr) {
            while (g_reader_run) {
                usleep(250 * 1000);
            }
            break;
        }

        /* Strip trailing newline. */
        const size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') {
            line[n - 1] = '\0';
        }

        pthread_mutex_lock(&g_q_mutex);
        if (g_count < k_queue_depth) {
            snprintf(g_queue[g_tail % k_queue_depth], k_line_max, "%s", line);
            g_tail++;
            g_count++;
        }
        pthread_mutex_unlock(&g_q_mutex);
    }
    return nullptr;
}

}  // namespace

void hip_input_start() {
    if (g_reader_run) {
        return;
    }
    g_reader_run = 1;
    if (pthread_create(&g_reader_tid, nullptr, stdin_reader_main, nullptr) != 0) {
        g_reader_run = 0;
    }
}

void hip_input_stop() {
    if (!g_reader_run) {
        return;
    }
    g_reader_run = 0;
    pthread_join(g_reader_tid, nullptr);
    pthread_mutex_lock(&g_q_mutex);
    g_head = g_tail = g_count = 0;
    pthread_mutex_unlock(&g_q_mutex);
}

int hip_input_poll_line(char *buf, size_t buflen) {
    if (buf == nullptr || buflen == 0) {
        return 0;
    }
    buf[0] = '\0';
    pthread_mutex_lock(&g_q_mutex);
    if (g_count <= 0) {
        pthread_mutex_unlock(&g_q_mutex);
        return 0;
    }
    snprintf(buf, buflen, "%s", g_queue[g_head % k_queue_depth]);
    g_head++;
    g_count--;
    pthread_mutex_unlock(&g_q_mutex);
    return 1;
}
