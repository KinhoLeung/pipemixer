#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cli.h"
#include "eventloop.h"
#include "pw/common.h"
#include "utils.h"
#include "xmalloc.h"

struct cli_route {
    int32_t index;
    char *name, *description;
    bool active;
};

struct cli_profile {
    int32_t index;
    char *name, *description;
    bool active;
};

struct cli_state;

struct cli_node {
    struct cli_state *state;
    struct cli_node *next;
    struct node *node;
    struct event_hook *hook;
    char *name, *description;
    char **channels;
    float *volumes;
    unsigned channel_count, volume_count;
    struct cli_route *routes;
    unsigned route_count;
    bool props_seen, channels_seen, volume_seen, mute_seen, default_seen, routes_seen;
    bool has_device;
    bool mute, is_default, removed;
};

struct cli_device {
    struct cli_state *state;
    struct cli_device *next;
    struct device *device;
    struct event_hook *hook;
    char *name, *description;
    struct cli_profile *profiles;
    unsigned profile_count;
    bool props_seen, profiles_seen, removed;
};

struct cli_state {
    const struct cli_request *request;
    struct cli_node *nodes, *selected_node;
    struct cli_device *devices, *selected_device;
    struct event_hook *hook;
    struct spa_source *timer;
    char *defaults[DEFAULT_METADATA_KEY_COUNT];
    int sync_seq, sync_rounds, status;
    bool action_sent, done;
    float desired_volume;
    bool desired_mute;
    int channel_index;
};

static void cli_evaluate(struct cli_state *state);

static int usage(const char *message) {
    fprintf(stderr, "pipemixer: %s\nTry 'pipemixer --help' for usage.\n", message);
    return 2;
}

static bool parse_unsigned(const char *text, unsigned *result) {
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || end == text || *end || value > UINT_MAX || *text == '-') {
        return false;
    }
    *result = value;
    return true;
}

static bool parse_index(const char *text, int32_t *result) {
    unsigned value;
    if (!parse_unsigned(text, &value) || value > INT32_MAX) {
        return false;
    }
    *result = (int32_t)value;
    return true;
}

static bool valid_target(const char *target) {
    if (!target || !*target) {
        return false;
    }
    if (strncmp(target, "id:", 3) == 0) {
        unsigned id;
        return parse_unsigned(target + 3, &id);
    }
    if (strncmp(target, "serial:", 7) == 0) return target[7] != '\0';
    return true;
}

