#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <poll.h>
#include <math.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tui/tui.h"
#include "tui/pad.h"
#include "tui/routing.h"
#include "tui/monitors.h"
#include "tui/effects.h"
#include "tui/diagnostics-ui.h"
#include "tui/recording-ui.h"
#include "tui/automation-ui.h"
#include "automation.h"
#include "macros.h"
#include "log.h"
#include "xmalloc.h"
#include "utils.h"
#include "config.h"
#include "macros.h"
#include "eventloop.h"
#include "pw/common.h"
#include "pw/peak.h"
#include "pw/managed.h"
#include "scene.h"
#include "route-rules.h"

#define FOR_EACH_TAB(var) for (int var = 0; var < tui.tabs_count; var++)

enum color_pair {
    DEFAULT = 0,
    GREEN = 1,
    YELLOW = 2,
    RED = 3,
};

struct tui tui = {0};
static bool button_motion_tracking = false;
static WINDOW *notice_win;
static char notice[256];
static time_t notice_until;
static uint32_t effect_menu_id = PW_ID_ANY;
static struct scene_job *scene_pending;
static time_t scene_deadline;
static char **scene_names;
static unsigned scene_count;
static struct route_rule *route_menu_rules;
static unsigned route_menu_count;
static bool route_menu_active;
static time_t route_menu_tick;
static struct {
    bool active;
    pid_t pid;
    time_t deadline;
    char kind[8], name[49];
} audio_pending;

static void show_notice(const char *message) {
    snprintf(notice, sizeof(notice), "%s", message);
    notice_until = time(NULL) + 6;
}
void tui_notice(const char *message) { show_notice(message); }

static bool uses_xterm_mouse_protocol(void) {
    const char *term = getenv("TERM");
    return term && (strncmp(term, "xterm", 5) == 0
                    || strncmp(term, "screen", 6) == 0
                    || strncmp(term, "tmux", 4) == 0);
}

void tui_disable_button_motion_tracking(void) {
    if (!button_motion_tracking) return;
    button_motion_tracking = false;
    static const char reset[] = "\033[?1002l";
    const ssize_t written = write(STDOUT_FILENO, reset, sizeof(reset) - 1);
    (void)written;
}

struct node_layout {
    int usable_width, info_area_width, info_area_start;
    int volume_area_start, volume_bar_start, volume_bar_width;
};

static struct node_layout get_node_layout(void) {
    const int usable_width = tui.term_width - 2;
    const int two_thirds = usable_width / 3 * 2;
    const int bar_width_max = MAX(0, two_thirds - 14);
    const int bar_width = (bar_width_max / 15) * 15;
    const int volume_area_width = bar_width + 14;
    const int info_area_width = usable_width - volume_area_width - 1;
    const int volume_area_start = 1 + info_area_width + 1;
    return (struct node_layout){
        .usable_width = usable_width,
        .info_area_width = info_area_width,
        .info_area_start = 1,
        .volume_area_start = volume_area_start,
        .volume_bar_start = volume_area_start + 12,
        .volume_bar_width = bar_width,
    };
}

static enum tui_tab_type media_class_to_tui_tab(enum media_class class) {
    switch (class) {
    case STREAM_OUTPUT_AUDIO: return PLAYBACK;
    case STREAM_INPUT_AUDIO: return RECORDING;
    case AUDIO_SOURCE: return INPUT_DEVICES;
    case AUDIO_SINK: return OUTPUT_DEVICES;
    default: ABORT("Invalid media class passed to media_class_to_tui_tab");
    }
}

static const char *tui_tab_name(enum tui_tab_type tab) {
    switch (tab) {
    case PLAYBACK: return "Playback";
    case RECORDING: return "Recording";
    case OUTPUT_DEVICES: return "Output Devices";
    case INPUT_DEVICES: return "Input Devices";
    case CARDS: return "Cards";
    default: ABORT("Invalid tab type passed to tui_tab_name");
    }
}

static int find_tab(enum tui_tab_type type) {
    FOR_EACH_TAB(i) {
        if (tui.tabs[i].type == type) {
            return i;
        }
    }

    return -1;
}

static void trigger_update(void) {
    if (!tui.update_triggered) {
        if (pw_loop_signal_event(event_loop, tui.update_source) < 0) {
            ERROR("failed to trigger ui update");
        } else {
            tui.update_triggered = true;
        }
    }
}

static void trigger_resize(void) {
    if (!tui.resize_triggered) {
        if (pw_loop_signal_event(event_loop, tui.resize_source) < 0) {
            ERROR("failed to trigger ui resize");
        } else {
            tui.resize_triggered = true;
        }
    }
}

static int print_with_ellipsis(WINDOW *win, int y, int x,
                               const wchar_t string[], int string_len,
                               int max_columns) {
    if (max_columns <= 0 || string_len <= 0 || wmove(win, y, x) != OK) {
        return 0;
    }

    int columns = 0;
    for (int i = 0; i < string_len; i++) {
        const wchar_t wc[2] = { string[i], L'\0' };
        const int width = MAX(0, wcwidth(wc[0]));

        const bool truncate = (i < string_len - 1 && columns + width >= max_columns)
                           || (i == string_len - 1 && columns + width > max_columns);

        cchar_t cc;
        if (!truncate) {
            setcchar(&cc, wc, 0, 0, NULL);
            wadd_wch(win, &cc);
            columns += width;
        } else {
            setcchar(&cc, L"…", 0, 0, NULL);
            wadd_wch(win, &cc);
            columns += 1;
            break;
        }
    }

    return columns;
}

static void tui_tab_item_draw_node(const struct tui_tab_item *const item,
                                   enum tui_tab_item_draw_mask mask) {
    #define DRAW(element) if (mask & TUI_TAB_ITEM_DRAW_##element)

    const struct tui_tab_item_node_data *d = &item->as.node;

    const struct node_layout layout = get_node_layout();
    const int usable_width = layout.usable_width;
    const int info_area_width = layout.info_area_width;
    const int info_area_start = layout.info_area_start;
    const int volume_area_start = layout.volume_area_start;
    const int volume_bar_start = layout.volume_bar_start;
    const int volume_bar_width = layout.volume_bar_width;

    const bool focused = item->focused;
    const bool muted = d->muted;

    TRACE("tui_draw_node: id %d mask %x", d->id, mask);

    WINDOW *const win = tui.pad_win;

    /* prevents leftover artifacts */
    DRAW(BLANKS) {
        for (int i = 0; i < item->height; i++) {
            wmove(win, item->pos + i, 0);
            wclrtoeol(win);
        }
    }

    if (focused) {
        wattron(win, A_BOLD);
    }

    DRAW(DESCRIPTION) {
        int cols = 0;
        if (d->is_default) {
            cols += print_with_ellipsis(win, item->pos + 1, info_area_start,
                                        L"[*] ", wcslen(L"[*] "), usable_width);
        }
        cols += print_with_ellipsis(win, item->pos + 1, info_area_start + cols,
                                    d->info.data, d->info.len, usable_width - cols);

        for (int i = cols; i < usable_width; i++) {
            waddch(win, ' ');
        }
    }

    DRAW(CHANNELS) {
        if (muted) {
            wattron(win, A_DIM);
        }

        for (unsigned i = 0; i < d->n_channels; i++) {
            const struct channel_info *c = &d->channels[i];

            const int pos = item->pos + i + 2;

            const int vol_int = (int)roundf(c->volume * 100);

            mvwprintw(win, pos, volume_area_start, "%5s %-3d ", c->name, vol_int);

            /* draw volume bar */
            int pair = DEFAULT;
            const int step = volume_bar_width / 3;
            const int thresh = vol_int * volume_bar_width / 150;
            for (int j = 0; j < volume_bar_width; j++) {
                cchar_t cc;
                if (step > 0 && j % step == 0 && !muted) {
                    pair += 1;
                }
                setcchar(&cc, (j < thresh) ? config.bar_full_char : config.bar_empty_char,
                         0, pair, NULL);
                mvwadd_wch(win, pos, volume_bar_start + j, &cc);
            }
        }

        wattroff(win, A_DIM);
    }

    DRAW(PEAK) {
        if (muted) {
            wattron(win, A_DIM);
        }

        for (unsigned i = 0; i < d->n_channels; i++) {
            const int pos = item->pos + i + 2;
            const float level = peak_meter_level(d->meter, d->channels[i].name, i);
            const float db = level > 0 ? 20.0f * log10f(level) : -INFINITY;
            char value[8];
            if (!d->meter || peak_meter_failed(d->meter)) {
                snprintf(value, sizeof(value), "n/a");
            } else if (level == 0) {
                snprintf(value, sizeof(value), "-inf");
            } else {
                snprintf(value, sizeof(value), "%+4.0f", fmaxf(-99, fminf(99, db)));
            }

            if (info_area_width >= 14) {
                const int meter_width = info_area_width - 13;
                const float scaled = fmaxf(0, fminf(1, (db + 60.0f) / 60.0f));
                const int filled = level > 0 ? MAX(1, (int)ceilf(scaled * meter_width)) : 0;

                mvwaddstr(win, pos, info_area_start, "Pk [");
                for (int j = 0; j < meter_width; j++) {
                    const bool full = j < filled;
                    const int pair = !full ? DEFAULT
                                   : j >= meter_width * 9 / 10 ? RED
                                   : j >= meter_width * 7 / 10 ? YELLOW : GREEN;
                    cchar_t cc;
                    setcchar(&cc, full ? config.bar_full_char : config.bar_empty_char,
                             0, pair, NULL);
                    mvwadd_wch(win, pos, info_area_start + 4 + j, &cc);
                }
                mvwprintw(win, pos, info_area_start + 4 + meter_width,
                          "] %4s %2s", value,
                          d->meter && !peak_meter_failed(d->meter) ? "dB" : "  ");
            } else if (info_area_width >= 7) {
                mvwprintw(win, pos, info_area_start, "Pk %4s", value);
            }
        }

        wattroff(win, A_DIM);
    }

    DRAW(DECORATIONS) {
        if (muted) {
            wattron(win, A_DIM);
        }

        for (unsigned i = 0; i < d->n_channels; i++) {
            const int pos = item->pos + i + 2;

            const wchar_t *wchar_left, *wchar_right;
            cchar_t cchar_left, cchar_right;

            if (d->n_channels == 1) {
                wchar_left = config.volume_frame.ml;
                wchar_right = config.volume_frame.mr;
            } else if (i == 0) {
                wchar_left = config.volume_frame.tl;
                wchar_right = config.volume_frame.tr;
            } else if (i == d->n_channels - 1) {
                wchar_left = config.volume_frame.bl;
                wchar_right = config.volume_frame.br;
            } else {
                wchar_left = config.volume_frame.cl;
                wchar_right = config.volume_frame.cr;
            }
            setcchar(&cchar_left, wchar_left, 0, DEFAULT, NULL);
            setcchar(&cchar_right, wchar_right, 0, DEFAULT, NULL);
            mvwadd_wch(win, pos, volume_bar_start - 1, &cchar_left);
            mvwadd_wch(win, pos, volume_bar_start + volume_bar_width, &cchar_right);

            const wchar_t *wchar_focus;
            if (focused && (!item->as.node.unlocked_channels
                            || item->as.node.focused_channel == i)) {
                wchar_focus = config.volume_frame.f;
            } else {
                wchar_focus = L" ";
            }
            cchar_t cchar_focus;
            setcchar(&cchar_focus, wchar_focus, 0, DEFAULT, NULL);
            mvwadd_wch(win, pos, volume_bar_start - 2, &cchar_focus);
            mvwadd_wch(win, pos, volume_bar_start + volume_bar_width + 1, &cchar_focus);
        }

        wattroff(win, A_DIM);
    }

    DRAW(ROUTES) {
        if (!d->n_routes) {
            goto routes_end;
        }

        const int routes_line_pos = item->pos + item->height - 2;

        int cols = 0;
        cols += print_with_ellipsis(win, routes_line_pos, 1,
                                    L"Routes: ", wcslen(L"Routes: "),
                                    usable_width);

        if (!d->n_routes) {
            wattron(win, A_DIM);
            cols += print_with_ellipsis(win, routes_line_pos, 1 + cols,
                                        L"(none)", wcslen(L"(none)"),
                                        usable_width - cols);
        } else {
            if (d->active_route) {
                /* draw active route first */
                cols += print_with_ellipsis(win, routes_line_pos, 1 + cols,
                                            d->active_route->description.data,
                                            d->active_route->description.len,
                                            usable_width - cols);
            }

            wattron(win, A_DIM);
            for (unsigned i = 0; i < d->n_routes; i++) {
                const struct route_info *p = &d->routes[i];
                if (p == d->active_route) {
                    continue;
                }

                if (i > 0 || d->active_route) {
                    cols += print_with_ellipsis(win, routes_line_pos, 1 + cols,
                                                config.routes_separator,
                                                wcslen(config.routes_separator),
                                                usable_width - cols);
                }

                cols += print_with_ellipsis(win, routes_line_pos, 1 + cols,
                                            p->description.data, p->description.len,
                                            usable_width - cols);
            }
        }

        wattroff(win, A_DIM);
    }
routes_end:

    DRAW(BORDERS) {
        /* box */
        wmove(win, item->pos, 0);
        waddwstr(win, config.borders.tl);
        for (int x = 1; x < tui.term_width - 1; x++) {
            waddwstr(win, config.borders.ts);
        }
        waddwstr(win, config.borders.tr);

        wmove(win, item->pos + item->height - 1, 0);
        waddwstr(win, config.borders.bl);
        for (int x = 1; x < tui.term_width - 1; x++) {
            waddwstr(win, config.borders.bs);
        }
        waddwstr(win, config.borders.br);

        for (int y = 1; y < item->height - 1; y++) {
            wmove(win, item->pos + y, 0);
            waddwstr(win, config.borders.ls);
        }

        for (int y = 1; y < item->height - 1; y++) {
            wmove(win, item->pos + y, tui.term_width - 1);
            waddwstr(win, config.borders.ls);
        }
    }

    wattroff(win, A_BOLD);

    #undef DRAW
}

