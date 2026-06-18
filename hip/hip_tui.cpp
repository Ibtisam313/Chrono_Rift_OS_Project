/*
 * hip_tui.cpp — Optional ncurses dashboard for HIP: scrollable battle log + turn input.
 * Suppress with CR_HIP_TUI=0 or CR_TEST_MODE=1. Requires a TTY on stdin and stdout.
 */
#include "hip_tui.h"

#include "../shared.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>  // memcpy, memset, snprintf
#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>

using namespace std;

namespace {

cr_shared_state_t *g_state = nullptr;
pthread_mutex_t g_nc_mu = PTHREAD_MUTEX_INITIALIZER;
volatile int g_tui_on = 0;
volatile int g_line_edit = 0;
volatile int g_hud_run = 0;
pthread_t g_hud_tid{};

int g_use_color = 0;
int g_log_scroll_back = 0;
int g_log_viewport_h = 8;

struct HipSnapRow {
    char name[CR_MAX_NAME_LEN];
    int hp;
    int max_hp;
    int stamina;
    int max_stamina;
    int alive;
    char wbuf[80];
};

/* Point-in-time copy of everything the HUD draws (avoids torn reads vs arbiter). */
struct HipSnap {
    int total_npc_kills;
    int npc_alive;
    int npc_count;
    int pl_alive;
    int pl_count;
    int turn_seq;
    int game_running;
    int active_id;
    int active_is_player;
    char active_name[CR_MAX_NAME_LEN];
    HipSnapRow pl[CR_MAX_PLAYERS];
    int npc_n;
    HipSnapRow npc[CR_MAX_NPCS];
    uint64_t log_write_seq;
    cr_log_entry_t logs[CR_MAX_LOG_LINES];
};

enum {
    CP_TITLE = 1,
    CP_SUMMARY,
    CP_PLAYER,
    CP_NPC,
    CP_NPC_HURT,
    CP_LOG,
    CP_MENU,
    CP_FOOTER,
};

int hip_tui_wants_enabled() {
    const char *test = getenv("CR_TEST_MODE");
    if (test != nullptr && strcmp(test, "1") == 0) {
        return 0;
    }
    const char *opt = getenv("CR_HIP_TUI");
    if (opt != nullptr && strcmp(opt, "0") == 0) {
        return 0;
    }
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

void draw_hp_bar(int row, int col, int hp, int max_hp, int width) {
    if (max_hp <= 0) {
        max_hp = 1;
    }
    int filled = (hp * width + max_hp - 1) / max_hp;
    filled = max(0, min(width, filled));
    mvaddch(row, col++, '[');
    for (int i = 0; i < width; ++i) {
        mvaddch(row, col++, i < filled ? '#' : '.');
    }
    mvaddch(row, col++, ']');
}

void format_weapons(char *buf, size_t cap, const cr_entity_state_t *e) {
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    size_t n = 0;
    int i = 0;
    while (i < CR_MAX_INVENTORY_SLOTS && n + 6 < cap) {
        const int w = e->inventory.slots[i];
        if (w == CR_WEAPON_NONE) {
            ++i;
            continue;
        }
        int span = cr_weapon_slot_size(static_cast<cr_weapon_type_t>(w));
        if (span < 1) {
            span = 1;
        }
        n += snprintf(buf + n, cap - n, "%s%d", (n > 0) ? "," : "", w);
        i += span;
    }
    if (e->inventory.storage_count > 0 && n + 10 < cap) {
        snprintf(buf + n, cap - n, "|st%d", e->inventory.storage_count);
    }
}

int count_npc_alive(const cr_shared_state_t *s) {
    int n = 0;
    for (int i = 0; i < s->npc_count_current; ++i) {
        if (s->entities[CR_MAX_PLAYERS + i].alive) {
            ++n;
        }
    }
    return n;
}

int count_pl_alive(const cr_shared_state_t *s) {
    int n = 0;
    for (int i = 0; i < s->player_count; ++i) {
        if (s->entities[i].alive) {
            ++n;
        }
    }
    return n;
}

void hip_fill_snapshot(cr_shared_state_t *state, HipSnap *S) {
    memset(S, 0, sizeof(*S));
    S->total_npc_kills = state->total_npc_kills;
    S->npc_count = state->npc_count_current;
    S->pl_count = state->player_count;
    S->turn_seq = state->turn.turn_seq;
    S->game_running = state->game_running ? 1 : 0;
    S->npc_alive = count_npc_alive(state);
    S->pl_alive = count_pl_alive(state);

    const int aid = state->turn.active_entity_id;
    S->active_id = aid;
    if (aid >= 0 && aid < CR_MAX_ENTITIES) {
        const cr_entity_state_t *a = &state->entities[aid];
        snprintf(S->active_name, sizeof S->active_name, "%s", a->name);
        S->active_is_player = a->is_player ? 1 : 0;
    }

    for (int i = 0; i < state->player_count && i < CR_MAX_PLAYERS; ++i) {
        const cr_entity_state_t *e = &state->entities[i];
        HipSnapRow *r = &S->pl[i];
        snprintf(r->name, sizeof r->name, "%s", e->name);
        r->hp = e->hp;
        r->max_hp = e->max_hp;
        r->stamina = e->stamina;
        r->max_stamina = e->max_stamina;
        r->alive = e->alive;
        format_weapons(r->wbuf, sizeof r->wbuf, e);
    }

    S->npc_n = state->npc_count_current;
    if (S->npc_n > CR_MAX_NPCS) {
        S->npc_n = CR_MAX_NPCS;
    }
    for (int i = 0; i < S->npc_n; ++i) {
        const cr_entity_state_t *e = &state->entities[CR_MAX_PLAYERS + i];
        HipSnapRow *r = &S->npc[i];
        snprintf(r->name, sizeof r->name, "%s", e->name);
        r->hp = e->hp;
        r->max_hp = e->max_hp;
        r->stamina = e->stamina;
        r->max_stamina = e->max_stamina;
        r->alive = e->alive;
        format_weapons(r->wbuf, sizeof r->wbuf, e);
    }

    S->log_write_seq = state->log_write_seq;
    memcpy(S->logs, state->logs, sizeof(S->logs));
}

void hip_render_snapshot(const HipSnap *S) {
    clear();
    const int footer = LINES - 1;
    const int bottom_reserve = g_line_edit ? 7 : 1;
    const int last_usable = max(2, footer - bottom_reserve);

    if (g_use_color) {
        attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    }
    mvprintw(0, 0, " HIP - Player view  (log: arrows/PgUp/PgDn Home=newest End=oldest r=newest)");
    clrtoeol();
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);
    }

