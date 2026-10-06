#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define DIAGNOSTIC_NODES 256
#define DIAGNOSTIC_EVENTS 32
struct diagnostic_node {
    uint32_t id, driver, rate, quantum, xruns;
    uint64_t measured_ns;
    double wait_us, busy_us, cycle_ms, load_percent;
    bool used, xrun_known;
};
struct diagnostic_event { double time; char message[192]; };
struct diagnostics {
    void *private;
    struct diagnostic_node nodes[DIAGNOSTIC_NODES];
    struct diagnostic_event events[DIAGNOSTIC_EVENTS];
    unsigned n_events, node_count, link_count, error_count;
    bool profiler_available, failed;
    double cpu_percent, self_cpu_percent, audio_cpu_percent;
    uint64_t rss_kb, available_kb, total_kb;
};
struct diagnostics *diagnostics_create(void);
void diagnostics_destroy(struct diagnostics *d);
void diagnostics_sample(struct diagnostics *d);
void diagnostics_reset(struct diagnostics *d);
void diagnostics_print(struct diagnostics *d, bool json);
int diagnostics_cli(const char *target, unsigned duration_ms, bool json, unsigned timeout_ms);
uint64_t diagnostics_now_ns(void);