static void tui_tab_item_draw_device(const struct tui_tab_item *const item,
                                     enum tui_tab_item_draw_mask mask) {
    #define DRAW(element) if (mask & TUI_TAB_ITEM_DRAW_##element)

    const struct tui_tab_item_device_data *d = &item->as.device;
    const struct device *dev = item->as.device.dev;

    const int usable_width = tui.term_width - 2; /* account for box borders */

    const bool focused = item->focused;

    TRACE("tui_draw_device: id %d mask %x", device_id(dev), mask);

    WINDOW *const win = tui.pad_win;

    DRAW(BLANKS) {
        for (int i = 0; i < item->height; i++) {
            wmove(win, item->pos + i, 0);
            wclrtoeol(win);
        }
    }

    if (focused) {
        wattron(win, A_BOLD);
    }

    DRAW(DESCRIPTION) {
        int cols = print_with_ellipsis(win, item->pos + 1, 1,
                                       d->info.data, d->info.len, usable_width);

        for (int i = cols; i < usable_width; i++) {
            waddch(win, ' ');
        }
    }

    DRAW(PROFILES) {
        /* draw profiles */
        const int profiles_line_pos = item->pos + item->height - 2;

        int cols = 0;
        cols += print_with_ellipsis(win, profiles_line_pos, 1,
                                    L"Profiles: ", wcslen(L"Profiles: "),
                                    usable_width);

        if (!d->n_profiles) {
            wattron(win, A_DIM);
            cols += print_with_ellipsis(win, profiles_line_pos, 1 + cols,
                                        L"(none)", wcslen(L"(none)"),
                                        usable_width - cols);
        } else {
            if (d->active_profile) {
                /* draw active profile first */
                cols += print_with_ellipsis(win, profiles_line_pos, 1 + cols,
                                            d->active_profile->description.data,
                                            d->active_profile->description.len,
                                            usable_width - cols);
            }

            wattron(win, A_DIM);
            for (unsigned i = 0; i < d->n_profiles; i++) {
                const struct profile_info *p = &d->profiles[i];
                if (p == d->active_profile) {
                    continue;
                }

                cols += print_with_ellipsis(win, profiles_line_pos, 1 + cols,
                                            config.profiles_separator,
                                            wcslen(config.profiles_separator),
                                            usable_width - cols);

                cols += print_with_ellipsis(win, profiles_line_pos, 1 + cols,
                                            p->description.data, p->description.len,
                                            usable_width - cols);
            }
        }

        wattroff(win, A_DIM);
    }

    DRAW(BORDERS) {
        wmove(win, item->pos, 0);
        waddwstr(win, config.borders.tl);
        for (int x = 1; x < tui.term_width - 1; x++) {
            waddwstr(win, config.borders.ts);
        }
        waddwstr(win, config.borders.tr);

        wmove(win, item->pos + item->height - 1, 0);
        waddwstr(win, config.borders.bl);
        for (int x = 1; x < tui.term_width - 1; x++) {
            waddwstr(win, config.borders.bs);
        }
        waddwstr(win, config.borders.br);

        for (int y = 1; y < item->height - 1; y++) {
            wmove(win, item->pos + y, 0);
            waddwstr(win, config.borders.ls);
        }

        for (int y = 1; y < item->height - 1; y++) {
            wmove(win, item->pos + y, tui.term_width - 1);
            waddwstr(win, config.borders.ls);
        }
    }

    wattroff(win, A_BOLD);

    #undef DRAW
}

static void tui_tab_item_draw(const struct tui_tab_item *const item,
                              enum tui_tab_item_draw_mask mask) {
    if (item->tab_index != tui.tab_index) {
        return;
    }

    switch (item->type) {
    case TUI_TAB_ITEM_TYPE_NODE:
        tui_tab_item_draw_node(item, mask);
        break;
    case TUI_TAB_ITEM_TYPE_DEVICE:
        tui_tab_item_draw_device(item, mask);
        break;
    }
}

static void start_node_meter(struct tui_tab_item *item) {
    if (item->type != TUI_TAB_ITEM_TYPE_NODE || item->tab_index != tui.tab_index
        || item->as.node.n_channels == 0 || item->as.node.meter || !tui.peak_source) {
        return;
    }

    struct tui_tab_item_node_data *d = &item->as.node;
    const enum media_class class = node_media_class(d->node);
    const bool capture_sink = class == AUDIO_SINK || class == STREAM_INPUT_AUDIO;
    d->meter = peak_meter_create(node_meter_target(d->node), capture_sink,
                                 class != AUDIO_SOURCE);
}

static void stop_node_meter(struct tui_tab_item *item) {
    if (item->type == TUI_TAB_ITEM_TYPE_NODE) {
        peak_meter_destroy(item->as.node.meter);
        item->as.node.meter = NULL;
    }
}

static void set_tab_meters(int tab_index, bool active) {
    const struct tui_tab *tab = &tui.tabs[tab_index];
    LIST_FOREACH(elem, &tab->items) {
        struct tui_tab_item *item = CONTAINER_OF(elem, struct tui_tab_item, link);
        if (active) {
            start_node_meter(item);
        } else {
            stop_node_meter(item);
        }
    }
}

/* this only updates scroll pos and does not actually draw anything */
static void tui_tab_item_ensure_visible(const struct tui_tab_item *const item) {
    struct tui_tab *const tab = &tui.tabs[item->tab_index];

    /* minus top bar */
    const int visible_height = tui.term_height - 1;

    if (tab->scroll_pos > item->pos) {
        tab->scroll_pos = item->pos;
    } else if ((item->pos + item->height) > (tab->scroll_pos + visible_height)) {
        /* a + b = c + d <=> c = a + b - d */
        tab->scroll_pos = (item->pos + item->height) - visible_height;
    }
}

static void tui_tab_item_focus(struct tui_tab_item *const item, bool draw, bool user) {
    struct tui_tab *const tab = &tui.tabs[item->tab_index];
    if (user) {
        tab->user_changed_focus = true;
    }

    if (tab->focused == item) {
        return;
    } else if (tab->focused != NULL) {
        tab->focused->focused = false;
        if (draw) {
            tui_tab_item_draw(tab->focused, TUI_TAB_ITEM_DRAW_EVERYTHING);
        }
    }

    tab->focused = item;
    item->focused = true;
    if (draw) {
        tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_EVERYTHING);
    }

    tui_tab_item_ensure_visible(item);
}

static void tui_tab_item_unfocus(struct tui_tab_item *const item, bool draw) {
    struct tui_tab *const tab = &tui.tabs[item->tab_index];

    if (tab->focused != item) {
        WARN("tui_tab_item_unfocus called on unfocused item");
        return;
    }

    struct tui_tab_item *next = NULL;
    if (item->link.next != &tab->items) {
        next = CONTAINER_OF(item->link.next, struct tui_tab_item, link);
    } else if (item->link.prev != &tab->items) {
        next = CONTAINER_OF(item->link.prev, struct tui_tab_item, link);
    }

    if (next != NULL) {
        tui_tab_item_focus(next, draw, false);
    } else {
        tab->focused = NULL;
    }
}

void tui_bind_change_focus(union tui_bind_data data) {
    enum tui_direction direction = data.direction;

    if (tui.menu_active) {
        tui_menu_change_focus(tui.menu, (direction == UP) ? -1 : 1);
        return;
    }

    struct tui_tab *const tab = &tui.tabs[tui.tab_index];
    struct tui_tab_item *const f = tab->focused;
    if (f == NULL) {
        return;
    }

    switch (direction) {
    case DOWN: {
        const bool channel = f->type == TUI_TAB_ITEM_TYPE_NODE
                          && f->as.node.unlocked_channels
                          && f->as.node.focused_channel < f->as.node.n_channels - 1;
        if (channel) {
            f->as.node.focused_channel += 1;
            tui_tab_item_draw(f, TUI_TAB_ITEM_DRAW_DECORATIONS);
        } else {
            struct tui_tab_item *next = NULL;

            if (&f->link != tab->items.prev) {
                // f is not the last element
                next = CONTAINER_OF(f->link.next, struct tui_tab_item, link);
            } else if (config.wraparound) {
                // f is last, wrap around to the beginning
                next = CONTAINER_OF(tab->items.next, struct tui_tab_item, link);
            } else {
                break;
            }

            if (next->type == TUI_TAB_ITEM_TYPE_NODE && next->as.node.unlocked_channels) {
                next->as.node.focused_channel = 0;
            }
            tui_tab_item_focus(next, true, true);
        }
        break;
    }
    case UP: {
        const bool channel = f->type == TUI_TAB_ITEM_TYPE_NODE
                          && f->as.node.unlocked_channels
                          && f->as.node.focused_channel > 0;
        if (channel) {
            f->as.node.focused_channel -= 1;
            tui_tab_item_draw(f, TUI_TAB_ITEM_DRAW_DECORATIONS);
        } else {
            struct tui_tab_item *next = NULL;

            if (&f->link != tab->items.next) {
                // f is not the first element
                next = CONTAINER_OF(f->link.prev, struct tui_tab_item, link);
            } else if (config.wraparound) {
                // f is first, wrap around to the end
                next = CONTAINER_OF(tab->items.prev, struct tui_tab_item, link);
            } else {
                break;
            }

            if (next->type == TUI_TAB_ITEM_TYPE_NODE && next->as.node.unlocked_channels) {
                next->as.node.focused_channel = next->as.node.n_channels - 1;
            }
            tui_tab_item_focus(next, true, true);
        }
        break;
    }
    }
}

