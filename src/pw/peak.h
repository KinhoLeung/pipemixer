#pragma once

#include <stdbool.h>

/* Mark our capture streams so the registry does not show them as recording apps. */
#define PEAK_METER_NODE_PROPERTY "pipemixer.peak-meter"
#define PEAK_METER_NODE_PREFIX "pipemixer.peak."

struct peak_meter;

struct peak_meter *peak_meter_create(const char *target, bool capture_sink, bool passive);
void peak_meter_destroy(struct peak_meter *meter);

/* Advance the meter by one 50 ms display interval. Returns true if it changed. */
bool peak_meter_step(struct peak_meter *meter);
bool peak_meter_failed(const struct peak_meter *meter);
float peak_meter_level(const struct peak_meter *meter, const char *channel_name,
                       unsigned channel_index);
