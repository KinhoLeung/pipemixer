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
    unsigned rate;
    uint64_t frames, window_frames;
    unsigned stale;
    double sum[SPA_AUDIO_MAX_CHANNELS], total_sum[SPA_AUDIO_MAX_CHANNELS];
    float rms[SPA_AUDIO_MAX_CHANNELS], hold[SPA_AUDIO_MAX_CHANNELS], maximum[SPA_AUDIO_MAX_CHANNELS];
    unsigned hold_ticks[SPA_AUDIO_MAX_CHANNELS];
    uint64_t clipped[SPA_AUDIO_MAX_CHANNELS], invalid[SPA_AUDIO_MAX_CHANNELS];
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
    meter->rate = format.rate;
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
                    meter->frames++; meter->window_frames++;
                    for (unsigned channel = 0; channel < meter->n_channels; channel++) {
                        const float value = fabsf(pcm[i + channel]);
                        if (!isfinite(value)) { meter->invalid[channel]++; continue; }
                        if (value >= 1) meter->clipped[channel]++;
                        meter->sum[channel] += (double)value * value;
                        meter->total_sum[channel] += (double)value * value;
                        meter->maximum[channel] = fmaxf(meter->maximum[channel], value);
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
        "state.restore-props", "false", "state.restore-target", "false",
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
    meter->stale = meter->window_frames ? 0 : meter->stale + 1;

    for (unsigned i = 0; i < meter->n_channels; i++) {
        /* About 20 dB/s of falloff at a 50 ms display interval. */
        float level = fmaxf(meter->pending[i], meter->displayed[i] * 0.89f);
        if (level < 0.001f) {
            level = 0;
        }
        changed |= level != meter->displayed[i];
        meter->displayed[i] = level;
        meter->rms[i] = meter->window_frames ? sqrt(meter->sum[i] / meter->window_frames) : 0;
        if (meter->pending[i] >= meter->hold[i]) { meter->hold[i] = meter->pending[i]; meter->hold_ticks[i] = 40; }
        else if (meter->hold_ticks[i]) meter->hold_ticks[i]--;
        else meter->hold[i] *= .89f;
        meter->sum[i] = 0;
        meter->pending[i] = 0;
    }

    meter->window_frames = 0;
    return changed;
}

void peak_meter_snapshot(const struct peak_meter *meter, struct meter_snapshot *s, bool cumulative) {
    *s = (struct meter_snapshot){0}; if (!meter) return;
    s->channels = meter->n_channels; s->rate = meter->rate; s->frames = meter->frames;
    s->failed = meter->failed; s->active = meter->frames && meter->stale < 20;
    memcpy(s->positions, meter->positions, sizeof(s->positions));
    for (unsigned i = 0; i < s->channels; i++) s->channel[i] = (struct meter_channel){
        .peak = cumulative ? meter->maximum[i] : meter->displayed[i],
        .rms = cumulative ? (meter->frames ? sqrt(meter->total_sum[i] / meter->frames) : 0) : meter->rms[i],
        .hold = meter->hold[i], .clipped = meter->clipped[i], .invalid = meter->invalid[i]};
}
void peak_meter_reset(struct peak_meter *meter) {
    if (!meter) return;
    meter->frames = meter->window_frames = 0;
    memset(meter->sum, 0, sizeof(meter->sum)); memset(meter->total_sum, 0, sizeof(meter->total_sum));
    memset(meter->maximum, 0, sizeof(meter->maximum)); memset(meter->hold, 0, sizeof(meter->hold));
    memset(meter->clipped, 0, sizeof(meter->clipped)); memset(meter->invalid, 0, sizeof(meter->invalid));
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
