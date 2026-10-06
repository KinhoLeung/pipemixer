#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <spa/param/audio/raw.h>

/* Mark our capture streams so the registry does not show them as recording apps. */
#define PEAK_METER_NODE_PROPERTY "pipemixer.peak-meter"
#define PEAK_METER_NODE_PREFIX "pipemixer.peak."

struct peak_meter;
struct meter_channel { float peak, rms, hold; uint64_t clipped, invalid; };
struct meter_snapshot {
    unsigned channels, rate;
    uint64_t frames;
    bool failed, active;
    uint32_t positions[SPA_AUDIO_MAX_CHANNELS];
    struct meter_channel channel[SPA_AUDIO_MAX_CHANNELS];
};

struct peak_meter *peak_meter_create(const char *target, bool capture_sink, bool passive);
void peak_meter_destroy(struct peak_meter *meter);

/* Advance the meter by one 50 ms display interval. Returns true if it changed. */
bool peak_meter_step(struct peak_meter *meter);
bool peak_meter_failed(const struct peak_meter *meter);
float peak_meter_level(const struct peak_meter *meter, const char *channel_name,
                       unsigned channel_index);
void peak_meter_snapshot(const struct peak_meter *meter, struct meter_snapshot *snapshot, bool cumulative);
void peak_meter_reset(struct peak_meter *meter);
