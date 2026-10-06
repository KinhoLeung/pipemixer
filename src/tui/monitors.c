#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tui/monitors.h"
#include "tui/tui.h"
#include "tui/menu.h"
#include "tui/routing.h"
#include "monitor.h"
#include "pw/managed.h"
#include "utils.h"
#include "xmalloc.h"

enum monitor_view { CLOSED, LIST, DETAIL, DESTINATION, LISTEN, MIX, SOLO };
static enum monitor_view view;
static char selected[33];
static char *create_source;
static struct monitor *configs;
static unsigned n_configs;
static char **nodes;
static unsigned n_nodes;
static time_t last_poll;
static char last_label[128];

void monitor_ui_cancel(void) {
    monitors_free(configs, n_configs); configs = NULL; n_configs = 0;
    for (unsigned i = 0; i < n_nodes; i++) free(nodes[i]);
    free(nodes); nodes = NULL; n_nodes = 0; free(create_source); create_source = NULL;
    view = CLOSED;
}
void monitor_ui_cleanup(void) { monitor_ui_cancel(); *selected = 0; }
static struct monitor *current(void) {
    for (unsigned i = 0; i < n_configs; i++) if (streq(selected, configs[i].name)) return &configs[i];
    return NULL;
}
static bool load(void) {
    monitors_free(configs, n_configs); configs = NULL; n_configs = 0; char error[256] = {0};
    if (monitors_load(&configs, &n_configs, error, sizeof(error)) < 0) { tui_notice(error); return false; }
    if (!*selected && n_configs == 1) snprintf(selected, sizeof(selected), "%s", configs[0].name);
    if (*selected && !current()) *selected = 0;
    return true;
}
static uint32_t source_node(void) {
    if (tui.routing_active) return routing_selected_node(false);
    struct tui_tab_item *item = tui.tabs[tui.tab_index].focused;
    return item && item->type == TUI_TAB_ITEM_TYPE_NODE && graph_node_has_ports(item->as.node.id, PW_DIRECTION_OUTPUT)
        ? item->as.node.id : PW_ID_ANY;
}
static const char *source_name(void) {
    const char *name = graph_node_name(source_node()); return monitor_valid_node(name) ? name : NULL;
}
static bool edit(enum monitor_action action, const char *value, const char *const *sources, unsigned count) {
    char error[256] = {0};
    if (monitor_edit(selected, action, value, sources, count, error, sizeof(error)) < 0) { tui_notice(error); return false; }
    if (!route_rules_running()) tui_notice("Monitor configuration saved; start the audio service to apply it");
    else if (action == MONITOR_LISTEN && current() && current()->n_solo) tui_notice("Listening source saved; clear Solo to hear it");
    else tui_notice("Monitor updated");
    return true;
}
static void close_menu(void) { tui_bind_cancel_selection((union tui_bind_data){0}); }
static void open_detail(void);
static void open_nodes(enum monitor_view next);
static void update_menu(void);

