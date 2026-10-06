#include <string.h>
#include <errno.h>
#include <math.h>
#include <spa/pod/parser.h>
#include <spa/pod/iter.h>
#include <spa/pod/dynamic.h>
#include <spa/param/props.h>
#include <spa/utils/string.h>

#include "pw/graph.h"
#include "pw/peak.h"
#include "pw/effects.h"
#include "pw/effect-chain.h"
#include "collections/map.h"
#include "utils.h"
#include "xmalloc.h"

static struct {
    struct pw_core *core;
    struct pw_registry *registry;
    struct map nodes, ports, links;
    void (*changed)(void);
    struct pending_link *pending;
} graph;

struct pending_link {
    struct pw_proxy *proxy;
    struct spa_hook listener;
    struct pending_link *next;
};

static void release_pending(void *data, uint32_t id) {
    struct pending_link *pending = data;
    struct pending_link **slot = &graph.pending;
    while (*slot && *slot != pending) slot = &(*slot)->next;
    if (*slot) *slot = pending->next;
    spa_hook_remove(&pending->listener);
    /* The server owns the lingering link after it has been bound. */
    pw_proxy_destroy(pending->proxy);
    free(pending);
}

static const struct pw_proxy_events pending_events = {
    .version = PW_VERSION_PROXY_EVENTS,
    .bound = release_pending,
};

static void update_props(struct dict *dest, const struct spa_dict *props) {
    if (!props) return;
    const struct spa_dict_item *item;
    spa_dict_for_each(item, props) {
        if (item->value) dict_insert(dest, item->key, item->value);
        else dict_remove(dest, item->key);
    }
}

static void notify(void) {
    if (graph.changed) graph.changed();
}

static void on_node_info(void *data, const struct pw_node_info *info) {
    struct graph_node *node = data;
    if (info->change_mask & PW_NODE_CHANGE_MASK_STATE) {
        node->state = info->state; free(node->error); node->error = info->error ? xstrdup(info->error) : NULL;
    }
    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) update_props(&node->props, info->props);
    node->ready = true;
    if (!node->controls_subscribed && streq(dict_get(&node->props, "pipemixer.kind"), "effect")
        && streq(dict_get(&node->props, "pipemixer.role"), "input")) {
        node->controls_subscribed = true;
        pw_node_subscribe_params(node->proxy, (uint32_t[]){SPA_PARAM_PropInfo, SPA_PARAM_Props}, 2);
    }
    notify();
}

static struct graph_control *get_control(struct graph_node *node, const char *name) {
    for (unsigned i = 0; i < node->n_controls; i++)
        if (streq(node->controls[i].name, name)) return &node->controls[i];
    node->controls = xreallocarray(node->controls, node->n_controls + 1, sizeof(node->controls[0]));
    struct graph_control *control = &node->controls[node->n_controls++];
    *control = (struct graph_control){ .name = xstrdup(name) };
    return control;
}

static bool numeric(uint32_t type, const void *body, double *value) {
    switch (type) {
    case SPA_TYPE_Float: { float v; memcpy(&v, body, sizeof(v)); *value = v; break; }
    case SPA_TYPE_Double: memcpy(value, body, sizeof(*value)); break;
    case SPA_TYPE_Int: case SPA_TYPE_Bool: { int32_t v; memcpy(&v, body, sizeof(v)); *value = v; break; }
    default: return false;
    }
    return isfinite(*value);
}