int cli_parse(int argc, char **argv, struct cli_request *request) {
    if (!argc) {
        return usage("missing command");
    }

    const char *command = argv[0];
    int min_args = 0, max_args = 0;
    bool has_target = false;

    if (strcmp(command, "list") == 0) {
        request->command = CLI_LIST;
        max_args = 1;
        if (argc == 2) {
            if (strcmp(argv[1], "nodes") == 0) request->devices = false;
            else if (strcmp(argv[1], "devices") == 0) request->devices = true;
            else return usage("list accepts 'nodes' or 'devices'");
        }
    } else if (strcmp(command, "get-default") == 0) {
        request->command = CLI_GET_DEFAULT;
        min_args = max_args = 1;
        if (argc == 2) request->target = argv[1];
        if (argc == 2 && strcmp(argv[1], "sink") && strcmp(argv[1], "source")) {
            return usage("get-default accepts 'sink' or 'source'");
        }
    } else if (strcmp(command, "get-volume") == 0) {
        request->command = CLI_GET_VOLUME;
        min_args = 1; max_args = 2; has_target = true;
        if (argc == 3) request->channel = argv[2];
    } else if (strcmp(command, "set-volume") == 0) {
        request->command = CLI_SET_VOLUME;
        min_args = 2; max_args = 3; has_target = true;
        if (argc == 4) request->channel = argv[3];
        if (argc >= 3) {
            char *end;
            errno = 0;
            float value = strtof(argv[2], &end);
            if (end != argv[2] && *end == '%') end++;
            if (errno || end == argv[2] || *end || !isfinite(value)
                || value < 0 || value > 150) {
                return usage("volume must be between 0 and 150 percent");
            }
            request->volume = value;
        }
    } else if (strcmp(command, "get-mute") == 0) {
        request->command = CLI_GET_MUTE;
        min_args = max_args = 1; has_target = true;
    } else if (strcmp(command, "set-mute") == 0) {
        request->command = CLI_SET_MUTE;
        min_args = max_args = 2; has_target = true;
        if (argc >= 3) {
            if (strcmp(argv[2], "on") == 0) request->mute = CLI_MUTE_ON;
            else if (strcmp(argv[2], "off") == 0) request->mute = CLI_MUTE_OFF;
            else if (strcmp(argv[2], "toggle") == 0) request->mute = CLI_MUTE_TOGGLE;
            else return usage("mute value must be on, off, or toggle");
        }
    } else if (strcmp(command, "set-default") == 0) {
        request->command = CLI_SET_DEFAULT;
        min_args = max_args = 1; has_target = true;
    } else if (strcmp(command, "list-routes") == 0) {
        request->command = CLI_LIST_ROUTES;
        min_args = max_args = 1; has_target = true;
    } else if (strcmp(command, "set-route") == 0) {
        request->command = CLI_SET_ROUTE;
        min_args = max_args = 2; has_target = true;
        if (argc >= 3 && !parse_index(argv[2], &request->index)) {
            return usage("route index must be a nonnegative integer");
        }
    } else if (strcmp(command, "set-target") == 0) {
        request->command = CLI_SET_TARGET;
        min_args = max_args = 2; has_target = true;
        if (argc >= 3) {
            request->destination = argv[2];
            if (strcmp(request->destination, "default") != 0 &&
                !valid_target(request->destination)) {
                return usage("invalid destination");
            }
        }
    } else if (strcmp(command, "list-profiles") == 0) {
        request->command = CLI_LIST_PROFILES;
        min_args = max_args = 1; has_target = true;
    } else if (strcmp(command, "set-profile") == 0) {
        request->command = CLI_SET_PROFILE;
        min_args = max_args = 2; has_target = true;
        if (argc >= 3 && !parse_index(argv[2], &request->index)) {
            return usage("profile index must be a nonnegative integer");
        }
    } else {
        return usage("unknown command");
    }

    if (argc - 1 < min_args || argc - 1 > max_args) {
        return usage("wrong number of command arguments");
    }
    if (has_target) {
        request->target = argv[1];
        if (!valid_target(request->target)) return usage("invalid target");
    }
    if (request->channel && !*request->channel) return usage("empty channel name");
    return 0;
}

static void finish(struct cli_state *state, int status, const char *format, ...) {
    if (state->done) return;
    if (format) {
        va_list ap;
        va_start(ap, format);
        fputs("pipemixer: ", stderr);
        vfprintf(stderr, format, ap);
        fputc('\n', stderr);
        va_end(ap);
    }
    state->status = status;
    state->done = true;
    pw_main_loop_quit(main_loop);
}

static void print_json_string(const char *value) {
    char *quoted = json_quote(value ?: "");
    fputs(quoted, stdout);
    free(quoted);
}

static const char *class_name(enum media_class media_class) {
    switch (media_class) {
    case AUDIO_SINK: return "sink";
    case AUDIO_SOURCE: return "source";
    case STREAM_OUTPUT_AUDIO: return "playback";
    case STREAM_INPUT_AUDIO: return "recording";
    default: return "unknown";
    }
}

static void free_routes(struct cli_node *entry) {
    for (unsigned i = 0; i < entry->route_count; i++) {
        free(entry->routes[i].name);
        free(entry->routes[i].description);
    }
    free(entry->routes);
    entry->routes = NULL;
    entry->route_count = 0;
}

static void free_profiles(struct cli_device *entry) {
    for (unsigned i = 0; i < entry->profile_count; i++) {
        free(entry->profiles[i].name);
        free(entry->profiles[i].description);
    }
    free(entry->profiles);
    entry->profiles = NULL;
    entry->profile_count = 0;
}

