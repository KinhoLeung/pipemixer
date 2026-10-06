#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tui/routing-view.h"
#include "xmalloc.h"
#include "utils.h"
#include "macros.h"

struct routing_fold { char *key; struct routing_fold *next; };
struct raw_port { uint32_t id; const char *node, *name; enum routing_kind kind; unsigned sort; };
struct collect {
    const struct routing_filter *filter;
    enum pw_direction direction;
    struct raw_port *ports;
    unsigned count;
};

const char *routing_kind_name(enum routing_kind kind) {
    static const char *names[] = {"All", "Devices", "Buses", "Sends", "Effects", "Applications", "Other"};
    return kind < ROUTING_KIND_COUNT ? names[kind] : "All";
}
const char *routing_grouping_name(enum routing_grouping grouping) {
    return grouping == ROUTING_NODE ? "node" : grouping == ROUTING_KIND ? "type" : "flat";
}
static enum routing_kind port_kind(const struct graph_node *node) {
    const char *kind = dict_get(&node->props, "pipemixer.kind"), *class = dict_get(&node->props, PW_KEY_MEDIA_CLASS);
    if (streq(kind, "bus") || streq(kind, "monitor")) return ROUTING_BUSES;
    if (streq(kind, "send")) return ROUTING_SENDS;
    if (streq(kind, "effect")) return ROUTING_EFFECTS;
    if (class && !strncmp(class, "Stream/", 7)) return ROUTING_APPLICATIONS;
    if (streq(class, "Audio/Sink") || streq(class, "Audio/Source")
        || streq(class, "Audio/Duplex")) return ROUTING_DEVICES;
    return ROUTING_OTHER;
}
static bool contains(const char *value, const char *query) { return value && strcasestr(value, query); }
static void collect_port(const struct graph_port *port, void *data) {
    struct collect *collect = data;
    if (port->direction != collect->direction || !graph_port_is_audio(port)) return;
    const struct graph_node *node = graph_node_find(port->node_id);
    const char *name = graph_node_name(port->node_id);
    if (!node || !name) return;
    enum routing_kind kind = port_kind(node);
    if (collect->filter->kind != ROUTING_ALL && collect->filter->kind != kind) return;
    const char *query = collect->filter->text, *port_name = dict_get(&port->props, PW_KEY_PORT_NAME) ?: "";
    if (*query && !contains(name, query) && !contains(port_name, query)
        && !contains(dict_get(&node->props, PW_KEY_NODE_DESCRIPTION), query)
        && !contains(dict_get(&node->props, PW_KEY_APP_NAME), query)
        && !contains(dict_get(&port->props, PW_KEY_PORT_ALIAS), query)
        && !contains(dict_get(&port->props, PW_KEY_AUDIO_CHANNEL), query)) return;
    collect->ports = xreallocarray(collect->ports, collect->count + 1, sizeof(*collect->ports));
    collect->ports[collect->count++] = (struct raw_port){.id = port->id, .node = name, .name = port_name,
        .kind = kind, .sort = collect->filter->grouping == ROUTING_KIND ? kind : 0};
}
static int compare_ports(const void *a, const void *b) {
    const struct raw_port *left = a, *right = b;
    int result = (left->sort > right->sort) - (left->sort < right->sort);
    if (!result) result = strcmp(left->node, right->node);
    if (!result) result = strcmp(left->name, right->name);
    return result ?: (left->id > right->id) - (left->id < right->id);
}
static bool folded(const struct routing_filter *filter, const char *key) {
    for (const struct routing_fold *fold = filter->folds; fold; fold = fold->next)
        if (streq(fold->key, key)) return true;
    return false;
}
static struct routing_item *append(struct routing_axis *axis) {
    axis->items = xreallocarray(axis->items, axis->count + 1, sizeof(*axis->items));
    struct routing_item *item = &axis->items[axis->count++];
    *item = (struct routing_item){.port = PW_ID_ANY}; return item;
}
const struct routing_item *routing_view_selected(const struct routing_axis *axis) {
    return axis->count && axis->cursor >= 0 && (unsigned)axis->cursor < axis->count ? &axis->items[axis->cursor] : NULL;
}
void routing_view_clear(struct routing_axis *axis) {
    for (unsigned i = 0; i < axis->count; i++) {
        free(axis->items[i].key); free(axis->items[i].group); free(axis->items[i].label);
    }
    free(axis->items); axis->items = NULL; axis->count = 0;
}
void routing_view_build(struct routing_axis *axis, enum pw_direction direction, const struct routing_filter *filter) {
    const struct routing_item *old = routing_view_selected(axis);
    char *previous = old ? xstrdup(old->key) : NULL;
    routing_view_clear(axis);
    struct collect collect = {.filter = filter, .direction = direction};
    graph_foreach_port(collect_port, &collect);
    if (collect.count > 1) qsort(collect.ports, collect.count, sizeof(*collect.ports), compare_ports);
    const char side = direction == PW_DIRECTION_OUTPUT ? 'o' : 'i';
    for (unsigned i = 0; i < collect.count;) {
        unsigned end = i + 1;
        char *group = NULL;
        bool closed = false;
        if (filter->grouping != ROUTING_FLAT) {
            while (end < collect.count && (filter->grouping == ROUTING_NODE
                ? streq(collect.ports[i].node, collect.ports[end].node)
                : collect.ports[i].kind == collect.ports[end].kind)) end++;
            const char *name = filter->grouping == ROUTING_NODE ? collect.ports[i].node : routing_kind_name(collect.ports[i].kind);
            xasprintf(&group, "%c:%s:%s", side, routing_grouping_name(filter->grouping), name);
            closed = folded(filter, group);
            struct routing_item *header = append(axis);
            header->key = xstrdup(group); header->group = xstrdup(group); header->ports = end - i; header->folded = closed;
            xasprintf(&header->label, "%c %s (%u)", closed ? '+' : '-', name, end - i);
        }
        if (!closed) for (unsigned p = i; p < end; p++) {
            struct routing_item *item = append(axis);
            item->port = collect.ports[p].id;
            xasprintf(&item->key, "%c:%u", side, item->port);
            item->group = group ? xstrdup(group) : NULL;
            const struct graph_port *port = graph_port_find(item->port);
            const char *alias = dict_get(&port->props, PW_KEY_PORT_ALIAS);
            xasprintf(&item->label, "%s#%u %s%s%s", group ? "  " : "", item->port,
                      alias ?: collect.ports[p].node, alias ? "" : ":", alias ? "" : collect.ports[p].name);
        }
        free(group); i = end;
    }
    axis->cursor = MIN(axis->cursor, MAX(0, (int)axis->count - 1));
    for (unsigned i = 0; previous && i < axis->count; i++) if (streq(previous, axis->items[i].key)) axis->cursor = i;
    free(previous); free(collect.ports);
}
bool routing_view_fold(struct routing_axis *axis, struct routing_filter *filter) {
    const struct routing_item *selected = routing_view_selected(axis);
    if (!selected || !selected->group) return false;
    struct routing_fold **next = &filter->folds; unsigned count = 0;
    while (*next && !streq((*next)->key, selected->group)) { next = &(*next)->next; count++; }
    if (*next) { struct routing_fold *fold = *next; *next = fold->next; free(fold->key); free(fold); }
    else {
        if (count >= 1024) return false;
        struct routing_fold *fold = xcalloc(1, sizeof(*fold)); fold->key = xstrdup(selected->group);
        fold->next = filter->folds; filter->folds = fold;
    }
    for (unsigned i = 0; i < axis->count; i++) if (streq(axis->items[i].key, selected->group)) { axis->cursor = i; break; }
    return true;
}
unsigned routing_view_members(const struct routing_axis *axis, enum pw_direction direction,
                              const struct routing_filter *filter, bool node, uint32_t **ids) {
    *ids = NULL;
    const struct routing_item *selected = routing_view_selected(axis); if (!selected) return 0;
    if (selected->port != PW_ID_ANY && !node) { *ids = xmalloc(sizeof(**ids)); **ids = selected->port; return 1; }
    struct collect collect = {.filter = filter, .direction = direction}; graph_foreach_port(collect_port, &collect);
    const struct graph_port *port = graph_port_find(selected->port);
    unsigned count = 0;
    for (unsigned i = 0; i < collect.count; i++) {
        char *key;
        xasprintf(&key, "%c:%s:%s", direction == PW_DIRECTION_OUTPUT ? 'o' : 'i', routing_grouping_name(filter->grouping),
                  filter->grouping == ROUTING_NODE ? collect.ports[i].node : routing_kind_name(collect.ports[i].kind));
        bool member = port && node ? graph_port_find(collect.ports[i].id)->node_id == port->node_id : streq(selected->group, key);
        free(key);
        if (!member) continue;
        *ids = xreallocarray(*ids, count + 1, sizeof(**ids)); (*ids)[count++] = collect.ports[i].id;
    }
    free(collect.ports); return count;
}
void routing_filter_clear(struct routing_filter *filter) {
    while (filter->folds) {
        struct routing_fold *fold = filter->folds; filter->folds = fold->next; free(fold->key); free(fold);
    }
}