static void on_node_param(void *data, int seq, uint32_t id, uint32_t index,
                          uint32_t next, const struct spa_pod *param) {
    struct graph_node *node = data;
    if (!param || !spa_pod_is_object(param)) return;
    if (id == SPA_PARAM_PropInfo) {
        const struct spa_pod_prop *name_prop = spa_pod_find_prop(param, NULL, SPA_PROP_INFO_name);
        const struct spa_pod_prop *type_prop = spa_pod_find_prop(param, NULL, SPA_PROP_INFO_type);
        const struct spa_pod_prop *params_prop = spa_pod_find_prop(param, NULL, SPA_PROP_INFO_params);
        const char *name;
        bool params = false;
        if (!name_prop || !type_prop || !params_prop || spa_pod_get_string(&name_prop->value, &name) < 0
            || spa_pod_get_bool(&params_prop->value, &params) < 0 || !params) return;
        uint32_t count, choice;
        const struct spa_pod *values = spa_pod_get_values(&type_prop->value, &count, &choice);
        double def;
        if (!count || values->size < spa_pod_type_size(values->type)
            || !numeric(values->type, SPA_POD_BODY_CONST(values), &def)) return;
        struct graph_control *control = get_control(node, name);
        control->type = values->type;
        control->default_value = def;
        control->minimum = control->maximum = def;
        control->writable = false;
        if (choice == SPA_CHOICE_Range && count >= 3) {
            const char *body = SPA_POD_BODY_CONST(values);
            if (!numeric(values->type, body + values->size, &control->minimum)
                || !numeric(values->type, body + 2 * values->size, &control->maximum)) return;
            control->writable = control->maximum > control->minimum;
        } else if (values->type == SPA_TYPE_Bool && count > 1) {
            control->minimum = 0; control->maximum = 1; control->writable = true;
        }
        control->visible = effect_control_config(dict_get(&node->props, "pipemixer.preset"), name,
                                                &control->minimum, &control->maximum, &control->default_value);
        const char *spec = dict_get(&node->props, "pipemixer.chain");
        if (spec) {
            struct effect_chain chain;
            control->visible = effect_chain_parse(spec, &chain) == 0
                && effect_chain_control(&chain, name, &control->minimum, &control->maximum, &control->default_value);
        }
        control->has_info = true;
    } else if (id == SPA_PARAM_Props) {
        const struct spa_pod_prop *prop = spa_pod_find_prop(param, NULL, SPA_PROP_params);
        if (prop && spa_pod_is_struct(&prop->value)) {
            struct spa_pod_parser parser;
            struct spa_pod_frame frame;
            spa_pod_parser_pod(&parser, &prop->value);
            spa_pod_parser_push_struct(&parser, &frame);
            const char *name;
            struct spa_pod *value;
            while (spa_pod_parser_get_string(&parser, &name) >= 0
                   && spa_pod_parser_get_pod(&parser, &value) >= 0) {
                double number;
                if (value->size >= spa_pod_type_size(value->type)
                    && numeric(value->type, SPA_POD_BODY_CONST(value), &number)) {
                    struct graph_control *control = get_control(node, name);
                    control->value = number; control->has_value = true;
                }
            }
            spa_pod_parser_pop(&parser, &frame);
        }
        node->controls_ready = true;
    }
    notify();
}

static void on_port_info(void *data, const struct pw_port_info *info) {
    struct graph_port *port = data;
    port->direction = info->direction;
    if (info->change_mask & PW_PORT_CHANGE_MASK_PROPS) update_props(&port->props, info->props);
    const char *node_id = dict_get(&port->props, PW_KEY_NODE_ID);
    if (node_id) spa_atou32(node_id, &port->node_id, 10);
    port->ready = true;
    notify();
}

static void on_link_info(void *data, const struct pw_link_info *info) {
    struct graph_link *link = data;
    link->info = pw_link_info_update(link->info, info);
    notify();
}

static const struct pw_node_events node_events = {
    .version = PW_VERSION_NODE_EVENTS, .info = on_node_info, .param = on_node_param,
};
static const struct pw_port_events port_events = {
    .version = PW_VERSION_PORT_EVENTS, .info = on_port_info,
};
static const struct pw_link_events link_events = {
    .version = PW_VERSION_LINK_EVENTS, .info = on_link_info,
};

void graph_init(struct pw_core *core, struct pw_registry *registry, void (*changed)(void)) {
    graph.core = core;
    graph.registry = registry;
    graph.changed = changed;
}