static void redraw_current_tab(void) {
    const struct tui_tab *tab = &tui.tabs[tui.tab_index];

    int bottom = 0;
    LIST_FOREACH(elem, &tab->items) {
        struct tui_tab_item *item = CONTAINER_OF(elem, struct tui_tab_item, link);
        tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_EVERYTHING);
        bottom += item->height;
    }

    if (wmove(tui.pad_win, bottom, 0) != OK) {
        WARN("wmove(tui.pad_win, %d, 0) failed!", bottom);
    } else {
        TRACE("wclrtobot(tui.pad_win) bottom %d", bottom);
        wclrtobot(tui.pad_win);
    }

    if (bottom == 0) {
        /* empty tab */
        static const char empty[] = "Empty";
        wattron(tui.pad_win, A_DIM);
        mvwaddstr(tui.pad_win,
                  (tui.term_height - 1) / 2, (tui.term_width / 2) - (strlen(empty) / 2),
                  empty);
        wattroff(tui.pad_win, A_DIM);
    }
}

static void redraw_status_bar(void) {
    wmove(tui.bar_win, 0, 0);

    FOR_EACH_TAB(tab_index) {
        if (tab_index != tui.tab_index) {
            wattron(tui.bar_win, A_DIM);
        } else {
            wattron(tui.bar_win, A_BOLD);
        }
        waddstr(tui.bar_win, tui_tab_name(tui.tabs[tab_index].type));
        if (tab_index != tui.tab_index) {
            wattroff(tui.bar_win, A_DIM);
        } else {
            wattroff(tui.bar_win, A_BOLD);
        }
        waddstr(tui.bar_win, "   ");
    }

    wclrtoeol(tui.bar_win);
}

void tui_bind_focus_last(union tui_bind_data data) {
    if (tui.menu_active) { tui.menu->selected = tui.menu->n_items - 1; return; }
    struct tui_tab *const tab = &tui.tabs[tui.tab_index];

    if (list_is_empty(&tab->items)) {
        return;
    }

    struct tui_tab_item *last = CONTAINER_OF(tab->items.prev, struct tui_tab_item, link);
    tui_tab_item_focus(last, true, true);
}

void tui_bind_focus_first(union tui_bind_data data) {
    if (tui.menu_active) { tui.menu->selected = 0; return; }
    struct tui_tab *const tab = &tui.tabs[tui.tab_index];

    if (list_is_empty(&tab->items)) {
        return;
    }

    struct tui_tab_item *first = CONTAINER_OF(tab->items.next, struct tui_tab_item, link);
    tui_tab_item_focus(first, true, true);
}

void tui_bind_change_volume(union tui_bind_data data) {
    const enum tui_direction direction = data.direction;
    const struct tui_tab_item *const focused = tui.tabs[tui.tab_index].focused;

    if (focused == NULL || focused->type != TUI_TAB_ITEM_TYPE_NODE || tui.menu_active) {
        return;
    }

    float delta = (direction == UP) ? config.volume_step : -config.volume_step;

    const struct node *node = focused->as.node.node;
    int result=automation_take_control(node_id(node),NULL);
    if(result<0){show_notice(strerror(-result));return;}
    if (focused->as.node.unlocked_channels) {
        node_change_volume(node, false, delta, focused->as.node.focused_channel);
    } else {
        node_change_volume(node, false, delta, ALL_CHANNELS);
    }
}

void tui_bind_set_volume(union tui_bind_data data) {
    const float vol = data.volume;
    const struct tui_tab_item *const focused = tui.tabs[tui.tab_index].focused;

    if (focused == NULL || focused->type != TUI_TAB_ITEM_TYPE_NODE || tui.menu_active) {
        return;
    }

    const struct node *node = focused->as.node.node;
    int result=automation_take_control(node_id(node),NULL);
    if(result<0){show_notice(strerror(-result));return;}
    if (focused->as.node.unlocked_channels) {
        node_change_volume(node, true, vol, focused->as.node.focused_channel);
    } else {
        node_change_volume(node, true, vol, ALL_CHANNELS);
    }
}

void tui_bind_change_mute(union tui_bind_data data) {
    const enum tui_change_mode mode = data.change_mode;
    const struct tui_tab_item *const focused = tui.tabs[tui.tab_index].focused;

    if (focused == NULL || focused->type != TUI_TAB_ITEM_TYPE_NODE || tui.menu_active) {
        return;
    }

    const struct tui_tab_item_node_data *d = &focused->as.node;

    const struct node *node = focused->as.node.node;
    switch (mode) {
    case ENABLE:
        node_set_mute(node, true);
        break;
    case DISABLE:
        node_set_mute(node, false);
        break;
    case TOGGLE:
        node_set_mute(node, !d->muted);
        break;
    }
}

void tui_bind_change_channel_lock(union tui_bind_data data) {
    enum tui_change_mode mode = data.change_mode;
    struct tui_tab_item *const focused = tui.tabs[tui.tab_index].focused;

    if (focused == NULL || focused->type != TUI_TAB_ITEM_TYPE_NODE || tui.menu_active) {
        return;
    }

    bool change = false;
    switch (mode) {
    case ENABLE:
        if (!focused->as.node.unlocked_channels) {
            focused->as.node.unlocked_channels = true;
            change = true;
        }
        break;
    case DISABLE:
        if (focused->as.node.unlocked_channels) {
            focused->as.node.unlocked_channels = false;
            change = true;
        }
        break;
    case TOGGLE:
        focused->as.node.unlocked_channels = !focused->as.node.unlocked_channels;
        change = true;
        break;
    }

    if (change) {
        tui_tab_item_draw(focused, TUI_TAB_ITEM_DRAW_DECORATIONS);
    }
}

static void tui_set_tab_and_redraw(int new_tab_index) {
    if (tui.tab_index == new_tab_index) {
        return;
    } else if (new_tab_index < 0 || new_tab_index > tui.tabs_count - 1) {
        WARN("tui_set_tab_and_redraw: OOB new_tab_index %d", new_tab_index);
        return;
    }

    set_tab_meters(tui.tab_index, false);
    tui.tab_index = new_tab_index;
    set_tab_meters(tui.tab_index, true);
    redraw_current_tab();
    redraw_status_bar();

    TRACE("current tab is: index %d (%s)",
          tui.tab_index, tui_tab_name(tui.tabs[tui.tab_index].type));
}

void tui_bind_change_tab(union tui_bind_data data) {
    int new_tab_index = tui.tab_index;
    switch (data.direction) {
    case UP:
        if (tui.tab_index == tui.tabs_count - 1) {
            new_tab_index = 0;
        } else {
            new_tab_index = tui.tab_index + 1;
        }
        break;
    case DOWN:
        if (tui.tab_index == 0) {
            new_tab_index = tui.tabs_count - 1;
        } else {
            new_tab_index = tui.tab_index - 1;
        }
        break;
    }

    tui_bind_set_tab_index((union tui_bind_data){
        .index = new_tab_index
    });
}

void tui_bind_set_tab(union tui_bind_data data) {
    int tab_index = find_tab(data.tab);
    if (tab_index < 0) {
        return;
    }

    tui_bind_set_tab_index((union tui_bind_data){ .index = tab_index });
}

void tui_bind_set_tab_index(union tui_bind_data data) {
    const int index = data.index;

    if (tui.menu_active) {
        return;
    }

    if (tui.routing_active) tui_bind_toggle_routing((union tui_bind_data){0});

    tui_set_tab_and_redraw(index);
}

void tui_bind_set_default(union tui_bind_data data) {
    struct tui_tab_item *const focused = tui.tabs[tui.tab_index].focused;

    if (focused == NULL || focused->type != TUI_TAB_ITEM_TYPE_NODE || tui.menu_active) {
        return;
    }

    const struct node *const node = focused->as.node.node;
    node_set_default(node);
}

static const char *target_node_label(const struct node *node) {
    const struct dict *props = node_properties(node);
    const char *description = dict_get(props, "node.description");
    const char *name = dict_get(props, "node.name");
    return description ?: name ?: "Unnamed device";
}

struct target_menu_candidates {
    enum media_class media_class;
    struct node **nodes;
    unsigned count;
};

static void collect_target_node(struct node *node, void *data) {
    struct target_menu_candidates *candidates = data;
    if (node_media_class(node) != candidates->media_class) return;
    candidates->nodes = xreallocarray(candidates->nodes, candidates->count + 1,
                                     sizeof(candidates->nodes[0]));
    candidates->nodes[candidates->count++] = node;
}

static int compare_target_nodes(const void *a, const void *b) {
    const struct node *left = *(const struct node *const *)a;
    const struct node *right = *(const struct node *const *)b;
    int result = strcmp(target_node_label(left), target_node_label(right));
    if (result) return result;
    return (node_id(left) > node_id(right)) - (node_id(left) < node_id(right));
}

static void on_target_selection_done(struct tui_menu *menu, struct tui_menu_item *pick) {
    const uint32_t stream_id = menu->data.uint;
    const uint32_t target_id = pick->data.uint;
    if (!pipewire_set_stream_target(stream_id, target_id)) {
        WARN("failed to set target %u for stream %u", target_id, stream_id);
    }
    tui_menu_free(menu);
    tui.menu_active = false;
    redraw_current_tab();
}

void tui_bind_select_target(union tui_bind_data data) {
    const struct tui_tab_item *focused = tui.tabs[tui.tab_index].focused;
    if (!focused || focused->type != TUI_TAB_ITEM_TYPE_NODE || tui.menu_active
        || !pipewire_default_available()) return;

    const enum media_class stream_class = node_media_class(focused->as.node.node);
    struct target_menu_candidates candidates = {0};
    if (stream_class == STREAM_OUTPUT_AUDIO) {
        candidates.media_class = AUDIO_SINK;
    } else if (stream_class == STREAM_INPUT_AUDIO) {
        candidates.media_class = AUDIO_SOURCE;
    } else {
        return;
    }

    pipewire_foreach_node(collect_target_node, &candidates);
    if (candidates.count > 1) {
        qsort(candidates.nodes, candidates.count, sizeof(candidates.nodes[0]),
              compare_target_nodes);
    }

    tui.menu = tui_menu_create(candidates.count + 1);
    tui.menu->callback = on_target_selection_done;
    tui.menu->data.uint = focused->as.node.id;
    tui_menu_resize(tui.menu, tui.term_width, tui.term_height);
    wstring_printf(&tui.menu->header, L"Select %ls for %ls",
                   stream_class == STREAM_OUTPUT_AUDIO ? L"output" : L"input",
                   focused->as.node.description.data ?: L"stream");

    struct tui_menu_item *item = &tui.menu->items[0];
    wstring_printf(&item->wstr, L"Follow default %ls",
                   stream_class == STREAM_OUTPUT_AUDIO ? L"output" : L"input");
    item->data.uint = PW_ID_ANY;

    const uint32_t current_target = pipewire_get_stream_target(focused->as.node.id);
    for (unsigned i = 0; i < candidates.count; i++) {
        const struct node *node = candidates.nodes[i];
        item = &tui.menu->items[i + 1];
        wstring_printf(&item->wstr, L"%s (id:%u)", target_node_label(node), node_id(node));
        item->data.uint = node_id(node);
        if (node_id(node) == current_target) tui.menu->selected = i + 1;
    }
    free(candidates.nodes);
    tui.menu_active = true;
}

