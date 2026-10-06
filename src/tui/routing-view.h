#pragma once

#include "pw/graph.h"

enum routing_grouping { ROUTING_FLAT, ROUTING_NODE, ROUTING_KIND };
enum routing_kind { ROUTING_ALL, ROUTING_DEVICES, ROUTING_BUSES, ROUTING_SENDS,
                    ROUTING_EFFECTS, ROUTING_APPLICATIONS, ROUTING_OTHER, ROUTING_KIND_COUNT };

struct routing_fold;
struct routing_filter {
    enum routing_grouping grouping;
    enum routing_kind kind;
    char text[128];
    struct routing_fold *folds;
};

struct routing_item {
    uint32_t port;
    char *key, *group, *label;
    unsigned ports;
    bool folded;
};

struct routing_axis {
    struct routing_item *items;
    unsigned count;
    int cursor, scroll;
};

const char *routing_kind_name(enum routing_kind kind);
const char *routing_grouping_name(enum routing_grouping grouping);
void routing_view_build(struct routing_axis *axis, enum pw_direction direction, const struct routing_filter *filter);
bool routing_view_fold(struct routing_axis *axis, struct routing_filter *filter);
/* Selected port/group members under the current filter; node expands a port's node. */
unsigned routing_view_members(const struct routing_axis *axis, enum pw_direction direction,
                              const struct routing_filter *filter, bool node, uint32_t **ids);
const struct routing_item *routing_view_selected(const struct routing_axis *axis);
void routing_view_clear(struct routing_axis *axis);
void routing_filter_clear(struct routing_filter *filter);