void graph_global(uint32_t id, const char *type, uint32_t version,
                  const struct spa_dict *props) {
    if (streq(type, PW_TYPE_INTERFACE_Node)) {
        struct graph_node *node = xcalloc(1, sizeof(*node));
        node->id = id;
        update_props(&node->props, props);
        node->proxy = pw_registry_bind(graph.registry, id, type,
                                       version < PW_VERSION_NODE ? version : PW_VERSION_NODE, 0);
        if (!node->proxy) { dict_free(&node->props); free(node); return; }
        map_insert(&graph.nodes, id, node);
        pw_node_add_listener(node->proxy, &node->listener, &node_events, node);
    } else if (streq(type, PW_TYPE_INTERFACE_Port)) {
        struct graph_port *port = xcalloc(1, sizeof(*port));
        port->id = id;
        port->node_id = PW_ID_ANY;
        update_props(&port->props, props);
        port->proxy = pw_registry_bind(graph.registry, id, type,
                                       version < PW_VERSION_PORT ? version : PW_VERSION_PORT, 0);
        if (!port->proxy) { dict_free(&port->props); free(port); return; }
        map_insert(&graph.ports, id, port);
        pw_port_add_listener(port->proxy, &port->listener, &port_events, port);
    } else if (streq(type, PW_TYPE_INTERFACE_Link)) {
        struct graph_link *link = xcalloc(1, sizeof(*link));
        link->id = id;
        link->proxy = pw_registry_bind(graph.registry, id, type,
                                       version < PW_VERSION_LINK ? version : PW_VERSION_LINK, 0);
        if (!link->proxy) { free(link); return; }
        map_insert(&graph.links, id, link);
        pw_link_add_listener(link->proxy, &link->listener, &link_events, link);
    }
}

static void free_node(struct graph_node *node) {
    spa_hook_remove(&node->listener);
    pw_proxy_destroy((struct pw_proxy *)node->proxy);
    dict_free(&node->props);
    for (unsigned i = 0; i < node->n_controls; i++) free(node->controls[i].name);
    free(node->controls);
    free(node->error);
    free(node);
}

static void free_port(struct graph_port *port) {
    spa_hook_remove(&port->listener);
    pw_proxy_destroy((struct pw_proxy *)port->proxy);
    dict_free(&port->props);
    free(port);
}

static void free_link(struct graph_link *link) {
    spa_hook_remove(&link->listener);
    pw_proxy_destroy((struct pw_proxy *)link->proxy);
    /* Failed negotiations can remove the global before its first info event. */
    if (link->info) pw_link_info_free(link->info);
    free(link);
}

void graph_global_remove(uint32_t id) {
    struct graph_node *node = map_remove(&graph.nodes, id);
    struct graph_port *port = map_remove(&graph.ports, id);
    struct graph_link *link = map_remove(&graph.links, id);
    if (node) free_node(node);
    if (port) free_port(port);
    if (link) free_link(link);
    if (node || port || link) notify();
}

void graph_cleanup(void) {
    graph.changed = NULL;
    while (graph.pending) {
        struct pending_link *pending = graph.pending;
        graph.pending = pending->next;
        spa_hook_remove(&pending->listener);
        pw_proxy_destroy(pending->proxy);
        free(pending);
    }
    struct graph_link *link;
    MAP_FOREACH(&graph.links, &link) free_link(link);
    struct graph_port *port;
    MAP_FOREACH(&graph.ports, &port) free_port(port);
    struct graph_node *node;
    MAP_FOREACH(&graph.nodes, &node) free_node(node);
    map_free(&graph.links);
    map_free(&graph.ports);
    map_free(&graph.nodes);
    graph.registry = NULL;
    graph.core = NULL;
}

bool graph_ready(void) {
    struct graph_node *node;
    MAP_FOREACH(&graph.nodes, &node) if (!node->ready) return false;
    struct graph_port *port;
    MAP_FOREACH(&graph.ports, &port) if (!port->ready) return false;
    struct graph_link *link;
    MAP_FOREACH(&graph.links, &link) if (!link->info) return false;
    return true;
}

const struct graph_node *graph_node_find(uint32_t id) { return map_get(&graph.nodes, id); }
const struct graph_port *graph_port_find(uint32_t id) { return map_get(&graph.ports, id); }
const struct graph_link *graph_link_find(uint32_t id) { return map_get(&graph.links, id); }

static bool node_hidden(const struct graph_node *node) {
    if (!node) return true;
    const char *name = dict_get(&node->props, PW_KEY_NODE_NAME);
    return streq(dict_get(&node->props, "pipemixer.internal"), "true")
        || streq(dict_get(&node->props, PEAK_METER_NODE_PROPERTY), "true")
        || (name && strncmp(name, PEAK_METER_NODE_PREFIX, strlen(PEAK_METER_NODE_PREFIX)) == 0);
}