struct audio_candidates { uint32_t *ids; unsigned count; };

static int compare_audio_ids(const void *a, const void *b) {
    uint32_t left = *(const uint32_t *)a, right = *(const uint32_t *)b;
    return (left > right) - (left < right);
}

static void collect_managed(const struct graph_node *node, void *data) {
    if (!graph_node_is_audio(node) || !streq(dict_get(&node->props, "pipemixer.managed"), "1")
        || streq(dict_get(&node->props, "pipemixer.kind"), "monitor")
        || !streq(dict_get(&node->props, "pipemixer.role"), "input")) return;
    struct audio_candidates *candidates = data;
    candidates->ids = xreallocarray(candidates->ids, candidates->count + 1, sizeof(uint32_t));
    candidates->ids[candidates->count++] = node->id;
}

static void start_audio(const char *kind, uint32_t source, uint32_t destination, const char *preset) {
    if (audio_pending.active) { show_notice("An audio path is still being created"); return; }
    unsigned index = 1;
    do { snprintf(audio_pending.name, sizeof(audio_pending.name), "%s%u", kind, index++); }
    while (managed_find(kind, audio_pending.name, NULL));
    char *src = NULL, *dest = NULL;
    if (streq(kind, "send")) {
        const struct graph_node *s = graph_node_find(source), *d = graph_node_find(destination);
        if (!s || !d) { show_notice("Selected audio endpoint disappeared"); return; }
        if (graph_would_cycle(source, destination)) { show_notice("Send would create a feedback loop"); return; }
        xasprintf(&src, "serial:%s", dict_get(&s->props, PW_KEY_OBJECT_SERIAL));
        xasprintf(&dest, "serial:%s", dict_get(&d->props, PW_KEY_OBJECT_SERIAL));
    }
    audio_pending.pid = managed_spawn(kind, audio_pending.name, preset ?: src, dest);
    free(src); free(dest);
    if (audio_pending.pid < 0) { show_notice(strerror(-audio_pending.pid)); return; }
    snprintf(audio_pending.kind, sizeof(audio_pending.kind), "%s", kind);
    audio_pending.active = true;
    audio_pending.deadline = time(NULL) + 8;
    show_notice("Creating audio path...");
}

static void on_send_selected(struct tui_menu *menu, struct tui_menu_item *pick) {
    uint32_t source = menu->data.uint, destination = pick->data.uint;
    tui_bind_cancel_selection((union tui_bind_data){0});
    start_audio("send", source, destination, NULL);
}

static void select_send_destination(uint32_t source) {
    struct target_menu_candidates candidates = { .media_class = AUDIO_SINK };
    pipewire_foreach_node(collect_target_node, &candidates);
    if (!candidates.count) { show_notice("No audio sink available"); free(candidates.nodes); return; }
    qsort(candidates.nodes, candidates.count, sizeof(candidates.nodes[0]), compare_target_nodes);
    tui.menu = tui_menu_create(candidates.count);
    tui.menu->callback = on_send_selected;
    tui.menu->data.uint = source;
    wstring_printf(&tui.menu->header, L"Send a copy to an output device or bus");
    for (unsigned i = 0; i < candidates.count; i++) {
        wstring_printf(&tui.menu->items[i].wstr, L"%s (id:%u)", target_node_label(candidates.nodes[i]), node_id(candidates.nodes[i]));
        tui.menu->items[i].data.uint = node_id(candidates.nodes[i]);
    }
    free(candidates.nodes);
    tui_menu_resize(tui.menu, tui.term_width, tui.term_height);
    tui.menu_active = true;
}

static void on_audio_selected(struct tui_menu *menu, struct tui_menu_item *pick) {
    uint32_t action = pick->data.uint, source = menu->data.uint;
    tui_bind_cancel_selection((union tui_bind_data){0});
    if (action == PW_ID_ANY) start_audio("bus", 0, 0, NULL);
    else if (action == PW_ID_ANY - 1) select_send_destination(source);
    else {
        const struct graph_node *node = graph_node_find(action);
        if (!node || !streq(dict_get(&node->props, "pipemixer.managed"), "1")) {
            show_notice("Audio path already disappeared"); return;
        }
        int result = managed_remove(dict_get(&node->props, "pipemixer.kind"), dict_get(&node->props, "pipemixer.group"));
        show_notice(result < 0 ? strerror(-result) : "Removing audio path...");
    }
}

void tui_bind_manage_audio(union tui_bind_data data) {
    if (tui.menu_active || tui.routing_active) return;
    const struct tui_tab_item *focused = tui.tabs[tui.tab_index].focused;
    uint32_t source = focused && focused->type == TUI_TAB_ITEM_TYPE_NODE ? focused->as.node.id : PW_ID_ANY;
    bool can_send = source != PW_ID_ANY && graph_node_has_ports(source, PW_DIRECTION_OUTPUT);
    struct audio_candidates candidates = {0};
    graph_foreach_node(collect_managed, &candidates);
    if (candidates.count > 1) qsort(candidates.ids, candidates.count, sizeof(uint32_t), compare_audio_ids);
    unsigned base = can_send ? 2 : 1;
    tui.menu = tui_menu_create(base + candidates.count);
    tui.menu->callback = on_audio_selected;
    tui.menu->data.uint = source;
    wstring_printf(&tui.menu->header, L"Audio buses and independent sends");
    wstring_printf(&tui.menu->items[0].wstr, L"Create a stereo bus (virtual output + microphone)");
    tui.menu->items[0].data.uint = PW_ID_ANY;
    if (can_send) {
        wstring_printf(&tui.menu->items[1].wstr, L"Create an independent send from the focused node");
        tui.menu->items[1].data.uint = PW_ID_ANY - 1;
    }
    for (unsigned i = 0; i < candidates.count; i++) {
        const struct graph_node *node = graph_node_find(candidates.ids[i]);
        wstring_printf(&tui.menu->items[base + i].wstr, L"Delete %s %s", dict_get(&node->props, "pipemixer.kind"), dict_get(&node->props, "pipemixer.group"));
        tui.menu->items[base + i].data.uint = node->id;
    }
    free(candidates.ids);
    tui_menu_resize(tui.menu, tui.term_width, tui.term_height);
    tui.menu_active = true;
}

static void refresh_effect_menu(void) {
    if (!tui.menu_active || effect_menu_id == PW_ID_ANY) return;
    const struct graph_node *node = graph_node_find(effect_menu_id);
    if (!node) {
        tui_bind_cancel_selection((union tui_bind_data){0});
        show_notice("Effect disappeared");
        return;
    }
    const struct graph_control *wet = graph_control_find(node, "wet:Mult");
    const struct graph_control *dry = graph_control_find(node, "dry:Mult");
    wstring_clear(&tui.menu->header);
    wstring_printf(&tui.menu->header, L"%s: h/l adjust, Enter reset, Space bypass%s",
                   dict_get(&node->props, "pipemixer.group"),
                   wet && dry && wet->value == 0 && dry->value == 1 ? " [ON]" : "");
    for (unsigned i = 0; i < tui.menu->n_items; i++) {
        unsigned index = tui.menu->items[i].data.uint;
        if (index >= node->n_controls) continue;
        const struct graph_control *control = &node->controls[index];
        wstring_clear(&tui.menu->items[i].wstr);
        wstring_printf(&tui.menu->items[i].wstr, L"%s = %.6g  [%.6g, %.6g]",
                       control->name, control->value, control->minimum, control->maximum);
    }
}

static void on_scene_selected(struct tui_menu *menu, struct tui_menu_item *pick) {
    unsigned action = pick->data.uint;
    char name[49];
    if (action == 0) {
        unsigned number = 1;
        for (;; number++) {
            snprintf(name, sizeof(name), "scene%u", number);
            bool found = false;
            for (unsigned i = 0; i < scene_count; i++) if (streq(name, scene_names[i])) found = true;
            if (!found) break;
        }
    } else if (action != PW_ID_ANY) snprintf(name, sizeof(name), "%s", scene_names[(action - 1) / 3]);
    else snprintf(name, sizeof(name), "off");
    tui_bind_cancel_selection((union tui_bind_data){0});
    char error[256] = {0}; int result;
    if (action == 0) {
        result = scene_save(name, error, sizeof(error));
        if (!result) snprintf(error, sizeof(error), "Saved scene %s; press s to load it", name);
        else if (result == -EAGAIN) snprintf(error, sizeof(error), "Audio state is still updating; retry saving shortly");
    } else if (action == PW_ID_ANY || action % 3 == 0) {
        result = scene_startup_set(name, error, sizeof(error));
        if (!result) snprintf(error, sizeof(error), streq(name, "off") ? "Startup scene restoration disabled" : "Startup scene set to %s", name);
    } else if (action % 3 == 2) {
        result = scene_delete(name);
        snprintf(error, sizeof(error), result < 0 ? "Cannot delete scene: %s" : "Deleted scene %s", result < 0 ? strerror(-result) : name);
    } else {
        scene_pending = scene_load(name, error, sizeof(error));
        if (scene_pending) { scene_deadline = time(NULL) + 30; snprintf(error, sizeof(error), "Loading scene %s...", name); }
    }
    show_notice(error[0] ? error : "Scene operation failed");
}

static void update_route_menu(void) {
    wstring_printf(&tui.menu->header, L"Automatic routing | engine: %s | h/l: priority", route_rules_running() ? "running" : "stopped");
    struct route_rule_state *states = xcalloc(route_menu_count ?: 1, sizeof(*states));
    route_rules_observe(route_menu_rules, route_menu_count, states);
    for (unsigned i = 0; i < route_menu_count; i++) {
        const struct route_rule *rule = &route_menu_rules[i];
        struct route_rule_state state = states[i];
        const char *target = state.active_target == -2 ? "multiple targets" : state.active_target > 0 ? rule->fallbacks[state.active_target - 1] : rule->input;
        wstring_printf(&tui.menu->items[i * 2].wstr, L"%s %s [%s %u/%u%s%s p=%d g=%s]: %s -> %s",
                       rule->enabled ? "Disable" : "Enable", rule->name, state.state, state.connected_pairs, state.total_pairs,
                       rule->glob ? " glob" : "", state.using_fallback ? " fallback" : "", rule->priority, rule->group ?: "-", rule->output, target);
        wstring_printf(&tui.menu->items[i * 2 + 1].wstr, L"Delete %s", rule->name);
    }
    free(states);
}

static bool routing_rule_key(wint_t ch) {
    if (!route_menu_active || !route_menu_count || (ch != 'h' && ch != 'l' && ch != KEY_LEFT && ch != KEY_RIGHT)) return false;
    struct route_rule *rule = &route_menu_rules[tui.menu->selected / 2];
    int priority = rule->priority + (ch == 'l' || ch == KEY_RIGHT ? 10 : -10);
    priority = MAX(-1000000, MIN(1000000, priority));
    char error[256] = {0};
    if (route_rule_set_priority(rule->name, priority, NULL, error, sizeof(error)) == 0) {
        rule->priority = priority; update_route_menu();
        snprintf(error, sizeof(error), "Rule %s priority set to %d", rule->name, priority);
    }
    show_notice(error); return true;
}

static char *node_port_pattern(uint32_t node) {
    const char *name = graph_node_name(node); char *pattern = xmalloc(strlen(name) * 2 + 3), *p = pattern;
    for (; *name; name++) { if (strchr("\\[]*?", *name)) *p++ = '\\'; *p++ = *name; }
    *p++ = ':'; *p++ = '*'; *p = 0; return pattern;
}

