#pragma once

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-types.h>

#include "events.h"
#include "pw/types.h"
#include "collections/dict.h"

enum media_class {
    MEDIA_CLASS_START,
    STREAM_OUTPUT_AUDIO,
    STREAM_INPUT_AUDIO,
    AUDIO_SOURCE,
    AUDIO_SINK,
    MEDIA_CLASS_END,
};

struct node;
struct device;

struct node *node_create(struct pw_node *pw_node, uint32_t id,
                         enum media_class media_class, const char *serial);

struct node *node_ref(struct node *node);
void node_unref(struct node **pnode);

uint32_t node_id(const struct node *node);
enum media_class node_media_class(const struct node *node);
const char *node_meter_target(const struct node *node);
const struct dict *node_properties(const struct node *node);

#define ALL_CHANNELS ((uint32_t)-1)

void node_set_mute(const struct node *node, bool mute);
/* Borrowed current values, or NULL until the first Props response. */
const struct param_props *node_get_params(const struct node *node);
int node_set_volumes(const struct node *node, const float volumes[], unsigned count);
void node_change_volume(const struct node *node, bool absolute, float volume, uint32_t channel);
int node_set_route(const struct node *node, uint32_t route_index);
const struct param_route *node_get_routes(const struct node *node, unsigned *count);
bool node_routes_ready(const struct node *node);
/* Device globals can arrive after their node's first info response. */
void node_bind_device(struct node *node, struct device *device);
bool node_set_default(const struct node *node);

struct node_events {
    void (*removed)(struct node *node, void *data);
    void (*routes)(struct node *node,
                   const struct param_route routes[], unsigned routes_count,
                   void *data);
    void (*props)(struct node *node, const struct dict *props, void *data);
    void (*channels)(struct node *node,
                     const char *channel_names[], unsigned channel_count,
                     void *data);
    void (*volume)(struct node *node,
                   const float channel_volumes[], unsigned channel_count,
                   void *data);
    void (*mute)(struct node *node, bool mute, void *data);
    void (*default_)(struct node *node, bool is_default, void *data);
};

struct event_hook *node_add_listener(struct node *node,
                                     const struct node_events *event,
                                     void *data);