bool graph_port_is_audio(const struct graph_port *port) {
    if (!port || !port->ready || node_hidden(graph_node_find(port->node_id))) return false;
    const char *format = dict_get(&port->props, PW_KEY_FORMAT_DSP);
    return (format && strstr(format, "audio")) || dict_get(&port->props, "audio.channel");
}

bool graph_node_is_audio(const struct graph_node *node) {
    if (!node || !node->ready || node_hidden(node)) return false;
    const char *class = dict_get(&node->props, PW_KEY_MEDIA_CLASS);
    if (class && strstr(class, "Audio")) return true;
    struct graph_port *port;
    MAP_FOREACH(&graph.ports, &port) {
        if (port->node_id == node->id && graph_port_is_audio(port)) return true;
    }
    return false;
}

bool graph_link_is_audio(const struct graph_link *link) {
    return link && link->info
        && graph_port_is_audio(graph_port_find(link->info->output_port_id))
        && graph_port_is_audio(graph_port_find(link->info->input_port_id));
}

const char *graph_node_name(uint32_t id) {
    const struct graph_node *node = graph_node_find(id);
    return node ? dict_get(&node->props, PW_KEY_NODE_NAME) : NULL;
}

int graph_resolve_port(const char *name, enum pw_direction direction, uint32_t *id) {
    unsigned matches = 0;
    uint32_t requested = PW_ID_ANY;
    if (strncmp(name, "id:", 3) == 0 && !spa_atou32(name + 3, &requested, 10)) return -EINVAL;
    struct graph_port *port;
    MAP_FOREACH(&graph.ports, &port) {
        if (!graph_port_is_audio(port) || port->direction != direction) continue;
        bool match = requested == port->id;
        if (requested == PW_ID_ANY) {
            char *full_name;
            xasprintf(&full_name, "%s:%s", graph_node_name(port->node_id) ?: "",
                      dict_get(&port->props, PW_KEY_PORT_NAME) ?: "");
            match = streq(name, full_name);
            free(full_name);
        }
        if (match) { *id = port->id; matches++; }
    }
    return matches == 1 ? 0 : matches ? -ENOTUNIQ : -ENOENT;
}

const struct graph_link *graph_link_between(uint32_t output, uint32_t input) {
    struct graph_link *link;
    MAP_FOREACH(&graph.links, &link) {
        if (link->info && link->info->output_port_id == output && link->info->input_port_id == input)
            return link;
    }
    return NULL;
}

static bool would_cycle(uint32_t output_node, uint32_t input_node, const struct graph_pair *pairs, unsigned n_pairs) {
    const unsigned capacity = graph.nodes.n_entries + 1;
    uint32_t *visited = xcalloc(capacity, sizeof(visited[0]));
    unsigned head = 0, count = 1;
    visited[0] = input_node;
    bool cycle = false;
    while (head < count) {
        uint32_t current = visited[head++];
        if (current == output_node) { cycle = true; break; }
        struct graph_link *link;
        MAP_FOREACH(&graph.links, &link) {
            if (!graph_link_is_audio(link) || link->info->output_node_id != current) continue;
            uint32_t next = link->info->input_node_id;
            bool seen = false;
            for (unsigned i = 0; i < count; i++) if (visited[i] == next) { seen = true; break; }
            if (!seen && count < capacity) visited[count++] = next;
        }
        for (unsigned p = 0; p < n_pairs; p++) {
            const struct graph_port *out = graph_port_find(pairs[p].output), *in = graph_port_find(pairs[p].input);
            if (!out || !in || out->node_id != current) continue;
            bool seen = false;
            for (unsigned i = 0; i < count; i++) if (visited[i] == in->node_id) { seen = true; break; }
            if (!seen && count < capacity) visited[count++] = in->node_id;
        }
        const struct graph_node *node = graph_node_find(current);
        const char *group = node ? dict_get(&node->props, "pipemixer.group") : NULL;
        if (node && group && streq(dict_get(&node->props, "pipemixer.role"), "input")) {
            struct graph_node *peer;
            MAP_FOREACH(&graph.nodes, &peer) {
                if (!streq(dict_get(&peer->props, "pipemixer.group"), group)
                    || !streq(dict_get(&peer->props, "pipemixer.kind"), dict_get(&node->props, "pipemixer.kind"))
                    || !streq(dict_get(&peer->props, "pipemixer.role"), "output")) continue;
                bool seen = false;
                for (unsigned i = 0; i < count; i++) if (visited[i] == peer->id) { seen = true; break; }
                if (!seen && count < capacity) visited[count++] = peer->id;
            }
        }
    }
    free(visited);
    return cycle;
}
bool graph_would_cycle(uint32_t output_node, uint32_t input_node) { return would_cycle(output_node, input_node, NULL, 0); }
int graph_validate_connections(const struct graph_pair *pairs, unsigned count) {
    if (count > 1024) return -E2BIG;
    for (unsigned p = 0; p < count; p++) {
        const struct graph_port *out = graph_port_find(pairs[p].output), *in = graph_port_find(pairs[p].input);
        if (!graph_port_is_audio(out) || !graph_port_is_audio(in)) return -ENOENT;
        if (out->direction != PW_DIRECTION_OUTPUT || in->direction != PW_DIRECTION_INPUT) return -EINVAL;
    }
    for (unsigned p = 0; p < count; p++)
        if (would_cycle(graph_port_find(pairs[p].output)->node_id, graph_port_find(pairs[p].input)->node_id, pairs, count)) return -ELOOP;
    return 0;
}