void tui_bind_save_routing_batch(union tui_bind_data data) {
    if (!tui.routing_active) { show_notice("Press r and select source/destination nodes before saving a batch rule"); return; }
    tui_bind_manage_routing_rules((union tui_bind_data){.index = 1});
}

static void on_route_rule_selected(struct tui_menu *menu, struct tui_menu_item *pick) {
    unsigned action = pick->data.uint;
    char name[49]; bool enabled = false;
    if (action != PW_ID_ANY) {
        snprintf(name, sizeof(name), "%s", route_menu_rules[action / 2].name);
        enabled = route_menu_rules[action / 2].enabled;
    }
    tui_bind_cancel_selection((union tui_bind_data){0});
    char error[256] = {0}; int result;
    if (action == PW_ID_ANY) { show_notice("Press r, select output/input ports, then a to save an automatic routing rule"); return; }
    if (action % 2) result = route_rule_delete(name, error, sizeof(error));
    else result = route_rule_enable(name, !enabled, error, sizeof(error));
    if (!result) snprintf(error, sizeof(error), "Rule %s %s; the running engine applies changes automatically", name,
                          action % 2 ? "deleted" : enabled ? "disabled" : "enabled");
    show_notice(error);
}

void tui_bind_manage_routing_rules(union tui_bind_data data) {
    if (tui.menu_active || scene_pending || audio_pending.active) return;
    char error[256] = {0}; struct route_rule *rules; unsigned count;
    int result = route_rules_load(&rules, &count, error, sizeof(error));
    if (result < 0) { if (tui.routing_active) routing_error(error); else show_notice(error); return; }
    if (tui.routing_active) {
        uint32_t output, input;
        if (!routing_selected(&output, &input)) { route_rules_free(rules, count); routing_error("Select an audio output and input port first"); return; }
        char name[49];
        for (unsigned n = 1; ; n++) {
            snprintf(name, sizeof(name), "route%u", n); bool used = false;
            for (unsigned i = 0; i < count; i++) if (streq(rules[i].name, name)) used = true;
            if (!used) break;
        }
        route_rules_free(rules, count);
        const struct graph_port *out = graph_port_find(output), *in = graph_port_find(input);
        char *source, *destination;
        bool batch = data.index == 1;
        if (batch) { source = node_port_pattern(out->node_id); destination = node_port_pattern(in->node_id); }
        else {
            xasprintf(&source, "%s:%s", graph_node_name(out->node_id), dict_get(&out->props, PW_KEY_PORT_NAME));
            xasprintf(&destination, "%s:%s", graph_node_name(in->node_id), dict_get(&in->props, PW_KEY_PORT_NAME));
        }
        result = route_rule_create_glob(name, source, destination, batch, error, sizeof(error)); free(source); free(destination);
        if (!result) snprintf(error, sizeof(error), "Saved rule %s; press r then a to manage automatic routing", name);
        routing_error(error); return;
    }
    route_menu_rules = rules; route_menu_count = count; route_menu_active = true;
    tui.menu = tui_menu_create(count ? count * 2 : 1); tui.menu->callback = on_route_rule_selected;
    update_route_menu();
    if (!count) { wstring_printf(&tui.menu->items[0].wstr, L"No rules. Press r, select ports, then a to save a rule"); tui.menu->items[0].data.uint = PW_ID_ANY; }
    else for (unsigned i = 0; i < count * 2; i++) tui.menu->items[i].data.uint = i;
    tui_menu_resize(tui.menu, tui.term_width, tui.term_height); tui.menu_active = true;
}

void tui_bind_manage_scenes(union tui_bind_data data) {
    if (tui.menu_active || tui.routing_active) return;
    if (scene_pending || audio_pending.active) { show_notice("Wait for the current audio operation to finish"); return; }
    int result = scene_list(&scene_names, &scene_count);
    if (result < 0) { show_notice(strerror(-result)); return; }
    char *startup = scene_startup_get();
    if (!startup) { scene_list_free(scene_names, scene_count); scene_names = NULL; scene_count = 0; show_notice("Cannot read startup scene selection"); return; }
    tui.menu = tui_menu_create(2 + scene_count * 3);
    tui.menu->callback = on_scene_selected;
    wstring_printf(&tui.menu->header, L"Scenes | startup: %s", startup);
    free(startup);
    wstring_printf(&tui.menu->items[0].wstr, L"Save current setup as a new scene");
    tui.menu->items[0].data.uint = 0;
    for (unsigned i = 0; i < scene_count; i++) {
        wstring_printf(&tui.menu->items[1 + i * 3].wstr, L"Load %s", scene_names[i]);
        tui.menu->items[1 + i * 3].data.uint = 1 + i * 3;
        wstring_printf(&tui.menu->items[2 + i * 3].wstr, L"Delete %s", scene_names[i]);
        tui.menu->items[2 + i * 3].data.uint = 2 + i * 3;
        wstring_printf(&tui.menu->items[3 + i * 3].wstr, L"Restore %s on startup", scene_names[i]);
        tui.menu->items[3 + i * 3].data.uint = 3 + i * 3;
    }
    wstring_printf(&tui.menu->items[1 + scene_count * 3].wstr, L"Disable startup scene restoration");
    tui.menu->items[1 + scene_count * 3].data.uint = PW_ID_ANY;
    tui_menu_resize(tui.menu, tui.term_width, tui.term_height); tui.menu_active = true;
}

static void set_effect_value(const struct graph_node *node, const struct graph_control *control, double value) {
    value = fmin(control->maximum, fmax(control->minimum, value));
    if (control->type == SPA_TYPE_Int || control->type == SPA_TYPE_Bool) value = round(value);
    const char *names[] = {control->name};
    int result = automation_take_control(node->id,control->name);
    if(result>=0)result=graph_set_controls(node->id,names,&value,1);
    DEBUG("effect %u parameter %s -> %.6g: %d", node->id, control->name, value, result);
    if (result < 0) show_notice(strerror(-result));
}

static void on_effect_reset(struct tui_menu *menu, struct tui_menu_item *pick) {
    const struct graph_node *node = graph_node_find(effect_menu_id);
    if (!node || pick->data.uint >= node->n_controls) return;
    const struct graph_control *control = &node->controls[pick->data.uint];
    set_effect_value(node, control, control->default_value);
}

static bool effect_editable(const struct graph_node *node, const struct graph_control *control) {
    return control->has_info && control->has_value && control->writable && control->visible
           && (streq(dict_get(&node->props, "pipemixer.preset"), "custom")
               || (!streq(control->name, "wet:Mult") && !streq(control->name, "dry:Mult")));
}

static void open_effect_parameters(uint32_t id) {
    const struct graph_node *node = graph_node_find(id);
    if (!node) { show_notice("Effect disappeared"); return; }
    if (!node->controls_ready) { show_notice("Effect parameters are loading; press e again"); return; }
    unsigned count = 0;
    for (unsigned i = 0; i < node->n_controls; i++) {
        const struct graph_control *c = &node->controls[i];
        if (effect_editable(node, c)) count++;
    }
    if (!count) { show_notice("This effect has no editable plugin parameters"); return; }
    tui.menu = tui_menu_create(count);
    tui.menu->callback = on_effect_reset;
    count = 0;
    for (unsigned i = 0; i < node->n_controls; i++) {
        const struct graph_control *c = &node->controls[i];
        if (effect_editable(node, c))
            tui.menu->items[count++].data.uint = i;
    }
    effect_menu_id = id;
    tui.menu_active = true;
    tui_menu_resize(tui.menu, tui.term_width, tui.term_height);
    refresh_effect_menu();
}

static bool effect_key(wint_t key) {
    if (!tui.menu_active || effect_menu_id == PW_ID_ANY) return false;
    const struct graph_node *node = graph_node_find(effect_menu_id);
    if (!node) return false;
    if (key == ' ') {
        const struct graph_control *wet = graph_control_find(node, "wet:Mult");
        const struct graph_control *dry = graph_control_find(node, "dry:Mult");
        if (!wet || !dry) { show_notice("This custom chain has no wet/dry bypass"); return true; }
        const char *names[] = {"wet:Mult", "dry:Mult"};
        const double values[] = {wet->value == 0 && dry->value == 1 ? 1 : 0,
                                 wet->value == 0 && dry->value == 1 ? 0 : 1};
        int result=automation_take_control(node->id,names[0]);
        if(result>=0)result=automation_take_control(node->id,names[1]);
        if(result>=0)result=graph_set_controls(node->id,names,values,2);
        if (result < 0) show_notice(strerror(-result));
        return true;
    }
    if (key != 'h' && key != 'l' && key != KEY_LEFT && key != KEY_RIGHT) return false;
    unsigned index = tui.menu->items[tui.menu->selected].data.uint;
    if (index >= node->n_controls) return true;
    const struct graph_control *control = &node->controls[index];
    double step = (control->maximum - control->minimum) / 100;
    const char *label = strrchr(control->name, ':') + 1;
    if (streq(label, "Freq")) step = fmax(1, fabs(control->value) * .1);
    else if (streq(label, "Q")) step = .1;
    else if (streq(label, "Gain")) step = .5;
    else if (streq(label, "Mult")) step = .05;
    if (control->type == SPA_TYPE_Int || control->type == SPA_TYPE_Bool) step = fmax(1, round(step));
    set_effect_value(node, control, control->value + (key == 'h' || key == KEY_LEFT ? -step : step));
    return true;
}

static void on_effect_selected(struct tui_menu *menu, struct tui_menu_item *pick) {
    uint32_t action = pick->data.uint;
    tui_bind_cancel_selection((union tui_bind_data){0});
    if (action >= PW_ID_ANY-2)
        start_audio("effect", 0, 0, action == PW_ID_ANY ? "eq" : action==PW_ID_ANY-1 ? "voice" : "empty");
    else if(!effect_ui_open(action)) open_effect_parameters(action);
}

void tui_bind_manage_effects(union tui_bind_data data) {
    if (tui.menu_active || tui.routing_active) return;
    const struct tui_tab_item *focused = tui.tabs[tui.tab_index].focused;
    const struct graph_node *node = focused && focused->type == TUI_TAB_ITEM_TYPE_NODE
                                  ? graph_node_find(focused->as.node.id) : NULL;
    if (node && streq(dict_get(&node->props, "pipemixer.kind"), "effect")) {
        node = managed_find("effect", dict_get(&node->props, "pipemixer.group"), "input");
        if (node && !effect_ui_open(node->id)) open_effect_parameters(node->id);
        return;
    }
    struct audio_candidates candidates = {0};
    graph_foreach_node(collect_managed, &candidates);
    if (candidates.count > 1) qsort(candidates.ids, candidates.count, sizeof(uint32_t), compare_audio_ids);
    unsigned count = 0;
    for (unsigned i = 0; i < candidates.count; i++)
        if (streq(dict_get(&graph_node_find(candidates.ids[i])->props, "pipemixer.kind"), "effect")) count++;
    tui.menu = tui_menu_create(count + 3);
    tui.menu->callback = on_effect_selected;
    wstring_printf(&tui.menu->header, L"Effect chains: create or edit");
    wstring_printf(&tui.menu->items[0].wstr, L"Create a three-band stereo equalizer");
    wstring_printf(&tui.menu->items[1].wstr, L"Create a voice chain (high-pass, noise gate, presence EQ)");
    tui.menu->items[0].data.uint = PW_ID_ANY;
    tui.menu->items[1].data.uint = PW_ID_ANY - 1;
    wstring_printf(&tui.menu->items[2].wstr,L"Create an empty chain");
    tui.menu->items[2].data.uint=PW_ID_ANY-2;
    count = 3;
    for (unsigned i = 0; i < candidates.count; i++) {
        node = graph_node_find(candidates.ids[i]);
        if (!streq(dict_get(&node->props, "pipemixer.kind"), "effect")) continue;
        wstring_printf(&tui.menu->items[count].wstr, L"Edit %s", dict_get(&node->props, "pipemixer.group"));
        tui.menu->items[count++].data.uint = node->id;
    }
    free(candidates.ids);
    tui_menu_resize(tui.menu, tui.term_width, tui.term_height);
    tui.menu_active = true;
}