    if (g_use_color) {
        attron(COLOR_PAIR(CP_SUMMARY));
    }
    mvprintw(1, 0, " Kills %d/%d  NPCs %d/%d alive  Players %d/%d  turn#=%d  game=%d", S->total_npc_kills,
             CR_WIN_KILL_TARGET, S->npc_alive, S->npc_count, S->pl_alive, S->pl_count, S->turn_seq,
             S->game_running);
    clrtoeol();
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_SUMMARY));
    }

    if (S->active_id >= 0) {
        mvprintw(2, 0, " Active: %-12s id=%d %s", S->active_name, S->active_id,
                 S->active_is_player ? "(you if id matches)" : "(NPC)");
    } else {
        mvprintw(2, 0, " Active: (waiting...)");
    }
    clrtoeol();

    int row = 3;
    const int stat_w = 28;
    const int name_col = 11;
    int barw = COLS - name_col - 2 - stat_w;
    barw = max(4, min(16, barw));

    if (row < last_usable) {
        mvprintw(row++, 0, " PARTY");
        clrtoeol();
    }
    for (int i = 0; i < S->pl_count; ++i) {
        if (row > last_usable) {
            break;
        }
        const HipSnapRow *e = &S->pl[i];
        if (g_use_color) {
            const int h = (e->alive && e->max_hp > 0 && e->hp * 3 < e->max_hp);
            attron(COLOR_PAIR(h ? CP_NPC_HURT : CP_PLAYER));
        }
        mvprintw(row, 0, "  %-9s", e->name);
        draw_hp_bar(row, 11, e->alive ? e->hp : 0, e->max_hp > 0 ? e->max_hp : 1, barw);
        mvprintw(row, 11 + barw + 2, "%4d/%-4d st%3d/%3d %s", e->hp, e->max_hp, e->stamina, e->max_stamina,
                 e->alive ? "" : "X");
        clrtoeol();
        ++row;
        if (row > last_usable) {
            break;
        }
        mvprintw(row++, 2, "w:%.*s", max(0, COLS - 4), e->wbuf);
        clrtoeol();
        if (g_use_color) {
            const int h = (e->alive && e->max_hp > 0 && e->hp * 3 < e->max_hp);
            attroff(COLOR_PAIR(h ? CP_NPC_HURT : CP_PLAYER));
        }
    }

    if (row <= last_usable) {
        mvprintw(row++, 0, " ENEMIES");
        clrtoeol();
    }
    for (int i = 0; i < S->npc_n; ++i) {
        if (row > last_usable) {
            break;
        }
        const HipSnapRow *e = &S->npc[i];
        if (g_use_color) {
            const int h = (e->alive && e->max_hp > 0 && e->hp * 3 < e->max_hp);
            attron(COLOR_PAIR(h ? CP_NPC_HURT : CP_NPC));
        }
        mvprintw(row, 0, "  %-9s", e->name);
        draw_hp_bar(row, 11, e->alive ? e->hp : 0, e->max_hp > 0 ? e->max_hp : 1, barw);
        mvprintw(row, 11 + barw + 2, "%4d/%-4d st%3d/%3d %s", e->hp, e->max_hp, e->stamina, e->max_stamina,
                 e->alive ? "" : "X");
        clrtoeol();
        ++row;
        if (e->wbuf[0] != '\0') {
            if (row > last_usable) {
                break;
            }
            mvprintw(row++, 2, "w:%.*s", max(0, COLS - 4), e->wbuf);
            clrtoeol();
        }
        if (g_use_color) {
            const int h = (e->alive && e->max_hp > 0 && e->hp * 3 < e->max_hp);
            attroff(COLOR_PAIR(h ? CP_NPC_HURT : CP_NPC));
        }
    }

    if (row < last_usable) {
        ++row;
    }
    if (row > last_usable) {
        row = last_usable;
    }
    if (g_use_color) {
        attron(COLOR_PAIR(CP_LOG) | A_BOLD);
    }
    if (row <= last_usable) {
        mvprintw(row++, 0, " LOG");
        clrtoeol();
    }
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_LOG) | A_BOLD);
    }

    int max_rows = last_usable - row + 1;
    if (max_rows < 1) {
        max_rows = 1;
    }
    g_log_viewport_h = max_rows;

    const uint64_t wseq = S->log_write_seq;
    int total = 0;
    uint64_t oldest_k = 0;
    uint64_t newest_k = 0;
    if (wseq > 0) {
        newest_k = wseq - 1;
        oldest_k = (wseq > (uint64_t)CR_MAX_LOG_LINES) ? (wseq - (uint64_t)CR_MAX_LOG_LINES) : 0;
        total = (int)(newest_k - oldest_k + 1);
    }
    int vis = min(max_rows, total);
    int max_sc = total - vis;
    if (max_sc < 0) {
        max_sc = 0;
    }
    if (g_log_scroll_back > max_sc) {
        g_log_scroll_back = max_sc;
    }
    if (g_log_scroll_back < 0) {
        g_log_scroll_back = 0;
    }
    /* Never paint "scr" on title/summary/active rows (0-2). */
    if (total > vis && COLS > 50 && row >= 4 && (row - 1) >= 3 && (row - 1) <= last_usable) {
        if (g_use_color) {
            attron(COLOR_PAIR(CP_FOOTER));
        }
        mvprintw(row - 1, min(12, COLS - 20), "scr %d/%d", g_log_scroll_back, max_sc);
        clrtoeol();
        if (g_use_color) {
            attroff(COLOR_PAIR(CP_FOOTER));
        }
    }

    int tw = COLS - 10;
    if (tw < 16) {
        tw = 16;
    }
    if (tw > CR_LOG_LINE_LEN) {
        tw = CR_LOG_LINE_LEN;
    }
    if (wseq > 0 && vis > 0) {
        const uint64_t end_k = newest_k - (uint64_t)g_log_scroll_back;
        const uint64_t start_k = end_k - (uint64_t)(vis - 1);
        for (uint64_t k = start_k; k <= end_k; ++k) {
            if (row > last_usable) {
                break;
            }
            const cr_log_entry_t *en = &S->logs[k % CR_MAX_LOG_LINES];
            if (g_use_color) {
                attron(COLOR_PAIR(CP_LOG));
            }
            mvprintw(row++, 0, " #%-4lu %.*s", (unsigned long)en->seq, tw, en->text);
            clrtoeol();
            if (g_use_color) {
                attroff(COLOR_PAIR(CP_LOG));
            }
        }
    }

    while (row <= last_usable) {
        move(row++, 0);
        clrtoeol();
    }
    if (g_use_color) {
        attron(COLOR_PAIR(CP_FOOTER));
    }
    if (!g_line_edit) {
        mvprintw(footer, 0, " Scroll log when not entering choice | ASP window shows NPC picks");
        clrtoeol();
    }
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_FOOTER));
    }
    refresh();
}