static int connect_owned(uint32_t output, uint32_t input, const char *rule, const char *scope, const char *batch) {
    const struct graph_port *out = graph_port_find(output), *in = graph_port_find(input);
    if (!graph_port_is_audio(out) || !graph_port_is_audio(in)) return -ENOENT;
    if (out->direction != PW_DIRECTION_OUTPUT || in->direction != PW_DIRECTION_INPUT) return -EINVAL;
    if (graph_link_between(output, input)) return 0;
    if (graph_would_cycle(out->node_id, in->node_id)) return -ELOOP;
    struct pw_properties *props = pw_properties_new(
        PW_KEY_OBJECT_LINGER, "true", "pipemixer.link", "true", NULL);
    if (!props) return -ENOMEM;
    if (rule && scope) {
        pw_properties_set(props, "pipemixer.route-rule", rule);
        pw_properties_set(props, "pipemixer.rule-set", scope);
    }
    if (batch) pw_properties_set(props, "pipemixer.batch", batch);
    pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", output);
    pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", input);
    struct pw_proxy *proxy = pw_core_create_object(graph.core, "link-factory",
                                                  PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
                                                  &props->dict, 0);
    pw_properties_free(props);
    if (!proxy) return -(errno ?: EIO);
    struct pending_link *pending = xcalloc(1, sizeof(*pending));
    pending->proxy = proxy;
    pending->next = graph.pending;
    graph.pending = pending;
    pw_proxy_add_listener(proxy, &pending->listener, &pending_events, pending);
    return 0;
}
int graph_connect_saved(uint32_t output,uint32_t input,const char *rule,const char *scope,const char *batch) {
    return connect_owned(output,input,rule,scope,batch);
}
int graph_connect_rule(uint32_t output, uint32_t input, const char *rule, const char *scope) { return connect_owned(output, input, rule, scope, NULL); }
int graph_connect_batch(uint32_t output, uint32_t input, const char *batch) { return connect_owned(output, input, NULL, NULL, batch); }
int graph_connect(uint32_t output, uint32_t input) { return graph_connect_rule(output, input, NULL, NULL); }
int graph_destroy_link(uint32_t id) { return pw_registry_destroy(graph.registry, id); }

int graph_disconnect(uint32_t output, uint32_t input) {
    struct graph_link *link;
    MAP_FOREACH(&graph.links, &link) {
        if (!link->info || link->info->output_port_id != output || link->info->input_port_id != input) continue;
        int result = pw_registry_destroy(graph.registry, link->id);
        if (result < 0) return result;
    }
    return 0;
}

