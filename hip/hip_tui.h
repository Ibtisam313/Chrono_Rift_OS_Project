#ifndef HIP_TUI_H
#define HIP_TUI_H

#include "../shared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call before hip_input_start. Returns 1 if ncurses HIP HUD is active (stdin reader skipped). */
int hip_tui_init(cr_shared_state_t *state);
void hip_tui_shutdown(void);
int hip_tui_is_enabled(void);

/*
 * Blocks for one line of player input while drawing the dashboard + menu.
 * Caller must not hold state->state_mutex.
 */
void hip_tui_read_player_line(cr_shared_state_t *state, int player_id, char *line, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
