#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#include <spa/utils/json.h>

#include "scene.h"
#include "monitor.h"
#include "route-rules.h"
#include "pw/common.h"
#include "pw/graph.h"
#include "pw/managed.h"
#include "pw/effect-chain.h"
#include "utils.h"
#include "xmalloc.h"

#define SCENE_LIMIT (8 * 1024 * 1024)
#define NODE_LIMIT 1024
#define LINK_LIMIT 8192
#define PATH_LIMIT 256

/* A small owned JSON tree. Bounded depth/count/size keep files predictable on
 * the board. Duplicate object keys and non-finite numbers are rejected. */
enum json_type { J_NULL, J_STRING, J_NUMBER, J_BOOL, J_ARRAY, J_OBJECT };
struct json {
    enum json_type type;
    char *key, *text;
    double number;
    bool boolean;
    struct json **items;
    unsigned count;
};
static int clear_scene_recovery(const char *name);

static struct json *jnew(enum json_type type) {
    struct json *v = xcalloc(1, sizeof(*v)); v->type = type; return v;
}
static struct json *jstr(const char *text) {
    struct json *v = jnew(text ? J_STRING : J_NULL); v->text = xstrdup(text); return v;
}
static struct json *jnum(double number) { struct json *v = jnew(J_NUMBER); v->number = number; return v; }
static struct json *jbool(bool value) { struct json *v = jnew(J_BOOL); v->boolean = value; return v; }
static void jadd(struct json *parent, const char *key, struct json *value) {
    value->key = xstrdup(key);
    parent->items = xreallocarray(parent->items, parent->count + 1, sizeof(*parent->items));
    parent->items[parent->count++] = value;
}
static void jfree(struct json *v) {
    if (!v) return;
    for (unsigned i = 0; i < v->count; i++) jfree(v->items[i]);
    free(v->items); free(v->key); free(v->text); free(v);
}
static struct json *jclone(const struct json *v) {
    struct json *copy = jnew(v->type);
    copy->text = xstrdup(v->text); copy->number = v->number; copy->boolean = v->boolean;
    for (unsigned i = 0; i < v->count; i++) jadd(copy, v->items[i]->key, jclone(v->items[i]));
    return copy;
}
static void jreplace_string(struct json *v, const char *text) {
    free(v->text); v->text = xstrdup(text); v->type = text ? J_STRING : J_NULL;
}
static struct json *jget(const struct json *v, const char *key) {
    if (v && v->type == J_OBJECT)
        for (unsigned i = 0; i < v->count; i++) if (streq(v->items[i]->key, key)) return v->items[i];
    return NULL;
}
static const char *jtext(const struct json *v, const char *key) {
    const struct json *item = jget(v, key);
    return item && item->type == J_STRING ? item->text : NULL;
}
static bool typed(const struct json *v, enum json_type type) { return v && v->type == type; }
static void jwrite(FILE *file, const struct json *v, unsigned depth) {
    if (v->type == J_STRING) { char *q = json_quote(v->text); fputs(q, file); free(q); }
    else if (v->type == J_NUMBER) fprintf(file, "%.9g", v->number);
    else if (v->type == J_BOOL) fputs(v->boolean ? "true" : "false", file);
    else if (v->type == J_NULL) fputs("null", file);
    else {
        fputc(v->type == J_OBJECT ? '{' : '[', file);
        for (unsigned i = 0; i < v->count; i++) {
            if (i) fputc(',', file);
            fputc('\n', file); fprintf(file, "%*s", (depth + 1) * 2, "");
            if (v->type == J_OBJECT) {
                char *q = json_quote(v->items[i]->key); fprintf(file, "%s: ", q); free(q);
            }
            jwrite(file, v->items[i], depth + 1);
        }
        if (v->count) { fputc('\n', file); fprintf(file, "%*s", depth * 2, ""); }
        fputc(v->type == J_OBJECT ? '}' : ']', file);
    }
}
static struct json *jparse(struct spa_json *iter, const char *token, int len, unsigned depth, unsigned *count, unsigned limit) {
    if (depth > 12 || ++*count > limit) return NULL;
    struct json *v = NULL;
    if (len == 1 && (*token == '{' || *token == '[')) {
        v = jnew(*token == '{' ? J_OBJECT : J_ARRAY);
        struct spa_json sub; spa_json_enter(iter, &sub);
        const char *next; int size;
        while ((size = spa_json_next(&sub, &next)) > 0) {
            char *key = NULL;
            if (v->type == J_OBJECT) {
                if (*next != '"' || size > 4096) goto bad;
                key = xcalloc(size + 1, 1);
                if (spa_json_parse_stringn(next, size, key, size + 1) <= 0 || jget(v, key)) { free(key); goto bad; }
                if ((size = spa_json_next(&sub, &next)) <= 0) { free(key); goto bad; }
            }
            struct json *child = jparse(&sub, next, size, depth + 1, count, limit);
            if (!child) { free(key); goto bad; }
            jadd(v, key, child); free(key);
        }
        if (size < 0) goto bad;
    } else if (*token == '"') {
        v = jnew(J_STRING); v->text = xcalloc(len + 1, 1);
        if (spa_json_parse_stringn(token, len, v->text, len + 1) <= 0) goto bad;
        /* Embedded NUL escapes cannot serve as names or graph contents. */
        for (int i = 0; i + 1 < len; i++) if (token[i] == '\\') {
            if (i + 5 < len && !strncmp(token + i, "\\u0000", 6)) goto bad;
            i++;
        }
    } else if (spa_json_is_null(token, len)) v = jnew(J_NULL);
    else {
        bool boolean;
        if (spa_json_parse_bool(token, len, &boolean) > 0) v = jbool(boolean);
        else {
            char *number = strndup(token, len), *end;
            errno = 0; double value = strtod(number, &end);
            bool ok = !errno && end != number && !*end && isfinite(value);
            free(number); if (!ok) return NULL; v = jnum(value);
        }
    }
    return v;
bad:
    jfree(v); return NULL;
}

static int fail(char *error, size_t size, int code, const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(error, size, format, args); va_end(args); return -code;
}
char *scene_directory(void) {
    const char *base = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *path;
    if (base && *base) xasprintf(&path, "%s/pipemixer/scenes", base);
    else if (home && *home) xasprintf(&path, "%s/.config/pipemixer/scenes", home);
    else { errno = ENOENT; return NULL; }
    return path;
}
char *scene_filename(const char *name) {
    if (!managed_valid_name(name)) { errno = EINVAL; return NULL; }
    char *directory = scene_directory(), *path;
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
static int compare_names(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
int scene_list(char ***names, unsigned *count) {
    *names = NULL; *count = 0;
    char *path = scene_directory(); if (!path) return -errno;
    DIR *dir = opendir(path); free(path);
    if (!dir) return errno == ENOENT ? 0 : -errno;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 5, ".json")) continue;
        char *name = strndup(entry->d_name, n - 5);
        if (!managed_valid_name(name)) { free(name); continue; }
        *names = xreallocarray(*names, *count + 1, sizeof(**names));
        (*names)[(*count)++] = name;
    }
    closedir(dir);
    if (*count > 1) qsort(*names, *count, sizeof(**names), compare_names);
    return 0;
}
void scene_list_free(char **names, unsigned count) { for (unsigned i = 0; i < count; i++) free(names[i]); free(names); }
int scene_delete(const char *name) {
    char *path = scene_filename(name); if (!path) return -errno;
    char *startup = scene_startup_get();
    if (!startup) { free(path); return -errno; }
    if (streq(startup, name)) {
        char error[128]; int result = scene_startup_set("off", error, sizeof(error));
        if (result < 0) { free(startup); free(path); return result; }
    }
    free(startup);
    int result = unlink(path) < 0 && errno != ENOENT ? -errno : 0; free(path);
    if (!result) result = clear_scene_recovery(name);
    return result;
}

static char *startup_filename(void) {
    char *directory = scene_directory(); if (!directory) return NULL;
    char *suffix = strrchr(directory, '/'); *suffix = 0;
    char *path; xasprintf(&path, "%s/startup-scene", directory); free(directory); return path;
}
char *scene_startup_get(void) {
    char *path = startup_filename(); if (!path) return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC); free(path);
    if (fd < 0) return errno == ENOENT ? xstrdup("off") : NULL;
    char buffer[64] = {0}; ssize_t got;
    do { got = read(fd, buffer, sizeof(buffer) - 1); } while (got < 0 && errno == EINTR);
    int saved = errno; close(fd);
    if (got < 0) { errno = saved; return NULL; }
    if (got == sizeof(buffer) - 1 || memchr(buffer, 0, got)) { errno = EINVAL; return NULL; }
    if (got > 0 && buffer[got - 1] == '\n') buffer[--got] = 0;
    if (!managed_valid_name(buffer)) { errno = EINVAL; return NULL; }
    return xstrdup(buffer);
}
int scene_startup_set(const char *name, char *error, size_t size) {
    if (!managed_valid_name(name)) return fail(error, size, EINVAL, "invalid startup scene name");
    if (!streq(name, "off")) { int result = scene_validate(name, error, size); if (result < 0) return result; }
    char *filename = startup_filename(), *temporary = NULL, *directory = NULL;
    if (!filename) return fail(error, size, errno, "cannot locate startup scene selection");
    directory = xstrdup(filename); *strrchr(directory, '/') = 0;
    int result = make_directory(directory); if (result < 0) goto out;
    xasprintf(&temporary, "%s.tmp-XXXXXX", filename);
    int fd = mkostemp(temporary, O_CLOEXEC); if (fd < 0) { result = -errno; goto out; }
    FILE *file = fdopen(fd, "w"); if (!file) { result = -errno; close(fd); goto out; }
    fprintf(file, "%s\n", name);
    if (ferror(file) || fflush(file) || fsync(fd)) result = -(errno ?: EIO);
    if (fclose(file) && !result) result = -errno;
    if (!result && rename(temporary, filename) < 0) result = -errno;
    if (!result) {
        int dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirfd < 0) result = -errno;
        else { if (fsync(dirfd) < 0) result = -errno; close(dirfd); }
    }
out:
    if (temporary) unlink(temporary);
    if (result < 0) fail(error, size, -result, "cannot save startup scene selection: %s", strerror(-result));
    free(temporary); free(filename); free(directory); return result;
}