int graph_resolve_node(const char *name, uint32_t *id) {
    unsigned matches = 0;
    uint32_t requested = PW_ID_ANY;
    bool by_id = strncmp(name, "id:", 3) == 0;
    bool by_serial = strncmp(name, "serial:", 7) == 0;
    if (by_id && !spa_atou32(name + 3, &requested, 10)) return -EINVAL;
    struct graph_node *node;
    MAP_FOREACH(&graph.nodes, &node) {
        if (!graph_node_is_audio(node)) continue;
        bool match = by_id ? node->id == requested : by_serial
            ? streq(name + 7, dict_get(&node->props, PW_KEY_OBJECT_SERIAL))
            : streq(name, graph_node_name(node->id));
        if (match) { *id = node->id; matches++; }
    }
    return matches == 1 ? 0 : matches ? -ENOTUNIQ : -ENOENT;
}

bool graph_node_has_ports(uint32_t id, enum pw_direction direction) {
    struct graph_port *port;
    MAP_FOREACH(&graph.ports, &port) {
        if (port->node_id == id && port->direction == direction && graph_port_is_audio(port)) return true;
    }
    return false;
}

int graph_destroy_node(uint32_t id) {
    return pw_registry_destroy(graph.registry, id);
}

const struct graph_control *graph_control_find(const struct graph_node *node, const char *name) {
    if (!node) return NULL;
    for (unsigned i = 0; i < node->n_controls; i++)
        if (streq(node->controls[i].name, name)) return &node->controls[i];
    return NULL;
}

int graph_validate_controls(uint32_t id, const char *names[], const double values[], unsigned count) {
    const struct graph_node *node = graph_node_find(id);
    if (!node) return -ENOENT;
    for (unsigned i = 0; i < count; i++) {
        const struct graph_control *control = graph_control_find(node, names[i]);
        if (!control || !control->has_info || !control->has_value || !control->visible) return -ENOENT;
        if (!control->writable) return -EACCES;
        if (!isfinite(values[i]) || values[i] < control->minimum || values[i] > control->maximum)
            return -ERANGE;
        if ((control->type == SPA_TYPE_Int || control->type == SPA_TYPE_Bool) && floor(values[i]) != values[i])
            return -EINVAL;
    }
    return 0;
}

int graph_set_controls(uint32_t id, const char *names[], const double values[], unsigned count) {
    int validation = graph_validate_controls(id, names, values, count);
    if (validation < 0) return validation;
    const struct graph_node *node = graph_node_find(id);
    struct spa_pod_dynamic_builder builder;
    struct spa_pod_frame object, params;
    spa_pod_dynamic_builder_init(&builder, NULL, 0, 1024);
    spa_pod_builder_push_object(&builder.b, &object, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder.b, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(&builder.b, &params);
    for (unsigned i = 0; i < count; i++) {
        const struct graph_control *control = graph_control_find(node, names[i]);
        spa_pod_builder_string(&builder.b, names[i]);
        if (control->type == SPA_TYPE_Bool) spa_pod_builder_bool(&builder.b, values[i] != 0);
        else if (control->type == SPA_TYPE_Int) spa_pod_builder_int(&builder.b, values[i]);
        else if (control->type == SPA_TYPE_Double) spa_pod_builder_double(&builder.b, values[i]);
        else spa_pod_builder_float(&builder.b, values[i]);
    }
    spa_pod_builder_pop(&builder.b, &params);
    const struct spa_pod *pod = spa_pod_builder_pop(&builder.b, &object);
    int result = pod ? pw_node_set_param(node->proxy, SPA_PARAM_Props, 0, pod) : -ENOMEM;
    spa_pod_dynamic_builder_clean(&builder);
    return result;
}

void graph_foreach_node(void (*callback)(const struct graph_node *, void *), void *data) {
    struct graph_node *node;
    MAP_FOREACH(&graph.nodes, &node) callback(node, data);
}
void graph_foreach_port(void (*callback)(const struct graph_port *, void *), void *data) {
    struct graph_port *port;
    MAP_FOREACH(&graph.ports, &port) callback(port, data);
}
void graph_foreach_link(void (*callback)(const struct graph_link *, void *), void *data) {
    struct graph_link *link;
    MAP_FOREACH(&graph.links, &link) callback(link, data);
}
