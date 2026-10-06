#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <spa/utils/json.h>

#include "route-rules.h"
#include "monitor.h"
#include "scene.h"
#include "eventloop.h"
#include "pw/common.h"
#include "pw/graph.h"
#include "pw/managed.h"
#include "utils.h"
#include "xmalloc.h"

#define RULE_LIMIT 256
#define FILE_LIMIT 65536
#define ENDPOINT_LIMIT 8192
#define MATCH_LIMIT 256
#define PAIR_LIMIT 1024
#define TOTAL_PAIR_LIMIT 8192

static int fail(char *error, size_t size, int code, const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(error, size, format, args); va_end(args);
    return -code;
}

char *route_rules_directory(void) {
    char *path = scene_directory();
    if (path) strcpy(strrchr(path, '/') + 1, "rules");
    return path;
}

static char *filename(const char *name) {
    if (!managed_valid_name(name)) { errno = EINVAL; return NULL; }
    char *directory = route_rules_directory(), *path;
    if (!directory) return NULL;
    xasprintf(&path, "%s/%s.json", directory, name); free(directory); return path;
}

static int make_directory(const char *path) {
    char *copy = xstrdup(path);
    for (char *p = copy + 1; ; p++) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        if (mkdir(copy, 0700) < 0 && errno != EEXIST) { int e = errno; free(copy); return -e; }
        struct stat st;
        if (stat(copy, &st) < 0 || !S_ISDIR(st.st_mode)) { free(copy); return -ENOTDIR; }
        *p = saved; if (!saved) break;
    }
    free(copy); return 0;
}

bool route_rule_valid_endpoint(const char *endpoint) {
    if (!endpoint || strlen(endpoint) > ENDPOINT_LIMIT || !strncmp(endpoint, "id:", 3)
        || !strncmp(endpoint, "serial:", 7)) return false;
    const char *colon = strrchr(endpoint, ':');
    return colon && colon != endpoint && colon[1];
}

static char *string_token(const char *token, int len) {
    if (len < 2 || *token != '"') return NULL;
    for (int i = 0; i + 1 < len; i++) if (token[i] == '\\') {
        if (i + 5 < len && !strncmp(token + i, "\\u0000", 6)) return NULL;
        i++;
    }
    char *string = xcalloc(len + 1, 1);
    if (spa_json_parse_stringn(token, len, string, len + 1) <= 0) { free(string); return NULL; }
    return string;
}

static bool integer_token(const char *token, int len, int minimum, int maximum, int *result) {
    if (len <= 0 || len >= 32) return false;
    int start = token[0] == '-' ? 1 : 0;
    if (start == len) return false;
    for (int i = start; i < len; i++) if (token[i] < '0' || token[i] > '9') return false;
    char buffer[32]; memcpy(buffer, token, len); buffer[len] = 0;
    errno = 0; long value = strtol(buffer, NULL, 10);
    if (errno || value < minimum || value > maximum) return false;
    *result = value; return true;
}
static void clear_fallbacks(struct route_rule *rule) {
    for (unsigned i = 0; i < rule->n_fallbacks; i++) free(rule->fallbacks[i]);
    free(rule->fallbacks); rule->fallbacks = NULL; rule->n_fallbacks = 0;
}
static void free_rule(struct route_rule *rule) {
    free(rule->name); free(rule->output); free(rule->input); free(rule->group); clear_fallbacks(rule);
    *rule = (struct route_rule){0};
}
static bool valid_fallbacks(const struct route_rule *rule) {
    if (rule->n_fallbacks > ROUTE_FALLBACK_LIMIT || rule->switch_delay_ms > 60000) return false;
    for (unsigned i = 0; i < rule->n_fallbacks; i++) {
        if (!route_rule_valid_endpoint(rule->fallbacks[i]) || streq(rule->fallbacks[i], rule->input)) return false;
        for (unsigned j = 0; j < i; j++) if (streq(rule->fallbacks[i], rule->fallbacks[j])) return false;
    }
    return true;
}

static int read_rule(const char *name, struct route_rule *rule, char *error, size_t size) {
    rule->switch_delay_ms = 1000;
    char *path = filename(name); if (!path) return fail(error, size, errno, "cannot locate routing rule");
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK); free(path);
    if (fd < 0) return fail(error, size, errno, "cannot read rule '%s': %s", name, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > FILE_LIMIT) {
        close(fd); return fail(error, size, EINVAL, "rule '%s' is empty, too large or not a regular file", name);
    }
    char *data = xcalloc(st.st_size + 1, 1); size_t n = 0;
    while (n < (size_t)st.st_size) {
        ssize_t got = read(fd, data + n, st.st_size - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += got;
    }
    close(fd);
    struct spa_json iter = SPA_JSON_INIT(data, n), object;
    const char *token; int len; unsigned seen = 0; bool valid = false;
    if (n != (size_t)st.st_size || memchr(data, 0, n) || spa_json_enter_object(&iter, &object) <= 0) goto done;
    while ((len = spa_json_next(&object, &token)) > 0) {
        char *key = string_token(token, len); if (!key) goto done;
        const char *keys[] = {"format", "version", "name", "enabled", "output", "input", "match", "priority", "exclusive_group", "fallbacks", "switch_delay_ms"};
        unsigned field = 0;
        while (field < 11 && !streq(key, keys[field])) field++;
        free(key);
        if (field == 11 || (seen & (1u << field)) || (len = spa_json_next(&object, &token)) <= 0) goto done;
        seen |= 1u << field;
        if (field == 1) { if (len != 1 || (*token != '1' && *token != '2')) goto done; rule->version = *token - '0'; }
        else if (field == 3) { if (spa_json_parse_bool(token, len, &rule->enabled) <= 0) goto done; }
        else if (field == 7) { if (!integer_token(token, len, -1000000, 1000000, &rule->priority)) goto done; }
        else if (field == 10) { int delay; if (!integer_token(token, len, 0, 60000, &delay)) goto done; rule->switch_delay_ms = delay; }
        else if (field == 9) {
            if (!spa_json_is_array(token, len)) goto done;
            struct spa_json array; spa_json_enter(&object, &array);
            while ((len = spa_json_next(&array, &token)) > 0) {
                if (rule->n_fallbacks == ROUTE_FALLBACK_LIMIT) goto done;
                char *value = string_token(token, len);
                if (!value || !route_rule_valid_endpoint(value)) { free(value); goto done; }
                rule->fallbacks = xreallocarray(rule->fallbacks, rule->n_fallbacks + 1, sizeof(char *));
                rule->fallbacks[rule->n_fallbacks++] = value;
            }
            if (len < 0) goto done;
        }
        else {
            char *value = string_token(token, len); if (!value) goto done;
            if (field == 0) { bool ok = streq(value, "pipemixer.route-rule"); free(value); if (!ok) goto done; }
            else if (field == 2) { bool ok = streq(value, name); free(value); if (!ok) goto done; }
            else if (field == 6) { bool ok = streq(value, "exact") || streq(value, "glob"); rule->glob = streq(value, "glob"); free(value); if (!ok) goto done; }
            else if (field == 8) { if (*value && !managed_valid_name(value)) { free(value); goto done; } rule->group = value; }
            else { if (!route_rule_valid_endpoint(value)) { free(value); goto done; }
                   if (field == 4) rule->output = value; else rule->input = value; }
        }
    }
    valid = len == 0 && (rule->version == 1 ? seen == 63u : (seen & 127u) == 127u) && valid_fallbacks(rule) && spa_json_next(&iter, &token) == 0;
done:
    free(data);
    if (!valid) {
        free_rule(rule);
        return fail(error, size, EINVAL, "invalid routing rule '%s'", name);
    }
    rule->name = xstrdup(name); return 0;
}

