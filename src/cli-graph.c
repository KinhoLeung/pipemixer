#include <stdio.h>
#include <stdlib.h>

#include "cli-graph.h"
#include "pw/graph.h"
#include "utils.h"
#include "xmalloc.h"

struct row { uint32_t id; const void *object; };
struct rows { struct row *items; unsigned count; enum cli_list_kind kind; };

static void add_row(struct rows *rows, uint32_t id, const void *object) {
    rows->items = xreallocarray(rows->items, rows->count + 1, sizeof(rows->items[0]));
    rows->items[rows->count++] = (struct row){ id, object };
}
static void collect_node(const struct graph_node *node, void *data) {
    struct rows *rows = data;
    if (rows->kind == CLI_LIST_BUSES || rows->kind == CLI_LIST_SENDS || rows->kind == CLI_LIST_EFFECTS) {
        const char *kind = rows->kind == CLI_LIST_BUSES ? "bus" : rows->kind == CLI_LIST_SENDS ? "send" : "effect";
        if (!streq(dict_get(&node->props, "pipemixer.managed"), "1")
            || !streq(dict_get(&node->props, "pipemixer.kind"), kind)
            || !streq(dict_get(&node->props, "pipemixer.role"), "input")) return;
    }
    if (graph_node_is_audio(node)) add_row(data, node->id, node);
}
static void collect_port(const struct graph_port *port, void *data) {
    if (graph_port_is_audio(port)) add_row(data, port->id, port);
}
static void collect_link(const struct graph_link *link, void *data) {
    if (graph_link_is_audio(link)) add_row(data, link->id, link);
}
static int compare_rows(const void *a, const void *b) {
    const struct row *left = a, *right = b;
    return (left->id > right->id) - (left->id < right->id);
}
static void json_string(const char *value) {
    if (!value) { fputs("null", stdout); return; }
    char *quoted = json_quote(value);
    fputs(quoted, stdout);
    free(quoted);
}
static void property(const char *key, const struct dict *props, const char *name) {
    printf(",\"%s\":", key);
    json_string(dict_get(props, name));
}
static void endpoint(uint32_t id, bool json) {
    const struct graph_port *port = graph_port_find(id);
    const char *name = port ? dict_get(&port->props, PW_KEY_PORT_NAME) : NULL;
    const char *node_name = port ? graph_node_name(port->node_id) : NULL;
    if (json) {
        printf("{\"port_id\":%u,\"node_id\":%u,\"node_name\":", id, port ? port->node_id : PW_ID_ANY);
        json_string(node_name);
        fputs(",\"port_name\":", stdout); json_string(name);
        fputc('}', stdout);
    } else {
        printf("%s:%s", node_name ?: "?", name ?: "?");
    }
}

static void print_array(enum cli_list_kind kind, bool json) {
    struct rows rows = { .kind = kind };
    if (kind == CLI_LIST_PORTS) graph_foreach_port(collect_port, &rows);
    else if (kind == CLI_LIST_LINKS) graph_foreach_link(collect_link, &rows);
    else graph_foreach_node(collect_node, &rows);
    if (rows.count) qsort(rows.items, rows.count, sizeof(rows.items[0]), compare_rows);
    if (json) fputc('[', stdout);
    for (unsigned i = 0; i < rows.count; i++) {
        if (json && i) fputc(',', stdout);
        if (kind == CLI_LIST_PORTS) {
            const struct graph_port *port = rows.items[i].object;
            const char *direction = port->direction == PW_DIRECTION_OUTPUT ? "output" : "input";
            if (json) {
                printf("{\"id\":%u,\"node_id\":%u,\"direction\":\"%s\",\"node_name\":",
                       port->id, port->node_id, direction);
                json_string(graph_node_name(port->node_id));
                property("name", &port->props, PW_KEY_PORT_NAME);
                property("alias", &port->props, PW_KEY_PORT_ALIAS);
                property("channel", &port->props, "audio.channel");
                fputs(streq(dict_get(&port->props, PW_KEY_PORT_MONITOR), "true")
                      ? ",\"monitor\":true}" : ",\"monitor\":false}", stdout);
            } else {
                printf("%u\t%s\t", port->id, direction);
                endpoint(port->id, false);
                printf("\t%s\n", dict_get(&port->props, "audio.channel") ?: "");
            }
        } else if (kind == CLI_LIST_LINKS) {
            const struct graph_link *link = rows.items[i].object;
            const struct pw_link_info *info = link->info;
            if (json) {
                printf("{\"id\":%u,\"output\":", link->id);
                endpoint(info->output_port_id, true);
                fputs(",\"input\":", stdout); endpoint(info->input_port_id, true);
                fputs(",\"state\":", stdout); json_string(pw_link_state_as_string(info->state));
                fputs(",\"error\":", stdout); json_string(info->error);
                const char *passive = info->props ? spa_dict_lookup(info->props, PW_KEY_LINK_PASSIVE) : NULL;
                fputs(streq(passive, "true") ? ",\"passive\":true}" : ",\"passive\":false}", stdout);
            } else {
                printf("%u\t", link->id); endpoint(info->output_port_id, false);
                fputs(" -> ", stdout); endpoint(info->input_port_id, false);
                printf("\t%s\n", pw_link_state_as_string(info->state));
            }
        } else {
            const struct graph_node *node = rows.items[i].object;
            if (json) {
                printf("{\"id\":%u", node->id);
                property("name", &node->props, PW_KEY_NODE_NAME);
                property("description", &node->props, PW_KEY_NODE_DESCRIPTION);
                property("class", &node->props, PW_KEY_MEDIA_CLASS);
                property("kind", &node->props, "pipemixer.kind");
                property("group", &node->props, "pipemixer.group");
                property("role", &node->props, "pipemixer.role");
                fputc('}', stdout);
            } else {
                printf("%u\t%s\t%s\n", node->id, graph_node_name(node->id) ?: "",
                       dict_get(&node->props, PW_KEY_MEDIA_CLASS) ?: "");
            }
        }
    }
    if (json) fputc(']', stdout);
    free(rows.items);
}

void cli_graph_print(enum cli_list_kind kind, bool json) {
    if (kind != CLI_LIST_GRAPH) {
        print_array(kind, json);
    } else {
        fputs(json ? "{\"nodes\":" : "Nodes\n", stdout);
        print_array(CLI_LIST_NODES, json);
        fputs(json ? ",\"ports\":" : "Ports\n", stdout);
        print_array(CLI_LIST_PORTS, json);
        fputs(json ? ",\"links\":" : "Links\n", stdout);
        print_array(CLI_LIST_LINKS, json);
        if (json) fputc('}', stdout);
    }
    if (json) fputc('\n', stdout);
}