static int node_index(const struct json *doc, const char *name) {
    const struct json *nodes = jget(doc, "nodes");
    if (name) for (unsigned i = 0; i < nodes->count; i++) if (streq(jtext(nodes->items[i], "name"), name)) return i;
    return -1;
}
static int saved_device_index(const struct json *doc, const char *name) {
    const struct json *devices = jget(doc, "devices");
    if (name && devices) for (unsigned i = 0; i < devices->count; i++) if (streq(name, jtext(devices->items[i], "name"))) return i;
    return -1;
}
static char *path_node_name(const struct json *path, const char *role) {
    char *name; xasprintf(&name, "pipemixer.%s.%s.%s", jtext(path, "kind"), jtext(path, "name"), role); return name;
}
static int path_owner(const struct json *doc, const char *name) {
    const struct json *paths = jget(doc, "paths");
    for (unsigned i = 0; i < paths->count; i++) {
        char *in = path_node_name(paths->items[i], "input"), *out = path_node_name(paths->items[i], "output");
        bool match = streq(name, in) || streq(name, out); free(in); free(out); if (match) return i;
    }
    return -1;
}
struct edge { unsigned from, to; };
static bool valid_rule_owner(const char *rule, const char *scope) {
    if (!managed_valid_name(rule) || !scope || strlen(scope) != 16) return false;
    for (unsigned i = 0; i < 16; i++)
        if (!(scope[i] >= '0' && scope[i] <= '9') && !(scope[i] >= 'a' && scope[i] <= 'f')) return false;
    return true;
}
static int validate_schema(struct json *doc, char *error, size_t size) {
    const struct json *version = jget(doc, "version"), *paths = jget(doc, "paths"), *nodes = jget(doc, "nodes"),
                      *links = jget(doc, "links"), *defaults = jget(doc, "defaults");
    const struct json *devices = jget(doc, "devices");
    if (!streq(jtext(doc, "format"), "pipemixer.scene") || !typed(version, J_NUMBER) || (version->number != 1 && version->number != 2)
        || !typed(paths, J_ARRAY) || !typed(nodes, J_ARRAY) || !typed(links, J_ARRAY) || !typed(defaults, J_OBJECT)
        || paths->count > PATH_LIMIT || nodes->count > NODE_LIMIT || links->count > LINK_LIMIT)
        return fail(error, size, EINVAL, "invalid scene format, version or size");
    if ((version->number == 2 && !typed(devices, J_ARRAY)) || (devices && (!typed(devices, J_ARRAY) || devices->count > NODE_LIMIT)))
        return fail(error, size, EINVAL, "invalid scene hardware devices");
    for (unsigned i = 0; devices && i < devices->count; i++) {
        const char *name = jtext(devices->items[i], "name"), *profile = jtext(devices->items[i], "profile");
        if (!name || !*name || strlen(name) > 4096 || !profile || !*profile || strlen(profile) > 4096 || saved_device_index(doc, name) != (int)i)
            return fail(error, size, EINVAL, "invalid or duplicate hardware device/profile");
    }
    for (unsigned i = 0; i < nodes->count; i++) {
        const struct json *node = nodes->items[i];
        const char *name = jtext(node, "name"), *class = jtext(node, "class");
        if (!name || !*name || strlen(name) > 4096 || !class || node_index(doc, name) != (int)i)
            return fail(error, size, EINVAL, "invalid or duplicate node name");
        const struct json *route = jget(node, "route"), *device = jget(node, "device");
        if ((device && (device->type != J_STRING || saved_device_index(doc, device->text) < 0))
            || (route && (route->type != J_STRING || !*route->text || strlen(route->text) > 4096 || !device)))
            return fail(error, size, EINVAL, "invalid hardware route/device on '%s'", name);
        const struct json *channels = jget(node, "channels"), *params = jget(node, "params"), *mute = jget(node, "mute");
        if (!typed(channels, J_ARRAY) || channels->count > SPA_AUDIO_MAX_CHANNELS
            || !typed(params, J_ARRAY) || params->count > 1024 || !typed(mute, J_BOOL))
            return fail(error, size, EINVAL, "invalid state for node '%s'", name);
        for (unsigned c = 0; c < channels->count; c++) {
            const struct json *channel = channels->items[c], *volume = jget(channel, "volume");
            const char *channel_name = jtext(channel, "name");
            if (!channel_name || !*channel_name || !typed(volume, J_NUMBER) || volume->number < 0 || volume->number > 1000)
                return fail(error, size, EINVAL, "invalid channel volume on '%s'", name);
            for (unsigned k = 0; k < c; k++) if (streq(channel_name, jtext(channels->items[k], "name")))
                return fail(error, size, EINVAL, "duplicate channel on '%s'", name);
        }
        for (unsigned c = 0; c < params->count; c++) {
            const char *param = jtext(params->items[c], "name");
            if (!param || !*param || !typed(jget(params->items[c], "value"), J_NUMBER))
                return fail(error, size, EINVAL, "invalid DSP parameter on '%s'", name);
            for (unsigned k = 0; k < c; k++) if (streq(param, jtext(params->items[k], "name")))
                return fail(error, size, EINVAL, "duplicate DSP parameter on '%s'", name);
        }
        const struct json *target = jget(node, "target");
        if (target && (!typed(target, J_STRING) || (*target->text && node_index(doc, target->text) < 0)))
            return fail(error, size, EINVAL, "invalid stream target on '%s'", name);
    }
    struct edge *edges = xcalloc(links->count + paths->count * 3 + 1, sizeof(*edges));
    unsigned n_edges = 0;
    int result = 0;
    for (unsigned i = 0; i < paths->count; i++) {
        const struct json *path = paths->items[i];
        const char *kind = jtext(path, "kind"), *name = jtext(path, "name");
        if ((!streq(kind, "bus") && !streq(kind, "send") && !streq(kind, "effect") && !streq(kind, "monitor")) || !managed_valid_name(name)) {
            result = fail(error, size, EINVAL, "invalid managed path"); goto done;
        }
        for (unsigned k = 0; k < i; k++) if (streq(kind, jtext(paths->items[k], "kind")) && streq(name, jtext(paths->items[k], "name"))) {
            result = fail(error, size, EINVAL, "duplicate managed path '%s'", name); goto done;
        }
        char *in = path_node_name(path, "input"), *out = path_node_name(path, "output");
        int input = node_index(doc, in), output = node_index(doc, out); free(in); free(out);
        if (input < 0 || output < 0) { result = fail(error, size, EINVAL, "missing nodes for '%s'", name); goto done; }
        edges[n_edges++] = (struct edge){input, output};
        if (streq(kind, "monitor") && (!monitor_valid_name(name) || !monitor_valid_scope(jtext(path, "monitor_set")))) {
            result = fail(error, size, EINVAL, "invalid monitor ownership"); goto done;
        }
        if (streq(kind, "send")) {
            int source = node_index(doc, jtext(path, "source")), destination = node_index(doc, jtext(path, "destination"));
            if (source < 0 || destination < 0) { result = fail(error, size, EINVAL, "missing send endpoints for '%s'", name); goto done; }
            edges[n_edges++] = (struct edge){source, input}; edges[n_edges++] = (struct edge){output, destination};
        }
        if (streq(kind, "effect")) {
            const char *preset = jtext(path, "preset"), *graph = jtext(path, "graph");
            if (!streq(preset, "eq") && !streq(preset, "voice") && !streq(preset, "custom")) {
                result = fail(error, size, EINVAL, "invalid effect preset for '%s'", name); goto done;
            }
            if (streq(preset, "custom") && (!graph || graph[0] != '{' || strlen(graph) > 1024 * 1024)) {
                result = fail(error, size, EINVAL, "missing custom graph for '%s'", name); goto done;
            }
            const struct json *spec = jget(path, "chain"); struct effect_chain chain;
            if (spec && (!typed(spec,J_STRING) || !streq(preset,"custom") || effect_chain_parse(spec->text,&chain) < 0)) {
                result = fail(error,size,EINVAL,"invalid editable chain for '%s'",name); goto done;
            }
        }
    }
    for (unsigned i = 0; i < links->count; i++) {
        const struct json *link = links->items[i], *out = jget(link, "output"), *in = jget(link, "input");
        int source = node_index(doc, jtext(out, "node")), destination = node_index(doc, jtext(in, "node"));
        if (source < 0 || destination < 0 || !jtext(out, "port") || !*jtext(out, "port") || !jtext(in, "port") || !*jtext(in, "port")) {
            result = fail(error, size, EINVAL, "invalid link endpoints"); goto done;
        }
        if ((jget(link, "route_rule") || jget(link, "rule_set"))
            && !valid_rule_owner(jtext(link, "route_rule"), jtext(link, "rule_set"))) {
            result = fail(error, size, EINVAL, "invalid scene link rule ownership"); goto done;
        }
        const struct json *batch=jget(link,"batch");
        if(batch&&(!typed(batch,J_STRING)||!*batch->text||strlen(batch->text)>128)){result=fail(error,size,EINVAL,"invalid link batch");goto done;}
        edges[n_edges++] = (struct edge){source, destination};
    }
    for (unsigned i = 0; i < defaults->count; i++) {
        const struct json *value = defaults->items[i];
        if ((!streq(value->key, "sink") && !streq(value->key, "source"))
            || (value->type != J_NULL && (value->type != J_STRING || node_index(doc, value->text) < 0))) {
            result = fail(error, size, EINVAL, "invalid default device"); goto done;
        }
    }
    /* Kahn's algorithm checks the desired graph, including implicit DSP and
     * loopback edges, before any worker or link is created. */
    unsigned *degree = xcalloc(nodes->count + 1, sizeof(*degree)), *queue = xcalloc(nodes->count + 1, sizeof(*queue));
    for (unsigned i = 0; i < n_edges; i++) degree[edges[i].to]++;
    unsigned used = 0;
    for (unsigned i = 0; i < nodes->count; i++) if (!degree[i]) queue[used++] = i;
    for (unsigned at = 0; at < used; at++) for (unsigned i = 0; i < n_edges; i++)
        if (edges[i].from == queue[at] && !--degree[edges[i].to]) queue[used++] = edges[i].to;
    if (used != nodes->count) result = fail(error, size, ELOOP, "scene would create an audio feedback loop");
    free(degree); free(queue);
done:
    free(edges); return result;
}
static struct json *read_document(const char *path, size_t limit, char *error, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { fail(error, size, errno, "cannot open '%s': %s", path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > limit) {
        close(fd); errno = EINVAL; fail(error, size, errno, "scene is empty, too large or not a regular file"); return NULL;
    }
    char *data = xcalloc(st.st_size + 1, 1); size_t n = 0;
    while (n < (size_t)st.st_size) {
        ssize_t got = read(fd, data + n, st.st_size - n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        n += got;
    }
    close(fd);
    struct json *doc = NULL;
    if (n == (size_t)st.st_size && !memchr(data, 0, n)) {
        struct spa_json iter = SPA_JSON_INIT(data, n); const char *token; unsigned count = 0;
        int len = spa_json_next(&iter, &token);
        if (len > 0) doc = jparse(&iter, token, len, 0, &count, limit > SCENE_LIMIT ? 131072 : 65536);
        if (doc && spa_json_next(&iter, &token) != 0) { jfree(doc); doc = NULL; }
    }
    free(data);
    if (!doc) { errno = EINVAL; fail(error, size, errno, "invalid scene JSON"); return NULL; }
    return doc;
}
static struct json *read_scene(const char *name, char *error, size_t size) {
    char *path = scene_filename(name);
    if (!path) { fail(error, size, errno, "cannot locate scene"); return NULL; }
    struct json *doc = read_document(path, SCENE_LIMIT, error, size); free(path);
    if (!doc) return NULL;
    int result = validate_schema(doc, error, size);
    if (result < 0) { jfree(doc); errno = -result; return NULL; }
    return doc;
}
int scene_validate(const char *name, char *error, size_t size) {
    struct json *doc = read_scene(name, error, size); if (!doc) return -errno; jfree(doc); return 0;
}

struct lookup { const char *name; uint32_t id; unsigned count; };
static void lookup_node(const struct graph_node *node, void *data) {
    struct lookup *lookup = data;
    if (graph_node_is_audio(node) && streq(lookup->name, graph_node_name(node->id))) { lookup->id = node->id; lookup->count++; }
}
static int resolve_node(const char *name, uint32_t *id) {
    struct lookup lookup = {.name = name}; graph_foreach_node(lookup_node, &lookup);
    if (lookup.count != 1) return lookup.count ? -ENOTUNIQ : -ENOENT;
    *id = lookup.id; return 0;
}
struct port_lookup { uint32_t node, id; const char *name; enum pw_direction direction; unsigned count; };
static void lookup_port(const struct graph_port *port, void *data) {
    struct port_lookup *find = data;
    if (port->node_id == find->node && port->direction == find->direction && graph_port_is_audio(port)
        && streq(find->name, dict_get(&port->props, PW_KEY_PORT_NAME))) { find->id = port->id; find->count++; }
}
static int resolve_endpoint(const struct json *endpoint, enum pw_direction direction, uint32_t *id) {
    uint32_t node; int result = resolve_node(jtext(endpoint, "node"), &node); if (result < 0) return result;
    struct port_lookup lookup = {.node = node, .name = jtext(endpoint, "port"), .direction = direction};
    graph_foreach_port(lookup_port, &lookup);
    if (lookup.count != 1) return lookup.count ? -ENOTUNIQ : -ENOENT;
    *id = lookup.id; return 0;
}
static struct json *endpoint_json(uint32_t port_id) {
    const struct graph_port *port = graph_port_find(port_id);
    struct json *endpoint = jnew(J_OBJECT);
    jadd(endpoint, "node", jstr(graph_node_name(port->node_id)));
    jadd(endpoint, "port", jstr(dict_get(&port->props, PW_KEY_PORT_NAME))); return endpoint;
}
struct capture { struct json *doc; int status; char *error; size_t size; };
static void capture_device(struct device *device, void *data) {
    struct capture *capture = data; if (capture->status) return;
    if (!device_profiles_ready(device)) { capture->status = -EAGAIN; return; }
    unsigned count; const struct param_profile *profiles = device_get_profiles(device, &count);
    const struct param_profile *active = NULL;
    for (unsigned i = 0; i < count; i++) if (profiles[i].active) {
        if (active) { capture->status = fail(capture->error, capture->size, ENOTUNIQ, "hardware device has multiple active profiles"); return; }
        active = &profiles[i];
    }
    if (!count) return;
    if (!active) { capture->status = -EAGAIN; return; }
    const char *name = dict_get(device_properties(device), PW_KEY_DEVICE_NAME);
    if (!name || !*name) { capture->status = fail(capture->error, capture->size, EINVAL, "hardware device has no stable name"); return; }
    struct json *item = jnew(J_OBJECT); jadd(item, "name", jstr(name)); jadd(item, "profile", jstr(active->name)); jadd(jget(capture->doc, "devices"), NULL, item);
}
struct send_endpoint { uint32_t input, output; const char *source, *destination; bool ambiguous; };
static void capture_send_link(const struct graph_link *link, void *data) {
    struct send_endpoint *endpoint = data;
    if (!graph_link_is_audio(link)) return;
    const struct pw_link_info *info = link->info;
    if (info->input_node_id == endpoint->input) {
        const char *name = graph_node_name(info->output_node_id);
        if (endpoint->source && !streq(endpoint->source, name)) endpoint->ambiguous = true;
        endpoint->source = name;
    }
    if (info->output_node_id == endpoint->output) {
        const char *name = graph_node_name(info->input_node_id);
        if (endpoint->destination && !streq(endpoint->destination, name)) endpoint->ambiguous = true;
        endpoint->destination = name;
    }
}
static void capture_node(const struct graph_node *graph, void *data) {
    struct capture *capture = data; if (capture->status || !graph_node_is_audio(graph)) return;
    const char *name = graph_node_name(graph->id), *class = dict_get(&graph->props, PW_KEY_MEDIA_CLASS);
    if (!name || !*name || !class) { capture->status = fail(capture->error, capture->size, EINVAL, "audio node has no stable name/class"); return; }
    struct json *node = jnew(J_OBJECT), *channels = jnew(J_ARRAY), *params = jnew(J_ARRAY);
    jadd(node, "name", jstr(name)); jadd(node, "class", jstr(class));
    jadd(node, "channels", channels); jadd(node, "params", params);
    struct node *legacy = node_lookup(graph->id);
    const struct param_props *props = node_get_params(legacy);
    /* Stream adapters initially report zero channels before format/port
     * negotiation. Do not persist that temporary layout as a valid scene. */
    if (legacy && (!props || !props->n_channels)) { jfree(node); capture->status = -EAGAIN; return; }
    jadd(node, "mute", jbool(props && props->mute));
    const char *hardware_id = dict_get(&graph->props, PW_KEY_DEVICE_ID); uint32_t device_id;
    if (hardware_id && spa_atou32(hardware_id, &device_id, 10)) {
        struct device *device = device_lookup(device_id);
        const char *hardware = device ? dict_get(device_properties(device), PW_KEY_DEVICE_NAME) : NULL;
        if (hardware && saved_device_index(capture->doc, hardware) >= 0) {
            jadd(node, "device", jstr(hardware));
            if (!node_routes_ready(legacy)) { jfree(node); capture->status = -EAGAIN; return; }
            unsigned count; const struct param_route *routes = node_get_routes(legacy, &count);
            for (unsigned r = 0; r < count; r++) if (routes[r].active) { jadd(node, "route", jstr(routes[r].name)); break; }
        }
    }
    if (props) for (unsigned c = 0; c < props->n_channels; c++) {
        struct json *channel = jnew(J_OBJECT);
        jadd(channel, "name", jstr(props->channel_names[c]));
        jadd(channel, "volume", jnum(props->channel_volumes[c] * 100)); jadd(channels, NULL, channel);
    }
    if (streq(dict_get(&graph->props, "pipemixer.kind"), "effect")
        && streq(dict_get(&graph->props, "pipemixer.role"), "input")) {
        if (!graph->controls_ready) { jfree(node); capture->status = -EAGAIN; return; }
        for (unsigned c = 0; c < graph->n_controls; c++) {
            const struct graph_control *control = &graph->controls[c];
            if (!control->has_info || !control->has_value || !control->visible || !control->writable) continue;
            struct json *param = jnew(J_OBJECT); jadd(param, "name", jstr(control->name));
            jadd(param, "value", jnum(control->value)); jadd(params, NULL, param);
        }
    }
    bool managed = streq(dict_get(&graph->props, "pipemixer.managed"), "1");
    if (!managed && (streq(class, "Stream/Output/Audio") || streq(class, "Stream/Input/Audio"))) {
        uint32_t target = pipewire_get_stream_target(graph->id);
        jadd(node, "target", jstr(target == PW_ID_ANY ? "" : graph_node_name(target)));
    }
    jadd(jget(capture->doc, "nodes"), NULL, node);
    if (!managed || !streq(dict_get(&graph->props, "pipemixer.role"), "input")) return;
    const char *kind = dict_get(&graph->props, "pipemixer.kind"), *group = dict_get(&graph->props, "pipemixer.group");
    struct json *path = jnew(J_OBJECT); jadd(path, "kind", jstr(kind)); jadd(path, "name", jstr(group));
    if (streq(kind, "monitor")) jadd(path, "monitor_set", jstr(dict_get(&graph->props, "pipemixer.monitor-set")));
    if (streq(kind, "effect")) {
        const char *preset = dict_get(&graph->props, "pipemixer.preset"), *effect_graph = dict_get(&graph->props, "pipemixer.filter.graph");
        jadd(path, "preset", jstr(preset));
        if (streq(preset, "custom")) jadd(path, "graph", jstr(effect_graph));
        const char *spec = dict_get(&graph->props, "pipemixer.chain");
        if (spec) jadd(path,"chain",jstr(spec));
    } else if (streq(kind, "send")) {
        const struct graph_node *output = managed_find(kind, group, "output");
        struct send_endpoint endpoint = {.input = graph->id, .output = output ? output->id : PW_ID_ANY};
        graph_foreach_link(capture_send_link, &endpoint);
        const char *source = dict_get(&graph->props, "pipemixer.source"), *destination = dict_get(&graph->props, "pipemixer.destination");
        if (!source) source = endpoint.source;
        if (!destination) destination = endpoint.destination;
        if ((!source || !destination) || (endpoint.ambiguous && (!dict_get(&graph->props, "pipemixer.source") || !dict_get(&graph->props, "pipemixer.destination")))) {
            jfree(path); capture->status = fail(capture->error, capture->size, EINVAL, "cannot determine send '%s' endpoints", group); return;
        }
        jadd(path, "source", jstr(source)); jadd(path, "destination", jstr(destination));
    }
    jadd(jget(capture->doc, "paths"), NULL, path);
}
static void capture_link(const struct graph_link *link, void *data) {
    struct capture *capture = data; if (capture->status || !graph_link_is_audio(link)) return;
    struct json *item = jnew(J_OBJECT);
    jadd(item, "output", endpoint_json(link->info->output_port_id));
    jadd(item, "input", endpoint_json(link->info->input_port_id));
    const char *rule = link->info->props ? spa_dict_lookup(link->info->props, "pipemixer.route-rule") : NULL,
               *scope = link->info->props ? spa_dict_lookup(link->info->props, "pipemixer.rule-set") : NULL;
    if (valid_rule_owner(rule, scope)) {
        jadd(item, "route_rule", jstr(rule)); jadd(item, "rule_set", jstr(scope));
    }
    const char *batch=link->info->props?spa_dict_lookup(link->info->props,"pipemixer.batch"):NULL;
    if(batch&&*batch&&strlen(batch)<=128)jadd(item,"batch",jstr(batch));
    jadd(jget(capture->doc, "links"), NULL, item);
}
static int compare_objects(const void *a, const void *b) {
    const struct json *left = *(struct json *const *)a, *right = *(struct json *const *)b;
    const char *name = jtext(left, "name"), *other = jtext(right, "name");
    return strcmp(name ?: "", other ?: "");
}
static int scene_lock(bool shared) {
    char *path;
    xasprintf(&path, "%s/pipemixer-scene.lock", getenv("XDG_RUNTIME_DIR") ?: "/tmp");
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600); free(path);
    if (fd < 0) return -errno;
    /* Recovery owns this lock only during one engine tick. Give that short
     * update time to finish; a concurrent scene load still reports busy. */
    for (unsigned attempt = 0; ; attempt++) {
        if (flock(fd, (shared ? LOCK_SH : LOCK_EX) | LOCK_NB) == 0) return fd;
        int result = -errno;
        if ((errno != EWOULDBLOCK && errno != EINTR) || attempt == 20) {
            close(fd); return result == -EWOULDBLOCK ? -EBUSY : result;
        }
        const struct timespec delay = {.tv_nsec = 5000000};
        nanosleep(&delay, NULL);
    }
}
int scene_save(const char *name, char *error, size_t size) {
    if (!graph_ready()) return -EAGAIN;
    int lock = scene_lock(true);
    if (lock < 0) return fail(error, size, -lock, "another scene is being loaded");
    struct json *doc = jnew(J_OBJECT);
    jadd(doc, "format", jstr("pipemixer.scene")); jadd(doc, "version", jnum(2));
    jadd(doc, "devices", jnew(J_ARRAY));
    jadd(doc, "paths", jnew(J_ARRAY)); jadd(doc, "nodes", jnew(J_ARRAY)); jadd(doc, "links", jnew(J_ARRAY));
    struct json *defaults = jnew(J_OBJECT); jadd(doc, "defaults", defaults);
    jadd(defaults, "sink", jstr(pipewire_get_default(DEFAULT_AUDIO_SINK)));
    jadd(defaults, "source", jstr(pipewire_get_default(DEFAULT_AUDIO_SOURCE)));
    struct capture capture = {.doc = doc, .error = error, .size = size};
    pipewire_foreach_device(capture_device, &capture);
    graph_foreach_node(capture_node, &capture); graph_foreach_link(capture_link, &capture);
    int result = capture.status;
    if (!result) result = validate_schema(doc, error, size);
    if (result < 0) { jfree(doc); close(lock); return result; }
    struct json *nodes = jget(doc, "nodes"), *paths = jget(doc, "paths");
    if (nodes->count > 1) qsort(nodes->items, nodes->count, sizeof(*nodes->items), compare_objects);
    if (paths->count > 1) qsort(paths->items, paths->count, sizeof(*paths->items), compare_objects);
    char *directory = scene_directory(), *filename = scene_filename(name), *temporary = NULL;
    if (!directory || !filename) { result = -errno; goto out; }
    result = make_directory(directory); if (result < 0) goto out;
    xasprintf(&temporary, "%s.tmp-XXXXXX", filename);
    int fd = mkostemp(temporary, O_CLOEXEC); if (fd < 0) { result = -errno; goto out; }
    FILE *file = fdopen(fd, "w"); if (!file) { result = -errno; close(fd); goto out; }
    jwrite(file, doc, 0); fputc('\n', file);
    if (ferror(file) || fflush(file) || ftell(file) > SCENE_LIMIT || fsync(fd)) result = -(errno ?: EFBIG);
    if (fclose(file) && !result) result = -errno;
    if (!result && rename(temporary, filename) < 0) result = -errno;
    if (!result) {
        int dirfd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dirfd >= 0) { if (fsync(dirfd) < 0) result = -errno; close(dirfd); }
    }
out:
    if (temporary) unlink(temporary);
    if (result < 0) fail(error, size, -result, "cannot save scene: %s", strerror(-result));
    free(temporary); free(filename); free(directory); jfree(doc); close(lock); return result;
}

