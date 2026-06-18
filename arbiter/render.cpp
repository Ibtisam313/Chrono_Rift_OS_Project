//Layout: summary strip → players (HP bars + weapons) → enemies → scrollable battle log.
//Log: ↑/↓ k/j PgUp/Dn Home End. Press 'q' to close HUD only; Ctrl+C stops arbiter.
#include "../shared.h"
#include "arbiter_ui.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ncurses.h>
#include <pthread.h>
#include <unistd.h>

using namespace std;

namespace {

pthread_t g_render_thread;
int g_render_running = 0;
cr_shared_state_t *g_state = nullptr;
std::atomic<int> g_ncurses_ui_active{0};

enum {
    CP_TITLE = 1,
    CP_SUMMARY,
    CP_PLAYER,
    CP_NPC,
    CP_NPC_HURT,
    CP_BAR_OK,
    CP_BAR_LOW,
    CP_FOOTER,
    CP_LOG_LINE,
    CP_SEP,
};

int g_use_color = 0;
int g_log_scroll_back = 0;
int g_log_viewport_h = 8;

void draw_hp_bar(int row, int col, int hp, int max_hp, int width) {
    if (max_hp <= 0) {
        max_hp = 1;
    }
    int filled = (hp * width + max_hp - 1) / max_hp;
    filled = max(0, min(width, filled));
    const int low = (hp * 3 < max_hp && hp > 0);
    if (g_use_color) {
        attron(COLOR_PAIR(low ? CP_BAR_LOW : CP_BAR_OK));
    }
    mvaddch(row, col++, '[');
    for (int i = 0; i < width; ++i) {
        mvaddch(row, col++, i < filled ? '#' : '.');
    }
    mvaddch(row, col++, ']');
    if (g_use_color) {
        attroff(COLOR_PAIR(low ? CP_BAR_LOW : CP_BAR_OK));
    }
}

void format_equipped_weapons(char *buf, size_t cap, const cr_entity_state_t *e) {
    if (cap == 0) {
        return;
    }
    buf[0] = '\0';
    size_t n = 0;
    int i = 0;
    while (i < CR_MAX_INVENTORY_SLOTS && n + 8 < cap) {
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
    if (e->inventory.storage_count > 0 && n + 12 < cap) {
        snprintf(buf + n, cap - n, " |st:%d", e->inventory.storage_count);
    }
}

int count_alive_npcs(const cr_shared_state_t *state) {
    int n = 0;
    for (int i = 0; i < state->npc_count_current; ++i) {
        if (state->entities[CR_MAX_PLAYERS + i].alive) {
            ++n;
        }
    }
    return n;
}

int count_alive_players(const cr_shared_state_t *state) {
    int n = 0;
    for (int i = 0; i < state->player_count; ++i) {
        if (state->entities[i].alive) {
            ++n;
        }
    }
    return n;
}

void draw_separator(int row) {
    if (g_use_color) {
        attron(COLOR_PAIR(CP_SEP));
    }
    for (int c = 0; c < COLS; ++c) {
        mvaddch(row, c, '-');
    }
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_SEP));
    }
}