void route_rules_free(struct route_rule *rules, unsigned count) {
    for (unsigned i = 0; i < count; i++) free_rule(&rules[i]);
    free(rules);
}
static int compare_rules(const void *a, const void *b) {
    const struct route_rule *left = a, *right = b;
    if (left->priority != right->priority) return left->priority > right->priority ? -1 : 1;
    return strcmp(left->name, right->name);
}
int route_rules_load(struct route_rule **rules, unsigned *count, char *error, size_t size) {
    *rules = NULL; *count = 0;
    char *path = route_rules_directory(); if (!path) return fail(error, size, errno, "cannot locate routing rules");
    DIR *directory = opendir(path); free(path);
    if (!directory) return errno == ENOENT ? 0 : fail(error, size, errno, "cannot open routing rule directory");
    struct dirent *entry; int result = 0;
    errno = 0;
    while ((entry = readdir(directory))) {
        size_t len = strlen(entry->d_name);
        if (len < 6 || strcmp(entry->d_name + len - 5, ".json")) continue;
        char *name = strndup(entry->d_name, len - 5);
        if (!managed_valid_name(name)) { free(name); result = fail(error, size, EINVAL, "invalid routing rule filename"); break; }
        if (*count == RULE_LIMIT) { free(name); result = fail(error, size, E2BIG, "too many routing rules (maximum %u)", RULE_LIMIT); break; }
        *rules = xreallocarray(*rules, *count + 1, sizeof(**rules));
        (*rules)[*count] = (struct route_rule){0};
        result = read_rule(name, &(*rules)[*count], error, size); free(name);
        if (result < 0) break;
        (*count)++; errno = 0;
    }
    if (!entry && errno) result = fail(error, size, errno, "cannot enumerate routing rules");
    closedir(directory);
    if (result < 0) { route_rules_free(*rules, *count); *rules = NULL; *count = 0; return result; }
    if (*count > 1) qsort(*rules, *count, sizeof(**rules), compare_rules);
    return 0;
}