static void on_profile_selection_done(struct tui_menu *menu, struct tui_menu_item *pick) {
    const uint32_t device_id = menu->data.uint;
    const uint32_t profile_id = pick->data.uint;

    TRACE("on_profile_selection_done: device_id %d route_id %d", device_id, profile_id);
    struct device *device = device_lookup(device_id);
    if (device == NULL) {
        WARN("on_profile_selection_done: device with id %d does not exist", device_id);
    }
    device_set_profile(device, profile_id);

    tui_menu_free(menu);
    tui.menu_active = false;

    redraw_current_tab();
}

void tui_bind_select_profile(union tui_bind_data data) {
    struct tui_tab_item *focused = tui.tabs[tui.tab_index].focused;

    if (focused == NULL || focused->type != TUI_TAB_ITEM_TYPE_DEVICE || tui.menu_active) {
        return;
    }

    struct tui_tab_item_device_data *d = &focused->as.device;

    if (d->n_profiles < 2) {
        return;
    }

    tui.menu = tui_menu_create(d->n_profiles);
    tui.menu->callback = on_profile_selection_done;
    tui.menu->data.uint = d->id;

    tui_menu_resize(tui.menu, tui.term_width, tui.term_height);

    wstring_printf(&tui.menu->header, L"Select profile for %ls", d->description.data);

    for (size_t i = 0; i < d->n_profiles; i++) {
        const struct profile_info *p = &d->profiles[i];
        struct tui_menu_item *item = &tui.menu->items[i];

        wstring_printf(&item->wstr, L"%d. %ls (%ls)",
                       p->index, p->description.data, p->name.data);
        item->data.uint = p->index;

        if (p == d->active_profile) {
            tui.menu->selected = i;
        }
    }

    tui.menu_active = true;
}

static void on_route_selection_done(struct tui_menu *menu, struct tui_menu_item *pick) {
    const uint32_t node_id = menu->data.uint;
    const uint32_t route_id = pick->data.uint;

    TRACE("on_route_selection_done: node_id %d route_id %d", node_id, route_id);
    struct node *node = node_lookup(node_id);
    if (node == NULL) {
        WARN("on_route_selection_done: node with id %d does not exist", node_id);
    }
    node_set_route(node, route_id);

    tui_menu_free(menu);
    tui.menu_active = false;

    redraw_current_tab();
}

void tui_bind_select_route(union tui_bind_data data) {
    struct tui_tab_item *const focused = tui.tabs[tui.tab_index].focused;

    if (focused == NULL || focused->type != TUI_TAB_ITEM_TYPE_NODE || tui.menu_active) {
        return;
    }

    struct tui_tab_item_node_data *d = &focused->as.node;

    if (d->n_routes < 2) {
        return;
    }

    tui.menu = tui_menu_create(d->n_routes);
    tui.menu->callback = on_route_selection_done;
    tui.menu->data.uint = d->id;

    tui_menu_resize(tui.menu, tui.term_width, tui.term_height);

    wstring_printf(&tui.menu->header, L"Select route for %ls", d->description.data);

    for (size_t i = 0; i < d->n_routes; i++) {
        const struct route_info *p = &d->routes[i];
        struct tui_menu_item *item = &tui.menu->items[i];

        wstring_printf(&item->wstr, L"%d. %ls (%ls)",
                       p->index, p->description.data, p->name.data);
        item->data.uint = p->index;

        if (p == d->active_route) {
            tui.menu->selected = i;
        }
    }

    tui.menu_active = true;
}

void tui_bind_cancel_selection(union tui_bind_data data) {
    if (!tui.menu_active) {
        return;
    }

    tui_menu_free(tui.menu);
    tui.menu = NULL;
    tui.menu_active = false;
    effect_menu_id = PW_ID_ANY;
    scene_list_free(scene_names, scene_count); scene_names = NULL; scene_count = 0;
    route_rules_free(route_menu_rules, route_menu_count); route_menu_rules = NULL; route_menu_count = 0; route_menu_active = false;
    monitor_ui_cancel();
    effect_ui_cancel();
    diagnostic_ui_cancel();
    recording_ui_cancel();
    automation_ui_cancel();

    redraw_current_tab();
}

void tui_bind_confirm_selection(union tui_bind_data data) {
    if (!tui.menu_active) {
        return;
    }

    struct tui_menu *menu = tui.menu;
    menu->callback(menu, &menu->items[menu->selected]);
}

void tui_bind_quit_or_cancel_selection(union tui_bind_data data) {
    if (tui.menu_active) {
        tui_bind_cancel_selection(data);
    } else if (tui.routing_active) {
        tui_bind_toggle_routing(data);
    } else {
        tui_bind_quit(data);
    }
}

void tui_bind_quit(union tui_bind_data data) {
    if(recording_ui_quit()||automation_ui_quit())return;
    if(effect_ui_quit())return;
    pw_main_loop_quit(main_loop);
}

void tui_bind_toggle_routing(union tui_bind_data data) {
    if (tui.menu_active) return;
    tui.routing_active = !tui.routing_active;
    set_tab_meters(tui.tab_index, !tui.routing_active);
    redraw_current_tab();
    touchwin(tui.pad_win);
    touchwin(tui.bar_win);
}

/* Change size (height) of item to (new_height),
 * while also adjusting positions of other items in the same tab as needed.
 * DOES NOT DRAW ANYTHING BY ITSELF */
static bool tui_tab_item_resize(struct tui_tab_item *item, int new_height) {
    const int diff = new_height - item->height;
    if (diff == 0) {
        return false;
    }

    const struct tui_tab *const tab = &tui.tabs[item->tab_index];

    struct tui_tab_item *last = CONTAINER_OF(tab->items.prev, struct tui_tab_item, link);
    tui.pad_win = tui_set_pad_size(tui.pad_win,
                                   AT_LEAST, last->pos + last->height + diff,
                                   AT_LEAST, tui.term_width);

    TRACE("tui_tab_item_resize: resizing item %p from %d to %d",
          (void *)item, item->height, item->height + diff);
    item->height += diff;

    // iterate starting from the first element that should be moved
    // TODO: make this a macro?
    for (struct list *elem = item->link.next; elem != &tab->items; elem = elem->next) {
        struct tui_tab_item *next = CONTAINER_OF(elem, struct tui_tab_item, link);
        TRACE("tui_tab_item_resize: shifting item %p from %d to %d",
              (void *)next, next->pos, next->pos + diff);
        next->pos += diff;
    }

    return true;
}

static void on_device_profiles(struct device *dev,
                               const struct param_profile *profiles, unsigned n_profiles,
                               void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_device_data *d = &item->as.device;

    for (unsigned i = 0; i < d->n_profiles; i++) {
        struct profile_info *oldp = &d->profiles[i];
        wstring_free(&oldp->name);
        wstring_free(&oldp->description);
    }

    d->n_profiles = n_profiles;
    d->profiles = xreallocarray(d->profiles, d->n_profiles, sizeof(d->profiles[0]));
    d->active_profile = NULL;

    for (unsigned i = 0; i < d->n_profiles; i++) {
        struct profile_info *pi = &d->profiles[i];
        const struct param_profile *pp = &profiles[i];

        wstring_init(&pi->name);
        wstring_init(&pi->description);

        pi->index = pp->index;

        wstring_printf(&pi->name, L"%s", pp->name);
        wstring_printf(&pi->description, L"%s", pp->description);

        if (pp->active) {
            d->active_profile = pi;
        }
    }

    tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_PROFILES);
    trigger_update();
}

static void on_device_props(struct device *dev, const struct dict *props, void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_device_data *d = &item->as.device;

    wstring_clear(&d->info);
    format_render(config.device_format, props, &d->info);

    wstring_clear(&d->description);
    wstring_printf(&d->description, L"%s", dict_get(props, "device.description"));

    tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_DESCRIPTION);
    trigger_update();
}

static void on_device_removed(struct device *dev, void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_device_data *d = &item->as.device;

    TRACE("on_device_removed: id %d", item->as.device.id);

    event_hook_release(item->hook);
    device_unref(&item->as.device.dev);

    tui_tab_item_resize(item, 0);
    tui_tab_item_unfocus(item, false);

    list_remove(&item->link);

    if (item->tab_index == tui.tab_index) {
        redraw_current_tab();
        trigger_update();
    }

    wstring_free(&d->description);
    wstring_free(&d->info);
    for (unsigned i = 0; i < d->n_profiles; i++) {
        wstring_free(&d->profiles[i].name);
        wstring_free(&d->profiles[i].description);
    }
    free(d->profiles);

    free(item);
}

static const struct device_events device_events = {
    .props = on_device_props,
    .profiles = on_device_profiles,
    .removed = on_device_removed,
};

static void on_node_default(struct node *node, bool is_default, void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_node_data *d = &item->as.node;

    d->is_default = is_default;

    tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_DESCRIPTION);
    trigger_update();
}

static void on_node_mute(struct node *node, bool muted, void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_node_data *d = &item->as.node;

    d->muted = muted;

    tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_CHANNELS | TUI_TAB_ITEM_DRAW_DECORATIONS
                            | TUI_TAB_ITEM_DRAW_PEAK);
    trigger_update();
}

static void on_node_routes(struct node *node,
                           const struct param_route routes[], unsigned routes_count,
                           void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_node_data *d = &item->as.node;

    for (unsigned i = 0; i < d->n_routes; i++) {
        struct route_info *oldp = &d->routes[i];
        wstring_free(&oldp->name);
        wstring_free(&oldp->description);
    }

    const unsigned old_n_routes = d->n_routes;

    d->n_routes = routes_count;
    d->routes = xreallocarray(d->routes, d->n_routes, sizeof(d->routes[0]));
    d->active_route = NULL;

    for (unsigned i = 0; i < d->n_routes; i++) {
        struct route_info *pi = &d->routes[i];
        const struct param_route *pp = &routes[i];

        wstring_init(&pi->name);
        wstring_init(&pi->description);

        pi->index = pp->index;

        wstring_printf(&pi->name, L"%s", pp->name);
        wstring_printf(&pi->description, L"%s", pp->description);

        if (pp->active) {
            d->active_route = pi;
        }
    }

    if ((old_n_routes && !d->n_routes) || (!old_n_routes && d->n_routes)) {
        tui_tab_item_resize(item, d->n_channels + 3 + (bool)d->n_routes);
        if (item->tab_index == tui.tab_index) {
            redraw_current_tab();
        }
    } else {
        tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_ROUTES);
    }

    trigger_update();
}

static void on_node_volume(struct node *node,
                           const float channel_volumes[], unsigned channel_count,
                           void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_node_data *d = &item->as.node;

    for (unsigned i = 0; i < channel_count; i++) {
        d->channels[i].volume = channel_volumes[i];
    }

    tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_CHANNELS);
    trigger_update();
}