static void on_node_removed(struct node *node, void *data) {
    struct cli_node *entry = data;
    entry->removed = true;
    if (entry->state->selected_node == entry) {
        finish(entry->state, 3, "target node disappeared");
    } else {
        cli_evaluate(entry->state);
    }
}

static void on_node_props(struct node *node, const struct dict *props, void *data) {
    struct cli_node *entry = data;
    free(entry->name);
    free(entry->description);
    entry->name = xstrdup(dict_get(props, "node.name"));
    entry->description = xstrdup(dict_get(props, "node.description"));
    entry->has_device = dict_get(props, "device.id") != NULL;
    entry->props_seen = true;
    cli_evaluate(entry->state);
}

static void on_node_channels(struct node *node, const char *names[],
                             unsigned count, void *data) {
    struct cli_node *entry = data;
    for (unsigned i = 0; i < entry->channel_count; i++) free(entry->channels[i]);
    free(entry->channels);
    entry->channels = xcalloc(count ?: 1, sizeof(entry->channels[0]));
    entry->channel_count = count;
    for (unsigned i = 0; i < count; i++) entry->channels[i] = xstrdup(names[i] ?: "");
    entry->channels_seen = true;
    cli_evaluate(entry->state);
}

static void on_node_volume(struct node *node, const float volumes[],
                           unsigned count, void *data) {
    struct cli_node *entry = data;
    entry->volumes = xreallocarray(entry->volumes, count, sizeof(entry->volumes[0]));
    if (count) memcpy(entry->volumes, volumes, count * sizeof(entry->volumes[0]));
    entry->volume_count = count;
    entry->volume_seen = true;
    cli_evaluate(entry->state);
}

static void on_node_mute(struct node *node, bool mute, void *data) {
    struct cli_node *entry = data;
    entry->mute = mute;
    entry->mute_seen = true;
    cli_evaluate(entry->state);
}

static void on_node_default(struct node *node, bool is_default, void *data) {
    struct cli_node *entry = data;
    entry->is_default = is_default;
    entry->default_seen = true;
    cli_evaluate(entry->state);
}

static void on_node_routes(struct node *node, const struct param_route routes[],
                           unsigned count, void *data) {
    struct cli_node *entry = data;
    free_routes(entry);
    entry->routes = xcalloc(count ?: 1, sizeof(entry->routes[0]));
    entry->route_count = count;
    for (unsigned i = 0; i < count; i++) {
        entry->routes[i] = (struct cli_route){
            .index = routes[i].index,
            .name = xstrdup(routes[i].name ?: ""),
            .description = xstrdup(routes[i].description ?: ""),
            .active = routes[i].active,
        };
    }
    entry->routes_seen = true;
    cli_evaluate(entry->state);
}

static const struct node_events node_events = {
    .removed = on_node_removed,
    .props = on_node_props,
    .channels = on_node_channels,
    .volume = on_node_volume,
    .mute = on_node_mute,
    .default_ = on_node_default,
    .routes = on_node_routes,
};

static void on_device_removed(struct device *device, void *data) {
    struct cli_device *entry = data;
    entry->removed = true;
    if (entry->state->selected_device == entry) {
        finish(entry->state, 3, "target device disappeared");
    } else {
        cli_evaluate(entry->state);
    }
}

static void on_device_props(struct device *device, const struct dict *props, void *data) {
    struct cli_device *entry = data;
    free(entry->name);
    free(entry->description);
    entry->name = xstrdup(dict_get(props, "device.name"));
    entry->description = xstrdup(dict_get(props, "device.description"));
    entry->props_seen = true;
    cli_evaluate(entry->state);
}

static void on_device_profiles(struct device *device, const struct param_profile profiles[],
                               unsigned count, void *data) {
    struct cli_device *entry = data;
    free_profiles(entry);
    entry->profiles = xcalloc(count ?: 1, sizeof(entry->profiles[0]));
    entry->profile_count = count;
    for (unsigned i = 0; i < count; i++) {
        entry->profiles[i] = (struct cli_profile){
            .index = profiles[i].index,
            .name = xstrdup(profiles[i].name ?: ""),
            .description = xstrdup(profiles[i].description ?: ""),
            .active = profiles[i].active,
        };
    }
    entry->profiles_seen = true;
    cli_evaluate(entry->state);
}

