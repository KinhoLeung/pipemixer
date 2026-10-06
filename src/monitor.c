#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <spa/utils/json.h>
#include "monitor.h"
#include "scene.h"
#include "pw/managed.h"
#include "utils.h"
#include "xmalloc.h"

#define FILE_LIMIT 65536
bool monitor_valid_name(const char *name) { return managed_valid_name(name) && strlen(name) <= 32; }
bool monitor_valid_node(const char *name) {
    return name && *name && strlen(name) <= 1024 && strncmp(name, "id:", 3)
        && strncmp(name, "serial:", 7) && strncmp(name, "pipemixer.monitor.", 18);
}
bool monitor_valid_scope(const char *scope) {
    if (!scope || strlen(scope) != 16) return false;
    for (unsigned i = 0; i < 16; i++) if (!strchr("0123456789abcdef", scope[i])) return false;
    return true;
}
static int failure(char *error, size_t size, int result, const char *detail) {
    snprintf(error, size, "%s: %s", detail, strerror(-result)); return result;
}
static char *directory(void) {
    char *scenes = scene_directory(), *path = NULL;
    if (scenes) { *strrchr(scenes, '/') = 0; xasprintf(&path, "%s/monitors", scenes); free(scenes); }
    return path;
}
static char *filename(const char *name) {
    char *dir = directory(), *path = NULL;
    if (dir) { xasprintf(&path, "%s/%s.json", dir, name); free(dir); }
    return path;
}
static void clear_strings(char ***strings, unsigned *count) {
    for (unsigned i = 0; i < *count; i++) free((*strings)[i]);
    free(*strings); *strings = NULL; *count = 0;
}
static void clear_monitor(struct monitor *m) {
    free(m->name); free(m->destination); free(m->listen);
    clear_strings(&m->sources, &m->n_sources); clear_strings(&m->solo, &m->n_solo);
    *m = (struct monitor){0};
}
void monitors_free(struct monitor *monitors, unsigned count) {
    for (unsigned i = 0; i < count; i++) clear_monitor(&monitors[i]);
    free(monitors);
}
static char *string_token(const char *token, int len) {
    if (len < 2 || *token != '"') return NULL;
    for (int i = 0; i + 1 < len; i++) if (token[i] == '\\') {
        if (i + 5 < len && !strncmp(token + i, "\\u0000", 6)) return NULL;
        i++;
    }
    char *value = xcalloc(len + 1, 1);
    if (spa_json_parse_stringn(token, len, value, len + 1) <= 0) { free(value); return NULL; }
    return value;
}
static bool valid_list(char *const *nodes, unsigned count, const char *destination) {
    if (count > MONITOR_SOURCE_LIMIT) return false;
    for (unsigned i = 0; i < count; i++) {
        if (!monitor_valid_node(nodes[i]) || streq(nodes[i], destination)) return false;
        for (unsigned j = 0; j < i; j++) if (streq(nodes[i], nodes[j])) return false;
    }
    return true;
}
static int read_monitor(const char *name, struct monitor *m) {
    char *path = filename(name); if (!path) return -errno;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK); free(path);
    if (fd < 0) return -errno;
    struct stat st; int result = -EINVAL;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > FILE_LIMIT) { close(fd); return result; }
    char *data = xcalloc(st.st_size + 1, 1); size_t n = 0;
    while (n < (size_t)st.st_size) {
        ssize_t got = read(fd, data + n, st.st_size - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += got;
    }
    close(fd);
    struct spa_json iter = SPA_JSON_INIT(data, n), object;
    const char *token; int len; unsigned seen = 0;
    if (n != (size_t)st.st_size || memchr(data, 0, n) || spa_json_enter_object(&iter, &object) <= 0) goto done;
    while ((len = spa_json_next(&object, &token)) > 0) {
        char *key = string_token(token, len); if (!key) goto done;
        static const char *keys[] = {"format", "version", "name", "enabled", "destination", "sources", "listen", "solo"};
        unsigned field = 0; while (field < 8 && !streq(key, keys[field])) field++;
        free(key); if (field == 8 || (seen & (1u << field))) goto done;
        seen |= 1u << field;
        if ((len = spa_json_next(&object, &token)) <= 0) goto done;
        if (field == 1) { if (len != 1 || *token != '1') goto done; }
        else if (field == 3) { if (spa_json_parse_bool(token, len, &m->enabled) <= 0) goto done; }
        else if (field == 5 || field == 7) {
            struct spa_json array; if (len != 1 || *token != '[') goto done;
            spa_json_enter(&object, &array);
            char ***list = field == 5 ? &m->sources : &m->solo;
            unsigned *count = field == 5 ? &m->n_sources : &m->n_solo;
            while ((len = spa_json_next(&array, &token)) > 0) {
                char *value = string_token(token, len);
                if (!value || *count == MONITOR_SOURCE_LIMIT) { free(value); goto done; }
                *list = xreallocarray(*list, *count + 1, sizeof(**list)); (*list)[(*count)++] = value;
            }
            if (len < 0) goto done;
        } else if (field == 6 && spa_json_is_null(token, len)) { /* Normal mix. */ }
        else {
            char *value = string_token(token, len); if (!value) goto done;
            if (field == 0 || field == 2) {
                bool valid = streq(value, field == 0 ? "pipemixer.monitor" : name); free(value); if (!valid) goto done;
            } else if (field == 4) m->destination = value;
            else m->listen = value;
        }
    }
    if (len == 0 && seen == 255 && spa_json_next(&iter, &token) == 0 && monitor_valid_node(m->destination)
        && (!m->listen || (monitor_valid_node(m->listen) && !streq(m->listen, m->destination)))
        && valid_list(m->sources, m->n_sources, m->destination) && valid_list(m->solo, m->n_solo, m->destination)) {
        m->name = xstrdup(name); result = 0;
    }
done:
    free(data); if (result < 0) clear_monitor(m); return result;
}
static int compare_monitors(const void *a, const void *b) { return strcmp(((const struct monitor *)a)->name, ((const struct monitor *)b)->name); }
int monitors_load(struct monitor **monitors, unsigned *count, char *error, size_t size) {
    *monitors = NULL; *count = 0;
    char *path = directory(); if (!path) return failure(error, size, -errno, "Cannot locate monitors");
    DIR *dir = opendir(path); free(path);
    if (!dir) return errno == ENOENT ? 0 : failure(error, size, -errno, "Cannot open monitors");
    struct dirent *entry; int result = 0; errno = 0;
    while ((entry = readdir(dir))) {
        size_t len = strlen(entry->d_name); if (len < 6 || strcmp(entry->d_name + len - 5, ".json")) continue;
        char *name = strndup(entry->d_name, len - 5);
        if (!monitor_valid_name(name) || *count == MONITOR_LIMIT) { free(name); result = -EINVAL; break; }
        *monitors = xreallocarray(*monitors, *count + 1, sizeof(**monitors)); (*monitors)[*count] = (struct monitor){0};
        result = read_monitor(name, &(*monitors)[*count]); free(name); if (result < 0) break;
        (*count)++; errno = 0;
    }
    if (!entry && errno) result = -errno;
    closedir(dir);
    if (result < 0) { monitors_free(*monitors, *count); *monitors = NULL; *count = 0; return failure(error, size, result, "Invalid monitor configuration"); }
    if (*count > 1) qsort(*monitors, *count, sizeof(**monitors), compare_monitors);
    return 0;
}
static int write_lock(void) {
    char *path = directory(); if (!path) return -errno;
    for (char *p = path + 1; ; p++) {
        if (*p && *p != '/') continue;
        char saved = *p; *p = 0;
        if (mkdir(path, 0700) < 0 && errno != EEXIST) { int e = errno; free(path); return -e; }
        struct stat st; if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) { free(path); return -ENOTDIR; }
        *p = saved; if (!saved) break;
    }
    char *lock; xasprintf(&lock, "%s/.lock", path); free(path);
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600); free(lock);
    if (fd < 0) return -errno;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) { int e = errno; close(fd); return -e; }
    return fd;
}
static void write_strings(FILE *file, char *const *list, unsigned count) {
    fputc('[', file);
    for (unsigned i = 0; i < count; i++) { char *value = json_quote(list[i]); fprintf(file, "%s%s", i ? ", " : "", value); free(value); }
    fputc(']', file);
}
static int sync_directory(void) {
    char *path = directory(); if (!path) return -errno;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC); free(path);
    if (fd < 0) return -errno;
    int result = fsync(fd) < 0 ? -errno : 0; close(fd); return result;
}
static int write_monitor(const struct monitor *m, bool create) {
    char *path = filename(m->name), *temp = NULL; if (!path) return -errno;
    xasprintf(&temp, "%s.tmp-XXXXXX", path);
    int fd = mkostemp(temp, O_CLOEXEC), result = 0;
    if (fd < 0) { result = -errno; goto done; }
    FILE *file = fdopen(fd, "w"); if (!file) { result = -errno; close(fd); goto done; }
    char *name = json_quote(m->name), *dest = json_quote(m->destination), *listen = m->listen ? json_quote(m->listen) : xstrdup("null");
    fprintf(file, "{\n  \"format\": \"pipemixer.monitor\",\n  \"version\": 1,\n  \"name\": %s,\n  \"enabled\": %s,\n  \"destination\": %s,\n  \"sources\": ", name, m->enabled ? "true" : "false", dest);
    write_strings(file, m->sources, m->n_sources); fprintf(file, ",\n  \"listen\": %s,\n  \"solo\": ", listen);
    write_strings(file, m->solo, m->n_solo); fputs("\n}\n", file); free(name); free(dest); free(listen);
    if (ferror(file) || fflush(file) || fsync(fd)) result = -(errno ?: EIO);
    if (!result && ftell(file) > FILE_LIMIT) result = -E2BIG;
    if (fclose(file) && !result) result = -errno;
    if (!result && (create ? link(temp, path) : rename(temp, path)) < 0) result = -errno;
    if (!result) result = sync_directory();
done:
    if (temp) unlink(temp);
    free(path); free(temp); return result;
}
static void add_string(char ***list, unsigned *count, const char *value) {
    *list = xreallocarray(*list, *count + 1, sizeof(**list)); (*list)[(*count)++] = xstrdup(value);
}
int monitor_edit(const char *name, enum monitor_action action, const char *value,
                 const char *const *sources, unsigned count, char *error, size_t size) {
    if (!monitor_valid_name(name) || count > MONITOR_SOURCE_LIMIT) return failure(error, size, -EINVAL, "Invalid monitor name or source count");
    int lock = write_lock(); if (lock < 0) return failure(error, size, lock, "Cannot lock monitors");
    struct monitor *monitors; unsigned n; int result = monitors_load(&monitors, &n, error, size);
    if (result < 0) { close(lock); return result; }
    struct monitor created = {0}, *m = NULL;
    for (unsigned i = 0; i < n; i++) if (streq(monitors[i].name, name)) m = &monitors[i];
    if (action == MONITOR_CREATE) {
        if (m) { result = -EEXIST; goto done; }
        if (n == MONITOR_LIMIT || !monitor_valid_node(value)) { result = -EINVAL; goto done; }
        m = &created; m->name = xstrdup(name); m->destination = xstrdup(value); m->enabled = true;
        for (unsigned i = 0; i < count; i++) add_string(&m->sources, &m->n_sources, sources[i]);
    } else if (!m) { result = -ENOENT; goto done; }
    else if (action == MONITOR_DELETE) {
        char *path = filename(name); result = path && unlink(path) == 0 ? sync_directory() : -(errno ?: EIO); free(path); goto done;
    } else if (action == MONITOR_ENABLE) {
        if (!streq(value, "on") && !streq(value, "off")) { result = -EINVAL; goto done; }
        m->enabled = streq(value, "on");
    } else if (action == MONITOR_SOURCES) {
        clear_strings(&m->sources, &m->n_sources);
        for (unsigned i = 0; i < count; i++) add_string(&m->sources, &m->n_sources, sources[i]);
    } else if (action == MONITOR_LISTEN) { free(m->listen); m->listen = streq(value, "mix") ? NULL : xstrdup(value); }
    else if (action == MONITOR_SOLO_CLEAR) clear_strings(&m->solo, &m->n_solo);
    else if (action >= MONITOR_SOLO_ON && action <= MONITOR_SOLO_TOGGLE) {
        if (!monitor_valid_node(value)) { result = -EINVAL; goto done; }
        unsigned index = 0; while (index < m->n_solo && !streq(m->solo[index], value)) index++;
        bool add = action == MONITOR_SOLO_ON || (action == MONITOR_SOLO_TOGGLE && index == m->n_solo);
        if (add && index == m->n_solo) {
            if (m->n_solo == MONITOR_SOURCE_LIMIT) { result = -E2BIG; goto done; }
            add_string(&m->solo, &m->n_solo, value);
        } else if (!add && index < m->n_solo) {
            free(m->solo[index]); memmove(m->solo + index, m->solo + index + 1, (m->n_solo - index - 1) * sizeof(*m->solo)); m->n_solo--;
        }
    } else { result = -EINVAL; goto done; }
    if (!monitor_valid_node(m->destination) || (m->listen && (!monitor_valid_node(m->listen) || streq(m->listen, m->destination)))
        || !valid_list(m->sources, m->n_sources, m->destination) || !valid_list(m->solo, m->n_solo, m->destination)) result = -EINVAL;
    else result = write_monitor(m, action == MONITOR_CREATE);
done:
    clear_monitor(&created); monitors_free(monitors, n); close(lock);
    return result < 0 ? failure(error, size, result, "Cannot update monitor") : 0;
}
const char *monitor_mode(const struct monitor *m) { return m->n_solo ? "solo" : m->listen ? "listen" : "mix"; }
static unsigned active_count(const struct monitor *m) { return m->n_solo ? m->n_solo : m->listen ? 1 : m->n_sources; }
static const char *active_source(const struct monitor *m, unsigned i) { return m->n_solo ? m->solo[i] : m->listen ? m->listen : m->sources[i]; }
static char *pattern(const char *node) {
    char *value = xmalloc(strlen(node) * 2 + 3), *p = value;
    for (; *node; node++) { if (strchr("\\[]*?", *node)) *p++ = '\\'; *p++ = *node; }
    *p++ = ':'; *p++ = '*'; *p = 0; return value;
}
static struct route_rule make_rule(const struct monitor *m, unsigned index) {
    char *name, *node; xasprintf(&name, "monitor_%s_%u", m->name, index);
    xasprintf(&node, "pipemixer.monitor.%s.%s", m->name, index ? "input" : "output");
    struct route_rule rule = {.name = name, .enabled = m->enabled, .glob = true, .version = 2};
    rule.output = pattern(index ? active_source(m, index - 1) : node);
    rule.input = pattern(index ? node : m->destination); free(node); return rule;
}
static bool rule_belongs(const char *rule, const char *name);
int monitors_append_rules(const struct monitor *monitors, unsigned count, struct route_rule **rules,
                          unsigned *n_rules, char *error, size_t size) {
    for (unsigned i = 0; i < count; i++) for (unsigned r = 0; r < *n_rules; r++)
        if (rule_belongs((*rules)[r].name, monitors[i].name))
            return failure(error, size, -EEXIST, "A route rule uses a monitor's reserved name");
    for (unsigned i = 0; i < count; i++) for (unsigned p = 0; p <= active_count(&monitors[i]); p++) {
        struct route_rule rule = make_rule(&monitors[i], p);
        for (unsigned r = 0; r < *n_rules; r++) if (streq((*rules)[r].name, rule.name)) {
            free(rule.name); free(rule.input); free(rule.output);
            return failure(error, size, -EEXIST, "A route rule uses a monitor's reserved name");
        }
        *rules = xreallocarray(*rules, *n_rules + 1, sizeof(**rules)); (*rules)[(*n_rules)++] = rule;
    }
    return 0;
}
static bool rule_belongs(const char *rule, const char *name) {
    char prefix[48]; snprintf(prefix, sizeof(prefix), "monitor_%s_", name);
    if (!rule || strncmp(rule, prefix, strlen(prefix))) return false;
    const char *p = rule + strlen(prefix); if (!*p) return false;
    for (; *p; p++) if (*p < '0' || *p > '9') return false;
    return true;
}
struct conflict { uint32_t input; const char *name, *scope; bool found; };
static void foreign_link(const struct graph_link *link, void *data) {
    struct conflict *c = data; if (!link->info || link->info->input_node_id != c->input) return;
    const struct spa_dict *props = link->info->props;
    if (!props || !streq(spa_dict_lookup(props, "pipemixer.rule-set"), c->scope)
        || !rule_belongs(spa_dict_lookup(props, "pipemixer.route-rule"), c->name)) c->found = true;
}
static bool monitor_conflict(const struct monitor *m, const char *scope) {
    const struct graph_node *in = managed_find("monitor", m->name, "input"), *out = managed_find("monitor", m->name, "output");
    if ((in && !streq(dict_get(&in->props, "pipemixer.monitor-set"), scope))
        || (out && !streq(dict_get(&out->props, "pipemixer.monitor-set"), scope))) return true;
    struct conflict c = {.input = in ? in->id : PW_ID_ANY, .name = m->name, .scope = scope};
    if (in) graph_foreach_link(foreign_link, &c);
    return c.found;
}
struct orphans { const struct monitor *monitors; unsigned count; const char *scope; };
static void remove_orphan(const struct graph_node *node, void *data) {
    struct orphans *o = data;
    if (!streq(dict_get(&node->props, "pipemixer.kind"), "monitor")
        || !streq(dict_get(&node->props, "pipemixer.role"), "input")
        || !streq(dict_get(&node->props, "pipemixer.monitor-set"), o->scope)) return;
    const char *name = dict_get(&node->props, "pipemixer.group");
    for (unsigned i = 0; i < o->count; i++) if (streq(name, o->monitors[i].name)) return;
    managed_remove("monitor", name);
}
void monitors_reconcile(struct monitor_runtime *runtime, const struct monitor *monitors, unsigned count,
                        struct route_rule *rules, unsigned n_rules, const char *scope) {
    struct orphans o = {monitors, count, scope}; graph_foreach_node(remove_orphan, &o);
    for (unsigned w = 0; w < MONITOR_LIMIT; w++) {
        struct monitor_worker *worker = &runtime->workers[w]; if (!*worker->name) continue;
        bool keep = false; for (unsigned i = 0; i < count; i++) if (streq(monitors[i].name, worker->name)) keep = true;
        if (!keep) { managed_cancel(worker->pid); *worker = (struct monitor_worker){0}; }
    }
    for (unsigned i = 0; i < count; i++) {
        const struct monitor *m = &monitors[i]; bool conflict = monitor_conflict(m, scope);
        for (unsigned r = 0; r < n_rules; r++) if (rule_belongs(rules[r].name, m->name)) rules[r].enabled = m->enabled && !conflict;
        if (conflict) continue;
        struct monitor_worker *worker = NULL;
        for (unsigned w = 0; w < MONITOR_LIMIT; w++) if (streq(runtime->workers[w].name, m->name)) worker = &runtime->workers[w];
        if (!worker) for (unsigned w = 0; w < MONITOR_LIMIT; w++) if (!*runtime->workers[w].name) {
            worker = &runtime->workers[w]; snprintf(worker->name, sizeof(worker->name), "%s", m->name); break;
        }
        if (!worker) continue;
        if (managed_ready("monitor", m->name)) {
            worker->retry = 0;
            if (worker->pid > 0) managed_child_ready(worker->pid);
            continue;
        }
        if (worker->pid > 0 && managed_child_status(worker->pid) == -EAGAIN) { managed_child_ready(worker->pid); continue; }
        if (worker->retry) { worker->retry--; continue; }
        if (managed_find("monitor", m->name, NULL)) continue;
        worker->pid = managed_spawn("monitor", m->name, scope, NULL); worker->retry = 20;
    }
    managed_reap();
}
void monitors_runtime_clear(struct monitor_runtime *runtime) {
    for (unsigned w = 0; w < MONITOR_LIMIT; w++) {
        struct monitor_worker *worker = &runtime->workers[w];
        if (worker->pid > 0 && !managed_ready("monitor", worker->name)) managed_cancel(worker->pid);
    }
    managed_cleanup();
}
struct live_monitor {
    const struct monitor *monitor;
    const char *scope;
    uint32_t input, output;
    unsigned connected;
    bool changing;
};
static void observe_link(const struct graph_link *link, void *data) {
    struct live_monitor *live = data; const struct monitor *m = live->monitor;
    if (!graph_link_is_audio(link) || !link->info->props) return;
    const struct spa_dict *props = link->info->props;
    if (!streq(spa_dict_lookup(props, "pipemixer.rule-set"), live->scope)
        || !rule_belongs(spa_dict_lookup(props, "pipemixer.route-rule"), m->name)) return;
    unsigned index = 0;
    if (link->info->input_node_id == live->input) {
        const char *source = graph_node_name(link->info->output_node_id);
        unsigned p = 0; while (p < active_count(m) && !streq(source, active_source(m, p))) p++;
        if (p == active_count(m)) { live->changing = true; return; }
        index = p + 1;
    } else if (link->info->output_node_id == live->output) {
        if (!streq(graph_node_name(link->info->input_node_id), m->destination)) { live->changing = true; return; }
    } else return;
    char expected[49]; snprintf(expected, sizeof(expected), "monitor_%s_%u", m->name, index);
    if (!streq(expected, spa_dict_lookup(props, "pipemixer.route-rule"))) { live->changing = true; return; }
    const struct graph_port *out = graph_port_find(link->info->output_port_id), *in = graph_port_find(link->info->input_port_id);
    const char *left = dict_get(&out->props, PW_KEY_AUDIO_CHANNEL), *right = dict_get(&in->props, PW_KEY_AUDIO_CHANNEL);
    if ((left || right) && !streq(left, right)) { live->changing = true; return; }
    if (link->info->state >= PW_LINK_STATE_PAUSED) live->connected++;
}
const char *monitor_state(const struct monitor *m, unsigned *connected, unsigned *total) {
    *connected = *total = 0;
    if (!m->enabled) return "disabled";
    char *scope = route_rules_scope(); bool conflict = !scope || monitor_conflict(m, scope);
    if (conflict) { free(scope); return "conflict"; }
    if (!managed_ready("monitor", m->name)) { free(scope); return "waiting"; }
    struct route_rule *rules = NULL; unsigned n = 0; char error[256];
    monitors_append_rules(m, 1, &rules, &n, error, sizeof(error));
    struct route_rule_state *states = xcalloc(n, sizeof(*states)); route_rules_observe(rules, n, states);
    const char *state = "connected";
    for (unsigned i = 0; i < n; i++) {
        *total += states[i].total_pairs;
        if (streq(states[i].state, "blocked") || streq(states[i].state, "error")) state = "blocked";
        else if (!streq(states[i].state, "connected") && !streq(state, "blocked")) state = "waiting";
    }
    struct live_monitor live = {.monitor = m, .scope = scope, .input = managed_find("monitor", m->name, "input")->id,
                                .output = managed_find("monitor", m->name, "output")->id};
    graph_foreach_link(observe_link, &live); *connected = live.connected;
    if (live.changing) state = "switching";
    else if (*connected != *total && !streq(state, "blocked")) state = "waiting";
    free(scope); free(states); route_rules_free(rules, n); return state;
}
void monitors_print(const struct monitor *monitors, unsigned count, bool json) {
    bool running = route_rules_running();
    if (json) printf("{\"engine_running\":%s,\"monitors\":[", running ? "true" : "false");
    else printf("Monitor engine: %s\n", running ? "running" : "stopped");
    for (unsigned i = 0; i < count; i++) {
        const struct monitor *m = &monitors[i]; unsigned connected, total; const char *state = monitor_state(m, &connected, &total);
        if (!json) { printf("%s\t%s %s (%u/%u) -> %s\n", m->name, state, monitor_mode(m), connected, total, m->destination); continue; }
        char *name = json_quote(m->name), *dest = json_quote(m->destination), *listen = m->listen ? json_quote(m->listen) : xstrdup("null");
        printf("%s{\"name\":%s,\"enabled\":%s,\"destination\":%s,\"mode\":\"%s\",\"state\":\"%s\",\"connected_pairs\":%u,\"total_pairs\":%u,\"listen\":%s,\"sources\":", i ? "," : "", name, m->enabled ? "true" : "false", dest, monitor_mode(m), state, connected, total, listen);
        write_strings(stdout, m->sources, m->n_sources); fputs(",\"solo\":", stdout); write_strings(stdout, m->solo, m->n_solo);
        fputs(",\"active_sources\":[", stdout);
        for (unsigned p = 0; p < active_count(m); p++) { char *source = json_quote(active_source(m, p)); printf("%s%s", p ? "," : "", source); free(source); }
        fputs("]}", stdout); free(name); free(dest); free(listen);
    }
    if (json) fputs("]}\n", stdout);
}
