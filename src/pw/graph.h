#pragma once

#include <pipewire/pipewire.h>
#include "collections/dict.h"

/* Borrowed objects, valid until the next PipeWire event is dispatched. */
struct graph_control {
    char *name;
    uint32_t type;
    double value, minimum, maximum, default_value;
    bool has_value, has_info, writable, visible;
};

struct graph_node {
    uint32_t id;
    struct dict props;
    struct pw_node *proxy;
    struct spa_hook listener;
    bool ready;
    enum pw_node_state state;
    char *error;
    struct graph_control *controls;
    unsigned n_controls;
    bool controls_ready, controls_subscribed;
};

struct graph_port {
    uint32_t id, node_id;
    enum pw_direction direction;
    struct dict props;
    struct pw_port *proxy;
    struct spa_hook listener;
    bool ready;
};

struct graph_link {
    uint32_t id;
    struct pw_link_info *info;
    struct pw_link *proxy;
    struct spa_hook listener;
};

void graph_init(struct pw_core *core, struct pw_registry *registry, void (*changed)(void));
void graph_cleanup(void);
void graph_global(uint32_t id, const char *type, uint32_t version,
                  const struct spa_dict *props);
void graph_global_remove(uint32_t id);
bool graph_ready(void);

const struct graph_node *graph_node_find(uint32_t id);
const struct graph_port *graph_port_find(uint32_t id);
const struct graph_link *graph_link_find(uint32_t id);
bool graph_node_is_audio(const struct graph_node *node);
bool graph_port_is_audio(const struct graph_port *port);
bool graph_link_is_audio(const struct graph_link *link);
const char *graph_node_name(uint32_t id);

/* Resolve exact node.name:port.name, or id:N. Return negative errno on failure. */
int graph_resolve_port(const char *name, enum pw_direction direction, uint32_t *id);
int graph_resolve_node(const char *name, uint32_t *id);
bool graph_node_has_ports(uint32_t id, enum pw_direction direction);
bool graph_would_cycle(uint32_t output_node, uint32_t input_node);
struct graph_pair { uint32_t output, input; };
int graph_validate_connections(const struct graph_pair *pairs, unsigned count);
const struct graph_link *graph_link_between(uint32_t output, uint32_t input);
int graph_connect(uint32_t output, uint32_t input);
int graph_connect_rule(uint32_t output, uint32_t input, const char *rule, const char *scope);
int graph_connect_batch(uint32_t output, uint32_t input, const char *batch);
int graph_connect_saved(uint32_t output, uint32_t input, const char *rule, const char *scope, const char *batch);
int graph_disconnect(uint32_t output, uint32_t input);
int graph_destroy_link(uint32_t id);
int graph_destroy_node(uint32_t id);
const struct graph_control *graph_control_find(const struct graph_node *node, const char *name);
int graph_set_controls(uint32_t id, const char *names[], const double values[], unsigned count);
int graph_validate_controls(uint32_t id, const char *names[], const double values[], unsigned count);

void graph_foreach_node(void (*callback)(const struct graph_node *, void *), void *data);
void graph_foreach_port(void (*callback)(const struct graph_port *, void *), void *data);
void graph_foreach_link(void (*callback)(const struct graph_link *, void *), void *data);
