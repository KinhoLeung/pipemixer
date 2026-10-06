#pragma once
#include "pw/graph.h"

struct routing_mark { uint32_t id; char *identity; };
struct routing_marks { struct routing_mark *ports; unsigned count; };
struct routing_batch {
    struct graph_pair *pairs;
    unsigned count, position, unchanged, skipped;
    bool active, disconnect, waiting, rollback;
    uint64_t deadline;
    char tag[80], message[256];
};
bool routing_marks_has(const struct routing_marks *marks, uint32_t id);
bool routing_marks_toggle(struct routing_marks *marks, const uint32_t *ids, unsigned count);
bool routing_marks_prune(struct routing_marks *marks);
void routing_marks_clear(struct routing_marks *marks);
int routing_batch_start(struct routing_batch *batch, const struct routing_marks *outputs,
                        const struct routing_marks *inputs, bool disconnect);
bool routing_batch_step(struct routing_batch *batch);
void routing_batch_abort(struct routing_batch *batch, const char *reason);
void routing_batch_clear(struct routing_batch *batch);
