#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/raw-types.h>

#include "pw/peak.h"
#include "eventloop.h"
#include "log.h"
#include "xmalloc.h"

struct peak_meter {
    struct pw_stream *stream;
    unsigned n_channels;
    bool positioned;
    bool dirty, failed;
    uint32_t positions[SPA_AUDIO_MAX_CHANNELS];
    float pending[SPA_AUDIO_MAX_CHANNELS];
    float displayed[SPA_AUDIO_MAX_CHANNELS];
};

static void on_format_changed(void *data, uint32_t id, const struct spa_pod *param) {
    struct peak_meter *meter = data;

    if (id != SPA_PARAM_Format) {
        return;
    }

    meter->n_channels = 0;
    meter->dirty = true;
    memset(meter->pending, 0, sizeof(meter->pending));
    memset(meter->displayed, 0, sizeof(meter->displayed));

    if (!param) {
        return;
    }

    struct spa_audio_info_raw format = {0};
    if (spa_format_audio_raw_parse(param, &format) < 0
        || format.format != SPA_AUDIO_FORMAT_F32
        || format.channels == 0 || format.channels > SPA_AUDIO_MAX_CHANNELS) {
        meter->failed = true;
        WARN("peak meter received an unsupported audio format");
        return;
    }

    meter->failed = false;
    meter->n_channels = format.channels;
    meter->positioned = !(format.flags & SPA_AUDIO_FLAG_UNPOSITIONED);
    memcpy(meter->positions, format.position,
           meter->n_channels * sizeof(meter->positions[0]));
}

static void on_process(void *data) {
    struct peak_meter *meter = data;
    struct pw_buffer *buffer;

    while ((buffer = pw_stream_dequeue_buffer(meter->stream))) {
        const struct spa_buffer *spa_buffer = buffer->buffer;

        if (meter->n_channels && spa_buffer->n_datas) {
            const struct spa_data *audio = &spa_buffer->datas[0];
            const struct spa_chunk *chunk = audio->chunk;

            if (audio->data && chunk && chunk->offset < audio->maxsize) {
                const size_t available = audio->maxsize - chunk->offset;
                const size_t bytes = chunk->size < available ? chunk->size : available;
                const size_t samples = bytes / sizeof(float);
                const float *pcm = (const float *)((const uint8_t *)audio->data + chunk->offset);

                /* F32 is interleaved; ignore an incomplete frame at the end. */
                for (size_t i = 0; i + meter->n_channels <= samples; i += meter->n_channels) {
                    for (unsigned channel = 0; channel < meter->n_channels; channel++) {
                        const float value = fabsf(pcm[i + channel]);
                        if (isfinite(value) && value > meter->pending[channel]) {
                            meter->pending[channel] = value;
                        }
                    }
                }
            }
        }

        pw_stream_queue_buffer(meter->stream, buffer);
    }
}

static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error) {
    if (state == PW_STREAM_STATE_ERROR) {
        struct peak_meter *meter = data;
        meter->failed = true;
        meter->dirty = true;
        WARN("peak meter stream failed: %s", error ?: "unknown error");
    }
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .param_changed = on_format_changed,
    .process = on_process,
    .state_changed = on_state_changed,
};

struct peak_meter *peak_meter_create(const char *target, bool capture_sink, bool passive) {
    if (!target) {
        return NULL;
    }

    struct peak_meter *meter = xcalloc(1, sizeof(*meter));
    char *name;
    xasprintf(&name, PEAK_METER_NODE_PREFIX "%s", target);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_NODE_NAME, name,
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_TARGET_OBJECT, target,
        PW_KEY_NODE_PASSIVE, passive ? "true" : "false",
        PW_KEY_NODE_DONT_RECONNECT, "true",
        "node.dont-fallback", "true",
        PW_KEY_STREAM_MONITOR, "true",
        PEAK_METER_NODE_PROPERTY, "true",
        NULL);
    free(name);
    if (!props) {
        WARN("failed to allocate peak meter properties");
        free(meter);
        return NULL;
    }

    if (capture_sink) {
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    }

    meter->stream = pw_stream_new_simple(event_loop, "pipemixer peak meter",
                                         props, &stream_events, meter);
    if (!meter->stream) {
        WARN("failed to create peak meter stream");
        free(meter);
        return NULL;
    }

    uint8_t pod_buffer[256];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    const struct spa_pod *format = spa_format_audio_raw_build(
        &builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32));

    if (pw_stream_connect(meter->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_DONT_RECONNECT,
                          &format, 1) < 0) {
        WARN("failed to connect peak meter to %s", target);
        peak_meter_destroy(meter);
        return NULL;
    }

    return meter;
}

void peak_meter_destroy(struct peak_meter *meter) {
    if (!meter) {
        return;
    }
    if (meter->stream) {
        pw_stream_destroy(meter->stream);
    }
    free(meter);
}

bool peak_meter_step(struct peak_meter *meter) {
    bool changed = meter->dirty;
    meter->dirty = false;

    for (unsigned i = 0; i < meter->n_channels; i++) {
        /* About 20 dB/s of falloff at a 50 ms display interval. */
        float level = fmaxf(meter->pending[i], meter->displayed[i] * 0.89f);
        if (level < 0.001f) {
            level = 0;
        }
        changed |= level != meter->displayed[i];
        meter->displayed[i] = level;
        meter->pending[i] = 0;
    }

    return changed;
}

bool peak_meter_failed(const struct peak_meter *meter) {
    return meter->failed;
}

float peak_meter_level(const struct peak_meter *meter, const char *channel_name,
                       unsigned channel_index) {
    if (!meter) {
        return 0;
    }

    if (meter->positioned && channel_name) {
        for (unsigned i = 0; i < meter->n_channels; i++) {
            const char *name = spa_type_audio_channel_to_short_name(meter->positions[i]);
            if (name && strcmp(name, channel_name) == 0) {
                return meter->displayed[i];
            }
        }
    }

    return channel_index < meter->n_channels ? meter->displayed[channel_index] : 0;
}
