#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ROUTE_FALLBACK_LIMIT 8

struct route_rule {
    char *name, *output, *input, *group;
    bool enabled, glob;
    unsigned version;
    int priority;
    char **fallbacks;
    unsigned n_fallbacks, switch_delay_ms;
};

struct route_rule_state {
    const char *state, *reason;
    uint32_t output, input;
    unsigned matched_outputs, matched_inputs, total_pairs, connected_pairs;
    unsigned suppressed_pairs;
    bool using_fallback;
    int active_target;
};

char *route_rules_directory(void);
char *route_rules_scope(void);
bool route_rule_valid_endpoint(const char *endpoint);
int route_rules_load(struct route_rule **rules, unsigned *count, char *error, size_t size);
void route_rules_free(struct route_rule *rules, unsigned count);
int route_rule_create(const char *name, const char *output, const char *input, char *error, size_t size);
int route_rule_create_glob(const char *name, const char *output, const char *input, bool glob, char *error, size_t size);
int route_rule_create_config(const struct route_rule *rule, char *error, size_t size);
int route_rule_set_priority(const char *name, int priority, const char *group, char *error, size_t size);
int route_rule_set_fallbacks(const char *name, const char *const *fallbacks, unsigned count, int delay, char *error, size_t size);
int route_rule_enable(const char *name, bool enabled, char *error, size_t size);
int route_rule_delete(const char *name, char *error, size_t size);
struct route_rule_state route_rule_observe(const struct route_rule *rule);
void route_rules_observe(const struct route_rule *rules, unsigned count, struct route_rule_state *states);
bool route_rules_running(void);
void route_rules_print(const struct route_rule *rules, unsigned count, bool json);
/* Foreground engine; the board audio supervisor keeps it running. */
int route_rules_run(void);