static const struct device_events device_events = {
    .removed = on_device_removed,
    .props = on_device_props,
    .profiles = on_device_profiles,
};

static void on_pw_node(struct node *node, void *data) {
    struct cli_state *state = data;
    for (struct cli_node *entry = state->nodes; entry; entry = entry->next) {
        if (entry->node == node) return;
    }
    struct cli_node *entry = xcalloc(1, sizeof(*entry));
    entry->state = state;
    entry->node = node;
    entry->next = state->nodes;
    state->nodes = entry;
    entry->hook = node_add_listener(node, &node_events, entry);
    cli_evaluate(state);
}

static void on_pw_device(struct device *device, void *data) {
    struct cli_state *state = data;
    for (struct cli_device *entry = state->devices; entry; entry = entry->next) {
        if (entry->device == device) return;
    }
    struct cli_device *entry = xcalloc(1, sizeof(*entry));
    entry->state = state;
    entry->device = device;
    entry->next = state->devices;
    state->devices = entry;
    entry->hook = device_add_listener(device, &device_events, entry);
    cli_evaluate(state);
}

static void on_pw_default(enum default_metadata_key key, const char *value, void *data) {
    struct cli_state *state = data;
    free(state->defaults[key]);
    state->defaults[key] = xstrdup(value);
    cli_evaluate(state);
}

static void on_pw_stream_target(uint32_t stream_id, void *data) {
    cli_evaluate(data);
}

static void on_pw_sync(int seq, void *data) {
    struct cli_state *state = data;
    if (state->done || seq != state->sync_seq) return;
    state->sync_rounds++;
    if (state->sync_rounds < 3) {
        state->sync_seq = pipewire_sync();
        if (state->sync_seq < 0) finish(state, 1, "PipeWire sync failed");
    } else {
        cli_evaluate(state);
    }
}

static void on_pw_error(int code, const char *message, void *data) {
    struct cli_state *state = data;
    finish(state, 1, "PipeWire error %d: %s", code, message ?: "unknown error");
}

static const struct pipewire_events pipewire_events = {
    .node = on_pw_node,
    .device = on_pw_device,
    .default_ = on_pw_default,
    .stream_target = on_pw_stream_target,
    .sync = on_pw_sync,
    .error = on_pw_error,
};

static void on_timeout(void *data, uint64_t expirations) {
    struct cli_state *state = data;
    finish(state, 1, "timed out after %u ms waiting for PipeWire state",
           state->request->timeout_ms);
}

static bool target_is_id(const char *target, unsigned *id) {
    return strncmp(target, "id:", 3) == 0 && parse_unsigned(target + 3, id);
}

static bool node_matches(const struct cli_node *entry, const char *target) {
    unsigned id;
    if (target_is_id(target, &id)) return node_id(entry->node) == id;
    if (strncmp(target, "serial:", 7) == 0) {
        return streq(node_meter_target(entry->node), target + 7);
    }
    return streq(entry->name, target);
}

static bool device_matches(const struct cli_device *entry, const char *target) {
    unsigned id;
    if (target_is_id(target, &id)) return device_id(entry->device) == id;
    return streq(entry->name, target);
}

static int compare_nodes(const void *a, const void *b) {
    const struct cli_node *na = *(const struct cli_node *const *)a;
    const struct cli_node *nb = *(const struct cli_node *const *)b;
    return (node_id(na->node) > node_id(nb->node)) -
           (node_id(na->node) < node_id(nb->node));
}

static int compare_devices(const void *a, const void *b) {
    const struct cli_device *da = *(const struct cli_device *const *)a;
    const struct cli_device *db = *(const struct cli_device *const *)b;
    return (device_id(da->device) > device_id(db->device)) -
           (device_id(da->device) < device_id(db->device));
}