static int directory_sync(void) {
    char *path = route_rules_directory(); if (!path) return -errno;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); free(path);
    if (fd < 0) return -errno;
    int result = fsync(fd) < 0 ? -errno : 0; close(fd); return result;
}
static int write_lock(void) {
    char *path = route_rules_directory(); if (!path) return -errno;
    int result = make_directory(path);
    if (result < 0) { free(path); return result; }
    free(path);
    path = scene_directory(); if (!path) return -errno;
    *strrchr(path, '/') = 0;
    char *lock; xasprintf(&lock, "%s/route-rules.lock", path); free(path);
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600); free(lock);
    if (fd < 0) return -errno;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) { int e = errno; close(fd); return -e; }
    return fd;
}
static int write_rule(const struct route_rule *rule, bool create) {
    char *path = filename(rule->name), *temporary = NULL; if (!path) return -errno;
    xasprintf(&temporary, "%s.tmp-XXXXXX", path);
    int fd = mkostemp(temporary, O_CLOEXEC), result = 0;
    if (fd < 0) { result = -errno; goto done; }
    FILE *file = fdopen(fd, "w"); if (!file) { result = -errno; close(fd); goto done; }
    char *name = json_quote(rule->name), *output = json_quote(rule->output), *input = json_quote(rule->input);
    fprintf(file, "{\n  \"format\": \"pipemixer.route-rule\",\n  \"version\": %u,\n  \"name\": %s,\n  \"enabled\": %s,\n  \"output\": %s,\n  \"input\": %s",
            rule->version ?: 1, name, rule->enabled ? "true" : "false", output, input);
    if (rule->version == 2) fprintf(file, ",\n  \"match\": \"%s\"", rule->glob ? "glob" : "exact");
    if (rule->version == 2) {
        char *group = json_quote(rule->group ?: "");
        fprintf(file, ",\n  \"priority\": %d,\n  \"exclusive_group\": %s", rule->priority, group); free(group);
        fputs(",\n  \"fallbacks\": [", file);
        for (unsigned i = 0; i < rule->n_fallbacks; i++) {
            char *value = json_quote(rule->fallbacks[i]); fprintf(file, "%s%s", i ? ", " : "", value); free(value);
        }
        fprintf(file, "],\n  \"switch_delay_ms\": %u", rule->switch_delay_ms);
    }
    fputs("\n}\n", file);
    free(name); free(output); free(input);
    if (ferror(file) || fflush(file) || fsync(fd)) result = -(errno ?: EIO);
    if (!result && ftell(file) > FILE_LIMIT) result = -E2BIG;
    if (fclose(file) && !result) result = -errno;
    if (!result && (create ? link(temporary, path) : rename(temporary, path)) < 0) result = -errno;
    if (!result) result = directory_sync();
done:
    if (temporary) unlink(temporary);
    free(path); free(temporary); return result;
}
int route_rule_create_config(const struct route_rule *rule, char *error, size_t size) {
    const char *name = rule->name;
    if (!managed_valid_name(name) || !route_rule_valid_endpoint(rule->output) || !route_rule_valid_endpoint(rule->input)
        || (rule->group && *rule->group && !managed_valid_name(rule->group)) || rule->priority < -1000000 || rule->priority > 1000000 || !valid_fallbacks(rule))
        return fail(error, size, EINVAL, "use a valid rule name and node.name:port.name endpoints");
    int lock = write_lock(); if (lock < 0) return fail(error, size, -lock, "cannot lock routing rules: %s", strerror(-lock));
    struct route_rule *rules = NULL; unsigned count;
    int result = route_rules_load(&rules, &count, error, size); route_rules_free(rules, count);
    if (!result && count >= RULE_LIMIT) result = -E2BIG;
    if (!result) result = write_rule(rule, true);
    close(lock);
    if (result < 0 && !error[0]) fail(error, size, -result, "cannot create rule '%s': %s", name, strerror(-result));
    return result;
}
int route_rule_create_glob(const char *name, const char *output, const char *input, bool glob, char *error, size_t size) {
    return route_rule_create_config(&(struct route_rule){.name = (char *)name, .output = (char *)output,
        .input = (char *)input, .enabled = true, .glob = glob, .version = glob ? 2 : 1, .switch_delay_ms = 1000}, error, size);
}
int route_rule_create(const char *name, const char *output, const char *input, char *error, size_t size) {
    return route_rule_create_glob(name, output, input, false, error, size);
}
int route_rule_enable(const char *name, bool enabled, char *error, size_t size) {
    int lock = write_lock(); if (lock < 0) return fail(error, size, -lock, "cannot lock routing rules");
    struct route_rule rule = {0}; int result = read_rule(name, &rule, error, size);
    if (!result) { rule.enabled = enabled; result = write_rule(&rule, false); }
    free_rule(&rule); close(lock);
    if (result < 0 && !error[0]) fail(error, size, -result, "cannot update rule: %s", strerror(-result));
    return result;
}
int route_rule_set_priority(const char *name, int priority, const char *group, char *error, size_t size) {
    if (priority < -1000000 || priority > 1000000 || (group && !managed_valid_name(group)))
        return fail(error, size, EINVAL, "use priority -1000000..1000000 and a valid group name");
    int lock = write_lock(); if (lock < 0) return fail(error, size, -lock, "cannot lock routing rules");
    struct route_rule rule = {0}; int result = read_rule(name, &rule, error, size);
    if (!result) {
        rule.priority = priority; rule.version = 2;
        if (group) { free(rule.group); rule.group = streq(group, "off") ? NULL : xstrdup(group); }
        result = write_rule(&rule, false);
    }
    free_rule(&rule); close(lock);
    if (result < 0 && !error[0]) fail(error, size, -result, "cannot update priority: %s", strerror(-result));
    return result;
}
int route_rule_set_fallbacks(const char *name, const char *const *fallbacks, unsigned count, int delay, char *error, size_t size) {
    if (count > ROUTE_FALLBACK_LIMIT || delay < -1 || delay > 60000) return fail(error, size, EINVAL, "invalid fallback count or switch delay");
    int lock = write_lock(); if (lock < 0) return fail(error, size, -lock, "cannot lock routing rules");
    struct route_rule rule = {0}; int result = read_rule(name, &rule, error, size);
    if (!result) {
        rule.version = 2;
        if (delay >= 0) rule.switch_delay_ms = delay;
        if (fallbacks) {
            clear_fallbacks(&rule); rule.fallbacks = xcalloc(count ?: 1, sizeof(char *));
            for (unsigned i = 0; i < count; i++) rule.fallbacks[rule.n_fallbacks++] = xstrdup(fallbacks[i]);
        }
        result = valid_fallbacks(&rule) ? write_rule(&rule, false) : -EINVAL;
    }
    free_rule(&rule); close(lock);
    if (result < 0 && !error[0]) fail(error, size, -result, "cannot update fallbacks: %s", strerror(-result));
    return result;
}
int route_rule_delete(const char *name, char *error, size_t size) {
    char *path = filename(name); if (!path) return fail(error, size, errno, "invalid rule name");
    int lock = write_lock(); if (lock < 0) { free(path); return fail(error, size, -lock, "cannot lock routing rules"); }
    int result = unlink(path) < 0 && errno != ENOENT ? -errno : 0;
    free(path); if (!result) result = directory_sync(); close(lock);
    if (result < 0) fail(error, size, -result, "cannot delete rule: %s", strerror(-result));
    return result;
}