void draw_frame(cr_shared_state_t *state) {
    erase();

    const int npc_alive = count_alive_npcs(state);
    const int pl_alive = count_alive_players(state);
    const char *phase_name = "?";
    if (state->phase == CR_PHASE_INIT) {
        phase_name = "INIT";
    } else if (state->phase == CR_PHASE_RUNNING) {
        phase_name = "RUN";
    } else if (state->phase == CR_PHASE_FINISHED) {
        phase_name = "END";
    }

    if (g_use_color) {
        attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    }
    mvprintw(0, 0, " CHRONO RIFT - Arbiter HUD ");
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);
    }

    if (g_use_color) {
        attron(COLOR_PAIR(CP_SUMMARY));
    }
    mvprintw(1, 0,
             " Kills %d/%d  |  NPCs alive %d/%d  |  Players %d/%d  |  phase=%s  run=%d  pauseASP=%d",
             state->total_npc_kills, CR_WIN_KILL_TARGET, npc_alive, state->npc_count_current, pl_alive,
             state->player_count, phase_name, state->game_running ? 1 : 0,
             state->asp_paused_for_ultimate ? 1 : 0);
    clrtoeol();

    const cr_entity_state_t *active =
        (state->turn.active_entity_id >= 0 && state->turn.active_entity_id < CR_MAX_ENTITIES)
            ? &state->entities[state->turn.active_entity_id]
            : nullptr;
    if (active != nullptr) {
        mvprintw(2, 0, " Turn seq %-4d  |  NOW: %-12s id=%d (%s)  |  committed=%d", state->turn.turn_seq,
                 active->name, active->id, active->is_player ? "PLAYER" : "NPC",
                 state->turn.action_committed ? 1 : 0);
    } else {
        mvprintw(2, 0, " Turn seq %-4d  |  NOW: (scheduling...)", state->turn.turn_seq);
    }
    clrtoeol();
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_SUMMARY));
    }

    int row = 3;
    draw_separator(row++);

    mvprintw(row++, 0, " PLAYERS  (weapons = CR_WEAPON_* ids, st = storage count)");
    clrtoeol();
    const int stat_rhs = 32;
    const int name_col = 12;
    int bar_w = COLS - name_col - 2 - stat_rhs;
    bar_w = max(6, min(22, bar_w));

    char wbuf[96];
    const int footer_row = LINES - 1;
    const int min_log_rows = 4;
    const int entity_row_cap = max(6, footer_row - min_log_rows);
    for (int i = 0; i < state->player_count; ++i) {
        if (row >= entity_row_cap) {
            break;
        }
        const cr_entity_state_t *e = &state->entities[i];
        format_equipped_weapons(wbuf, sizeof wbuf, e);
        if (g_use_color) {
            const int hurt = (e->alive && e->max_hp > 0 && (e->hp * 3 < e->max_hp));
            attron(COLOR_PAIR(hurt ? CP_NPC_HURT : CP_PLAYER));
        }
        mvprintw(row, 0, "  %-10s", e->name);
        draw_hp_bar(row, 12, e->alive ? e->hp : 0, e->max_hp > 0 ? e->max_hp : 1, bar_w);
        mvprintw(row, 12 + bar_w + 2, " %4d/%-4d HP  st %3d/%3d %s", e->hp, e->max_hp, e->stamina,
                 e->max_stamina, e->alive ? " " : "DEAD");
        clrtoeol();
        ++row;
        if (row < entity_row_cap) {
            mvprintw(row++, 4, "wpn: %.*s", max(0, COLS - 6), wbuf);
            clrtoeol();
        }
        if (g_use_color) {
            const int hurtp = (e->alive && e->max_hp > 0 && (e->hp * 3 < e->max_hp));
            attroff(COLOR_PAIR(hurtp ? CP_NPC_HURT : CP_PLAYER));
        }
    }

    if (row < entity_row_cap) {
        mvprintw(row++, 0, " ENEMIES");
        clrtoeol();
    }
    for (int i = 0; i < state->npc_count_current; ++i) {
        if (row >= entity_row_cap) {
            break;
        }
        const cr_entity_state_t *e = &state->entities[CR_MAX_PLAYERS + i];
        const int hurt = (e->alive && e->max_hp > 0 && (e->hp * 3 < e->max_hp));
        if (g_use_color) {
            attron(COLOR_PAIR(hurt ? CP_NPC_HURT : CP_NPC));
        }
        format_equipped_weapons(wbuf, sizeof wbuf, e);
        mvprintw(row, 0, "  %-10s", e->name);
        draw_hp_bar(row, 12, e->alive ? e->hp : 0, e->max_hp > 0 ? e->max_hp : 1, bar_w);
        mvprintw(row, 12 + bar_w + 2, " %4d/%-4d HP  st %3d/%3d %s", e->hp, e->max_hp, e->stamina,
                 e->max_stamina, e->alive ? " " : "DEAD");
        clrtoeol();
        ++row;
        if (wbuf[0] != '\0' && row < entity_row_cap) {
            mvprintw(row++, 4, "wpn: %.*s", max(0, COLS - 6), wbuf);
            clrtoeol();
        }
        if (g_use_color) {
            attroff(COLOR_PAIR(hurt ? CP_NPC_HURT : CP_NPC));
        }
    }

    if (row < footer_row) {
        ++row;
    }
    if (g_use_color) {
        attron(COLOR_PAIR(CP_LOG_LINE) | A_BOLD);
    }
    mvprintw(row++, 0, " BATTLE LOG  (arrows k/j  PgUp/Dn  Home/r=newest  End=oldest)");
    clrtoeol();
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_LOG_LINE) | A_BOLD);
    }

    int max_log_rows = footer_row - row;
    if (max_log_rows < 1) {
        mvprintw(row++, 0, "(resize terminal taller for log)");
        clrtoeol();
        g_log_viewport_h = 1;
    } else {
        g_log_viewport_h = max_log_rows;

        const uint64_t wseq = state->log_write_seq;
        int total_lines = 0;
        uint64_t oldest_k = 0;
        uint64_t newest_k = 0;
        if (wseq > 0) {
            newest_k = wseq - 1;
            oldest_k = (wseq > (uint64_t)CR_MAX_LOG_LINES) ? (wseq - (uint64_t)CR_MAX_LOG_LINES) : 0;
            total_lines = (int)(newest_k - oldest_k + 1);
        }

        int visible = max_log_rows;
        if (visible > total_lines) {
            visible = total_lines;
        }
        int max_scroll = total_lines - visible;
        if (max_scroll < 0) {
            max_scroll = 0;
        }
        if (g_log_scroll_back > max_scroll) {
            g_log_scroll_back = max_scroll;
        }
        if (g_log_scroll_back < 0) {
            g_log_scroll_back = 0;
        }

        if (total_lines > 0 && visible < total_lines && COLS >= 55) {
            if (g_use_color) {
                attron(COLOR_PAIR(CP_LOG_LINE));
            }
            mvprintw(row - 1, 48, " scr %d/%d ", g_log_scroll_back, max_scroll);
            clrtoeol();
            if (g_use_color) {
                attroff(COLOR_PAIR(CP_LOG_LINE));
            }
        }

        int text_w = COLS - 12;
        if (text_w < 20) {
            text_w = 20;
        }
        if (text_w > CR_LOG_LINE_LEN) {
            text_w = CR_LOG_LINE_LEN;
        }

        if (wseq == 0) {
            if (g_use_color) {
                attron(COLOR_PAIR(CP_LOG_LINE));
            }
            mvprintw(row++, 0, " (no log yet)");
            clrtoeol();
            if (g_use_color) {
                attroff(COLOR_PAIR(CP_LOG_LINE));
            }
        } else if (visible > 0) {
            const uint64_t end_k = newest_k - (uint64_t)g_log_scroll_back;
            const uint64_t start_k = end_k - (uint64_t)(visible - 1);
            for (uint64_t k = start_k; k <= end_k; ++k) {
                if (row >= footer_row) {
                    break;
                }
                const cr_log_entry_t *entry = &state->logs[k % CR_MAX_LOG_LINES];
                if (g_use_color) {
                    attron(COLOR_PAIR(CP_LOG_LINE));
                }
                mvprintw(row++, 0, " #%lu %.*s", (unsigned long)entry->seq, text_w, entry->text);
                clrtoeol();
                if (g_use_color) {
                    attroff(COLOR_PAIR(CP_LOG_LINE));
                }
            }
        }
    }

    while (row < footer_row) {
        move(row++, 0);
        clrtoeol();
    }

    if (g_use_color) {
        attron(COLOR_PAIR(CP_FOOTER));
    }
    mvprintw(footer_row, 0,
             " q=close HUD  |  log scroll as above  |  Ctrl+C=stop arbiter  |  quiet console while open ");
    clrtoeol();
    if (g_use_color) {
        attroff(COLOR_PAIR(CP_FOOTER));
    }
    refresh();
}