static void on_node_channels(struct node *node,
                             const char *channel_names[], unsigned channel_count,
                             void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_node_data *d = &item->as.node;

    if (d->n_channels != channel_count) {
        stop_node_meter(item);
    }
    d->n_channels = channel_count;
    d->channels = xreallocarray(d->channels, d->n_channels, sizeof(d->channels[0]));
    if (d->n_channels == 0) {
        d->focused_channel = 0;
    } else if (d->focused_channel >= d->n_channels) {
        d->focused_channel = d->n_channels - 1;
    }

    for (unsigned i = 0; i < channel_count; i++) {
        d->channels[i].name = channel_names[i];
    }

    tui_tab_item_resize(item, d->n_channels + 3 + (bool)d->n_routes);
    start_node_meter(item);

    if (item->tab_index == tui.tab_index) {
        redraw_current_tab();
    }
    trigger_update();
}

static void on_node_props(struct node *node, const struct dict *props, void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_node_data *d = &item->as.node;

    wstring_clear(&d->info);
    format_render(config.node_format, props, &d->info);

    const char *node_description = dict_get(props, "node.description");
    const char *node_name = dict_get(props, "node.name");

    wstring_clear(&d->description);
    wstring_printf(&d->description, L"%s", node_description ?: node_name);

    start_node_meter(item);
    tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_DESCRIPTION);
    trigger_update();
}

static void on_node_removed(struct node *node, void *data) {
    struct tui_tab_item *item = data;
    struct tui_tab_item_node_data *d = &item->as.node;

    TRACE("tui_on_node_removed: id %d", d->id);

    stop_node_meter(item);
    event_hook_release(item->hook);
    node_unref(&item->as.node.node);

    tui_tab_item_resize(item, 0);
    tui_tab_item_unfocus(item, false);

    list_remove(&item->link);

    if (item->tab_index == tui.tab_index) {
        redraw_current_tab();
        trigger_update();
    }

    wstring_free(&d->description);
    wstring_free(&d->info);
    for (unsigned i = 0; i < d->n_routes; i++) {
        wstring_free(&d->routes[i].name);
        wstring_free(&d->routes[i].description);
    }
    free(d->routes);
    free(d->channels);

    free(item);
}

static const struct node_events node_events = {
    .removed = on_node_removed,
    .props = on_node_props,
    .channels = on_node_channels,
    .volume = on_node_volume,
    .routes = on_node_routes,
    .mute = on_node_mute,
    .default_ = on_node_default,
};

static void on_pipewire_device(struct device *dev, void *_) {
    TRACE("on_pipewire_device: id %d", device_id(dev));

    int tab_index = find_tab(CARDS);
    if (tab_index < 0) {
        return;
    }

    struct tui_tab_item *new_item = xmalloc(sizeof(*new_item));
    *new_item = (struct tui_tab_item){
        .tab_index = tab_index,
        .type = TUI_TAB_ITEM_TYPE_DEVICE,
        .as.device = {
            .id = device_id(dev),
            .dev = device_ref(dev),
        },
    };

    new_item->hook = device_add_listener(dev, &device_events, new_item);

    const int new_item_height = 4;
    list_insert_after(&tui.tabs[tab_index].items, &new_item->link);
    tui_tab_item_resize(new_item, new_item_height);

    if (tui.tabs[tab_index].focused == NULL || !tui.tabs[tab_index].user_changed_focus) {
        tui_tab_item_focus(new_item, false, false);
    }

    redraw_current_tab();
    trigger_update();
}

static void on_pipewire_node(struct node *node, void *_) {
    TRACE("tui_on_node_added: id %d", node_id(node));

    int tab_index = find_tab(media_class_to_tui_tab(node_media_class(node)));
    if (tab_index < 0) {
        return;
    }

    struct tui_tab_item *new_item = xmalloc(sizeof(*new_item));
    *new_item = (struct tui_tab_item){
        .tab_index = tab_index,
        .type = TUI_TAB_ITEM_TYPE_NODE,
        .as.node = {
            .id = node_id(node),
            .node = node_ref(node),
        }
    };

    new_item->hook = node_add_listener(node, &node_events, new_item);

    int new_item_height = new_item->as.node.n_channels + 3;
    list_insert_after(&tui.tabs[tab_index].items, &new_item->link);
    tui_tab_item_resize(new_item, new_item_height);

    if (tui.tabs[tab_index].focused == NULL || !tui.tabs[tab_index].user_changed_focus) {
        tui_tab_item_focus(new_item, false, false);
    }

    redraw_current_tab();
    trigger_update();
}

static void on_pipewire_graph(void *data) {
    if (route_menu_active) update_route_menu();
    if (tui.routing_active || effect_menu_id != PW_ID_ANY || effect_ui_active() || route_menu_active) trigger_update();
}

static void on_pipewire_error(int code, const char *message, void *data) {
    if (tui.routing_active) { routing_error(message); trigger_update(); }
    else if (effect_menu_id != PW_ID_ANY || effect_ui_active()) { show_notice(message); trigger_update(); }
}

static const struct pipewire_events pipewire_events = {
    .node = on_pipewire_node,
    .device = on_pipewire_device,
    .graph = on_pipewire_graph,
    .error = on_pipewire_error,
};

static void mouse_scroll_current_tab(int direction) {
    struct tui_tab *tab = &tui.tabs[tui.tab_index];
    int bottom = 0;
    LIST_FOREACH(elem, &tab->items) {
        const struct tui_tab_item *item = CONTAINER_OF(elem, struct tui_tab_item, link);
        bottom = MAX(bottom, item->pos + item->height);
    }
    const int visible = MAX(0, tui.term_height - 1);
    const int max_scroll = MAX(0, bottom - visible);
    tab->scroll_pos = MAX(0, MIN(max_scroll, tab->scroll_pos + direction * 3));
}

static void mouse_select_tab(int x) {
    int start = 0;
    FOR_EACH_TAB(i) {
        const int end = start + (int)strlen(tui_tab_name(tui.tabs[i].type)) + 3;
        if (x >= start && x < end) {
            tui_bind_set_tab_index((union tui_bind_data){ .index = i });
            return;
        }
        start = end;
    }
}

static struct tui_tab_item *mouse_item_at(int y) {
    if (y < 1 || y >= tui.term_height) return NULL;
    const struct tui_tab *tab = &tui.tabs[tui.tab_index];
    const int pad_y = tab->scroll_pos + y - 1;
    LIST_FOREACH(elem, &tab->items) {
        struct tui_tab_item *item = CONTAINER_OF(elem, struct tui_tab_item, link);
        if (pad_y >= item->pos && pad_y < item->pos + item->height) return item;
    }
    return NULL;
}

static void mouse_node_click(struct tui_tab_item *item, int x, int line, mmask_t button) {
    struct tui_tab_item_node_data *d = &item->as.node;
    const enum media_class media_class = node_media_class(d->node);
    tui_tab_item_focus(item, true, true);

    if (button & BUTTON2_PRESSED) {
        node_set_mute(d->node, !d->muted);
        return;
    }
    if (button & BUTTON3_PRESSED) {
        if (media_class == STREAM_OUTPUT_AUDIO || media_class == STREAM_INPUT_AUDIO) {
            tui_bind_select_target((union tui_bind_data){0});
        } else if (media_class == AUDIO_SINK || media_class == AUDIO_SOURCE) {
            node_set_default(d->node);
        }
        return;
    }
    if (!(button & BUTTON1_PRESSED)) return;

    if (line >= 2 && line < 2 + (int)d->n_channels) {
        const unsigned channel = (unsigned)(line - 2);
        if (d->unlocked_channels && d->focused_channel != channel) {
            d->focused_channel = channel;
            tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_DECORATIONS);
        }
        const struct node_layout layout = get_node_layout();
        if (layout.volume_bar_width > 0 && x >= layout.volume_bar_start
            && x < layout.volume_bar_start + layout.volume_bar_width) {
            const float fraction = layout.volume_bar_width == 1 ? 1.0f
                : (float)(x - layout.volume_bar_start) / (layout.volume_bar_width - 1);
            const float volume = fmaxf(config.volume_min,
                                       fminf(config.volume_max, 1.5f * fraction));
            int result=automation_take_control(node_id(d->node),NULL);
            if(result<0)show_notice(strerror(-result));
            else node_change_volume(d->node,true,volume,d->unlocked_channels?channel:ALL_CHANNELS);
        } else if (x >= layout.volume_area_start + 6
                   && x < layout.volume_area_start + 9) {
            node_set_mute(d->node, !d->muted);
        }
        return;
    }

    if (line == 1 && (media_class == STREAM_OUTPUT_AUDIO
                      || media_class == STREAM_INPUT_AUDIO)) {
        tui_bind_select_target((union tui_bind_data){0});
    } else if (line == item->height - 2 && d->n_routes >= 2) {
        tui_bind_select_route((union tui_bind_data){0});
    }
}

static void mouse_device_click(struct tui_tab_item *item, int line, mmask_t button) {
    tui_tab_item_focus(item, true, true);
    if ((button & BUTTON3_PRESSED)
        || ((button & BUTTON1_PRESSED) && line == item->height - 2)) {
        tui_bind_select_profile((union tui_bind_data){0});
    }
}

static void on_mouse_event(const MEVENT *event) {
    if (event->x < 0 || event->x >= tui.term_width
        || event->y < 0 || event->y >= tui.term_height) return;

    if (tui.routing_active && !tui.menu_active && event->y > 0) {
        routing_mouse(event);
        return;
    }

    if (event->bstate & (BUTTON4_PRESSED | BUTTON5_PRESSED)) {
        const int direction = event->bstate & BUTTON4_PRESSED ? -1 : 1;
        if (tui.menu_active) {
            tui_menu_change_focus(tui.menu, direction);
        } else {
            mouse_scroll_current_tab(direction);
        }
        return;
    }

    if (tui.menu_active) {
        if (event->bstate & BUTTON3_PRESSED) {
            tui_bind_cancel_selection((union tui_bind_data){0});
            return;
        }
        if (!(event->bstate & BUTTON1_PRESSED)) return;
        const struct tui_menu *menu = tui.menu;
        const int row = event->y - menu->y - 1;
        const int visible = MAX(0, menu->h - 2);
        if (event->x > menu->x && event->x < menu->x + menu->w - 1
            && row >= 0 && row < visible) {
            const unsigned index = tui_menu_first_visible(menu) + (unsigned)row;
            if (index < menu->n_items) {
                tui.menu->selected = index;
                tui_bind_confirm_selection((union tui_bind_data){0});
            }
        } else {
            tui_bind_cancel_selection((union tui_bind_data){0});
            if (event->y == 0) mouse_select_tab(event->x);
        }
        return;
    }

    if (event->y == 0) {
        if (event->bstate & BUTTON1_PRESSED) mouse_select_tab(event->x);
        return;
    }
    if (!(event->bstate & (BUTTON1_PRESSED | BUTTON2_PRESSED | BUTTON3_PRESSED))) return;
    struct tui_tab_item *item = mouse_item_at(event->y);
    if (!item) return;
    const int line = tui.tabs[tui.tab_index].scroll_pos + event->y - 1 - item->pos;
    if (item->type == TUI_TAB_ITEM_TYPE_NODE) {
        mouse_node_click(item, event->x, line, event->bstate);
    } else {
        mouse_device_click(item, line, event->bstate);
    }
}

static void dispatch_mouse_events(void) {
    MEVENT event;
    if (getmouse(&event) != OK) return;

    /* ncurses returns batched events newest first. Drain its bounded queue
     * before handling them so a buffered release cannot hide an earlier press. */
    dispatch_mouse_events();
    on_mouse_event(&event);
}