struct path_job { pid_t pid, preview_pid; char *temporary; bool ready, created, replace; char preview[49]; };
struct scene_job {
    struct json *doc;
    struct json *snapshot;
    char *name;
    struct path_job *paths;
    bool *node_sent, *link_sent;
    bool defaults_sent, remove_sent;
    unsigned phase;
    int lock_fd;
    unsigned skipped;
    bool activated;
    bool available, prepared;
    bool *profile_sent, *route_sent, *skip_devices;
    char *edit_name;
    bool *affected;
    unsigned replace_phase;
    bool committed;
    bool rolled_back;
    int edit_failure;
    char edit_error[256];
    time_t edit_deadline;
    int edit_pending_error;
};
static int activate_recovery(struct scene_job *job, char *error, size_t size);
static int complete_edit_recovery(struct scene_job *job);
struct device_lookup_state { const char *name; struct device *device; unsigned count; bool pending; };
static void find_device(struct device *device, void *data) {
    struct device_lookup_state *state = data;
    const struct dict *props = device_properties(device);
    if (!props) { state->pending = true; return; }
    if (streq(state->name, dict_get(props, PW_KEY_DEVICE_NAME))) { state->device = device; state->count++; }
}
static int resolve_device(const char *name, struct device **device) {
    struct device_lookup_state state = {.name = name}; pipewire_foreach_device(find_device, &state);
    if (state.pending) return -EAGAIN;
    if (state.count != 1) return state.count ? -ENOTUNIQ : -ENOENT;
    *device = state.device; return 0;
}
static int profile_choice(const struct json *saved, struct device **device, const struct param_profile **profile) {
    int result = resolve_device(jtext(saved, "name"), device); if (result < 0) return result;
    if (!device_profiles_ready(*device)) return -EAGAIN;
    unsigned count, matches = 0; const struct param_profile *profiles = device_get_profiles(*device, &count);
    for (unsigned i = 0; i < count; i++) if (streq(jtext(saved, "profile"), profiles[i].name)) { *profile = &profiles[i]; matches++; }
    if (matches != 1) return matches ? -ENOTUNIQ : -ENOTSUP;
    if (!(*profile)->active && (*profile)->availability == SPA_PARAM_AVAILABILITY_no) return -ENXIO;
    return 0;
}
static int route_choice(const struct json *saved, struct node **node, const struct param_route **route) {
    uint32_t id; int result = resolve_node(jtext(saved, "name"), &id); if (result < 0) return result;
    const struct graph_node *graph = graph_node_find(id);
    if (!streq(jtext(saved, "class"), dict_get(&graph->props, PW_KEY_MEDIA_CLASS))) return -EINVAL;
    const char *dev_id = dict_get(&graph->props, PW_KEY_DEVICE_ID); uint32_t did;
    if (!dev_id || !spa_atou32(dev_id, &did, 10)) return -ENODEV;
    struct device *device = device_lookup(did); const struct dict *props = device_properties(device);
    if (!props) return -EAGAIN;
    if (!streq(jtext(saved, "device"), dict_get(props, PW_KEY_DEVICE_NAME))) return -EXDEV;
    *node = node_lookup(id); if (!node_routes_ready(*node)) return -EAGAIN;
    unsigned count, matches = 0; const struct param_route *routes = node_get_routes(*node, &count);
    for (unsigned i = 0; i < count; i++) if (streq(jtext(saved, "route"), routes[i].name)) { *route = &routes[i]; matches++; }
    if (matches != 1) return matches ? -ENOTUNIQ : -ENOTSUP;
    if (!(*route)->active && (*route)->availability == SPA_PARAM_AVAILABILITY_no) return -ENXIO;
    return 0;
}
static int restore_profiles(struct scene_job *job, char *error, size_t size) {
    const struct json *devices = jget(job->doc, "devices"); bool pending = false;
    /* Validate the complete profile plan before changing any card. */
    for (unsigned i = 0; devices && i < devices->count; i++) {
        struct device *device; const struct param_profile *profile;
        int result = profile_choice(devices->items[i], &device, &profile);
        if (result == -EAGAIN) { pending = true; continue; }
        if (job->available && (result == -ENOENT || result == -ENOTSUP || result == -ENXIO)) { job->skip_devices[i] = true; continue; }
        job->skip_devices[i] = false;
        if (result < 0) return fail(error, size, -result, "hardware profile '%s' on '%s' unavailable: %s", jtext(devices->items[i], "profile"), jtext(devices->items[i], "name"), strerror(-result));
    }
    if (pending) return -EAGAIN;
    for (unsigned i = 0; devices && i < devices->count; i++) {
        if (job->skip_devices[i]) continue;
        struct device *device; const struct param_profile *profile;
        int result = profile_choice(devices->items[i], &device, &profile); if (result < 0) return result;
        if (profile->active) continue;
        pending = true;
        if (!job->profile_sent[i]) {
            result = device_set_profile(device, profile->index);
            if (result < 0) return fail(error, size, -result, "cannot restore profile on '%s'", jtext(devices->items[i], "name"));
            job->profile_sent[i] = true;
        }
    }
    return pending ? -EAGAIN : 0;
}
static int restore_routes(struct scene_job *job, char *error, size_t size) {
    const struct json *nodes = jget(job->doc, "nodes"); bool pending = false;
    for (unsigned i = 0; i < nodes->count; i++) {
        if (!jtext(nodes->items[i], "route")) continue;
        struct node *node; const struct param_route *route; int result = route_choice(nodes->items[i], &node, &route);
        if (result == -EAGAIN) { pending = true; continue; }
        if (result < 0) return fail(error, size, -result, "hardware route '%s' on '%s' unavailable", jtext(nodes->items[i], "route"), jtext(nodes->items[i], "name"));
    }
    if (pending) return -EAGAIN;
    for (unsigned i = 0; i < nodes->count; i++) {
        if (!jtext(nodes->items[i], "route")) continue;
        struct node *node; const struct param_route *route;
        int result = route_choice(nodes->items[i], &node, &route);
        if (result == -EAGAIN) { pending = true; continue; }
        if (result < 0) return fail(error, size, -result, "hardware route '%s' on '%s' unavailable: %s", jtext(nodes->items[i], "route"), jtext(nodes->items[i], "name"), strerror(-result));
        if (route->active) continue;
        pending = true;
        if (!job->route_sent[i]) {
            result = node_set_route(node, route->index); if (result < 0) return result;
            job->route_sent[i] = true;
        }
    }
    return pending ? -EAGAIN : 0;
}
static int path_matches(const struct json *path, char *error, size_t size) {
    const char *kind = jtext(path, "kind"), *name = jtext(path, "name");
    const struct graph_node *input = managed_find(kind, name, "input"), *output = managed_find(kind, name, "output");
    if (!input && !output) return 0;
    if (!input || !output) return -EAGAIN;
    if (streq(kind, "monitor") && (!streq(jtext(path, "monitor_set"), dict_get(&input->props, "pipemixer.monitor-set"))
        || !streq(jtext(path, "monitor_set"), dict_get(&output->props, "pipemixer.monitor-set"))))
        return fail(error, size, EEXIST, "monitor '%s' belongs to another configuration", name);
    if (streq(kind, "effect")) {
        if (!streq(jtext(path, "preset"), dict_get(&input->props, "pipemixer.preset")))
            return fail(error, size, EEXIST, "effect '%s' has a different preset", name);
        if (streq(jtext(path, "preset"), "custom") && !streq(jtext(path, "graph"), dict_get(&input->props, "pipemixer.filter.graph")))
            return fail(error, size, EEXIST, "effect '%s' has a different custom graph", name);
        const char *saved_chain=jtext(path,"chain"),*live_chain=dict_get(&input->props,"pipemixer.chain");
        if ((saved_chain||live_chain) && !streq(saved_chain,live_chain))
            return fail(error,size,EEXIST,"effect '%s' has a different chain",name);
    }
    if (streq(kind, "send")) {
        struct send_endpoint endpoint = {.input = input->id, .output = output->id};
        graph_foreach_link(capture_send_link, &endpoint);
        const char *source = dict_get(&input->props, "pipemixer.source") ?: endpoint.source,
                   *destination = dict_get(&input->props, "pipemixer.destination") ?: endpoint.destination;
        if (!streq(source, jtext(path, "source")) || !streq(destination, jtext(path, "destination")))
            return fail(error, size, EEXIST, "send '%s' has different endpoints", name);
    }
    return 0;
}
static void jremove(struct json *array, unsigned index) {
    jfree(array->items[index]);
    memmove(array->items + index, array->items + index + 1, (array->count - index - 1) * sizeof(*array->items));
    array->count--;
}
static unsigned prune_unavailable(struct json *doc, const bool *skip_devices, const bool *skip_routes) {
    struct json *nodes = jget(doc, "nodes"), *paths = jget(doc, "paths"), *links = jget(doc, "links"), *defaults = jget(doc, "defaults");
    bool *skip = xcalloc(nodes->count + 1, sizeof(*skip)), *skip_path = xcalloc(paths->count + 1, sizeof(*skip_path));
    for (unsigned i = 0; i < nodes->count; i++) {
        const char *name = jtext(nodes->items[i], "name"); uint32_t id;
        if (path_owner(doc, name) < 0 && resolve_node(name, &id) == -ENOENT) skip[i] = true;
        int device = saved_device_index(doc, jtext(nodes->items[i], "device"));
        if (device >= 0 && skip_devices[device]) skip[i] = true;
        if (skip_routes[i]) skip[i] = true;
    }
    bool changed;
    do {
        changed = false;
        for (unsigned i = 0; i < nodes->count; i++) {
            int target = node_index(doc, jtext(nodes->items[i], "target"));
            if (!skip[i] && target >= 0 && skip[target]) { skip[i] = true; changed = true; }
        }
        for (unsigned i = 0; i < paths->count; i++) {
            const struct json *path = paths->items[i]; if (skip_path[i] || !streq(jtext(path, "kind"), "send")) continue;
            int source = node_index(doc, jtext(path, "source")), destination = node_index(doc, jtext(path, "destination"));
            if (skip[source] || skip[destination]) {
                skip_path[i] = true; changed = true;
                char *in = path_node_name(path, "input"), *out = path_node_name(path, "output");
                skip[node_index(doc, in)] = true; skip[node_index(doc, out)] = true; free(in); free(out);
            }
        }
    } while (changed);
    for (unsigned i = links->count; i > 0; i--) {
        const struct json *link = links->items[i - 1];
        if (skip[node_index(doc, jtext(jget(link, "output"), "node"))] || skip[node_index(doc, jtext(jget(link, "input"), "node"))]) jremove(links, i - 1);
    }
    for (unsigned i = 0; i < defaults->count; i++) {
        struct json *value = defaults->items[i]; int index = node_index(doc, value->text);
        if (index >= 0 && skip[index]) { free(value->text); value->text = NULL; value->type = J_NULL; }
    }
    unsigned skipped = 0;
    for (unsigned i = paths->count; i > 0; i--) if (skip_path[i - 1]) jremove(paths, i - 1);
    for (unsigned i = nodes->count; i > 0; i--) if (skip[i - 1]) { jremove(nodes, i - 1); skipped++; }
    free(skip); free(skip_path); return skipped;
}
static int prepare_scene(struct scene_job *job, char *error, size_t size) {
    if (job->prepared) return 0;
    struct json *doc = job->doc;
    const struct json *saved_nodes = jget(doc, "nodes");
    bool *skip_routes = xcalloc(saved_nodes->count + 1, sizeof(*skip_routes));
    bool pending = false;
    /* Profile updates precede asynchronous node creation. A present restored
     * card's nodes must finish enumeration before optional-device pruning. */
    for (unsigned i = 0; i < saved_nodes->count; i++) {
        const struct json *saved = saved_nodes->items[i]; int device = saved_device_index(doc, jtext(saved, "device"));
        if (device < 0 || job->skip_devices[device]) continue;
        uint32_t id; int result = resolve_node(jtext(saved, "name"), &id);
        if (result == -ENOENT) { pending = true; continue; }
        if (result < 0) { free(skip_routes); return fail(error, size, -result, "hardware node '%s' is ambiguous", jtext(saved, "name")); }
        const struct param_props *props = node_get_params(node_lookup(id));
        if (jget(saved, "channels")->count && (!props || !props->n_channels)) { pending = true; continue; }
        if (jtext(saved, "route")) {
            struct node *node; const struct param_route *route; result = route_choice(saved, &node, &route);
            if (result == -EAGAIN) { pending = true; continue; }
            if (job->available && (result == -ENOTSUP || result == -ENXIO)) { skip_routes[i] = true; continue; }
            if (result < 0) { free(skip_routes); return fail(error, size, -result, "hardware route '%s' on '%s' unavailable", jtext(saved, "route"), jtext(saved, "name")); }
        }
    }
    if (pending) { free(skip_routes); return -EAGAIN; }
    job->skipped = job->available ? prune_unavailable(doc, job->skip_devices, skip_routes) : 0;
    free(skip_routes);
    const struct json *nodes = jget(doc, "nodes"), *paths = jget(doc, "paths");
    int result = 0;
    for (unsigned i = 0; i < nodes->count; i++) {
        const char *node_name = jtext(nodes->items[i], "name"); uint32_t id = PW_ID_ANY;
        result = resolve_node(node_name, &id);
        int owner = path_owner(doc, node_name);
        if (result == -ENOENT && owner >= 0) continue;
        if (result < 0) return fail(error, size, -result, "node '%s' is missing or ambiguous", node_name);
        const struct graph_node *node = graph_node_find(id);
        if (!streq(jtext(nodes->items[i], "class"), dict_get(&node->props, PW_KEY_MEDIA_CLASS))) {
            return fail(error, size, EINVAL, "node '%s' has a different media class", node_name);
        }
        if (owner >= 0 && (!streq(dict_get(&node->props, "pipemixer.managed"), "1")
            || !streq(dict_get(&node->props, "pipemixer.kind"), jtext(paths->items[owner], "kind"))
            || !streq(dict_get(&node->props, "pipemixer.group"), jtext(paths->items[owner], "name")))) {
            return fail(error, size, EEXIST, "node name '%s' is already in use", node_name);
        }
    }
    job->paths = xcalloc(paths->count + 1, sizeof(*job->paths));
    for (unsigned i = 0; i < paths->count; i++) {
        const struct json *path=paths->items[i]; const char *name=jtext(path,"name");
        result = path_matches(path,error,size);
        const struct graph_node *live=managed_find("effect",name,"input");
        bool editable=streq(jtext(path,"kind"),"effect") && (jtext(path,"chain") || (live && dict_get(&live->props,"pipemixer.chain")));
        if (result == -EEXIST && editable) { job->paths[i].replace=true; error[0]=0; }
        else if (result < 0 && result != -EAGAIN) return result;
        if (job->edit_name && streq(jtext(path,"kind"),"effect") && streq(name,job->edit_name)) job->paths[i].replace=true;
    }
    bool changed;
    do {
        changed=false;
        for(unsigned i=0;i<paths->count;i++) {
            const struct json *path=paths->items[i]; if(job->paths[i].replace || !streq(jtext(path,"kind"),"send"))continue;
            int src=path_owner(doc,jtext(path,"source")),dst=path_owner(doc,jtext(path,"destination"));
            if((src>=0 && job->paths[src].replace)||(dst>=0 && job->paths[dst].replace)) {job->paths[i].replace=true;changed=true;}
        }
    } while(changed);
    job->affected=xcalloc(nodes->count+1,sizeof(bool));
    for(unsigned i=0;i<nodes->count;i++) {
        int owner=path_owner(doc,jtext(nodes->items[i],"name"));
        job->affected[i]=owner>=0 && job->paths[owner].replace;
    }
    job->node_sent = xcalloc(nodes->count + 1, sizeof(*job->node_sent));
    job->route_sent = xcalloc(nodes->count + 1, sizeof(*job->route_sent));
    job->link_sent = xcalloc(jget(doc, "links")->count + 1, sizeof(*job->link_sent)); job->prepared = true;
    return 0;
}
static struct scene_job *load_scene(const char *name, bool available, char *error, size_t size) {
    struct json *doc = read_scene(name, error, size); if (!doc) return NULL;
    int lock = scene_lock(false);
    if (lock < 0) { jfree(doc); errno = -lock; fail(error, size, errno, "another scene is being loaded"); return NULL; }
    if (!available) {
        const struct json *nodes = jget(doc, "nodes");
        for (unsigned i = 0; i < nodes->count; i++) {
            const char *node = jtext(nodes->items[i], "name"); uint32_t id;
            if (path_owner(doc, node) >= 0 || jtext(nodes->items[i], "device")) continue;
            int result = resolve_node(node, &id);
            if (result < 0) { fail(error, size, -result, "external node '%s' is missing or ambiguous", node); jfree(doc); close(lock); errno = -result; return NULL; }
        }
    }
    struct scene_job *job = xcalloc(1, sizeof(*job)); job->doc = doc; job->snapshot = jclone(doc); job->lock_fd = lock;
    job->name = xstrdup(name); job->available = available;
    const struct json *devices = jget(doc, "devices"); unsigned count = devices ? devices->count : 0;
    job->profile_sent = xcalloc(count + 1, sizeof(*job->profile_sent)); job->skip_devices = xcalloc(count + 1, sizeof(*job->skip_devices));
    return job;
}
struct scene_job *scene_load(const char *name, char *error, size_t size) { return load_scene(name, false, error, size); }
struct scene_job *scene_load_available(const char *name, char *error, size_t size) { return load_scene(name, true, error, size); }
static void chain_param(struct json *params,const char *name,double value) {
    struct json *param=jnew(J_OBJECT);jadd(param,"name",jstr(name));jadd(param,"value",jnum(value));jadd(params,NULL,param);
}
struct scene_job *scene_edit_effect(const char *name,const struct effect_chain *chain,char *error,size_t size) {
    if(!managed_valid_name(name)){errno=EINVAL;return NULL;}
    if(!managed_find("effect",name,"input")){errno=ENOENT;fail(error,size,errno,"effect disappeared");return NULL;}
    int lock=scene_lock(false);if(lock<0){errno=-lock;fail(error,size,errno,"another scene or chain is being edited");return NULL;}
    struct json *doc=jnew(J_OBJECT);jadd(doc,"format",jstr("pipemixer.scene"));jadd(doc,"version",jnum(2));
    jadd(doc,"devices",jnew(J_ARRAY));jadd(doc,"nodes",jnew(J_ARRAY));jadd(doc,"paths",jnew(J_ARRAY));jadd(doc,"links",jnew(J_ARRAY));
    struct json *defaults=jnew(J_OBJECT);jadd(doc,"defaults",defaults);
    jadd(defaults,"sink",jstr(pipewire_get_default(DEFAULT_AUDIO_SINK)));jadd(defaults,"source",jstr(pipewire_get_default(DEFAULT_AUDIO_SOURCE)));
    struct capture capture={.doc=doc,.error=error,.size=size};pipewire_foreach_device(capture_device,&capture);
    graph_foreach_node(capture_node,&capture);graph_foreach_link(capture_link,&capture);
    if(capture.status<0){jfree(doc);close(lock);errno=-capture.status;return NULL;}
    struct scene_job *job=xcalloc(1,sizeof(*job));job->doc=doc;job->snapshot=jclone(doc);job->lock_fd=lock;job->edit_name=xstrdup(name);
    job->edit_deadline=time(NULL)+20;
    unsigned devices=jget(doc,"devices")->count;
    job->profile_sent=xcalloc(devices+1,sizeof(bool));job->skip_devices=xcalloc(devices+1,sizeof(bool));
    struct json *paths=jget(doc,"paths");
    for(unsigned i=0;i<paths->count;i++) {
        struct json *path=paths->items[i];if(!streq(jtext(path,"kind"),"effect")||!streq(jtext(path,"name"),name))continue;
        jreplace_string(jget(path,"preset"),"custom");char *graph=effect_chain_graph(chain),*spec=effect_chain_spec(chain);
        if(jget(path,"graph"))jreplace_string(jget(path,"graph"),graph);else jadd(path,"graph",jstr(graph));
        if(jget(path,"chain"))jreplace_string(jget(path,"chain"),spec);else jadd(path,"chain",jstr(spec));free(graph);free(spec);
        char *input=path_node_name(path,"input");struct json *node=jget(doc,"nodes")->items[node_index(doc,input)];free(input);
        struct json *params=jget(node,"params");while(params->count)jremove(params,params->count-1);
        unsigned count;const struct effect_processor *processors=effect_processors(&count);
        for(unsigned s=0;s<chain->count;s++) {
            const struct effect_stage *stage=&chain->stages[s];const struct effect_processor *p=&processors[stage->processor];char control[96];
            for(unsigned c=0;c<p->count;c++){snprintf(control,sizeof(control),"%s:%s",stage->id,p->params[c].name);chain_param(params,control,stage->values[c]);}
            snprintf(control,sizeof(control),"pm_%s_wet:Mult",stage->id);chain_param(params,control,!stage->bypass);
            snprintf(control,sizeof(control),"pm_%s_dry:Mult",stage->id);chain_param(params,control,stage->bypass);
        }
        chain_param(params,"wet:Mult",chain->wet);chain_param(params,"dry:Mult",chain->dry);
    }
    int result=validate_schema(doc,error,size);
    if(result<0){scene_job_free(job,false);errno=-result;return NULL;}
    return job;
}
unsigned scene_job_skipped(const struct scene_job *job) { return job ? job->skipped : 0; }
const char *scene_job_stage(const struct scene_job *job) {
    static const char *stages[] = {"hardware profiles/device enumeration", "hardware routes", "audio workers", "channel layout/DSP parameters",
                                  "volume/mute/stream targets/default devices", "obsolete connections", "connections/final parameters"};
    return job && job->phase < sizeof(stages) / sizeof(stages[0]) ? stages[job->phase] : "PipeWire state";
}