static void print_nodes(struct cli_state *state) {
    unsigned count = 0;
    for (struct cli_node *entry = state->nodes; entry; entry = entry->next) {
        if (!entry->removed) count++;
    }
    struct cli_node **sorted = xcalloc(count ?: 1, sizeof(sorted[0]));
    unsigned i = 0;
    for (struct cli_node *entry = state->nodes; entry; entry = entry->next) {
        if (!entry->removed) sorted[i++] = entry;
    }
    qsort(sorted, count, sizeof(sorted[0]), compare_nodes);
    if (state->request->json) fputc('[', stdout);
    for (i = 0; i < count; i++) {
        const struct cli_node *entry = sorted[i];
        if (state->request->json) {
            if (i) fputc(',', stdout);
            printf("{\"id\":%u,\"class\":", node_id(entry->node));
            print_json_string(class_name(node_media_class(entry->node)));
            fputs(",\"name\":", stdout); print_json_string(entry->name);
            fputs(",\"description\":", stdout); print_json_string(entry->description);
            fputs(",\"default\":", stdout);
            fputs(!entry->default_seen ? "null}" :
                  entry->is_default ? "true}" : "false}", stdout);
        } else {
            printf("%u\t%s\t%s\t%s\n", node_id(entry->node),
                   class_name(node_media_class(entry->node)), entry->name ?: "",
                   entry->description ?: "");
        }
    }
    if (state->request->json) fputs("]\n", stdout);
    free(sorted);
}

static void print_devices(struct cli_state *state) {
    unsigned count = 0;
    for (struct cli_device *entry = state->devices; entry; entry = entry->next) {
        if (!entry->removed) count++;
    }
    struct cli_device **sorted = xcalloc(count ?: 1, sizeof(sorted[0]));
    unsigned i = 0;
    for (struct cli_device *entry = state->devices; entry; entry = entry->next) {
        if (!entry->removed) sorted[i++] = entry;
    }
    qsort(sorted, count, sizeof(sorted[0]), compare_devices);
    if (state->request->json) fputc('[', stdout);
    for (i = 0; i < count; i++) {
        const struct cli_device *entry = sorted[i];
        if (state->request->json) {
            if (i) fputc(',', stdout);
            printf("{\"id\":%u,\"name\":", device_id(entry->device));
            print_json_string(entry->name);
            fputs(",\"description\":", stdout); print_json_string(entry->description);
            fputc('}', stdout);
        } else {
            printf("%u\t%s\t%s\n", device_id(entry->device), entry->name ?: "",
                   entry->description ?: "");
        }
    }
    if (state->request->json) fputs("]\n", stdout);
    free(sorted);
}

static bool volumes_match(const struct cli_state *state, const struct cli_node *entry) {
    if (!entry->volume_seen || !entry->volume_count) return false;
    for (unsigned i = 0; i < entry->volume_count; i++) {
        if (state->channel_index >= 0 && i != (unsigned)state->channel_index) continue;
        if (!isfinite(entry->volumes[i]) ||
            fabsf(entry->volumes[i] - state->desired_volume) > 0.01f) return false;
    }
    return true;
}

static void print_volumes(struct cli_state *state, const struct cli_node *entry) {
    const char *channel = state->request->channel;
    if (state->request->json) {
        printf("{\"id\":%u,\"name\":", node_id(entry->node));
        print_json_string(entry->name);
        fputs(",\"channels\":[", stdout);
    }
    bool first = true;
    for (unsigned i = 0; i < entry->volume_count; i++) {
        if (channel && i != (unsigned)state->channel_index) continue;
        const char *name = entry->channels[i];
        double percent = entry->volumes[i] * 100.0;
        if (state->request->json) {
            if (!first) fputc(',', stdout);
            fputs("{\"name\":", stdout); print_json_string(name);
            printf(",\"percent\":%.2f}", percent);
        } else if (channel) {
            printf("%.2f\n", percent);
        } else {
            printf("%s\t%.2f\n", name, percent);
        }
        first = false;
    }
    if (state->request->json) fputs("]}\n", stdout);
}