struct route_pair { uint32_t output, input; const char *state; bool selected, available; unsigned target; };
struct rule_plan {
    struct route_pair *pairs;
    uint32_t *output_ids;
    unsigned count, outputs, inputs, unmatched;
    unsigned suppressed;
    const char *state, *reason;
};
struct port_matches {
    const char *pattern;
    enum pw_direction direction;
    uint32_t *ids;
    unsigned count;
    bool overflow, ambiguous;
};
struct route_choice {
    char *rule, *node;
    unsigned current, desired;
    uint64_t since, retry_until[ROUTE_FALLBACK_LIMIT + 1], seen;
    struct route_choice *next;
};
struct planner_context { char *scope; struct route_choice **choices; uint64_t now; };
char *route_rules_scope(void);
static uint64_t milliseconds(void);
static char *port_name(uint32_t id) {
    const struct graph_port *port = graph_port_find(id); char *name;
    xasprintf(&name, "%s:%s", graph_node_name(port->node_id) ?: "", dict_get(&port->props, PW_KEY_PORT_NAME) ?: "");
    return name;
}
static void match_port(const struct graph_port *port, void *data) {
    struct port_matches *matches = data;
    if (port->direction != matches->direction || !graph_port_is_audio(port) || !graph_node_name(port->node_id)) return;
    char *name = port_name(port->id);
    bool match = fnmatch(matches->pattern, name, 0) == 0;
    if (match) {
        uint32_t unique;
        if (graph_resolve_port(name, matches->direction, &unique) == -ENOTUNIQ) matches->ambiguous = true;
        if (matches->count == MATCH_LIMIT) matches->overflow = true;
        else { matches->ids = xreallocarray(matches->ids, matches->count + 1, sizeof(uint32_t)); matches->ids[matches->count++] = port->id; }
    }
    free(name);
}
static int compare_ports(const void *a, const void *b) {
    char *left = port_name(*(const uint32_t *)a), *right = port_name(*(const uint32_t *)b);
    int result = strcmp(left, right); free(left); free(right); return result;
}
static int collect_ports(const char *pattern, bool glob, enum pw_direction direction, struct port_matches *matches) {
    *matches = (struct port_matches){.pattern = pattern, .direction = direction};
    if (glob) {
        graph_foreach_port(match_port, matches);
        if (matches->overflow) return -E2BIG;
        if (matches->ambiguous) return -ENOTUNIQ;
        if (matches->count > 1) qsort(matches->ids, matches->count, sizeof(uint32_t), compare_ports);
        return 0;
    }
    uint32_t id; int result = graph_resolve_port(pattern, direction, &id);
    if (result == -ENOENT) return 0;
    if (result < 0) return result;
    matches->ids = xmalloc(sizeof(uint32_t)); matches->ids[0] = id; matches->count = 1; return 0;
}
static const char *pair_state(uint32_t output, uint32_t input) {
    const struct graph_link *link = graph_link_between(output, input);
    if (link) return link->info->state == PW_LINK_STATE_ERROR ? "error" : link->info->state >= PW_LINK_STATE_PAUSED ? "connected" : "connecting";
    if (graph_would_cycle(graph_port_find(output)->node_id, graph_port_find(input)->node_id)) return "blocked";
    return "waiting";
}
static bool channels_match(const struct graph_port *output, const struct graph_port *input, bool single) {
    const char *out = dict_get(&output->props, PW_KEY_AUDIO_CHANNEL), *in = dict_get(&input->props, PW_KEY_AUDIO_CHANNEL);
    return out && in ? streq(out, in) : single && !out && !in;
}
static struct rule_plan plan_target(const struct route_rule *rule, const char *target) {
    struct rule_plan plan = {0};
    if (!rule->enabled) { plan.state = "disabled"; plan.reason = "rule is disabled"; return plan; }
    struct port_matches outputs = {0}, inputs = {0};
    int result = collect_ports(rule->output, rule->glob, PW_DIRECTION_OUTPUT, &outputs);
    if (!result) result = collect_ports(target, rule->glob, PW_DIRECTION_INPUT, &inputs);
    plan.outputs = outputs.count; plan.inputs = inputs.count;
    plan.output_ids = outputs.ids; outputs.ids = NULL;
    if (result < 0) {
        plan.state = result == -ENOTUNIQ ? "ambiguous" : "error";
        plan.reason = result == -ENOTUNIQ ? "endpoint name is ambiguous" : "endpoint match limit exceeded";
        goto done;
    }
    if (!outputs.count || !inputs.count) { plan.state = "waiting"; plan.reason = "endpoint is absent"; goto done; }
    for (unsigned o = 0; o < outputs.count; o++) {
        unsigned before = plan.count;
        for (unsigned i = 0; i < inputs.count; i++) {
            uint32_t out = plan.output_ids[o], in = inputs.ids[i];
            if (rule->glob && !channels_match(graph_port_find(out), graph_port_find(in), outputs.count == 1 && inputs.count == 1)) continue;
            if (plan.count == PAIR_LIMIT) {
                free(plan.pairs); plan.pairs = NULL; plan.count = 0;
                plan.state = "error"; plan.reason = "connection expansion limit exceeded"; goto done;
            }
            plan.pairs = xreallocarray(plan.pairs, plan.count + 1, sizeof(*plan.pairs));
            plan.pairs[plan.count++] = (struct route_pair){.output = out, .input = in, .state = pair_state(out, in), .selected = true, .available = true};
        }
        if (plan.count == before) plan.unmatched++;
    }
    if (!plan.count) { plan.state = "blocked"; plan.reason = "matched ports have no compatible audio channels"; }
done:
    free(outputs.ids); free(inputs.ids); return plan;
}
static bool eligible_for_node(const struct rule_plan *plan, uint32_t node) {
    if (plan->state) return false;
    bool matches = false;
    for (unsigned o = 0; o < plan->outputs; o++) {
        uint32_t output = plan->output_ids[o];
        if (graph_port_find(output)->node_id != node) continue;
        matches = true; bool covered = false;
        for (unsigned p = 0; p < plan->count; p++) {
            const struct route_pair *pair = &plan->pairs[p];
            if (pair->available && pair->output == output && !streq(pair->state, "blocked") && !streq(pair->state, "error")) covered = true;
        }
        if (!covered) return false;
    }
    return matches;
}
static struct route_choice *choice_for(struct planner_context *context, const char *rule, uint32_t node) {
    if (!context->choices) return NULL;
    const char *name = graph_node_name(node); unsigned count = 0;
    for (struct route_choice *c = *context->choices; c; c = c->next) {
        count++;
        if (streq(c->rule, rule) && streq(c->node, name)) { c->seen = context->now; return c; }
    }
    if (count >= TOTAL_PAIR_LIMIT) return NULL;
    struct route_choice *choice = xcalloc(1, sizeof(*choice));
    choice->rule = xstrdup(rule); choice->node = xstrdup(name);
    choice->current = choice->desired = UINT_MAX; choice->seen = context->now;
    choice->next = *context->choices; *context->choices = choice; return choice;
}
static bool has_owned_pair(const struct rule_plan *plan, uint32_t node, const char *rule, const char *scope) {
    for (unsigned p = 0; p < plan->count; p++) {
        const struct route_pair *pair = &plan->pairs[p];
        if (graph_port_find(pair->output)->node_id != node || !streq(pair->state, "connected")) continue;
        const struct graph_link *link = graph_link_between(pair->output, pair->input);
        if (link && link->info->props && streq(spa_dict_lookup(link->info->props, "pipemixer.route-rule"), rule)
            && streq(spa_dict_lookup(link->info->props, "pipemixer.rule-set"), scope)) return true;
    }
    return false;
}
static bool node_has_error(const struct rule_plan *plan, uint32_t node) {
    for (unsigned p = 0; p < plan->count; p++)
        if (graph_port_find(plan->pairs[p].output)->node_id == node && streq(plan->pairs[p].state, "error")) return true;
    return false;
}
static struct rule_plan plan_rule(const struct route_rule *rule, struct planner_context *context) {
    if (!rule->n_fallbacks || !rule->enabled) return plan_target(rule, rule->input);
    struct rule_plan targets[ROUTE_FALLBACK_LIMIT + 1];
    for (unsigned i = 0; i <= rule->n_fallbacks; i++) targets[i] = plan_target(rule, i ? rule->fallbacks[i - 1] : rule->input);
    struct rule_plan plan = {.outputs = targets[0].outputs, .output_ids = targets[0].output_ids};
    targets[0].output_ids = NULL;
    /* The primary plan still needs its output IDs for coverage checks. */
    targets[0].output_ids = xmalloc((plan.outputs ?: 1) * sizeof(uint32_t));
    if (plan.outputs) memcpy(targets[0].output_ids, plan.output_ids, plan.outputs * sizeof(uint32_t));
    unsigned used = 0;
    for (unsigned o = 0; o < plan.outputs; o++) {
        uint32_t node = graph_port_find(plan.output_ids[o])->node_id;
        bool visited = false;
        for (unsigned j = 0; j < o; j++) if (graph_port_find(plan.output_ids[j])->node_id == node) visited = true;
        if (visited) continue;
        struct route_choice *choice = choice_for(context, rule->name, node);
        if (context->choices && !choice) { plan.state = "error"; plan.reason = "source state limit exceeded"; goto done; }
        unsigned best = UINT_MAX, active = UINT_MAX;
        for (unsigned i = 0; i <= rule->n_fallbacks; i++) {
            if (choice && node_has_error(&targets[i], node)) choice->retry_until[i] = context->now + 5000;
            bool viable = eligible_for_node(&targets[i], node) && !(choice && context->now < choice->retry_until[i]);
            if (!viable) continue;
            if (best == UINT_MAX) best = i;
            if (active == UINT_MAX && has_owned_pair(&targets[i], node, rule->name, context->scope)) active = i;
        }
        if (choice) {
            if (choice->current == UINT_MAX) choice->current = active != UINT_MAX ? active : best;
            bool current_ok = choice->current <= rule->n_fallbacks && eligible_for_node(&targets[choice->current], node)
                && context->now >= choice->retry_until[choice->current];
            if (!current_ok || best == UINT_MAX) { choice->current = best; choice->desired = UINT_MAX; }
            else if (best < choice->current) {
                if (choice->desired != best) { choice->desired = best; choice->since = context->now; }
                if (context->now - choice->since >= rule->switch_delay_ms) { choice->current = best; choice->desired = UINT_MAX; }
            } else choice->desired = UINT_MAX;
            best = choice->current;
        } else if (active != UINT_MAX) best = active;
        const struct rule_plan *chosen = &targets[best <= rule->n_fallbacks ? best : 0];
        if (best != UINT_MAX) used |= 1u << best;
        for (unsigned p = 0; p < chosen->count; p++) {
            struct route_pair pair = chosen->pairs[p];
            if (graph_port_find(pair.output)->node_id != node) continue;
            if (plan.count == PAIR_LIMIT) { plan.state = "error"; plan.reason = "connection expansion limit exceeded"; goto done; }
            pair.available = pair.selected = best != UINT_MAX; pair.target = best != UINT_MAX ? best : 0;
            plan.pairs = xreallocarray(plan.pairs, plan.count + 1, sizeof(*plan.pairs)); plan.pairs[plan.count++] = pair;
        }
        if (best == UINT_MAX) plan.unmatched++;
    }
    for (unsigned i = 0; i <= rule->n_fallbacks; i++) if (used & (1u << i)) plan.inputs += targets[i].inputs;
    if (!plan.count) {
        plan.state = targets[0].state ?: "waiting";
        plan.reason = targets[0].reason ?: "no target covers all matching source channels";
    }
done:
    for (unsigned i = 0; i <= rule->n_fallbacks; i++) { free(targets[i].pairs); free(targets[i].output_ids); }
    if (plan.state && plan.count) { free(plan.pairs); plan.pairs = NULL; plan.count = 0; }
    return plan;
}
static void choose_groups(const struct route_rule *rules, struct rule_plan *plans, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        if (!rules[i].group || !*rules[i].group) continue;
        for (unsigned p = 0; p < plans[i].count; p++) {
            struct route_pair *pair = &plans[i].pairs[p];
            uint32_t node = graph_port_find(pair->output)->node_id;
            unsigned best = count;
            for (unsigned j = 0; j < count; j++) {
                if (!streq(rules[i].group, rules[j].group) || !eligible_for_node(&plans[j], node)) continue;
                if (best == count || compare_rules(&rules[j], &rules[best]) < 0) best = j;
            }
            pair->selected = pair->available && best == i;
            if (!pair->selected && eligible_for_node(&plans[i], node)) plans[i].suppressed++;
        }
    }
}
static struct rule_plan *build_plans(const struct route_rule *rules, unsigned count, struct planner_context *context) {
    struct rule_plan *plans = xcalloc(count ?: 1, sizeof(*plans)); unsigned total = 0;
    for (unsigned i = 0; i < count; i++) {
        plans[i] = plan_rule(&rules[i], context);
        if (total + plans[i].count > TOTAL_PAIR_LIMIT) {
            free(plans[i].pairs); plans[i].pairs = NULL; plans[i].count = 0;
            plans[i].state = "error"; plans[i].reason = "rule set connection limit exceeded";
        }
        total += plans[i].count;
    }
    choose_groups(rules, plans, count); return plans;
}
static void free_plans(struct rule_plan *plans, unsigned count) {
    for (unsigned i = 0; i < count; i++) { free(plans[i].pairs); free(plans[i].output_ids); }
    free(plans);
}
static struct route_rule_state observe_plan(const struct rule_plan *plan) {
    struct route_rule_state state = {.output = PW_ID_ANY, .input = PW_ID_ANY, .active_target = -1,
        .matched_outputs = plan->outputs, .matched_inputs = plan->inputs, .suppressed_pairs = plan->suppressed};
    if (plan->state) { state.state = plan->state; state.reason = plan->reason; return state; }
    unsigned waiting = 0, blocked = 0, errors = 0, connecting = 0;
    for (unsigned i = 0; i < plan->count; i++) {
        const char *s = plan->pairs[i].state;
        if (!plan->pairs[i].selected) { blocked += streq(s, "blocked"); errors += streq(s, "error"); continue; }
        int target = plan->pairs[i].target;
        if (state.active_target == -1) state.active_target = target;
        else if (state.active_target != target) state.active_target = -2;
        state.using_fallback |= target > 0;
        state.total_pairs++;
        state.connected_pairs += streq(s, "connected"); waiting += streq(s, "waiting");
        blocked += streq(s, "blocked"); errors += streq(s, "error"); connecting += streq(s, "connecting");
    }
    if (plan->count) { state.output = plan->pairs[0].output; state.input = plan->pairs[0].input; }
    state.state = "waiting"; state.reason = "ready to connect";
    if (!state.total_pairs && plan->suppressed) { state.state = "suppressed"; state.reason = "a higher priority rule in this group handles the source"; }
    else if (!state.total_pairs && errors) { state.state = "error"; state.reason = "link negotiation failed"; }
    else if (!state.total_pairs && blocked) { state.state = "blocked"; state.reason = "connection would create an audio feedback loop"; }
    else if (!state.total_pairs) { state.state = "waiting"; state.reason = "destination cannot cover all matching source channels"; }
    else if (state.connected_pairs == state.total_pairs && !plan->unmatched && !plan->suppressed) { state.state = "connected"; state.reason = ""; }
    else if (state.connected_pairs) { state.state = "partial"; state.reason = "some connections are pending, blocked or missing channel partners"; }
    else if (errors) { state.state = "error"; state.reason = "link negotiation failed"; }
    else if (blocked && !waiting && !connecting) { state.state = "blocked"; state.reason = "connection would create an audio feedback loop"; }
    else if (connecting) { state.state = "connecting"; state.reason = "link negotiation in progress"; }
    return state;
}
struct route_rule_state route_rule_observe(const struct route_rule *rule) {
    struct route_rule_state state; route_rules_observe(rule, 1, &state); return state;
}
void route_rules_observe(const struct route_rule *rules, unsigned count, struct route_rule_state *states) {
    struct planner_context context = {.scope = route_rules_scope(), .now = milliseconds()};
    struct rule_plan *plans = build_plans(rules, count, &context);
    for (unsigned i = 0; i < count; i++) states[i] = observe_plan(&plans[i]);
    free_plans(plans, count);
    free(context.scope);
}