void *render_main(void *) {
    initscr();
    g_ncurses_ui_active.store(1);
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    timeout(0);
    keypad(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(CP_TITLE, COLOR_CYAN, -1);
        init_pair(CP_SUMMARY, COLOR_WHITE, -1);
        init_pair(CP_PLAYER, COLOR_GREEN, -1);
        init_pair(CP_NPC, COLOR_RED, -1);
        init_pair(CP_NPC_HURT, COLOR_YELLOW, -1);
        init_pair(CP_BAR_OK, COLOR_GREEN, -1);
        init_pair(CP_BAR_LOW, COLOR_RED, -1);
        init_pair(CP_FOOTER, COLOR_MAGENTA, -1);
        init_pair(CP_LOG_LINE, COLOR_BLACK, COLOR_WHITE);
        init_pair(CP_SEP, COLOR_BLUE, -1);
        g_use_color = 1;
    }

    while (g_render_running) {
        if (g_state != nullptr) {
            pthread_mutex_lock(&g_state->state_mutex);
            draw_frame(g_state);
            pthread_mutex_unlock(&g_state->state_mutex);
        }

        const int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            g_render_running = 0;
            break;
        }
        if (ch == KEY_UP || ch == 'k') {
            g_log_scroll_back++;
        } else if (ch == KEY_DOWN || ch == 'j') {
            if (g_log_scroll_back > 0) {
                g_log_scroll_back--;
            }
        } else if (ch == KEY_PPAGE) {
            g_log_scroll_back += std::max(1, g_log_viewport_h - 1);
        } else if (ch == KEY_NPAGE) {
            g_log_scroll_back -= std::max(1, g_log_viewport_h - 1);
        } else if (ch == KEY_HOME || ch == 'r' || ch == 'R') {
            g_log_scroll_back = 0;
        } else if (ch == KEY_END) {
            g_log_scroll_back = 1 << 30;
        }
        usleep(120000);
    }

    g_ncurses_ui_active.store(0);
    endwin();
    g_use_color = 0;
    return nullptr;
}

}  // namespace

extern "C" int cr_arbiter_ncurses_ui_active(void) {
    return g_ncurses_ui_active.load();
}

int cr_arbiter_render_start(cr_shared_state_t *state) {
    if (g_render_running) {
        return 0;
    }
    g_state = state;
    g_render_running = 1;
    return pthread_create(&g_render_thread, nullptr, render_main, nullptr);
}

void cr_arbiter_render_stop() {
    if (!g_render_running) {
        return;
    }
    g_render_running = 0;
    pthread_join(g_render_thread, nullptr);
}