static void on_stdin_ready(void *_, int _, uint32_t _) {
    wint_t ch;
    int status;
    while (errno = 0, (status = wget_wch(stdscr, &ch)) != ERR || errno == EINTR) {
        if (status == ERR) continue;
        if (ch == KEY_MOUSE) {
            if (!scene_pending) dispatch_mouse_events();
            trigger_update();
            continue;
        }
        if (ch == KEY_RESIZE) {
            WARN("KEY_RESIZE %s (%d)", key_name_from_key_code(ch), ch);
        }

        if (tui.routing_active && !tui.menu_active && routing_key(ch, status == KEY_CODE_YES)) {
            trigger_update();
            continue;
        }
        if (automation_ui_key(ch) || recording_ui_key(ch) || effect_ui_key(ch) || routing_rule_key(ch) || effect_key(ch)) {
            trigger_update();
            continue;
        }

        struct tui_bind *bind = map_get(&config.binds, ch);
        if (scene_pending && bind && bind->func != tui_bind_quit
            && bind->func != tui_bind_quit_or_cancel_selection && bind->func != tui_bind_change_focus
            && bind->func != tui_bind_set_tab && bind->func != tui_bind_set_tab_index
            && bind->func != tui_bind_change_tab) continue;
        if (tui.routing_active && !tui.menu_active && bind
            && bind->func != tui_bind_toggle_routing && bind->func != tui_bind_quit
            && bind->func != tui_bind_diagnostics
            && bind->func != tui_bind_manage_automation
            && bind->func != tui_bind_manage_recording
            && bind->func != tui_bind_manage_routing_rules
            && bind->func != tui_bind_save_routing_batch
            && bind->func != tui_bind_manage_monitors && bind->func != tui_bind_monitor_solo
            && bind->func != tui_bind_monitor_listen && bind->func != tui_bind_monitor_clear_solo
            && bind->func != tui_bind_quit_or_cancel_selection
            && bind->func != tui_bind_set_tab && bind->func != tui_bind_set_tab_index
            && bind->func != tui_bind_change_tab) continue;
        if (!bind) {
            DEBUG("unhandled key %s (%d)", key_name_from_key_code(ch), ch);
        } else {
            bind->func(bind->data);
            trigger_update();
        }
    }
}

static void on_resize_triggered(void *_, uint64_t _) {
    tui.resize_triggered = false;

    struct winsize winsize;
    if (ioctl(0 /* stdin */, TIOCGWINSZ, &winsize) < 0) {
        ERROR("failed to get new window size: %s", strerror(errno));
        return;
    }

    resize_term(winsize.ws_row, winsize.ws_col);
    tui.term_height = getmaxy(stdscr);
    tui.term_width = getmaxx(stdscr);
    DEBUG("new window dimensions %d lines %d columns", tui.term_height, tui.term_width);

    tui.pad_win = tui_set_pad_size(tui.pad_win,
                                   AT_LEAST, tui.term_height,
                                   EXACTLY, tui.term_width);
    if (tui.tabs[tui.tab_index].focused != NULL) {
        tui_tab_item_ensure_visible(tui.tabs[tui.tab_index].focused);
    }

    if (tui.bar_win != NULL) {
        delwin(tui.bar_win);
    }
    tui.bar_win = newwin(1, tui.term_width, 0, 0);

    redraw_current_tab();
    redraw_status_bar();

    if (tui.menu_active) {
        tui_menu_resize(tui.menu, tui.term_width, tui.term_height);
    }
}

static void on_peak_timer(void *_, uint64_t _) {
    bool changed = false;
    if (diagnostic_ui_poll()) changed = true;
    if (recording_ui_poll()) changed = true;
    if (automation_ui_poll()) changed = true;
    if(effect_ui_poll())changed=true;
    if (monitor_ui_poll()) changed = true;
    if (tui.routing_active && routing_poll()) changed = true;
    if (route_menu_active && route_menu_tick != time(NULL)) {
        route_menu_tick = time(NULL); update_route_menu(); changed = true;
    }
    managed_reap();
    if (scene_pending) {
        char error[256] = {0};
        int result = scene_step(scene_pending, error, sizeof(error));
        if (result == -EAGAIN && time(NULL) >= scene_deadline) {
            result = -ETIMEDOUT; snprintf(error, sizeof(error), "Scene loading timed out");
        }
        if (result != -EAGAIN) {
            scene_job_free(scene_pending, result < 0); scene_pending = NULL;
            show_notice(result < 0 ? (error[0] ? error : strerror(-result)) : "Scene loaded"); changed = true;
        }
    }
    if (audio_pending.active) {
        if (managed_child_status(audio_pending.pid) != -EAGAIN) {
            show_notice("Audio worker stopped; inspect its log in XDG_RUNTIME_DIR");
            audio_pending.active = false; changed = true;
        } else if (managed_child_ready(audio_pending.pid) && managed_ready(audio_pending.kind, audio_pending.name)) {
            char message[256];
            if (streq(audio_pending.kind, "effect"))
                snprintf(message, sizeof(message), "Created effect %s; press e to edit, r to route audio", audio_pending.name);
            else snprintf(message, sizeof(message), "Created %s %s; adjust its output volume to control this path", audio_pending.kind, audio_pending.name);
            show_notice(message); audio_pending.active = false; changed = true;
        } else if (time(NULL) >= audio_pending.deadline) {
            managed_cancel(audio_pending.pid);
            show_notice("Audio path creation timed out");
            audio_pending.active = false; changed = true;
        }
    }
    if (notice[0] && time(NULL) >= notice_until) {
        notice[0] = '\0'; touchwin(tui.pad_win); changed = true;
    }
    const struct tui_tab *tab = &tui.tabs[tui.tab_index];

    LIST_FOREACH(elem, &tab->items) {
        struct tui_tab_item *item = CONTAINER_OF(elem, struct tui_tab_item, link);
        if (item->type == TUI_TAB_ITEM_TYPE_NODE && item->as.node.meter
            && peak_meter_step(item->as.node.meter)) {
            tui_tab_item_draw(item, TUI_TAB_ITEM_DRAW_PEAK);
            changed = true;
        }
    }

    if (changed) {
        trigger_update();
    }
}

/* Coalesce pad changes and let ncurses handle terminal damage tracking. */
static void on_update_triggered(void *_, uint64_t _) {
    tui.update_triggered = false;
    refresh_effect_menu();

    pnoutrefresh(tui.pad_win,
                 tui.tabs[tui.tab_index].scroll_pos, 0,
                 1, 0,
                 tui.term_height - 1, tui.term_width - 1);

    wnoutrefresh(tui.bar_win);

    if (tui.routing_active) routing_draw(tui.term_height, tui.term_width);

    if (tui.menu_active) {
        tui_menu_draw(tui.menu);
    }

    if (notice[0]) {
        if (!notice_win) notice_win = newwin(1, tui.term_width, tui.term_height - 1, 0);
        else { wresize(notice_win, 1, tui.term_width); mvwin(notice_win, tui.term_height - 1, 0); }
        if (notice_win) {
            werase(notice_win);
            waddnstr(notice_win, notice, MAX(0, tui.term_width - 1));
            wnoutrefresh(notice_win);
        }
    }

    doupdate();
}

static void on_sigwinch(int _) {
    trigger_resize();
    trigger_update();
}

bool tui_init(void) {
    /* must set signal handler BEFORE ncurses init */
    sigaction(SIGWINCH, &(struct sigaction){
        .sa_handler = on_sigwinch,
        .sa_flags = SA_RESTART,
    }, NULL);

    initscr();
    refresh(); /* https://stackoverflow.com/a/22121866 */
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE); /* getch() will fail instead of blocking waiting for input */
    keypad(stdscr, TRUE);
    ESCDELAY = 50 /* ms */;
    if (config.mouse) {
        mouseinterval(0);
        /* Accept releases as well: ncurses can block waiting for another
         * event after filtering a release, even with nodelay enabled. */
        const mmask_t enabled = mousemask(BUTTON1_PRESSED | BUTTON1_RELEASED
                                          | BUTTON2_PRESSED | BUTTON2_RELEASED
                                          | BUTTON3_PRESSED | BUTTON3_RELEASED
                                          | BUTTON4_PRESSED | BUTTON4_RELEASED
                                          | BUTTON5_PRESSED | BUTTON5_RELEASED, NULL);
        if (enabled && uses_xterm_mouse_protocol()) {
            /* Some terminals select text under mode 1000, pausing live output.
             * Mode 1002 keeps press/release reporting and prevents selection. */
            fputs("\033[?1002h", stdout);
            fflush(stdout);
            button_motion_tracking = true;
        }
    }

    start_color();
    use_default_colors();
    init_pair(GREEN, COLOR_GREEN, -1);
    init_pair(YELLOW, COLOR_YELLOW, -1);
    init_pair(RED, COLOR_RED, -1);

    tui.tabs_count = config.tabs_count;
    tui.tabs = xcalloc(config.tabs_count, sizeof(tui.tabs[0]));
    FOR_EACH_TAB(i) {
        struct tui_tab *tab = &tui.tabs[i];

        tab->type = config.tabs[i];
        list_init(&tab->items);

        if (tab->type == config.default_tab) {
            tui.tab_index = i;
        }
    }

    tui.stdin_source = pw_loop_add_io(event_loop, 0, POLLIN, false, on_stdin_ready, event_loop);

    tui.resize_source = pw_loop_add_event(event_loop, on_resize_triggered, event_loop);
    tui.resize_triggered = false;

    tui.update_source = pw_loop_add_event(event_loop, on_update_triggered, event_loop);
    tui.update_triggered = false;

    tui.peak_source = pw_loop_add_timer(event_loop, on_peak_timer, NULL);
    if (tui.peak_source) {
        struct timespec interval = { .tv_sec = 0, .tv_nsec = 50000000 };
        if (pw_loop_update_timer(event_loop, tui.peak_source,
                                 &interval, &interval, false) < 0) {
            WARN("failed to start peak meter timer");
            pw_loop_destroy_source(event_loop, tui.peak_source);
            tui.peak_source = NULL;
        }
    } else {
        WARN("failed to create peak meter timer");
    }

    tui.pipewire_hook = pipewire_add_listener(&pipewire_events, &tui);

    /* pick up initial terminal size */
    on_resize_triggered(NULL, 0);

    return true;
}

void tui_cleanup(void) {
    diagnostic_ui_cancel();
    recording_ui_cleanup();
    automation_ui_cleanup();
    effect_ui_cleanup();
    monitor_ui_cleanup();
    scene_job_free(scene_pending, true); scene_pending = NULL;
    scene_list_free(scene_names, scene_count); scene_names = NULL; scene_count = 0;
    route_rules_free(route_menu_rules, route_menu_count); route_menu_rules = NULL; route_menu_count = 0; route_menu_active = false;
    if (tui.menu_active) { tui_menu_free(tui.menu); tui.menu_active = false; tui.menu = NULL; }
    if (audio_pending.active) managed_cancel(audio_pending.pid);
    managed_cleanup();
    if (notice_win) { delwin(notice_win); notice_win = NULL; }
    routing_cleanup();
    if (!tui.tabs) {
        return;
    }

    FOR_EACH_TAB(i) {
        set_tab_meters(i, false);
    }
    if (tui.peak_source) {
        pw_loop_destroy_source(event_loop, tui.peak_source);
        tui.peak_source = NULL;
    }
    event_hook_release(tui.pipewire_hook);
    tui.pipewire_hook = NULL;

    if (tui.bar_win != NULL) {
        delwin(tui.bar_win);
    }
    if (tui.pad_win != NULL) {
        delwin(tui.pad_win);
    }

    endwin();
    tui_disable_button_motion_tracking();
}