char *route_rules_scope(void) {
    char *directory = route_rules_directory(); if (!directory) return NULL;
    char *resolved = realpath(directory, NULL);
    const unsigned char *p = (const unsigned char *)(resolved ?: directory);
    uint64_t hash = UINT64_C(14695981039346656037);
    while (*p) { hash ^= *p++; hash *= UINT64_C(1099511628211); }
    char *scope; xasprintf(&scope, "%016" PRIx64, hash); free(resolved); free(directory); return scope;
}
static int engine_lock(bool probe) {
    char *scope = route_rules_scope(); if (!scope) return -errno;
    char *path; xasprintf(&path, "%s/pipemixer-routing-%s.lock", getenv("XDG_RUNTIME_DIR") ?: "/tmp", scope); free(scope);
    int fd = open(path, (probe ? O_RDONLY : O_RDWR | O_CREAT) | O_CLOEXEC | O_NOFOLLOW, 0600); free(path);
    if (fd < 0) return -errno;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) { int e = errno; close(fd); return -e; }
    return fd;
}
bool route_rules_running(void) {
    int fd = engine_lock(true); if (fd >= 0) close(fd); return fd == -EWOULDBLOCK;
}
void route_rules_print(const struct route_rule *rules, unsigned count, bool json) {
    bool running = route_rules_running();
    struct route_rule_state *states = xcalloc(count ?: 1, sizeof(*states)); route_rules_observe(rules, count, states);
    if (json) fprintf(stdout, "{\"engine_running\":%s,\"rules\":[", running ? "true" : "false");
    else printf("Automatic routing engine: %s\n", running ? "running" : "stopped");
    for (unsigned i = 0; i < count; i++) {
        struct route_rule_state state = states[i];
        if (json) {
            const char *active = state.active_target == 0 ? rules[i].input : state.active_target > 0 ? rules[i].fallbacks[state.active_target - 1] : NULL;
            char *name = json_quote(rules[i].name), *output = json_quote(rules[i].output), *input = json_quote(rules[i].input), *reason = json_quote(state.reason), *group = json_quote(rules[i].group ?: ""), *selected = active ? json_quote(active) : xstrdup("null");
            fprintf(stdout, "%s{\"name\":%s,\"enabled\":%s,\"output\":%s,\"input\":%s,\"match\":\"%s\",\"state\":\"%s\",\"reason\":%s,\"matched_outputs\":%u,\"matched_inputs\":%u,\"total_pairs\":%u,\"connected_pairs\":%u,\"priority\":%d,\"exclusive_group\":%s,\"suppressed_pairs\":%u,\"using_fallback\":%s,\"active_input\":%s,\"switch_delay_ms\":%u,\"fallbacks\":[",
                    i ? "," : "", name, rules[i].enabled ? "true" : "false", output, input, rules[i].glob ? "glob" : "exact", state.state, reason,
                    state.matched_outputs, state.matched_inputs, state.total_pairs, state.connected_pairs, rules[i].priority, group, state.suppressed_pairs,
                    state.using_fallback ? "true" : "false", selected, rules[i].switch_delay_ms);
            for (unsigned f = 0; f < rules[i].n_fallbacks; f++) { char *value = json_quote(rules[i].fallbacks[f]); fprintf(stdout, "%s%s", f ? "," : "", value); free(value); }
            fputs("]}", stdout);
            free(name); free(output); free(input); free(reason); free(group); free(selected);
        } else printf("%s\t%s (%u/%u) priority=%d group=%s%s\t%s -> %s%s%s\n", rules[i].name, state.state, state.connected_pairs, state.total_pairs, rules[i].priority, rules[i].group ?: "",
                      state.using_fallback ? " [fallback]" : "", rules[i].output,
                      state.active_target == -2 ? "multiple targets" : state.active_target > 0 ? rules[i].fallbacks[state.active_target - 1] : rules[i].input, *state.reason ? " | " : "", state.reason);
    }
    if (json) fputs("]}\n", stdout);
    free(states);
}