static void print_routes(struct cli_state *state, const struct cli_node *entry) {
    if (state->request->json) fputc('[', stdout);
    for (unsigned i = 0; i < entry->route_count; i++) {
        const struct cli_route *route = &entry->routes[i];
        if (state->request->json) {
            if (i) fputc(',', stdout);
            printf("{\"index\":%d,\"name\":", route->index);
            print_json_string(route->name);
            fputs(",\"description\":", stdout); print_json_string(route->description);
            fputs(route->active ? ",\"active\":true}" : ",\"active\":false}", stdout);
        } else {
            printf("%d\t%s\t%s\t%s\n", route->index,
                   route->active ? "active" : "", route->name, route->description);
        }
    }
    if (state->request->json) fputs("]\n", stdout);
}

static void print_profiles(struct cli_state *state, const struct cli_device *entry) {
    if (state->request->json) fputc('[', stdout);
    for (unsigned i = 0; i < entry->profile_count; i++) {
        const struct cli_profile *profile = &entry->profiles[i];
        if (state->request->json) {
            if (i) fputc(',', stdout);
            printf("{\"index\":%d,\"name\":", profile->index);
            print_json_string(profile->name);
            fputs(",\"description\":", stdout); print_json_string(profile->description);
            fputs(profile->active ? ",\"active\":true}" : ",\"active\":false}", stdout);
        } else {
            printf("%d\t%s\t%s\t%s\n", profile->index,
                   profile->active ? "active" : "", profile->name, profile->description);
        }
    }
    if (state->request->json) fputs("]\n", stdout);
}

static void evaluate_node(struct cli_state *state, struct cli_node *entry) {
    const struct cli_request *request = state->request;
    if (!entry->props_seen) return;

    switch (request->command) {
    case CLI_GET_VOLUME:
    case CLI_SET_VOLUME: {
        if (!entry->channels_seen || !entry->volume_seen ||
            entry->channel_count != entry->volume_count) return;
        if (!entry->volume_count) {
            finish(state, 3, "target has no volume channels");
            return;
        }
        for (unsigned i = 0; i < entry->volume_count; i++) {
            if (!isfinite(entry->volumes[i])) {
                finish(state, 1, "target reported a non-finite volume");
                return;
            }
        }
        if (request->channel) {
            state->channel_index = -1;
            for (unsigned i = 0; i < entry->channel_count; i++) {
                if (strcmp(entry->channels[i], request->channel) == 0) {
                    state->channel_index = (int)i;
                    break;
                }
            }
            if (state->channel_index < 0) {
                finish(state, 3, "channel '%s' not found", request->channel);
                return;
            }
        }
        if (request->command == CLI_GET_VOLUME) {
            print_volumes(state, entry);
            finish(state, 0, NULL);
            return;
        }
        if (volumes_match(state, entry)) {
            finish(state, 0, NULL);
        } else if (!state->action_sent) {
            state->action_sent = true;
            node_change_volume(entry->node, true, state->desired_volume,
                               state->channel_index < 0 ? ALL_CHANNELS : (uint32_t)state->channel_index);
        }
        return;
    }
    case CLI_GET_MUTE:
        if (!entry->mute_seen) return;
        if (request->json) {
            printf("{\"id\":%u,\"name\":", node_id(entry->node));
            print_json_string(entry->name);
            fputs(entry->mute ? ",\"muted\":true}\n" : ",\"muted\":false}\n", stdout);
        } else {
            puts(entry->mute ? "on" : "off");
        }
        finish(state, 0, NULL);
        return;
    case CLI_SET_MUTE:
        if (!entry->mute_seen) return;
        if (!state->action_sent) {
            state->desired_mute = request->mute == CLI_MUTE_TOGGLE ? !entry->mute
                                : request->mute == CLI_MUTE_ON;
        }
        if (entry->mute == state->desired_mute) {
            finish(state, 0, NULL);
        } else if (!state->action_sent) {
            state->action_sent = true;
            node_set_mute(entry->node, state->desired_mute);
        }
        return;
    case CLI_SET_DEFAULT:
        if (node_media_class(entry->node) != AUDIO_SINK &&
            node_media_class(entry->node) != AUDIO_SOURCE) {
            finish(state, 3, "target is not an audio sink or source");
        } else if (entry->default_seen && entry->is_default) {
            finish(state, 0, NULL);
        } else if (!state->action_sent) {
            if (!pipewire_default_available()) {
                finish(state, 1, "PipeWire default metadata is unavailable");
            } else if (!node_set_default(entry->node)) {
                finish(state, 1, "failed to set default node");
            } else {
                state->action_sent = true;
            }
        }
        return;
    case CLI_SET_TARGET: {
        const enum media_class stream_class = node_media_class(entry->node);
        enum media_class destination_class;
        if (stream_class == STREAM_OUTPUT_AUDIO) {
            destination_class = AUDIO_SINK;
        } else if (stream_class == STREAM_INPUT_AUDIO) {
            destination_class = AUDIO_SOURCE;
        } else {
            finish(state, 3, "target is not a playback or recording stream");
            return;
        }
        if (!pipewire_default_available()) {
            finish(state, 1, "PipeWire default metadata is unavailable");
            return;
        }

        uint32_t destination_id = PW_ID_ANY;
        if (strcmp(request->destination, "default") != 0) {
            unsigned matches = 0;
            for (struct cli_node *candidate = state->nodes; candidate; candidate = candidate->next) {
                if (candidate->removed || !candidate->props_seen ||
                    node_media_class(candidate->node) != destination_class ||
                    !node_matches(candidate, request->destination)) continue;
                destination_id = node_id(candidate->node);
                matches++;
            }
            if (matches > 1) {
                finish(state, 3, "ambiguous destination name; use id:N");
                return;
            }
            if (!matches) {
                finish(state, 3, "destination '%s' not found", request->destination);
                return;
            }
        }
        if (pipewire_stream_target_matches(node_id(entry->node), destination_id)) {
            finish(state, 0, NULL);
        } else if (!state->action_sent) {
            if (!pipewire_set_stream_target(node_id(entry->node), destination_id)) {
                finish(state, 1, "failed to set stream destination");
            } else {
                state->action_sent = true;
            }
        }
        return;
    }
    case CLI_LIST_ROUTES:
    case CLI_SET_ROUTE:
        if (!entry->routes_seen && entry->has_device) return;
        if (request->command == CLI_LIST_ROUTES) {
            print_routes(state, entry);
            finish(state, 0, NULL);
            return;
        }
        for (unsigned i = 0; i < entry->route_count; i++) {
            if (entry->routes[i].index != request->index) continue;
            if (entry->routes[i].active) {
                finish(state, 0, NULL);
            } else if (!state->action_sent) {
                state->action_sent = true;
                node_set_route(entry->node, request->index);
            }
            return;
        }
        finish(state, 3, "route index %d not found on target", request->index);
        return;
    default:
        return;
    }
}