static int effect_file(struct path_job *state,const struct json *path,char *error,size_t size) {
    if(state->temporary)return 0;
    xasprintf(&state->temporary,"%s/pipemixer-scene-XXXXXX",getenv("XDG_RUNTIME_DIR")?:"/tmp");
    int fd=mkostemp(state->temporary,O_CLOEXEC);if(fd<0)return fail(error,size,errno,"cannot prepare effect");
    FILE *file=fdopen(fd,"w");if(!file){close(fd);return -errno;}
    fprintf(file,"filter.graph = %s\n",jtext(path,"graph"));
    const char *spec=jtext(path,"chain");if(spec){char *quoted=json_quote(spec);fprintf(file,"pipemixer.chain = %s\n",quoted);free(quoted);}
    bool bad=ferror(file);if(fclose(file))bad=true;return bad?-EIO:0;
}
static int replace_paths(struct scene_job *job,char *error,size_t size) {
    const struct json *paths=jget(job->doc,"paths");
    if(job->replace_phase==0) {
        for(unsigned i=0;i<paths->count;i++) {
            struct path_job *state=&job->paths[i];const struct json *path=paths->items[i];
            if(!state->replace||!streq(jtext(path,"kind"),"effect"))continue;
            if(state->preview_pid>0) {
                if(managed_child_status(state->preview_pid)!=-EAGAIN)return fail(error,size,EIO,"new chain failed validation; original chain retained");
                if(!managed_child_ready(state->preview_pid)||!managed_ready("effect",state->preview))return -EAGAIN;
                continue;
            }
            const char *source=jtext(path,"preset");char *owned=NULL;
            if(streq(source,"custom")){int result=effect_file(state,path,error,size);if(result<0)return result;xasprintf(&owned,"@%s",state->temporary);source=owned;}
            snprintf(state->preview,sizeof(state->preview),"pmcheck_%ld_%u",(long)getpid(),i);
            state->preview_pid=managed_spawn_private("effect",state->preview,source,NULL);free(owned);
            if(state->preview_pid<0)return state->preview_pid;
            return -EAGAIN;
        }
        job->replace_phase++;
        for(unsigned i=0;i<paths->count;i++)if(job->paths[i].preview_pid>0)managed_cancel(job->paths[i].preview_pid);
        return -EAGAIN;
    }
    if(job->replace_phase==1) {
        for(unsigned i=0;i<paths->count;i++)if(job->paths[i].preview_pid>0 && managed_find("effect",job->paths[i].preview,NULL))return -EAGAIN;
        for(unsigned i=0;i<paths->count;i++)if(job->paths[i].replace) {
            int result=managed_remove(jtext(paths->items[i],"kind"),jtext(paths->items[i],"name"));if(result<0)return result;
        }
        job->committed=true;job->replace_phase++;return -EAGAIN;
    }
    if(job->replace_phase==2) {
        for(unsigned i=0;i<paths->count;i++)if(job->paths[i].replace && managed_find(jtext(paths->items[i],"kind"),jtext(paths->items[i],"name"),NULL))return -EAGAIN;
        job->replace_phase++;
    }
    return 0;
}
static int create_paths(struct scene_job *job, char *error, size_t size) {
    const struct json *paths = jget(job->doc, "paths"); bool pending = false;
    for (unsigned i = 0; i < paths->count; i++) {
        const struct json *path = paths->items[i]; struct path_job *state = &job->paths[i];
        const char *kind = jtext(path, "kind"), *name = jtext(path, "name");
        if(job->edit_name && !state->replace){state->ready=true;continue;}
        if (state->ready) {
            if (!managed_ready(kind, name)) return fail(error, size, ENOENT, "path '%s' disappeared", name);
            continue;
        }
        pending = true;
        if (state->pid > 0) {
            if (managed_child_status(state->pid) != -EAGAIN)
                return fail(error, size, EIO, "worker '%s' failed; inspect its runtime log", name);
            if (managed_child_ready(state->pid) && managed_ready(kind, name)) {
                state->ready = true;
                if (state->temporary) { unlink(state->temporary); free(state->temporary); state->temporary = NULL; }
            }
            continue;
        }
        if (managed_find(kind, name, NULL)) {
            int result = path_matches(path, error, size); if (result < 0 && result != -EAGAIN) return result;
            if (!result && managed_ready(kind, name)) state->ready = true;
            continue;
        }
        const char *source = NULL, *destination = NULL; char *owned = NULL;
        if (streq(kind, "send")) {
            uint32_t src, dest;
            source = jtext(path, "source"); destination = jtext(path, "destination");
            if (resolve_node(source, &src) < 0 || resolve_node(destination, &dest) < 0) continue;
            int source_path = path_owner(job->doc, source), destination_path = path_owner(job->doc, destination);
            if ((source_path >= 0 && !job->paths[source_path].ready)
                || (destination_path >= 0 && !job->paths[destination_path].ready)
                || !graph_node_has_ports(src, PW_DIRECTION_OUTPUT)
                || !graph_node_has_ports(dest, PW_DIRECTION_INPUT)) continue;
        } else if (streq(kind, "monitor")) source = jtext(path, "monitor_set");
        else if (streq(kind, "effect")) {
            source = jtext(path, "preset");
            if (streq(source, "custom")) {
                int result=effect_file(state,path,error,size);if(result<0)return result;
                xasprintf(&owned, "@%s", state->temporary); source = owned;
            }
        }
        state->pid = managed_spawn(kind, name, source, destination); free(owned);
        if (state->pid < 0) return fail(error, size, -state->pid, "cannot create %s '%s': %s", kind, name, strerror(-state->pid));
        state->created = true;
        /* One spawn per tick keeps graph updates and the TUI responsive. */
        return -EAGAIN;
    }
    return pending ? -EAGAIN : 0;
}
static int desired_volumes(const struct json *saved, const struct param_props *current, float values[], char *error, size_t size) {
    const struct json *channels = jget(saved, "channels");
    if (channels->count && (!current || !current->n_channels)) return -EAGAIN;
    if (channels->count != (current ? current->n_channels : 0))
        return fail(error, size, EINVAL, "channel layout changed on '%s'", jtext(saved, "name"));
    for (unsigned i = 0; i < channels->count; i++) {
        bool found = false;
        for (unsigned k = 0; k < channels->count; k++) if (streq(current->channel_names[i], jtext(channels->items[k], "name"))) {
            values[i] = jget(channels->items[k], "volume")->number / 100; found = true; break;
        }
        if (!found) return fail(error, size, EINVAL, "channel names changed on '%s'", jtext(saved, "name"));
    }
    return 0;
}
static void param_values(const struct json *params, const char *names[], double values[]) {
    for (unsigned i = 0; i < params->count; i++) { names[i] = jtext(params->items[i], "name"); values[i] = jget(params->items[i], "value")->number; }
}
static int validate_runtime(struct scene_job *job, char *error, size_t size) {
    const struct json *nodes = jget(job->doc, "nodes"), *links = jget(job->doc, "links"), *defaults = jget(job->doc, "defaults");
    for (unsigned i = 0; i < nodes->count; i++) {
        if(job->edit_name && !job->affected[i])continue;
        const struct json *saved = nodes->items[i], *params = jget(saved, "params"); uint32_t id;
        int result = resolve_node(jtext(saved, "name"), &id);
        if (result < 0) return fail(error, size, -result, "node '%s' disappeared", jtext(saved, "name"));
        const struct graph_node *node = graph_node_find(id);
        if (!streq(jtext(saved, "class"), dict_get(&node->props, PW_KEY_MEDIA_CLASS)))
            return fail(error, size, EINVAL, "media class changed on '%s'", jtext(saved, "name"));
        float volumes[SPA_AUDIO_MAX_CHANNELS];
        result = desired_volumes(saved, node_get_params(node_lookup(id)), volumes, error, size); if (result < 0) return result;
        if (params->count) {
            if (!node->controls_ready) return -EAGAIN;
            const char *names[params->count]; double values[params->count]; param_values(params, names, values);
            result = graph_validate_controls(id, names, values, params->count);
            if (result < 0) return fail(error, size, -result, "invalid DSP parameters on '%s': %s", jtext(saved, "name"), strerror(-result));
        }
        const char *target = jtext(saved, "target");
        if (target) {
            if (!pipewire_default_available()) return fail(error, size, ENOENT, "session metadata unavailable");
            if (*target) {
                uint32_t dest; result = resolve_node(target, &dest);
                const struct graph_node *destination = result ? NULL : graph_node_find(dest);
                const char *class = jtext(saved, "class");
                const char *expected = streq(class, "Stream/Output/Audio") ? "Audio/Sink" : streq(class, "Stream/Input/Audio") ? "Audio/Source" : NULL;
                if (!expected || !destination || !streq(expected, dict_get(&destination->props, PW_KEY_MEDIA_CLASS)))
                    return fail(error, size, EINVAL, "invalid target for '%s'", jtext(saved, "name"));
            }
        }
    }
    for (unsigned i = 0; i < defaults->count; i++) {
        const struct json *value = defaults->items[i]; if (value->type == J_NULL) continue;
        uint32_t id; int result = resolve_node(value->text, &id);
        if (result < 0 || !pipewire_default_available()) return fail(error, size, ENOENT, "default device unavailable");
        if (!streq(dict_get(&graph_node_find(id)->props, PW_KEY_MEDIA_CLASS), streq(value->key, "sink") ? "Audio/Sink" : "Audio/Source"))
            return fail(error, size, EINVAL, "default device has wrong media class");
    }
    for (unsigned i = 0; i < links->count; i++) {
        uint32_t out, in; int result = resolve_endpoint(jget(links->items[i], "output"), PW_DIRECTION_OUTPUT, &out);
        if (!result) result = resolve_endpoint(jget(links->items[i], "input"), PW_DIRECTION_INPUT, &in);
        if (result < 0) return fail(error, size, -result, "scene port is missing or ambiguous");
    }
    return 0;
}
static int apply_states(struct scene_job *job, char *error, size_t size) {
    const struct json *nodes = jget(job->doc, "nodes"), *defaults = jget(job->doc, "defaults");
    bool pending = false;
    for (unsigned i = 0; i < nodes->count; i++) {
        if(job->edit_name && !job->affected[i])continue;
        const struct json *saved = nodes->items[i], *params = jget(saved, "params"); uint32_t id;
        int result = resolve_node(jtext(saved, "name"), &id);
        if (result < 0) return fail(error, size, -result, "node '%s' disappeared", jtext(saved, "name"));
        struct node *legacy = node_lookup(id); const struct param_props *props = node_get_params(legacy);
        float volumes[SPA_AUDIO_MAX_CHANNELS]; result = desired_volumes(saved, props, volumes, error, size); if (result < 0) return result;
        bool match = true;
        if (props) {
            if (props->mute != jget(saved, "mute")->boolean) match = false;
            for (unsigned c = 0; c < props->n_channels; c++) if (fabs(props->channel_volumes[c] - volumes[c]) > 1e-4) match = false;
        }
        const struct graph_node *node = graph_node_find(id);
        const char *names[params->count + 1]; double values[params->count + 1]; param_values(params, names, values);
        for (unsigned c = 0; c < params->count; c++) {
            const struct graph_control *control = graph_control_find(node, names[c]);
            if (!control || !control->has_value) return fail(error, size, ENOENT, "DSP parameter disappeared");
            if (fabs(control->value - values[c]) > 1e-6 * fmax(1, fabs(values[c]))) match = false;
        }
        const char *target = jtext(saved, "target"); uint32_t dest = PW_ID_ANY;
        if (target) {
            if (*target && resolve_node(target, &dest) < 0) return fail(error, size, ENOENT, "stream target disappeared");
            if (!pipewire_stream_target_matches(id, dest)) match = false;
        }
        if (!match) {
            pending = true;
            if (job->node_sent[i]) continue;
            if (props && props->n_channels) {
                result = node_set_volumes(legacy, volumes, props->n_channels); if (result < 0) return result;
                node_set_mute(legacy, jget(saved, "mute")->boolean);
            }
            if (params->count) { result = graph_set_controls(id, names, values, params->count); if (result < 0) return result; }
            if (target && !pipewire_set_stream_target(id, dest)) return fail(error, size, EIO, "cannot restore stream target");
            job->node_sent[i] = true;
        }
    }
    bool defaults_match = true;
    for (unsigned i = 0; i < defaults->count; i++) {
        const struct json *value = defaults->items[i]; if (value->type == J_NULL) continue;
        if(job->edit_name){int index=node_index(job->doc,value->text);if(index<0||!job->affected[index])continue;}
        bool sink = streq(value->key, "sink");
        if (streq(value->text, pipewire_get_default(sink ? DEFAULT_AUDIO_SINK : DEFAULT_AUDIO_SOURCE))) continue;
        defaults_match = false;
        if (!job->defaults_sent && !pipewire_set_default(sink ? DEFAULT_CONFIGURED_AUDIO_SINK : DEFAULT_CONFIGURED_AUDIO_SOURCE, value->text))
            return fail(error, size, EIO, "cannot restore default device");
    }
    job->defaults_sent = true;
    return pending || !defaults_match ? -EAGAIN : 0;
}
struct remove_links { struct scene_job *job; bool send, pending; int result; };
static void remove_extra_link(const struct graph_link *link, void *data) {
    struct remove_links *remove = data; if (!graph_link_is_audio(link)) return;
    const struct pw_link_info *info = link->info;
    if(remove->job->edit_name) {
        int out=node_index(remove->job->doc,graph_node_name(info->output_node_id)),in=node_index(remove->job->doc,graph_node_name(info->input_node_id));
        if((out<0||!remove->job->affected[out])&&(in<0||!remove->job->affected[in]))return;
    }
    if (node_index(remove->job->doc, graph_node_name(info->output_node_id)) < 0
        || node_index(remove->job->doc, graph_node_name(info->input_node_id)) < 0) return;
    const struct json *links = jget(remove->job->doc, "links");
    for (unsigned i = 0; i < links->count; i++) {
        uint32_t out, in;
        if (!resolve_endpoint(jget(links->items[i], "output"), PW_DIRECTION_OUTPUT, &out)
            && !resolve_endpoint(jget(links->items[i], "input"), PW_DIRECTION_INPUT, &in)
            && out == info->output_port_id && in == info->input_port_id) return;
    }
    remove->pending = true;
    if (remove->send) { int result = graph_disconnect(info->output_port_id, info->input_port_id); if (result < 0) remove->result = result; }
}
static int scene_step_inner(struct scene_job *job, char *error, size_t size) {
    if (!job || !graph_ready()) return -EAGAIN;
    int result;
    if (job->phase == 0) {
        if(!job->edit_name){result = restore_profiles(job, error, size); if (result) return result;}
        result = prepare_scene(job, error, size); if (result) return result; job->phase++;
    }
    if (job->phase == 1) {
        if(!job->edit_name){result = restore_routes(job, error, size); if (result) return result;} job->phase++;
    }
    if (job->phase == 2) {
        result = replace_paths(job,error,size);if(result)return result;
        result = create_paths(job, error, size); if (result) return result; job->phase++;
    }
    if (job->phase == 3) {
        result = validate_runtime(job, error, size); if (result) return result; job->phase++;
    }
    if (job->phase == 4) {
        result = apply_states(job, error, size); if (result) return result; job->phase++;
    }
    if (job->phase == 5) {
        struct remove_links remove = {.job = job, .send = !job->remove_sent};
        graph_foreach_link(remove_extra_link, &remove); job->remove_sent = true;
        if (remove.result < 0) return fail(error, size, -remove.result, "cannot remove obsolete scene link");
        if (remove.pending) return -EAGAIN;
        job->phase++;
    }
    const struct json *links = jget(job->doc, "links");
    for (unsigned i = 0; i < links->count; i++) {
        if(job->edit_name) {
            int out=node_index(job->doc,jtext(jget(links->items[i],"output"),"node")),in=node_index(job->doc,jtext(jget(links->items[i],"input"),"node"));
            if((out<0||!job->affected[out])&&(in<0||!job->affected[in]))continue;
        }
        uint32_t out, in; result = resolve_endpoint(jget(links->items[i], "output"), PW_DIRECTION_OUTPUT, &out);
        if (!result) result = resolve_endpoint(jget(links->items[i], "input"), PW_DIRECTION_INPUT, &in);
        if (result < 0) return fail(error, size, -result, "scene endpoint disappeared");
        const struct graph_link *link = graph_link_between(out, in);
        if (link && link->info->state == PW_LINK_STATE_ERROR) return fail(error, size, EIO, "scene link failed: %s", link->info->error ?: "unknown error");
        if (link && link->info->state >= PW_LINK_STATE_PAUSED) continue;
        if (!job->link_sent[i]) {
            result = graph_connect_saved(out,in,jtext(links->items[i],"route_rule"),jtext(links->items[i],"rule_set"),jtext(links->items[i],"batch"));
            if (result < 0) return fail(error, size, -result, "cannot restore link: %s", result == -ELOOP ? "audio feedback loop" : strerror(-result));
            job->link_sent[i] = true;
        }
        return -EAGAIN;
    }
    result = apply_states(job, error, size);
    if (!result && !job->activated && !job->edit_name) {
        result = activate_recovery(job, error, size);
        if (!result) job->activated = true;
    }
    if(!result && !job->activated && job->edit_name){result=complete_edit_recovery(job);if(!result)job->activated=true;}
    return result;
}
bool scene_job_error(struct scene_job *job,int code,const char *message) {
    if(!job||!job->edit_name||!job->committed||job->edit_failure)return false;
    job->edit_pending_error=code<0?code:-EIO;snprintf(job->edit_error,sizeof(job->edit_error),"%s",message?:strerror(-job->edit_pending_error));return true;
}
int scene_step(struct scene_job *job,char *error,size_t size) {
    int result;
    if(job&&job->edit_pending_error){result=job->edit_pending_error;job->edit_pending_error=0;snprintf(error,size,"%s",job->edit_error);}
    else result=scene_step_inner(job,error,size);
    if(!job||!job->edit_name)return result;
    if(result==-EAGAIN&&time(NULL)>=job->edit_deadline){result=-ETIMEDOUT;snprintf(error,size,"chain edit timed out");}
    if(!result && job->edit_failure){job->rolled_back=true;snprintf(error,size,"%s; original chain restored",job->edit_error);return job->edit_failure;}
    if(result>=0||result==-EAGAIN||!job->committed||job->edit_failure)return result;
    job->edit_failure=result;snprintf(job->edit_error,sizeof(job->edit_error),"%s",error[0]?error:strerror(-result));
    const struct json *paths=jget(job->doc,"paths");
    for(unsigned i=0;i<paths->count;i++) {
        struct path_job *state=&job->paths[i];
        if(state->created)managed_cancel(state->pid);
        if(state->preview_pid>0)managed_cancel(state->preview_pid);
        if(state->temporary){unlink(state->temporary);free(state->temporary);}
    }
    jfree(job->doc);job->doc=jclone(job->snapshot);
    free(job->paths);job->paths=NULL;free(job->affected);job->affected=NULL;
    free(job->node_sent);job->node_sent=NULL;free(job->route_sent);job->route_sent=NULL;free(job->link_sent);job->link_sent=NULL;
    job->phase=0;job->replace_phase=0;job->prepared=false;job->committed=false;job->defaults_sent=false;job->remove_sent=false;
    job->activated=false;job->edit_deadline=time(NULL)+15;
    error[0]=0;return -EAGAIN;
}
void scene_job_free(struct scene_job *job, bool cancel) {
    if (!job) return;
    const struct json *paths = jget(job->doc, "paths");
    for (unsigned i = 0; i < paths->count; i++) {
        if (!job->paths) break;
        struct path_job *state = &job->paths[i];
        if(state->preview_pid>0)managed_cancel(state->preview_pid);
        if (cancel && !job->rolled_back && state->created) {
            managed_cancel(state->pid);
            managed_remove(jtext(paths->items[i], "kind"), jtext(paths->items[i], "name"));
        }
        if (state->temporary) { unlink(state->temporary); free(state->temporary); }
    }
    close(job->lock_fd);
    free(job->paths); free(job->node_sent); free(job->link_sent); free(job->profile_sent); free(job->route_sent); free(job->skip_devices);
    free(job->affected);free(job->edit_name);
    jfree(job->doc); jfree(job->snapshot); free(job->name); free(job);
}

