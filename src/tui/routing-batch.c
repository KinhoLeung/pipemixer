#include "i18n.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>

#include "tui/routing-batch.h"
#include "xmalloc.h"
#include "utils.h"

#define PORT_LIMIT 256
#define PAIR_LIMIT 1024

static uint64_t now(void) {
    struct timespec value; clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}
static char *identity(uint32_t id) {
    const struct graph_port *port = graph_port_find(id); if (!port) return NULL;
    const struct graph_node *node = graph_node_find(port->node_id); if (!node) return NULL;
    char *value;
    xasprintf(&value, "%s:%s:%s:%s", dict_get(&port->props, PW_KEY_OBJECT_SERIAL) ?: "",
        dict_get(&node->props, PW_KEY_OBJECT_SERIAL) ?: "", graph_node_name(node->id) ?: "",
        dict_get(&port->props, PW_KEY_PORT_NAME) ?: "");
    return value;
}
bool routing_marks_has(const struct routing_marks *marks, uint32_t id) {
    for (unsigned i = 0; i < marks->count; i++) if (marks->ports[i].id == id) return true;
    return false;
}
static void remove_mark(struct routing_marks *marks, unsigned index) {
    free(marks->ports[index].identity);
    memmove(marks->ports + index, marks->ports + index + 1, (marks->count - index - 1) * sizeof(*marks->ports));
    marks->count--;
}
bool routing_marks_toggle(struct routing_marks *marks, const uint32_t *ids, unsigned count) {
    unsigned missing = 0;
    for (unsigned i = 0; i < count; i++) missing += !routing_marks_has(marks, ids[i]);
    if (marks->count + missing > PORT_LIMIT) return false;
    for (unsigned i = 0; i < count; i++) {
        if (!missing) {
            for (unsigned p = 0; p < marks->count; p++) if (marks->ports[p].id == ids[i]) { remove_mark(marks, p); break; }
        } else if (!routing_marks_has(marks, ids[i])) {
            char *value = identity(ids[i]); if (!value) continue;
            marks->ports = xreallocarray(marks->ports, marks->count + 1, sizeof(*marks->ports));
            marks->ports[marks->count++] = (struct routing_mark){.id = ids[i], .identity = value};
        }
    }
    return true;
}
bool routing_marks_prune(struct routing_marks *marks) {
    bool changed = false;
    for (unsigned i = 0; i < marks->count;) {
        char *value = identity(marks->ports[i].id); bool keep = value && streq(value, marks->ports[i].identity); free(value);
        if (keep) i++; else { remove_mark(marks, i); changed = true; }
    }
    return changed;
}
void routing_marks_clear(struct routing_marks *marks) {
    for (unsigned i = 0; i < marks->count; i++) free(marks->ports[i].identity);
    free(marks->ports); *marks = (struct routing_marks){0};
}
static bool channels_match(uint32_t output, uint32_t input, bool single) {
    const struct graph_port *out = graph_port_find(output), *in = graph_port_find(input);
    const char *left = out ? dict_get(&out->props, PW_KEY_AUDIO_CHANNEL) : NULL,
               *right = in ? dict_get(&in->props, PW_KEY_AUDIO_CHANNEL) : NULL;
    return left && right ? streq(left, right) : single && !left && !right;
}
int routing_batch_start(struct routing_batch *batch, const struct routing_marks *outputs,
                        const struct routing_marks *inputs, bool disconnect) {
    if (batch->active) return -EBUSY;
    if (!outputs->count || !inputs->count) return -EINVAL;
    free(batch->pairs); *batch = (struct routing_batch){.disconnect = disconnect};
    bool used[PORT_LIMIT] = {0};
    for (unsigned o = 0; o < outputs->count; o++) {
        unsigned before = batch->count;
        for (unsigned i = 0; i < inputs->count; i++) {
            uint32_t out = outputs->ports[o].id, in = inputs->ports[i].id;
            if (!channels_match(out, in, outputs->count == 1 && inputs->count == 1)) continue;
            if (batch->count == PAIR_LIMIT) return -E2BIG;
            batch->pairs = xreallocarray(batch->pairs, batch->count + 1, sizeof(*batch->pairs));
            batch->pairs[batch->count++] = (struct graph_pair){out, in}; used[i] = true;
        }
        if (batch->count == before) batch->skipped++;
    }
    for (unsigned i = 0; i < inputs->count; i++) if (!used[i]) batch->skipped++;
    if (!batch->count) return -ENOTSUP;
    int result = disconnect ? 0 : graph_validate_connections(batch->pairs, batch->count);
    if (result < 0) return result;
    static unsigned sequence;
    snprintf(batch->tag, sizeof(batch->tag), "%ld:%" PRIu64 ":%u", (long)getpid(), now(), sequence++);
    batch->active = true;
    snprintf(batch->message, sizeof(batch->message), tr("Batch %s: 0/%u (%u ports without channel partners)"), tr(disconnect ? "disconnect" : "connect"), batch->count, batch->skipped);
    return 0;
}
static void remove_owned(const struct graph_link *link, void *data) {
    struct routing_batch *batch = data;
    if (link->info && link->info->props && streq(spa_dict_lookup(link->info->props, "pipemixer.batch"), batch->tag)) graph_destroy_link(link->id);
}
static void progress(struct routing_batch *batch) {
    snprintf(batch->message, sizeof(batch->message), tr("Batch %s: %u/%u | Esc cancels"),
        tr(batch->disconnect ? "disconnect" : "connect"), batch->position, batch->count);
}
void routing_batch_abort(struct routing_batch *batch, const char *reason) {
    if (!batch->active || batch->rollback) return;
    snprintf(batch->message, sizeof(batch->message), tr("Batch %s after %u/%u: %.130s%s"), batch->disconnect ? tr("stopped") : tr("rolled back"),
        batch->position, batch->count, tr(reason), batch->disconnect ? "" : tr("; existing links retained"));
    batch->rollback = true; batch->deadline = now() + 1000;
    if (!batch->disconnect) graph_foreach_link(remove_owned, batch);
}
bool routing_batch_step(struct routing_batch *batch) {
    if (!batch->active) return false;
    if (batch->rollback) {
        if (!batch->disconnect) graph_foreach_link(remove_owned, batch);
        if (now() >= batch->deadline) batch->active = false;
        return true;
    }
    if (batch->position == batch->count) {
        batch->active = false;
        snprintf(batch->message, sizeof(batch->message), tr("Batch %s complete: %u pairs (%u already %s, %u ports without partners)%s"),
            tr(batch->disconnect ? "disconnect" : "connect"), batch->count, batch->unchanged,
            tr(batch->disconnect ? "absent" : "connected"), batch->skipped,
            batch->disconnect ? tr("; enabled rules may reconnect") : "");
        return true;
    }
    const struct graph_pair *pair = &batch->pairs[batch->position];
    if (!graph_port_find(pair->output) || !graph_port_find(pair->input)) { routing_batch_abort(batch, tr("endpoint disappeared")); return true; }
    const struct graph_link *link = graph_link_between(pair->output, pair->input);
    if (batch->waiting) {
        if ((!batch->disconnect && link && link->info->state >= PW_LINK_STATE_PAUSED)
            || (batch->disconnect && !link)) { batch->position++; batch->waiting = false; progress(batch); }
        else if ((!batch->disconnect && link && link->info->state == PW_LINK_STATE_ERROR) || now() >= batch->deadline) {
            routing_batch_abort(batch, link && link->info->error ? link->info->error : tr("connection timed out"));
        }
        return true;
    }
    if ((!batch->disconnect && link && link->info->state >= PW_LINK_STATE_PAUSED) || (batch->disconnect && !link)) {
        batch->unchanged++; batch->position++; progress(batch); return true;
    }
    int result = batch->disconnect ? graph_disconnect(pair->output, pair->input) : graph_connect_batch(pair->output, pair->input, batch->tag);
    if (result < 0) routing_batch_abort(batch, result == -ELOOP ? tr("feedback loop") : strerror(-result));
    else { batch->waiting = true; batch->deadline = now() + 3000; }
    return true;
}
void routing_batch_clear(struct routing_batch *batch) {
    if (batch->active && !batch->disconnect) graph_foreach_link(remove_owned, batch);
    free(batch->pairs); *batch = (struct routing_batch){0};
}