struct engine {
    struct scene_recovery *recovery;
    struct route_rule *rules;
    struct monitor *monitors;
    unsigned n_monitors;
    struct monitor_runtime monitor_runtime;
    unsigned count, ticks, sync_rounds;
    int sync_seq, status;
    char *scope;
    char last_error[256];
    bool dirty;
    uint32_t pending_output, pending_input;
    uint64_t pending_until, retry_after;
    struct route_choice *choices;
    char pending_rule[49];
    unsigned pending_target;
};
static uint64_t milliseconds(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static void clear_choices(struct engine *engine, uint64_t before) {
    struct route_choice **next = &engine->choices;
    while (*next) {
        struct route_choice *choice = *next;
        if (before && choice->seen >= before) { next = &choice->next; continue; }
        *next = choice->next; free(choice->rule); free(choice->node); free(choice);
    }
}
static bool same_rules(const struct route_rule *a, unsigned na, const struct route_rule *b, unsigned nb) {
    if (na != nb) return false;
    for (unsigned i = 0; i < na; i++) {
        if (!streq(a[i].name, b[i].name) || !streq(a[i].output, b[i].output) || !streq(a[i].input, b[i].input)
            || !streq(a[i].group ?: "", b[i].group ?: "") || a[i].glob != b[i].glob || a[i].enabled != b[i].enabled
            || a[i].priority != b[i].priority || a[i].n_fallbacks != b[i].n_fallbacks || a[i].switch_delay_ms != b[i].switch_delay_ms) return false;
        for (unsigned f = 0; f < a[i].n_fallbacks; f++) if (!streq(a[i].fallbacks[f], b[i].fallbacks[f])) return false;
    }
    return true;
}
static void failed_target(struct engine *engine, const char *rule, uint32_t output, unsigned target) {
    const struct graph_port *port = graph_port_find(output); if (!port) return;
    const char *node = graph_node_name(port->node_id);
    for (struct route_choice *c = engine->choices; c; c = c->next) {
        if (!streq(c->rule, rule) || !streq(c->node, node) || target > ROUTE_FALLBACK_LIMIT) continue;
        uint64_t now = milliseconds();
        if (c->retry_until[target] <= now) for (unsigned i = 0; i < engine->count; i++) {
            const struct route_rule *r = &engine->rules[i];
            if (streq(r->name, rule) && target <= r->n_fallbacks)
                fprintf(stderr, "pipemixer routing: rule '%s' target '%s' failed; trying available backups\n", rule, target ? r->fallbacks[target - 1] : r->input);
        }
        c->retry_until[target] = now + 5000;
    }
}
static bool owns(const struct engine *engine, const struct graph_link *link) {
    return link->info && link->info->props && streq(spa_dict_lookup(link->info->props, "pipemixer.rule-set"), engine->scope);
}
struct reconcile_context { struct engine *engine; struct rule_plan *plans; unsigned removed; };
static void remove_obsolete(const struct graph_link *link, void *data) {
    struct reconcile_context *context = data; struct engine *engine = context->engine;
    if (!owns(engine, link)) return;
    const char *name = spa_dict_lookup(link->info->props, "pipemixer.route-rule");
    for (unsigned i = 0; i < engine->count; i++) {
        const struct route_rule *rule = &engine->rules[i];
        if (!rule->enabled || !streq(rule->name, name)) continue;
        for (unsigned p = 0; p < context->plans[i].count; p++) {
            const struct route_pair *pair = &context->plans[i].pairs[p];
            if (!pair->selected || pair->output != link->info->output_port_id || pair->input != link->info->input_port_id || streq(pair->state, "blocked")) continue;
            if (link->info->state != PW_LINK_STATE_ERROR) return;
            if (engine->pending_until && engine->pending_output == link->info->output_port_id
                && engine->pending_input == link->info->input_port_id) return;
            engine->retry_after = milliseconds() + 2000;
            break;
        }
    }
    if (graph_destroy_link(link->id) >= 0) context->removed++;
}
static void reload(struct engine *engine) {
    struct route_rule *rules; unsigned count; char error[256] = {0};
    int result = route_rules_load(&rules, &count, error, sizeof(error));
    struct monitor *monitors = NULL; unsigned n_monitors = 0;
    if (!result) result = monitors_load(&monitors, &n_monitors, error, sizeof(error));
    if (!result) result = monitors_append_rules(monitors, n_monitors, &rules, &count, error, sizeof(error));
    if (result < 0) {
        route_rules_free(rules, count); monitors_free(monitors, n_monitors);
        if (!streq(error, engine->last_error)) fprintf(stderr, "pipemixer routing: %s; keeping previous rules\n", error);
        snprintf(engine->last_error, sizeof(engine->last_error), "%s", error); return;
    }
    if (!same_rules(engine->rules, engine->count, rules, count)) clear_choices(engine, 0);
    route_rules_free(engine->rules, engine->count); engine->rules = rules; engine->count = count;
    monitors_free(engine->monitors, engine->n_monitors); engine->monitors = monitors; engine->n_monitors = n_monitors;
    engine->last_error[0] = 0; engine->dirty = true;
}
static void reconcile(struct engine *engine) {
    monitors_reconcile(&engine->monitor_runtime, engine->monitors, engine->n_monitors, engine->rules, engine->count, engine->scope);
    uint64_t now = milliseconds();
    if (now > 10000) clear_choices(engine, now - 10000);
    struct planner_context planner = {.scope = engine->scope, .choices = &engine->choices, .now = now};
    struct rule_plan *plans = build_plans(engine->rules, engine->count, &planner);
    struct reconcile_context context = {.engine = engine, .plans = plans};
    graph_foreach_link(remove_obsolete, &context);
    if (context.removed) { engine->dirty = true; engine->pending_until = 0; goto done; }
    if (engine->pending_until) {
        const struct graph_link *link = graph_link_between(engine->pending_output, engine->pending_input);
        if (link && link->info->state == PW_LINK_STATE_ERROR) {
            failed_target(engine, engine->pending_rule, engine->pending_output, engine->pending_target);
            if (owns(engine, link)) graph_destroy_link(link->id);
            engine->retry_after = now + 2000; engine->pending_until = 0;
        } else if (now >= engine->pending_until && (!link || link->info->state < PW_LINK_STATE_PAUSED)) {
            failed_target(engine, engine->pending_rule, engine->pending_output, engine->pending_target);
            engine->dirty = true; engine->pending_until = 0; goto done;
        } else if ((link && link->info->state >= PW_LINK_STATE_PAUSED)
                   || !graph_port_find(engine->pending_output) || !graph_port_find(engine->pending_input)) engine->pending_until = 0;
        else goto done;
    }
    if (now < engine->retry_after || !engine->dirty) goto done;
    engine->dirty = false;
    for (unsigned i = 0; i < engine->count; i++) {
        for (unsigned p = 0; p < plans[i].count; p++) {
            const struct route_pair *pair = &plans[i].pairs[p];
            if (!pair->selected || !streq(pair->state, "waiting")) continue;
            int result = graph_connect_rule(pair->output, pair->input, engine->rules[i].name, engine->scope);
            if (result < 0) { fprintf(stderr, "pipemixer routing: rule '%s': %s\n", engine->rules[i].name, strerror(-result)); failed_target(engine, engine->rules[i].name, pair->output, pair->target); engine->retry_after = now + 2000; goto done; }
            engine->pending_output = pair->output; engine->pending_input = pair->input; engine->pending_until = now + 3000;
            snprintf(engine->pending_rule, sizeof(engine->pending_rule), "%s", engine->rules[i].name); engine->pending_target = pair->target;
            engine->dirty = true; goto done; /* Observe each link before checking another pair for cycles. */
        }
    }
done:
    free_plans(plans, engine->count);
}
static void engine_tick(void *data, uint64_t expirations) {
    struct engine *engine = data;
    if (engine->ticks++ % 4 == 0) reload(engine);
    if (engine->sync_rounds < 3 || !graph_ready()) return;
    /* A scene loader owns this lock until its asynchronous operation finishes.
     * Route policies resume afterwards, using the resulting graph. */
    char *path; xasprintf(&path, "%s/pipemixer-scene.lock", getenv("XDG_RUNTIME_DIR") ?: "/tmp");
    int lock = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600); free(path);
    if (lock < 0) return;
    if (flock(lock, LOCK_EX | LOCK_NB) == 0 && !scene_recovery_tick(engine->recovery)) {
        flock(lock, LOCK_SH);
        reconcile(engine);
    }
    close(lock);
}
static void on_graph(void *data) { ((struct engine *)data)->dirty = true; }
static void on_sync(int seq, void *data) {
    struct engine *engine = data;
    if (seq != engine->sync_seq) return;
    if (++engine->sync_rounds < 3) engine->sync_seq = pipewire_sync();
}
static void on_error(int code, const char *message, void *data) {
    struct engine *engine = data; fprintf(stderr, "pipemixer routing: %s\n", message);
    if (scene_recovery_error(engine->recovery, code, message)) return;
    if (engine->pending_until && code != -EPIPE && code != -ECONNRESET && code != -ENOTCONN) {
        failed_target(engine, engine->pending_rule, engine->pending_output, engine->pending_target);
        bool backups = false;
        for (unsigned i = 0; i < engine->count; i++) if (streq(engine->rules[i].name, engine->pending_rule) && engine->rules[i].n_fallbacks) backups = true;
        engine->retry_after = backups ? 0 : milliseconds() + 2000;
        engine->pending_until = 0; engine->dirty = true; return;
    }
    engine->status = 1; pw_main_loop_quit(main_loop);
}
static void on_signal(void *data, int signal) { pw_main_loop_quit(main_loop); }
int route_rules_run(void) {
    char *directory = route_rules_directory(); if (!directory) return 1;
    int result = make_directory(directory); free(directory); if (result < 0) return 1;
    int lock = engine_lock(false);
    if (lock < 0) { fprintf(stderr, "pipemixer routing: cannot start engine: %s\n", strerror(-lock)); return 1; }
    struct engine engine = {.scope = route_rules_scope(), .dirty = true};
    engine.recovery = scene_recovery_new();
    static const struct pipewire_events events = {.graph = on_graph, .sync = on_sync, .error = on_error};
    struct event_hook *hook = pipewire_add_listener(&events, &engine);
    struct spa_source *timer = pw_loop_add_timer(event_loop, engine_tick, &engine);
    struct spa_source *signals[2] = {pw_loop_add_signal(event_loop, SIGINT, on_signal, &engine), pw_loop_add_signal(event_loop, SIGTERM, on_signal, &engine)};
    struct timespec interval = {.tv_nsec = 250000000};
    engine.sync_seq = pipewire_sync();
    if (!timer || !signals[0] || !signals[1] || engine.sync_seq < 0
        || pw_loop_update_timer(event_loop, timer, &interval, &interval, false) < 0) engine.status = 1;
    else pw_main_loop_run(main_loop);
    if (timer) pw_loop_destroy_source(event_loop, timer);
    for (unsigned i = 0; i < 2; i++) if (signals[i]) pw_loop_destroy_source(event_loop, signals[i]);
    event_hook_release(hook); route_rules_free(engine.rules, engine.count); clear_choices(&engine, 0);
    monitors_runtime_clear(&engine.monitor_runtime); monitors_free(engine.monitors, engine.n_monitors);
    scene_recovery_free(engine.recovery);
    free(engine.scope); close(lock);
    return engine.status;
}