void *hip_hud_thread(void *) {
    while (g_hud_run) {
        pthread_mutex_lock(&g_nc_mu);
        if (!g_line_edit && g_state != nullptr) {
            HipSnap snap{};
            pthread_mutex_lock(&g_state->state_mutex);
            const int gr = g_state->game_running ? 1 : 0;
            if (gr) {
                hip_fill_snapshot(g_state, &snap);
            }
            pthread_mutex_unlock(&g_state->state_mutex);
            if (gr) {
                hip_render_snapshot(&snap);
            } else {
                clear();
                mvprintw(0, 0, " Game finished - exiting HIP shortly.");
                refresh();
            }

            nodelay(stdscr, TRUE);
            const int ch = getch();
            if (ch == KEY_UP || ch == 'k') {
                g_log_scroll_back++;
            } else if (ch == KEY_DOWN || ch == 'j') {
                if (g_log_scroll_back > 0) {
                    g_log_scroll_back--;
                }
            } else if (ch == KEY_PPAGE) {
                g_log_scroll_back += max(1, g_log_viewport_h - 1);
            } else if (ch == KEY_NPAGE) {
                g_log_scroll_back -= max(1, g_log_viewport_h - 1);
            } else if (ch == KEY_HOME || ch == 'r' || ch == 'R') {
                g_log_scroll_back = 0;
            } else if (ch == KEY_END) {
                g_log_scroll_back = 1 << 20;
            }
        }
        pthread_mutex_unlock(&g_nc_mu);
        usleep(100000);
    }
    return nullptr;
}

}  // namespace

