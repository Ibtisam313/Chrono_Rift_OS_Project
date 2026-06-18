#ifndef ASP_TUI_H
#define ASP_TUI_H

#include "../shared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start/stop ASP ncurses HUD (NPC action matrix). No-op if not a TTY or CR_ASP_TUI=0. */
int asp_tui_init(cr_shared_state_t *state);
void asp_tui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
