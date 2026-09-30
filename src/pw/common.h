#pragma once

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-types.h>

#include "events.h"
#include "pw/node.h"
#include "pw/device.h"
#include "pw/types.h"

enum default_metadata_key {
    DEFAULT_AUDIO_SOURCE,
    DEFAULT_CONFIGURED_AUDIO_SOURCE,
    DEFAULT_AUDIO_SINK,
    DEFAULT_CONFIGURED_AUDIO_SINK,

    DEFAULT_METADATA_KEY_COUNT,
};

bool pipewire_init(void);
void pipewire_cleanup(void);

struct node *node_lookup(pw_id_t id);
struct device *device_lookup(pw_id_t id);

bool pipewire_set_default(enum default_metadata_key key, const char *value);
bool pipewire_default_available(void);
int pipewire_sync(void);

/* A stream target is a sink for playback or a source for recording.
 * PW_ID_ANY restores following the system default. */
bool pipewire_set_stream_target(uint32_t stream_id, uint32_t target_id);
uint32_t pipewire_get_stream_target(uint32_t stream_id);
bool pipewire_stream_target_matches(uint32_t stream_id, uint32_t target_id);
void pipewire_foreach_node(void (*callback)(struct node *node, void *data), void *data);

struct pipewire_events {
    void (*node)(struct node *node, void *data);
    void (*device)(struct device *dev, void *data);
    void (*default_)(enum default_metadata_key key, const char *val, void *data);
    void (*stream_target)(uint32_t stream_id, void *data);
    void (*sync)(int seq, void *data);
    void (*error)(int code, const char *message, void *data);
};

struct event_hook *pipewire_add_listener(const struct pipewire_events *events, void *data);