extern "C" int hip_tui_init(cr_shared_state_t *state) {
    g_state = state;
    g_tui_on = 0;
    if (!hip_tui_wants_enabled()) {
        return 0;
    }
    if (initscr() == nullptr) {
        return 0;
    }
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_TITLE, COLOR_CYAN, -1);
        init_pair(CP_SUMMARY, COLOR_WHITE, -1);
        init_pair(CP_PLAYER, COLOR_GREEN, -1);
        init_pair(CP_NPC, COLOR_RED, -1);
        init_pair(CP_NPC_HURT, COLOR_YELLOW, -1);
        init_pair(CP_LOG, COLOR_BLACK, COLOR_WHITE);
        init_pair(CP_MENU, COLOR_WHITE, COLOR_BLUE);
        init_pair(CP_FOOTER, COLOR_MAGENTA, -1);
        g_use_color = 1;
    }

    g_hud_run = 1;
    if (pthread_create(&g_hud_tid, nullptr, hip_hud_thread, nullptr) != 0) {
        endwin();
        g_hud_run = 0;
        return 0;
    }
    g_tui_on = 1;
    return 1;
}

extern "C" void hip_tui_shutdown(void) {
    if (!g_tui_on) {
        return;
    }
    g_hud_run = 0;
    pthread_join(g_hud_tid, nullptr);
    endwin();
    g_tui_on = 0;
    g_use_color = 0;
}

