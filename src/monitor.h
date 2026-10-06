#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "route-rules.h"

#define MONITOR_LIMIT 32
#define MONITOR_SOURCE_LIMIT 32
struct monitor {
    char *name, *destination, *listen;
    char **sources, **solo;
    unsigned n_sources, n_solo;
    bool enabled;
};
enum monitor_action {
    MONITOR_CREATE, MONITOR_DELETE, MONITOR_ENABLE, MONITOR_SOURCES,
    MONITOR_LISTEN, MONITOR_SOLO_ON, MONITOR_SOLO_OFF, MONITOR_SOLO_TOGGLE, MONITOR_SOLO_CLEAR,
};
struct monitor_worker { char name[33]; pid_t pid; unsigned retry; };
struct monitor_runtime { struct monitor_worker workers[MONITOR_LIMIT]; };
bool monitor_valid_name(const char *name);
bool monitor_valid_node(const char *name);
bool monitor_valid_scope(const char *scope);
int monitors_load(struct monitor **monitors, unsigned *count, char *error, size_t size);
void monitors_free(struct monitor *monitors, unsigned count);
int monitor_edit(const char *name, enum monitor_action action, const char *value,
                 const char *const *sources, unsigned count, char *error, size_t size);
int monitors_append_rules(const struct monitor *monitors, unsigned count, struct route_rule **rules,
                          unsigned *n_rules, char *error, size_t size);
void monitors_reconcile(struct monitor_runtime *runtime, const struct monitor *monitors, unsigned count,
                        struct route_rule *rules, unsigned n_rules, const char *scope);
void monitors_runtime_clear(struct monitor_runtime *runtime);
const char *monitor_mode(const struct monitor *monitor);
const char *monitor_state(const struct monitor *monitor, unsigned *connected, unsigned *total);
void monitors_print(const struct monitor *monitors, unsigned count, bool json);