/* Active recovery is a volatile, configuration-scoped snapshot plus a ledger
 * of object generations. Engine restarts retain the ledger; a full session
 * restore replaces it. Completed generations are never continuously enforced. */
struct recovery_request { char *sent, *observed; unsigned ticks, matched; pid_t pid; };
struct scene_recovery {
    struct json *doc;
    struct stat identity;
    struct recovery_request *nodes, *targets, *paths, *links, *devices, *routes;
    char pending_error[256];
};
static char *recovery_filename(void) {
    char *scope = route_rules_scope(), *path;
    if (!scope) return NULL;
    xasprintf(&path, "%s/pipemixer-recovery-%s.json", getenv("XDG_RUNTIME_DIR") ?: "/tmp", scope);
    free(scope); return path;
}
static int write_recovery(const struct json *doc) {
    char *path = recovery_filename(), *temporary = NULL;
    if (!path) return -errno;
    xasprintf(&temporary, "%s.tmp-XXXXXX", path);
    int fd = mkostemp(temporary, O_CLOEXEC), result = 0;
    if (fd < 0) { result = -errno; goto out; }
    FILE *file = fdopen(fd, "w");
    if (!file) { result = -errno; close(fd); goto out; }
    jwrite(file, doc, 0); fputc('\n', file);
    if (ferror(file) || fflush(file) || fsync(fd)) result = -(errno ?: EIO);
    if (fclose(file) && !result) result = -errno;
    if (!result && rename(temporary, path) < 0) result = -errno;
out:
    unlink(temporary); free(temporary); free(path); return result;
}
static const char *node_serial(const char *name) {
    uint32_t id;
    if (resolve_node(name, &id) < 0) return NULL;
    return dict_get(&graph_node_find(id)->props, PW_KEY_OBJECT_SERIAL);
}
static char *target_token(const struct json *node) {
    const char *name = jtext(node, "name"), *target = jtext(node, "target"), *serial = node_serial(name);
    if (!serial || !target) return NULL;
    const char *destination = *target ? node_serial(target) : "default";
    if (!destination) return NULL;
    char *token; xasprintf(&token, "%s:%s", serial, destination); return token;
}
static char *send_token(const struct json *path) {
    const char *source = node_serial(jtext(path, "source")), *destination = node_serial(jtext(path, "destination"));
    if (!source || !destination) return NULL;
    char *token; xasprintf(&token, "%s:%s", source, destination); return token;
}
static char *link_token(const struct json *link) {
    uint32_t output, input;
    if (resolve_endpoint(jget(link, "output"), PW_DIRECTION_OUTPUT, &output) < 0
        || resolve_endpoint(jget(link, "input"), PW_DIRECTION_INPUT, &input) < 0) return NULL;
    const struct graph_port *out = graph_port_find(output), *in = graph_port_find(input);
    const char *os = node_serial(jtext(jget(link, "output"), "node")),
               *is = node_serial(jtext(jget(link, "input"), "node")),
               *op = dict_get(&out->props, PW_KEY_OBJECT_SERIAL), *ip = dict_get(&in->props, PW_KEY_OBJECT_SERIAL);
    if (!os || !is || !op || !ip) return NULL;
    char *token; xasprintf(&token, "%s:%s:%s:%s", os, is, op, ip); return token;
}
static struct json *ledger(const char *token) {
    struct json *item = jnew(J_OBJECT);
    jadd(item, "token", jstr(token)); jadd(item, "blocked", jstr(NULL)); jadd(item, "reason", jstr("")); return item;
}
static bool completed(const struct json *item, const char *token) {
    return token && streq(jtext(item, "token"), token);
}
static bool blocked(const struct json *item, const char *token) {
    return token && streq(jtext(item, "blocked"), token);
}
static void complete_item(struct json *item, const char *token) {
    jreplace_string(jget(item, "token"), token); jreplace_string(jget(item, "blocked"), NULL);
    jreplace_string(jget(item, "reason"), "");
}
static void block_item(struct json *item, const char *token, const char *error) {
    jreplace_string(jget(item, "blocked"), token); jreplace_string(jget(item, "reason"), error);
    fprintf(stderr, "pipemixer recovery: %s\n", error);
}
static int activate_recovery(struct scene_job *job, char *error, size_t size) {
    struct json *doc = jnew(J_OBJECT), *saved = job->snapshot;
    jadd(doc, "format", jstr("pipemixer.recovery")); jadd(doc, "version", jnum(2));
    jadd(doc, "scene", jstr(job->name)); jadd(doc, "snapshot", jclone(saved));
    char cookie[16]; snprintf(cookie, sizeof(cookie), "%u", pipewire_server_cookie()); jadd(doc, "server", jstr(cookie));
    struct json *serials = jnew(J_ARRAY), *targets = jnew(J_ARRAY), *paths = jnew(J_ARRAY), *links = jnew(J_ARRAY), *defaults = jnew(J_OBJECT);
    struct json *devices = jnew(J_ARRAY), *routes = jnew(J_ARRAY);
    jadd(doc, "devices", devices); jadd(doc, "routes", routes);
    jadd(doc, "serials", serials); jadd(doc, "targets", targets); jadd(doc, "paths", paths); jadd(doc, "links", links); jadd(doc, "defaults", defaults);
    const struct json *nodes = jget(saved, "nodes"), *saved_paths = jget(saved, "paths"), *saved_links = jget(saved, "links"), *saved_defaults = jget(saved, "defaults");
    const struct json *saved_devices = jget(saved, "devices");
    for (unsigned i = 0; saved_devices && i < saved_devices->count; i++) {
        struct device *device = NULL;
        if (resolve_device(jtext(saved_devices->items[i], "name"), &device) < 0) device = NULL;
        jadd(devices, NULL, ledger(!job->skip_devices[i] ? device_serial(device) : NULL));
    }
    for (unsigned i = 0; i < nodes->count; i++) {
        const char *name = jtext(nodes->items[i], "name"); bool applied = node_index(job->doc, name) >= 0;
        jadd(serials, NULL, ledger(applied ? node_serial(name) : NULL));
        jadd(routes, NULL, ledger(applied && jtext(nodes->items[i], "route") ? node_serial(name) : NULL));
        char *token = applied ? target_token(nodes->items[i]) : NULL;
        jadd(targets, NULL, ledger(token)); free(token);
    }
    for (unsigned i = 0; i < saved_paths->count; i++) {
        const struct json *path = saved_paths->items[i]; char *name = path_node_name(path, "input");
        char *token = node_index(job->doc, name) >= 0 ? (streq(jtext(path, "kind"), "send") ? send_token(path) : xstrdup("created")) : NULL;
        jadd(paths, NULL, ledger(token)); free(token); free(name);
    }
    for (unsigned i = 0; i < saved_links->count; i++) {
        const struct json *link = saved_links->items[i];
        bool applied = node_index(job->doc, jtext(jget(link, "output"), "node")) >= 0
                    && node_index(job->doc, jtext(jget(link, "input"), "node")) >= 0;
        char *token = applied ? link_token(link) : NULL; jadd(links, NULL, ledger(token)); free(token);
    }
    for (unsigned i = 0; i < saved_defaults->count; i++) {
        const struct json *value = saved_defaults->items[i], *applied = jget(jget(job->doc, "defaults"), value->key);
        jadd(defaults, value->key, ledger(applied && applied->type == J_STRING ? node_serial(value->text) : NULL));
    }
    int result = write_recovery(doc); jfree(doc);
    return result < 0 ? fail(error, size, -result, "cannot activate device recovery: %s", strerror(-result)) : 0;
}
static struct json *read_recovery(char *error, size_t size) {
    char *path = recovery_filename(); if (!path) return NULL;
    struct json *doc = read_document(path, SCENE_LIMIT * 2, error, size); free(path);
    if (!doc) return NULL;
    const struct json *snapshot = jget(doc, "snapshot"), *version = jget(doc, "version");
    bool valid = streq(jtext(doc, "format"), "pipemixer.recovery") && typed(version, J_NUMBER) && (version->number == 1 || version->number == 2)
        && managed_valid_name(jtext(doc, "scene")) && validate_schema((struct json *)snapshot, error, size) == 0;
    static const char *keys[] = {"serials", "targets", "paths", "links"}, *sources[] = {"nodes", "nodes", "paths", "links"};
    for (unsigned k = 0; valid && k < 4; k++) {
        const struct json *items = jget(doc, keys[k]);
        valid = typed(items, J_ARRAY) && items->count == jget(snapshot, sources[k])->count;
        for (unsigned i = 0; valid && i < items->count; i++) {
            const struct json *token = jget(items->items[i], "token"), *denied = jget(items->items[i], "blocked");
            valid = token && (token->type == J_NULL || token->type == J_STRING) && denied && (denied->type == J_NULL || denied->type == J_STRING)
                && jtext(items->items[i], "reason");
        }
    }
    if (valid && version->number == 1) {
        if (!jget(doc, "devices")) jadd(doc, "devices", jnew(J_ARRAY));
        if (!jget(doc, "routes")) {
            struct json *routes = jnew(J_ARRAY);
            for (unsigned i = 0; i < jget(snapshot, "nodes")->count; i++) jadd(routes, NULL, ledger(NULL));
            jadd(doc, "routes", routes);
        }
    }
    const struct json *device_ledger = jget(doc, "devices"), *routes = jget(doc, "routes"), *saved_devices = jget(snapshot, "devices");
    if (valid) valid = typed(device_ledger, J_ARRAY) && device_ledger->count == (saved_devices ? saved_devices->count : 0)
        && typed(routes, J_ARRAY) && routes->count == jget(snapshot, "nodes")->count;
    const struct json *extra[] = {device_ledger, routes};
    for (unsigned k = 0; valid && k < 2; k++) for (unsigned i = 0; valid && i < extra[k]->count; i++) {
        const struct json *item = extra[k]->items[i], *token = jget(item, "token"), *denied = jget(item, "blocked");
        valid = token && (token->type == J_NULL || token->type == J_STRING) && denied && (denied->type == J_NULL || denied->type == J_STRING) && jtext(item, "reason");
    }
    const struct json *defaults = jget(doc, "defaults"), *saved_defaults = jget(snapshot, "defaults");
    if (valid) valid = typed(defaults, J_OBJECT) && defaults->count == saved_defaults->count;
    for (unsigned i = 0; valid && i < saved_defaults->count; i++) {
        const struct json *item = jget(defaults, saved_defaults->items[i]->key);
        valid = item && jget(item, "token") && jget(item, "blocked") && jtext(item, "reason");
    }
    if (!valid) { jfree(doc); errno = EINVAL; fail(error, size, errno, "invalid active recovery snapshot"); return NULL; }
    if (jtext(doc, "server") && pipewire_server_cookie()) {
        char cookie[16]; snprintf(cookie, sizeof(cookie), "%u", pipewire_server_cookie());
        if (!streq(jtext(doc, "server"), cookie)) {
            jfree(doc); errno = ESTALE;
            fail(error, size, errno, "recovery snapshot belongs to another audio session; restore the startup scene"); return NULL;
        }
    }
    return doc;
}
static void reset_requests(struct recovery_request *requests, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        free(requests[i].sent); free(requests[i].observed);
        if (requests[i].pid > 0 && !managed_child_ready(requests[i].pid)) managed_cancel(requests[i].pid);
    }
    free(requests);
}
static void recovery_reset(struct scene_recovery *recovery) {
    if (recovery->doc) {
        const struct json *saved = jget(recovery->doc, "snapshot");
        reset_requests(recovery->nodes, jget(saved, "nodes")->count);
        reset_requests(recovery->targets, jget(saved, "nodes")->count);
        reset_requests(recovery->paths, jget(saved, "paths")->count);
        reset_requests(recovery->links, jget(saved, "links")->count);
        const struct json *devices = jget(saved, "devices");
        reset_requests(recovery->devices, devices ? devices->count : 0);
        reset_requests(recovery->routes, jget(saved, "nodes")->count);
    }
    jfree(recovery->doc); recovery->doc = NULL;
    recovery->nodes = recovery->targets = recovery->paths = recovery->links = NULL;
    recovery->devices = recovery->routes = NULL;
    recovery->pending_error[0] = 0;
}
struct scene_recovery *scene_recovery_new(void) { return xcalloc(1, sizeof(struct scene_recovery)); }
void scene_recovery_free(struct scene_recovery *recovery) {
    if (recovery) { recovery_reset(recovery); free(recovery); }
}
int scene_recovery_clear(void) {
    int lock = scene_lock(false); if (lock < 0) return lock;
    char *path = recovery_filename(); int result = path ? 0 : -errno;
    if (path && unlink(path) < 0 && errno != ENOENT) result = -errno;
    free(path); close(lock); return result;
}
static int clear_scene_recovery(const char *name) {
    int lock = scene_lock(false); if (lock < 0) return lock;
    char error[256]; struct json *doc = read_recovery(error, sizeof(error));
    int result = 0;
    if (doc && streq(name, jtext(doc, "scene"))) {
        char *path = recovery_filename();
        if (path && unlink(path) < 0 && errno != ENOENT) result = -errno;
        free(path);
    }
    jfree(doc); close(lock); return result;
}
static int complete_edit_recovery(struct scene_job *job) {
    char error[256]={0};struct json *doc=read_recovery(error,sizeof(error));if(!doc)return errno==ENOENT?0:-EINVAL;
    const struct json *saved=jget(doc,"snapshot"),*nodes=jget(saved,"nodes"),*paths=jget(saved,"paths"),*links=jget(saved,"links"),*defaults=jget(saved,"defaults");
    for(unsigned i=0;i<nodes->count;i++) {
        const struct json *node=nodes->items[i];int current=node_index(job->doc,jtext(node,"name")),target=node_index(job->doc,jtext(node,"target"));
        if(current>=0&&job->affected[current])complete_item(jget(doc,"serials")->items[i],node_serial(jtext(node,"name")));
        if((current>=0&&job->affected[current])||(target>=0&&job->affected[target])){char *token=target_token(node);complete_item(jget(doc,"targets")->items[i],token);free(token);}
    }
    for(unsigned i=0;i<paths->count;i++) {
        const struct json *path=paths->items[i];char *name=path_node_name(path,"input");int index=node_index(job->doc,name);free(name);
        if(index>=0&&job->affected[index]){char *token=streq(jtext(path,"kind"),"send")?send_token(path):xstrdup("created");complete_item(jget(doc,"paths")->items[i],token);free(token);}
    }
    for(unsigned i=0;i<links->count;i++) {
        const struct json *link=links->items[i];int out=node_index(job->doc,jtext(jget(link,"output"),"node")),in=node_index(job->doc,jtext(jget(link,"input"),"node"));
        if((out>=0&&job->affected[out])||(in>=0&&job->affected[in])){char *token=link_token(link);complete_item(jget(doc,"links")->items[i],token);free(token);}
    }
    for(unsigned i=0;i<defaults->count;i++){const struct json *value=defaults->items[i];if(value->type!=J_STRING)continue;int index=node_index(job->doc,value->text);if(index>=0&&job->affected[index])complete_item(jget(jget(doc,"defaults"),value->key),node_serial(value->text));}
    int result=write_recovery(doc);jfree(doc);return result;
}
static void recovery_reload(struct scene_recovery *recovery) {
    char *path = recovery_filename(); if (!path) return;
    struct stat st; int result = lstat(path, &st); free(path);
    if (result < 0) { if (errno == ENOENT) recovery_reset(recovery); return; }
    if (recovery->doc && st.st_dev == recovery->identity.st_dev && st.st_ino == recovery->identity.st_ino) return;
    char error[256] = {0}; struct json *doc = read_recovery(error, sizeof(error));
    if (!doc) return; /* An incomplete/bad replacement never replaces valid state. */
    recovery_reset(recovery); recovery->doc = doc; recovery->identity = st;
    const struct json *saved = jget(doc, "snapshot"); unsigned nodes = jget(saved, "nodes")->count;
    recovery->nodes = xcalloc(nodes + 1, sizeof(*recovery->nodes)); recovery->targets = xcalloc(nodes + 1, sizeof(*recovery->targets));
    recovery->paths = xcalloc(jget(saved, "paths")->count + 1, sizeof(*recovery->paths));
    recovery->links = xcalloc(jget(saved, "links")->count + 1, sizeof(*recovery->links));
    const struct json *devices = jget(saved, "devices");
    recovery->devices = xcalloc((devices ? devices->count : 0) + 1, sizeof(*recovery->devices));
    recovery->routes = xcalloc(nodes + 1, sizeof(*recovery->routes));
}
static bool request_sent(struct recovery_request *request, const char *token) {
    if (!streq(request->observed, token)) {
        free(request->observed); request->observed = xstrdup(token);
        free(request->sent); request->sent = NULL; request->ticks = request->matched = 0;
    }
    return request->sent != NULL;
}
static void request_start(struct recovery_request *request, const char *token) {
    request_sent(request, token);
    free(request->sent); request->sent = xstrdup(token);
}
static bool recovery_result(struct json *item, struct recovery_request *request, const char *token, int result, const char *error, bool *dirty) {
    request_sent(request, token);
    if (!result) { complete_item(item, token); *dirty = true; return false; }
    if (result == -EAGAIN && ++request->ticks < 40) return true;
    block_item(item, token, *error ? error : result == -EAGAIN ? "state restore timed out; reload the scene to retry" : strerror(-result));
    *dirty = true; return false;
}
static int recover_node(const struct json *saved, struct recovery_request *request, const char *serial, char *error, size_t size) {
    uint32_t id; int result = resolve_node(jtext(saved, "name"), &id); if (result < 0) return result;
    const struct graph_node *node = graph_node_find(id);
    if (!streq(jtext(saved, "class"), dict_get(&node->props, PW_KEY_MEDIA_CLASS)))
        return fail(error, size, EINVAL, "media class changed on '%s'", jtext(saved, "name"));
    struct node *legacy = node_lookup(id); const struct param_props *props = node_get_params(legacy);
    float volumes[SPA_AUDIO_MAX_CHANNELS]; result = desired_volumes(saved, props, volumes, error, size); if (result < 0) return result;
    const struct json *params = jget(saved, "params");
    const char *names[params->count + 1]; double values[params->count + 1]; param_values(params, names, values);
    if (params->count) {
        if (!node->controls_ready) return -EAGAIN;
        result = graph_validate_controls(id, names, values, params->count); if (result < 0) return result;
    }
    bool match = !props || props->mute == jget(saved, "mute")->boolean;
    if (props) for (unsigned c = 0; c < props->n_channels; c++) if (fabs(props->channel_volumes[c] - volumes[c]) > 1e-4) match = false;
    for (unsigned c = 0; c < params->count; c++) {
        const struct graph_control *control = graph_control_find(node, names[c]);
        if (!control || !control->has_value) return -EAGAIN;
        if (fabs(control->value - values[c]) > 1e-6 * fmax(1, fabs(values[c]))) match = false;
    }
    /* WirePlumber may replay its own Props when a recreated adapter negotiates
     * its first links. Confirm stable values, with two bounded retries during
     * this initial recovery window, instead of continuously enforcing gains. */
    if (match) return ++request->matched >= 3 ? 0 : -EAGAIN;
    request->matched = 0;
    if (!request_sent(request, serial) || request->ticks == 4 || request->ticks == 8) {
        if (props && props->n_channels) { result = node_set_volumes(legacy, volumes, props->n_channels); if (result < 0) return result; node_set_mute(legacy, jget(saved, "mute")->boolean); }
        if (params->count) { result = graph_set_controls(id, names, values, params->count); if (result < 0) return result; }
        request_start(request, serial);
    }
    return -EAGAIN;
}
static bool hardware_node_ready_depth(const struct json *doc, const char *name, unsigned depth) {
    if (depth > PATH_LIMIT || !node_serial(name)) return false;
    const struct json *saved = jget(doc, "snapshot"); int index = node_index(saved, name);
    if (index < 0) return false;
    const struct json *node = jget(saved, "nodes")->items[index]; int device = saved_device_index(saved, jtext(node, "device"));
    if (device >= 0) {
        struct device *current;
        if (resolve_device(jtext(node, "device"), &current) < 0 || !completed(jget(doc, "devices")->items[device], device_serial(current))) return false;
    }
    if (jtext(node, "route") && !completed(jget(doc, "routes")->items[index], node_serial(name))) return false;
    int owner = path_owner(saved, name);
    if (owner >= 0) {
        const struct json *path = jget(saved, "paths")->items[owner];
        if (streq(jtext(path, "kind"), "send")) return hardware_node_ready_depth(doc, jtext(path, "source"), depth + 1)
            && hardware_node_ready_depth(doc, jtext(path, "destination"), depth + 1);
    }
    return true;
}
static bool hardware_node_ready(const struct json *doc, const char *name) { return hardware_node_ready_depth(doc, name, 0); }
static bool endpoint_state_ready(const struct json *doc, const char *name) {
    int index = node_index(jget(doc, "snapshot"), name);
    return index >= 0 && hardware_node_ready(doc, name) && completed(jget(doc, "serials")->items[index], node_serial(name));
}
static bool recovery_rejected(struct scene_recovery *recovery, bool apply) {
    if (!recovery || !recovery->doc) return false;
    struct recovery_request *requests[] = {recovery->devices, recovery->routes, recovery->nodes, recovery->targets, recovery->links};
    static const char *keys[] = {"devices", "routes", "serials", "targets", "links"}; bool pending = false;
    for (unsigned k = 0; k < 5; k++) {
        struct json *items = jget(recovery->doc, keys[k]);
        for (unsigned i = 0; i < items->count; i++) {
            const char *token = requests[k][i].sent;
            if (!token || completed(items->items[i], token) || blocked(items->items[i], token)) continue;
            pending = true;
            if (apply) block_item(items->items[i], token, recovery->pending_error);
        }
    }
    return pending;
}
bool scene_recovery_error(struct scene_recovery *recovery, int code, const char *message) {
    if (code == -EPIPE || code == -ECONNRESET || code == -ENOTCONN || !recovery_rejected(recovery, false)) return false;
    snprintf(recovery->pending_error, sizeof(recovery->pending_error), "restore rejected by audio server: %s", message);
    return true;
}
bool scene_recovery_tick(struct scene_recovery *recovery) {
    recovery_reload(recovery); if (!recovery->doc) return false;
    struct json *doc = recovery->doc, *saved = jget(doc, "snapshot"), *nodes = jget(saved, "nodes"), *paths = jget(saved, "paths"), *links = jget(saved, "links");
    bool busy = false, dirty = false; char error[256];
    if (recovery->pending_error[0]) { dirty = recovery_rejected(recovery, true); recovery->pending_error[0] = 0; }
    const struct json *devices = jget(saved, "devices");
    for (unsigned i = 0; devices && i < devices->count; i++) {
        const struct json *device_state = devices->items[i]; struct json *item = jget(doc, "devices")->items[i];
        struct device *device; if (resolve_device(jtext(device_state, "name"), &device) < 0) continue;
        const char *serial = device_serial(device); if (!serial || completed(item, serial) || blocked(item, serial)) continue;
        struct recovery_request *request = &recovery->devices[i]; request_sent(request, serial);
        const struct param_profile *profile; error[0] = 0;
        int result = profile_choice(device_state, &device, &profile);
        if (result == -ENXIO) {
            free(request->sent); request->sent = NULL; request->ticks = request->matched = 0;
            continue; /* A temporarily unavailable profile waits for capability changes. */
        }
        if (!result && profile->active) result = ++request->matched >= 3 ? 0 : -EAGAIN;
        else if (!result) {
            request->matched = 0;
            if (!request_sent(request, serial) || request->ticks == 4 || request->ticks == 8) {
                result = device_set_profile(device, profile->index);
                if (result >= 0) { request_start(request, serial); result = 0; }
            }
            if (!result) result = -EAGAIN;
        }
        if (result < 0 && result != -EAGAIN) snprintf(error, sizeof(error), "profile '%s' on '%s': %s", jtext(device_state, "profile"), jtext(device_state, "name"), strerror(-result));
        busy |= recovery_result(item, request, serial, result, error, &dirty);
    }
    for (unsigned i = 0; i < nodes->count; i++) {
        const struct json *node_state = nodes->items[i];
        if (!jtext(node_state, "route")) continue;
        int device_index = saved_device_index(saved, jtext(node_state, "device")); struct device *device;
        if (device_index < 0 || resolve_device(jtext(node_state, "device"), &device) < 0
            || !completed(jget(doc, "devices")->items[device_index], device_serial(device))) continue;
        struct json *item = jget(doc, "routes")->items[i]; const char *serial = node_serial(jtext(node_state, "name"));
        if (!serial || completed(item, serial) || blocked(item, serial)) continue;
        struct recovery_request *request = &recovery->routes[i]; request_sent(request, serial);
        struct node *node; const struct param_route *route; error[0] = 0;
        int result = route_choice(node_state, &node, &route);
        if (result == -ENXIO) { free(request->sent); request->sent = NULL; request->ticks = request->matched = 0; continue; }
        if (!result && route->active) result = ++request->matched >= 3 ? 0 : -EAGAIN;
        else if (!result) {
            request->matched = 0;
            if (!request_sent(request, serial) || request->ticks == 4 || request->ticks == 8) {
                result = node_set_route(node, route->index);
                if (result >= 0) { request_start(request, serial); result = 0; }
            }
            if (!result) result = -EAGAIN;
        }
        if (result < 0 && result != -EAGAIN) snprintf(error, sizeof(error), "route '%s' on '%s': %s", jtext(node_state, "route"), jtext(node_state, "name"), strerror(-result));
        busy |= recovery_result(item, request, serial, result, error, &dirty);
    }
    for (unsigned i = 0; i < paths->count; i++) {
        const struct json *path = paths->items[i]; struct json *item = jget(doc, "paths")->items[i];
        if (!streq(jtext(path, "kind"), "send")) continue;
        struct recovery_request *request = &recovery->paths[i];
        const char *source = jtext(path, "source"), *destination = jtext(path, "destination"), *ss = node_serial(source), *ds = node_serial(destination);
        if (!endpoint_state_ready(doc, source) || !endpoint_state_ready(doc, destination)) continue;
        uint32_t sid, did;
        if (!ss || !ds || resolve_node(source, &sid) < 0 || resolve_node(destination, &did) < 0
            || !graph_node_has_ports(sid, PW_DIRECTION_OUTPUT) || !graph_node_has_ports(did, PW_DIRECTION_INPUT)) continue;
        char *token; xasprintf(&token, "%s:%s", ss, ds);
        if (completed(item, token) || blocked(item, token)) { free(token); continue; }
        error[0] = 0; int result = path_matches(path, error, sizeof(error));
        if (!result && managed_ready("send", jtext(path, "name"))) {
            if (request->pid > 0 && !managed_child_ready(request->pid)) { free(token); busy = true; continue; }
            complete_item(item, token); request->pid = 0; dirty = true; free(token); continue;
        }
        if (!result && !managed_find("send", jtext(path, "name"), NULL) && !request->pid) {
            request->pid = managed_spawn("send", jtext(path, "name"), source, destination);
            result = request->pid < 0 ? (int)request->pid : -EAGAIN;
        } else if (result == -EAGAIN || !result) {
            result = -EAGAIN;
            if (request->pid > 0 && managed_child_status(request->pid) != -EAGAIN) result = -EIO;
            else if (request->pid > 0) managed_child_ready(request->pid);
        }
        bool pending = recovery_result(item, request, token, result, error, &dirty);
        if (!pending) {
            if (request->pid > 0) { managed_cancel(request->pid); managed_remove("send", jtext(path, "name")); }
            request->pid = 0;
        }
        busy |= pending; free(token);
    }
    for (unsigned i = 0; i < nodes->count; i++) {
        const struct json *node = nodes->items[i]; struct json *item = jget(doc, "serials")->items[i];
        if (!hardware_node_ready(doc, jtext(node, "name"))) continue;
        const char *serial = node_serial(jtext(node, "name"));
        if (!serial || completed(item, serial) || blocked(item, serial)) continue;
        error[0] = 0; request_sent(&recovery->nodes[i], serial);
        int result = recover_node(node, &recovery->nodes[i], serial, error, sizeof(error));
        busy |= recovery_result(item, &recovery->nodes[i], serial, result, error, &dirty);
    }
    if (!busy) for (unsigned i = 0; i < nodes->count; i++) {
        const struct json *node = nodes->items[i]; struct json *item = jget(doc, "targets")->items[i];
        const char *saved_target = jtext(node, "target");
        if (!hardware_node_ready(doc, jtext(node, "name")) || (saved_target && *saved_target && !hardware_node_ready(doc, saved_target))) continue;
        char *token = target_token(node); if (!token || completed(item, token) || blocked(item, token)) { free(token); continue; }
        uint32_t id, target = PW_ID_ANY;
        if (resolve_node(jtext(node, "name"), &id) < 0) { free(token); continue; }
        const char *destination = jtext(node, "target");
        if (*destination && resolve_node(destination, &target) < 0) { free(token); continue; }
        error[0] = 0; int result = -EAGAIN;
        if (pipewire_stream_target_matches(id, target)) result = 0;
        else if (!request_sent(&recovery->targets[i], token) && pipewire_default_available()) {
            if (pipewire_set_stream_target(id, target)) request_start(&recovery->targets[i], token);
            else result = -EINVAL;
        }
        busy |= recovery_result(item, &recovery->targets[i], token, result, error, &dirty); free(token);
    }
    const struct json *defaults = jget(saved, "defaults");
    for (unsigned i = 0; !busy && i < defaults->count; i++) {
        const struct json *value = defaults->items[i]; struct json *item = jget(jget(doc, "defaults"), value->key);
        if (value->type == J_STRING && !hardware_node_ready(doc, value->text)) continue;
        const char *serial = value->type == J_STRING ? node_serial(value->text) : NULL;
        if (!serial || completed(item, serial) || blocked(item, serial) || !pipewire_default_available()) continue;
        bool sink = streq(value->key, "sink"); uint32_t id;
        if (resolve_node(value->text, &id) < 0) continue;
        if (!streq(dict_get(&graph_node_find(id)->props, PW_KEY_MEDIA_CLASS), sink ? "Audio/Sink" : "Audio/Source")) {
            block_item(item, serial, "default device media class changed"); dirty = true; continue;
        }
        if (pipewire_set_default(sink ? DEFAULT_CONFIGURED_AUDIO_SINK : DEFAULT_CONFIGURED_AUDIO_SOURCE, value->text)) { complete_item(item, serial); dirty = true; }
    }
    for (unsigned i = 0; !busy && i < links->count; i++) {
        const struct json *link = links->items[i]; struct json *item = jget(doc, "links")->items[i];
        if (!hardware_node_ready(doc, jtext(jget(link, "output"), "node")) || !hardware_node_ready(doc, jtext(jget(link, "input"), "node"))) continue;
        /* Current routing/monitor policy owns tagged links, including priority,
         * fallback and Solo decisions; stale scene policy must not bypass it. */
        if (jtext(link, "route_rule")) continue;
        char *token = link_token(link); if (!token || completed(item, token) || blocked(item, token)) { free(token); continue; }
        uint32_t output, input; resolve_endpoint(jget(link, "output"), PW_DIRECTION_OUTPUT, &output); resolve_endpoint(jget(link, "input"), PW_DIRECTION_INPUT, &input);
        const struct graph_link *current = graph_link_between(output, input); int result = -EAGAIN; error[0] = 0;
        if (current && current->info && current->info->state == PW_LINK_STATE_ERROR) result = fail(error, sizeof(error), EIO, "recovery link failed: %s", current->info->error ?: "unknown error");
        else if (current && current->info && current->info->state >= PW_LINK_STATE_PAUSED) result = 0;
        else if (!request_sent(&recovery->links[i], token)) {
            result = graph_connect_saved(output,input,NULL,NULL,jtext(link,"batch"));
            if (result == -ELOOP) snprintf(error, sizeof(error), "saved connection would cause an audio feedback loop");
            if (!result) { request_start(&recovery->links[i], token); result = -EAGAIN; }
        }
        busy |= recovery_result(item, &recovery->links[i], token, result, error, &dirty); free(token);
    }
    if (dirty) {
        int result = write_recovery(doc);
        if (result < 0) fprintf(stderr, "pipemixer recovery: cannot update ledger: %s\n", strerror(-result));
        else { char *path = recovery_filename(); if (path) { lstat(path, &recovery->identity); free(path); } }
    }
    return busy;
}
static const char *recovery_state(const struct json *item, const char *token) {
    return !token ? "waiting" : completed(item, token) ? "restored" : blocked(item, token) ? "blocked" : "restoring";
}
int scene_recovery_print(bool json, char *error, size_t size) {
    struct json *doc = read_recovery(error, size);
    if (!doc && errno != ENOENT) return -errno;
    const struct json *snapshot = jget(doc, "snapshot"), *nodes = jget(snapshot, "nodes"), *devices = jget(snapshot, "devices");
    if (json) { char *name = doc ? json_quote(jtext(doc, "scene")) : xstrdup("null"); printf("{\"engine_running\":%s,\"scene\":%s,\"devices\":[", route_rules_running() ? "true" : "false", name); free(name); }
    else printf("Device recovery: %s; engine %s\n", doc ? jtext(doc, "scene") : "off", route_rules_running() ? "running" : "stopped");
    for (unsigned i = 0; devices && i < devices->count; i++) {
        const struct json *saved = devices->items[i], *item = jget(doc, "devices")->items[i];
        struct device *device = NULL; int result = resolve_device(jtext(saved, "name"), &device);
        const char *serial = result ? NULL : device_serial(device), *state = recovery_state(item, serial), *active = "", *reason = blocked(item, serial) ? jtext(item, "reason") : "";
        if (result == -ENOTUNIQ) { state = "blocked"; reason = "hardware device name is ambiguous"; }
        unsigned count; const struct param_profile *profiles = device_get_profiles(device, &count);
        for (unsigned p = 0; p < count; p++) if (profiles[p].active) active = profiles[p].name;
        if (json) { char *name = json_quote(jtext(saved, "name")), *desired = json_quote(jtext(saved, "profile")), *current = json_quote(active), *r = json_quote(reason);
            printf("%s{\"name\":%s,\"profile\":%s,\"active_profile\":%s,\"state\":\"%s\",\"reason\":%s}", i ? "," : "", name, desired, current, state, r); free(name); free(desired); free(current); free(r);
        } else printf("%s\tprofile=%s\t%s%s%s\n", jtext(saved, "name"), jtext(saved, "profile"), state, *reason ? " | " : "", reason);
    }
    if (json) fputs("],\"nodes\":[", stdout);
    for (unsigned i = 0; nodes && i < nodes->count; i++) {
        const struct json *saved = nodes->items[i], *item = jget(doc, "serials")->items[i], *route_item = jget(doc, "routes")->items[i];
        const char *name = jtext(saved, "name"), *serial = node_serial(name), *state = recovery_state(item, serial),
                   *reason = blocked(item, serial) ? jtext(item, "reason") : "", *route_state = jtext(saved, "route") ? recovery_state(route_item, serial) : "none";
        if (blocked(route_item, serial)) { state = "blocked"; reason = jtext(route_item, "reason"); }
        uint32_t id; if (resolve_node(name, &id) == -ENOTUNIQ) { state = "blocked"; reason = "audio node name is ambiguous"; }
        char *target = target_token(saved); const struct json *target_item = jget(doc, "targets")->items[i];
        const char *target_state = jtext(saved, "target") ? recovery_state(target_item, target) : "none";
        if (json) { char *n = json_quote(name), *r = json_quote(reason);
            printf("%s{\"name\":%s,\"state\":\"%s\",\"route_state\":\"%s\",\"target_state\":\"%s\",\"reason\":%s}", i ? "," : "", n, state, route_state, target_state, r); free(n); free(r);
        } else printf("%s\t%s route=%s target=%s%s%s\n", name, state, route_state, target_state, *reason ? " | " : "", reason);
        free(target);
    }
    if (json) puts("]}");
    jfree(doc); return 0;
}