static void evaluate_device(struct cli_state *state, struct cli_device *entry) {
    if (!entry->props_seen || !entry->profiles_seen) return;
    const struct cli_request *request = state->request;
    if (request->command == CLI_LIST_PROFILES) {
        print_profiles(state, entry);
        finish(state, 0, NULL);
        return;
    }
    for (unsigned i = 0; i < entry->profile_count; i++) {
        if (entry->profiles[i].index != request->index) continue;
        if (entry->profiles[i].active) {
            finish(state, 0, NULL);
        } else if (!state->action_sent) {
            state->action_sent = true;
            device_set_profile(entry->device, request->index);
        }
        return;
    }
    finish(state, 3, "profile index %d not found on target", request->index);
}

static void cli_evaluate(struct cli_state *state) {
    if (state->done || state->sync_rounds < 3) return;
    const struct cli_request *request = state->request;

    if (request->command == CLI_LIST) {
        if (request->devices) {
            for (struct cli_device *entry = state->devices; entry; entry = entry->next) {
                if (!entry->removed && !entry->props_seen) return;
            }
            print_devices(state);
        } else {
            for (struct cli_node *entry = state->nodes; entry; entry = entry->next) {
                if (!entry->removed && !entry->props_seen) return;
            }
            print_nodes(state);
        }
        finish(state, 0, NULL);
        return;
    }

    if (request->command == CLI_GET_DEFAULT) {
        const bool sink = strcmp(request->target ?: "", "sink") == 0;
        const enum default_metadata_key key = sink ? DEFAULT_AUDIO_SINK : DEFAULT_AUDIO_SOURCE;
        if (!pipewire_default_available()) {
            finish(state, 3, "default metadata is unavailable");
            return;
        }
        if (!state->defaults[key]) return;
        if (request->json) {
            fputs("{\"class\":", stdout); print_json_string(sink ? "sink" : "source");
            fputs(",\"name\":", stdout); print_json_string(state->defaults[key]);
            fputs("}\n", stdout);
        } else {
            puts(state->defaults[key]);
        }
        finish(state, 0, NULL);
        return;
    }

    const bool wants_device = request->command == CLI_LIST_PROFILES ||
                              request->command == CLI_SET_PROFILE;
    if (wants_device) {
        if (!state->selected_device) {
            unsigned matches = 0;
            bool pending = false;
            for (struct cli_device *entry = state->devices; entry; entry = entry->next) {
                if (entry->removed) continue;
                if (!entry->props_seen) pending = true;
                if (device_matches(entry, request->target)) {
                    state->selected_device = entry;
                    matches++;
                }
            }
            if (pending) { state->selected_device = NULL; return; }
            if (matches > 1) { finish(state, 3, "ambiguous device name; use id:N"); return; }
            if (!matches) { finish(state, 3, "device '%s' not found", request->target); return; }
        }
        evaluate_device(state, state->selected_device);
        return;
    }

    if (!state->selected_node) {
        unsigned matches = 0;
        bool pending = false;
        for (struct cli_node *entry = state->nodes; entry; entry = entry->next) {
            if (entry->removed) continue;
            if (!entry->props_seen) pending = true;
            if (node_matches(entry, request->target)) {
                state->selected_node = entry;
                matches++;
            }
        }
        if (pending) { state->selected_node = NULL; return; }
        if (matches > 1) { finish(state, 3, "ambiguous node name; use id:N"); return; }
        if (!matches) { finish(state, 3, "node '%s' not found", request->target); return; }
    }
    evaluate_node(state, state->selected_node);
}