extern "C" int hip_tui_is_enabled(void) {
    return g_tui_on ? 1 : 0;
}

extern "C" void hip_tui_read_player_line(cr_shared_state_t *state, int player_id, char *line, size_t cap) {
    if (line == nullptr || cap == 0) {
        return;
    }
    line[0] = '\0';
    if (!g_tui_on) {
        return;
    }

    /* Show newest log lines while choosing an action (avoids stuck at End/history scroll). */
    g_log_scroll_back = 0;

    pthread_mutex_lock(&g_nc_mu);
    g_line_edit = 1;
    HipSnap snap{};
    pthread_mutex_lock(&state->state_mutex);
    hip_fill_snapshot(state, &snap);
    pthread_mutex_unlock(&state->state_mutex);
    hip_render_snapshot(&snap);

    const char *pname = "?";
    if (player_id >= 0 && player_id < snap.pl_count && player_id < CR_MAX_PLAYERS) {
        pname = snap.pl[player_id].name;
    }
    const int seq = snap.turn_seq;

    const int m0 = max(0, LINES - 6);
    const int m1 = max(0, LINES - 5);
    const int m2 = max(0, LINES - 4);
    const int mp = max(0, LINES - 2);
    if (g_use_color) {
        attron(COLOR_PAIR(CP_MENU) | A_BOLD);
    }
    mvprintw(m0, 0, " >>> YOUR TURN: %s (id=%d)  turn_seq=%d <<<", pname, player_id, seq);
    clrtoeol();
    mvprintw(m1, 0,
             " 1 STRIKE  2 EXHAUST  3 USE_WEAPON  4 HEAL  5 SKIP  6 SWAP_IN  7 QUIT  (+ optional ids)");
    clrtoeol();
    mvprintw(m2, 0, " See shared.h CR_WEAPON_* for weapon numbers.");
    clrtoeol();
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_MENU) | A_BOLD);
    }
    mvprintw(mp, 0, " Enter choice: ");
    clrtoeol();
    move(mp, min(15, max(0, COLS - 2)));
    nodelay(stdscr, FALSE);
    echo();
    curs_set(1);
    char tmp[512];
    tmp[0] = '\0';
    const int nmax = (int)min(cap > 0 ? cap - 1 : 0, sizeof tmp - 1);
    if (nmax > 0) {
        wgetnstr(stdscr, tmp, nmax);
    }
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);

    /* Trim */
    char *s = tmp;
    while (*s && isspace((unsigned char)*s)) {
        ++s;
    }
    size_t L = strlen(s);
    while (L > 0 && isspace((unsigned char)s[L - 1])) {
        s[--L] = '\0';
    }
    snprintf(line, cap, "%s", s);

    g_line_edit = 0;
    pthread_mutex_unlock(&g_nc_mu);
}
