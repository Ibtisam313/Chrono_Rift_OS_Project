/*
 * asp_tui.cpp — Optional ncurses view of NPC action slots (what ASP last submitted).
 * Disable with CR_ASP_TUI=0 or when stdout is not a TTY.
 */
#include "asp_tui.h"

#include "../shared.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>

using namespace std;

namespace {

cr_shared_state_t *g_state = nullptr;
pthread_t g_tid{};
volatile int g_run = 0;
volatile int g_inited = 0;

const char *action_label(cr_action_type_t t) {
    switch (t) {
        case CR_ACTION_ATTACK_STRIKE:
            return "STRIKE";
        case CR_ACTION_ATTACK_EXHAUST:
            return "EXHAUST";
        case CR_ACTION_USE_WEAPON:
            return "WEAPON";
        case CR_ACTION_SWAP_IN:
            return "SWAP";
        case CR_ACTION_HEAL:
            return "HEAL";
        case CR_ACTION_SKIP:
            return "SKIP";
        case CR_ACTION_QUIT:
            return "QUIT";
        default:
            return "-";
    }
}

const char *status_label(cr_action_status_t s) {
    switch (s) {
        case CR_ACTION_STATUS_READY:
            return "READY";
        case CR_ACTION_STATUS_CONSUMED:
            return "USED";
        case CR_ACTION_STATUS_EMPTY:
            return "empty";
        case CR_ACTION_STATUS_TIMEOUT_SKIP:
            return "TMO";
        case CR_ACTION_STATUS_REJECTED:
            return "REJ";
        default:
            return "?";
    }
}

struct AspRowSnap {
    char name[CR_MAX_NAME_LEN];
    int eid;
    int hp, max_hp, st, max_st;
    int alive;
    cr_action_type_t atype;
    int tgt, weapon, turn_seq;
    cr_action_status_t stu;
};

void *asp_hud_main(void *) {
    if (initscr() == nullptr) {
        g_run = 0;
        g_inited = 0;
        return nullptr;
    }
    g_inited = 1;
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN, -1);
        init_pair(2, COLOR_GREEN, -1);
        init_pair(3, COLOR_RED, -1);
        init_pair(4, COLOR_YELLOW, -1);
    }
    clearok(stdscr, TRUE);

    AspRowSnap rows[CR_MAX_NPCS];

    while (g_run && g_state != nullptr) {
        int nrows = 0;
        int running = 0;
        pthread_mutex_lock(&g_state->state_mutex);
        cr_shared_state_t *st = g_state;
        if (st != nullptr) {
            running = st->game_running ? 1 : 0;
            nrows = st->npc_count_current;
            if (nrows > CR_MAX_NPCS) {
                nrows = CR_MAX_NPCS;
            }
            for (int i = 0; i < nrows; ++i) {
                const int eid = CR_MAX_PLAYERS + i;
                const cr_entity_state_t *en = &st->entities[eid];
                const cr_action_msg_t *a = &st->npc_actions[i];
                AspRowSnap *r = &rows[i];
                snprintf(r->name, sizeof r->name, "%s", en->name);
                r->eid = eid;
                r->hp = en->hp;
                r->max_hp = en->max_hp;
                r->st = en->stamina;
                r->max_st = en->max_stamina;
                r->alive = en->alive;
                r->atype = a->type;
                r->tgt = a->target_entity_id;
                r->weapon = (int)a->weapon;
                r->turn_seq = a->requested_by_turn_seq;
                r->stu = a->status;
            }
        }
        pthread_mutex_unlock(&g_state->state_mutex);

        erase();
        if (has_colors()) {
            attron(COLOR_PAIR(1) | A_BOLD);
        }
        mvprintw(0, 0, " ASP - NPC actions (slot i -> entity %d+i)", CR_MAX_PLAYERS);
        clrtoeol();
        if (has_colors()) {
            attroff(COLOR_PAIR(1) | A_BOLD);
        }
        mvprintw(1, 0, " act / tgt / wpn / turn# / q / HP (updates ~8/s)");
        clrtoeol();

        int row = 3;
        if (!running) {
            mvprintw(row, 0, " (game not running)");
            clrtoeol();
        } else {
            for (int i = 0; i < nrows && row < LINES - 2; ++i) {
                const AspRowSnap *r = &rows[i];
                if (has_colors()) {
                    attron(COLOR_PAIR(r->alive ? 3 : 4));
                }
                char line[256];
                snprintf(line, sizeof line,
                         "[%d] %-8.8s id=%d hp=%3d/%3d st=%3d/%3d | %-7s t=%2d w=%2d seq=%2d [%s]", i,
                         r->name, r->eid, r->hp, r->max_hp, r->st, r->max_st, action_label(r->atype),
                         r->tgt, r->weapon, r->turn_seq, status_label(r->stu));
                mvprintw(row++, 0, "%.*s", COLS > 0 ? COLS - 1 : 80, line);
                clrtoeol();
                if (has_colors()) {
                    attroff(COLOR_PAIR(r->alive ? 3 : 4));
                }
            }
        }
        mvprintw(LINES - 1, 0, " CR_ASP_TUI=0 disables this HUD");
        clrtoeol();
        refresh();
        usleep(120000);
    }

    endwin();
    g_inited = 0;
    return nullptr;
}

int asp_tui_disabled_by_env() {
    const char *e = getenv("CR_ASP_TUI");
    return (e != nullptr && strcmp(e, "0") == 0);
}

}  // namespace

extern "C" int asp_tui_init(cr_shared_state_t *state) {
    if (state == nullptr || asp_tui_disabled_by_env()) {
        return 0;
    }
    if (!isatty(STDOUT_FILENO)) {
        return 0;
    }
    g_state = state;
    g_run = 1;
    if (pthread_create(&g_tid, nullptr, asp_hud_main, nullptr) != 0) {
        g_run = 0;
        g_state = nullptr;
        return 0;
    }
    for (int i = 0; i < 50 && !g_inited; ++i) {
        usleep(10000);
    }
    return 1;
}

extern "C" void asp_tui_shutdown(void) {
    if (!g_run) {
        g_state = nullptr;
        return;
    }
    g_run = 0;
    pthread_join(g_tid, nullptr);
    g_state = nullptr;
}