static bool contains(char *const *list, unsigned count, const char *value) {
    for (unsigned i = 0; i < count; i++) if (streq(list[i], value)) return true;
    return false;
}
static void choose_node(struct tui_menu *menu, struct tui_menu_item *pick) {
    if (pick->data.uint >= n_nodes) return;
    char *node = xstrdup(nodes[pick->data.uint]); enum monitor_view action = view;
    if (action == DESTINATION) {
        char *source = xstrdup(create_source); close_menu();
        if (!load()) { free(source); free(node); return; }
        char name[33];
        for (unsigned n = 1; ; n++) {
            snprintf(name, sizeof(name), "monitor%u", n); bool used = managed_find("monitor", name, NULL);
            for (unsigned i = 0; i < n_configs; i++) if (streq(configs[i].name, name)) used = true;
            if (!used) break;
        }
        snprintf(selected, sizeof(selected), "%s", name);
        const char *sources[] = {source};
        if (edit(MONITOR_CREATE, node, sources, source ? 1 : 0)) open_detail();
        free(source);
    } else {
        if (!load() || !current()) { free(node); return; }
        struct monitor *m = current();
        if (action == LISTEN) {
            edit(MONITOR_LISTEN, node, NULL, 0); close_menu(); open_detail();
        } else if (action == SOLO) { edit(MONITOR_SOLO_TOGGLE, node, NULL, 0); load(); update_menu(); }
        else if (action == MIX) {
            const char *sources[MONITOR_SOURCE_LIMIT + 1]; unsigned count = 0;
            bool present = contains(m->sources, m->n_sources, node);
            for (unsigned i = 0; i < m->n_sources; i++) if (!streq(m->sources[i], node)) sources[count++] = m->sources[i];
            if (!present) sources[count++] = node;
            edit(MONITOR_SOURCES, NULL, sources, count); load(); update_menu();
        }
    }
    free(node);
}
static void choose_detail(struct tui_menu *menu, struct tui_menu_item *pick) {
    unsigned action = pick->data.uint;
    if (action == 1 || action == 2 || action == 3) {
        close_menu(); open_nodes(action == 1 ? LISTEN : action == 2 ? MIX : SOLO); return;
    }
    if (!load() || !current()) return;
    if (action == 0) edit(MONITOR_LISTEN, "mix", NULL, 0);
    else if (action == 4) edit(MONITOR_SOLO_CLEAR, NULL, NULL, 0);
    else if (action == 5) edit(MONITOR_ENABLE, current()->enabled ? "off" : "on", NULL, 0);
    else if (action == 6) {
        if (edit(MONITOR_DELETE, NULL, NULL, 0)) { *selected = 0; close_menu(); }
        return;
    }
    load(); update_menu();
}
static void choose_monitor(struct tui_menu *menu, struct tui_menu_item *pick) {
    unsigned action = pick->data.uint;
    if (action == PW_ID_ANY) { close_menu(); open_nodes(DESTINATION); return; }
    if (action >= n_configs) return;
    snprintf(selected, sizeof(selected), "%s", configs[action].name); close_menu(); open_detail();
}
static void open_detail(void) {
    if (!load() || !current()) return;
    view = DETAIL; tui.menu = tui_menu_create(7); tui.menu->callback = choose_detail;
    for (unsigned i = 0; i < 7; i++) tui.menu->items[i].data.uint = i;
    update_menu(); tui_menu_resize(tui.menu, tui.term_width, tui.term_height); tui.menu_active = true;
}
static int compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void add_node(const char *name) {
    if (!monitor_valid_node(name) || contains(nodes, n_nodes, name)) return;
    nodes = xreallocarray(nodes, n_nodes + 1, sizeof(*nodes)); nodes[n_nodes++] = xstrdup(name);
}
static void collect_node(const struct graph_node *node, void *data) {
    bool destination = *(bool *)data;
    if (!graph_node_has_ports(node->id, destination ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT)) return;
    const char *name = graph_node_name(node->id);
    if (destination && streq(name, create_source)) return;
    add_node(name);
}
static void open_nodes(enum monitor_view next) {
    if (!load() || (next != DESTINATION && !current())) return;
    if (next == DESTINATION) create_source = xstrdup(source_name());
    bool destination = next == DESTINATION; graph_foreach_node(collect_node, &destination);
    struct monitor *m = current();
    if (next == MIX || next == SOLO) {
        char **list = next == MIX ? m->sources : m->solo; unsigned count = next == MIX ? m->n_sources : m->n_solo;
        for (unsigned i = 0; i < count; i++) add_node(list[i]);
    }
    if (n_nodes > 1) qsort(nodes, n_nodes, sizeof(*nodes), compare_names);
    if (!n_nodes) { monitor_ui_cancel(); tui_notice("No matching audio nodes available"); return; }
    view = next; tui.menu = tui_menu_create(n_nodes); tui.menu->callback = choose_node;
    for (unsigned i = 0; i < n_nodes; i++) tui.menu->items[i].data.uint = i;
    if (destination && tui.routing_active) {
        const char *target = graph_node_name(routing_selected_node(true));
        for (unsigned i = 0; i < n_nodes; i++) if (streq(target, nodes[i])) tui.menu->selected = i;
    }
    update_menu(); tui_menu_resize(tui.menu, tui.term_width, tui.term_height); tui.menu_active = true;
}
static void update_menu(void) {
    if (!tui.menu || view == CLOSED) return;
    wstring_clear(&tui.menu->header);
    for (unsigned i=0;i<tui.menu->n_items;i++) wstring_clear(&tui.menu->items[i].wstr);
    struct monitor *m = current();
    if (view == LIST) {
        wstring_printf(&tui.menu->header, L"Independent monitors | engine: %s", route_rules_running() ? "running" : "stopped");
        for (unsigned i = 0; i < n_configs && i < tui.menu->n_items - 1; i++) {
            unsigned connected, total; const char *state = monitor_state(&configs[i], &connected, &total);
            wstring_printf(&tui.menu->items[i].wstr, L"%s%s [%s %s %u/%u] -> %s", streq(configs[i].name, selected) ? "* " : "", configs[i].name,
                state, monitor_mode(&configs[i]), connected, total, configs[i].destination);
        }
        wstring_printf(&tui.menu->items[tui.menu->n_items - 1].wstr, L"Create monitor... (choose output device)");
        return;
    }
    if (view == DESTINATION) {
        wstring_printf(&tui.menu->header, L"Monitor output | initial source: %s", create_source ?: "empty mix");
    } else if (!m) {
        wstring_printf(&tui.menu->header, L"Monitor removed; Esc to close"); return;
    } else if (view == DETAIL) {
        unsigned connected, total; const char *state = monitor_state(m, &connected, &total);
        wstring_printf(&tui.menu->header, L"Monitor %s | %s %s %u/%u | engine: %s", selected, state, monitor_mode(m), connected, total, route_rules_running() ? "running" : "stopped");
        wstring_printf(&tui.menu->items[0].wstr, L"Listen to normal mix (%u sources)%s", m->n_sources, !m->listen ? " [selected]" : "");
        wstring_printf(&tui.menu->items[1].wstr, L"Select listening source... [%s]", m->listen ?: "normal mix");
        wstring_printf(&tui.menu->items[2].wstr, L"Edit normal mix sources... (%u)", m->n_sources);
        wstring_printf(&tui.menu->items[3].wstr, L"Edit Solo sources... (%u)", m->n_solo);
        wstring_printf(&tui.menu->items[4].wstr, L"Clear all Solo; return to %s", m->listen ?: "normal mix");
        wstring_printf(&tui.menu->items[5].wstr, L"%s monitor", m->enabled ? "Disable" : "Enable");
        wstring_printf(&tui.menu->items[6].wstr, L"Delete monitor %s", selected); return;
    } else wstring_printf(&tui.menu->header, L"%s %s | Enter %s; Esc closes", selected,
        view == MIX ? "normal mix sources" : view == SOLO ? "Solo sources" : "listening source", view == LISTEN ? "selects" : "toggles");
    for (unsigned i = 0; i < n_nodes && i < tui.menu->n_items; i++) {
        bool on = m && (view == MIX ? contains(m->sources, m->n_sources, nodes[i]) : view == SOLO ? contains(m->solo, m->n_solo, nodes[i]) : streq(m->listen, nodes[i]));
        uint32_t id; bool missing = graph_resolve_node(nodes[i], &id) < 0;
        wstring_printf(&tui.menu->items[i].wstr, L"[%s] %s%s", on ? "x" : " ", nodes[i], missing ? " (offline)" : "");
    }
}
void tui_bind_manage_monitors(union tui_bind_data data) {
    if (tui.menu_active || !load()) return;
    view = LIST; tui.menu = tui_menu_create(n_configs + 1); tui.menu->callback = choose_monitor;
    for (unsigned i = 0; i < n_configs; i++) tui.menu->items[i].data.uint = i;
    tui.menu->items[n_configs].data.uint = PW_ID_ANY;
    update_menu(); tui_menu_resize(tui.menu, tui.term_width, tui.term_height); tui.menu_active = true;
}
static bool selected_monitor(void) {
    if (tui.menu_active || !load()) return false;
    if (!current()) { tui_notice("Press M to create or select a monitor first"); return false; }
    return true;
}
void tui_bind_monitor_solo(union tui_bind_data data) {
    if (!selected_monitor()) return;
    const char *source = source_name();
    if (!source) { tui_notice("Select a source node or output port before toggling Solo"); return; }
    edit(MONITOR_SOLO_TOGGLE, source, NULL, 0);
}
void tui_bind_monitor_listen(union tui_bind_data data) {
    if (!selected_monitor()) return;
    const char *source = source_name();
    if (source) edit(MONITOR_LISTEN, source, NULL, 0);
    else open_nodes(LISTEN);
}
void tui_bind_monitor_clear_solo(union tui_bind_data data) {
    if (selected_monitor()) edit(MONITOR_SOLO_CLEAR, NULL, NULL, 0);
}
bool monitor_ui_poll(void) {
    time_t now = time(NULL); if (now == last_poll) return false;
    last_poll = now;
    if (!load()) return view != CLOSED;
    if (view == LIST && tui.menu && tui.menu->n_items != n_configs + 1) {
        close_menu(); tui_bind_manage_monitors((union tui_bind_data){0});
    } else if (view != CLOSED) update_menu();
    struct monitor *m = current(); char label[128] = {0};
    if (m) {
        unsigned connected, total; const char *state = monitor_state(m, &connected, &total);
        snprintf(label, sizeof(label), "Monitor:%s %s [%u Solo] %s", m->name, monitor_mode(m), m->n_solo, state);
    }
    bool changed = !streq(last_label, label); snprintf(last_label, sizeof(last_label), "%s", label);
    routing_monitor_status(label); return view != CLOSED || (tui.routing_active && changed);
}