static void cleanup(struct cli_state *state) {
    event_hook_release(state->hook);
    if (state->timer) pw_loop_destroy_source(event_loop, state->timer);

    while (state->nodes) {
        struct cli_node *entry = state->nodes;
        state->nodes = entry->next;
        event_hook_release(entry->hook);
        free(entry->name);
        free(entry->description);
        for (unsigned i = 0; i < entry->channel_count; i++) free(entry->channels[i]);
        free(entry->channels);
        free(entry->volumes);
        free_routes(entry);
        free(entry);
    }
    while (state->devices) {
        struct cli_device *entry = state->devices;
        state->devices = entry->next;
        event_hook_release(entry->hook);
        free(entry->name);
        free(entry->description);
        free_profiles(entry);
        free(entry);
    }
    for (unsigned i = 0; i < DEFAULT_METADATA_KEY_COUNT; i++) free(state->defaults[i]);
}

int cli_run(const struct cli_request *request) {
    struct cli_state state = {
        .request = request,
        .status = 1,
        .channel_index = -1,
        .desired_volume = request->volume / 100.0f,
    };
    state.timer = pw_loop_add_timer(event_loop, on_timeout, &state);
    if (!state.timer) {
        fprintf(stderr, "pipemixer: failed to create CLI timeout timer\n");
        return 1;
    }
    struct timespec delay = {
        .tv_sec = request->timeout_ms / 1000,
        .tv_nsec = (request->timeout_ms % 1000) * 1000000L,
    };
    if (pw_loop_update_timer(event_loop, state.timer, &delay, NULL, false) < 0) {
        fprintf(stderr, "pipemixer: failed to start CLI timeout timer\n");
        cleanup(&state);
        return 1;
    }
    state.hook = pipewire_add_listener(&pipewire_events, &state);
    state.sync_seq = pipewire_sync();
    if (state.sync_seq < 0) {
        fprintf(stderr, "pipemixer: PipeWire sync failed\n");
        cleanup(&state);
        return 1;
    }
    pw_main_loop_run(main_loop);
    cleanup(&state);
    return state.status;
}
