#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cli.h"
#include "cli-graph.h"
#include "pw/graph.h"
#include "pw/managed.h"
#include "pw/effect-chain.h"
#include "scene.h"
#include "route-rules.h"
#include "monitor.h"
#include "diagnostics.h"
#include "recorder.h"
#include "automation.h"
#include "automation-config.h"
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
    struct spa_source *worker_poll;
    struct spa_source *signals[2];
    pid_t child_pid;
    struct scene_job *scene_job;
    char *defaults[DEFAULT_METADATA_KEY_COUNT];
    int sync_seq, sync_rounds, status, action_seq;
    bool action_sent, action_waiting, action_acked, done;
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

static bool parse_priority(const char *text, int32_t *result) {
    char *end; errno = 0; long value = strtol(text, &end, 10);
    if (errno || end == text || *end || value < -1000000 || value > 1000000) return false;
    *result = value; return true;
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

    if (!strcmp(command,"import-automation") || !strcmp(command,"check-automation") || !strcmp(command,"enable-automation") || !strcmp(command,"trigger-automation")) {
        bool enable=!strcmp(command,"enable-automation"),check=!strcmp(command,"check-automation"),trigger=!strcmp(command,"trigger-automation");
        if(argc!=(enable?3:2)&&!(check&&argc==1))return usage("import-automation FILE | check-automation [FILE] | enable-automation NAME on|off | trigger-automation NAME");
        if((enable||trigger)&&!managed_valid_name(argv[1]))return usage("invalid automation rule name");
        request->command=CLI_AUTOMATION;request->kind=command;request->target=argc>1?argv[1]:NULL;
        if(enable){if(strcmp(argv[2],"on")&&strcmp(argv[2],"off"))return usage("enable-automation accepts on or off");request->mute=!strcmp(argv[2],"on");}return 0;
    } else if (!strcmp(command,"automation-daemon") || !strcmp(command,"__automation-worker")) {
        unsigned fd=0; bool worker=!strcmp(command,"__automation-worker");
        if(argc!=(worker?2:1)||(worker&&(!parse_unsigned(argv[1],&fd)||fd>INT_MAX)))return usage("invalid automation worker arguments");
        request->command=CLI_AUTOMATION_DAEMON;request->worker_ready_fd=worker?(int)fd:-1;return 0;
    } else if (!strcmp(command,"start-automation") || !strcmp(command,"stop-automation") || !strcmp(command,"automation-status") || !strcmp(command,"cancel-fade")) {
        bool cancel=!strcmp(command,"cancel-fade");
        if(argc<1||argc>(cancel?3:1))return usage("start/stop-automation | automation-status | cancel-fade [TARGET [PARAMETER]]");
        request->command=CLI_AUTOMATION;request->kind=command;
        if(argc>1)request->target=argv[1];
        if(argc>2)request->channel=argv[2];
        if((request->target&&strlen(request->target)>=512)||(request->channel&&strlen(request->channel)>=128))return usage("automation target too long");
        return 0;
    } else if (!strcmp(command,"fade-volume") || !strcmp(command,"fade-effect-param")) {
        bool param=!strcmp(command,"fade-effect-param");unsigned duration;
        int count=param?5:4;
        if(argc<count||argc>count+1||!valid_target(argv[1])||strlen(argv[1])>=512||(param&&(!managed_valid_name(argv[1])||!*argv[2]||strlen(argv[2])>=128)))return usage("fade-volume TARGET PERCENT MS [linear|smooth] | fade-effect-param NAME PARAM VALUE MS [linear|smooth]");
        char *end;errno=0;double value=strtod(argv[param?3:2],&end);
        if(errno||end==argv[param?3:2]||*end||!isfinite(value)||(!param&&(value<0||value>150))||!parse_unsigned(argv[count-1],&duration)||duration<20||duration>3600000)return usage("fade value must be finite, volume 0..150 percent, duration 20..3600000 ms");
        if(argc>count&&strcmp(argv[count],"linear")&&strcmp(argv[count],"smooth"))return usage("fade curve must be linear or smooth");
        request->command=CLI_AUTOMATION;request->kind=command;request->target=argv[1];request->channel=param?argv[2]:NULL;request->value=value;request->index=duration;request->mute=argc>count&&!strcmp(argv[count],"smooth");return 0;
    } else if (!strcmp(command, "start-history") || !strcmp(command, "__capture-worker")) {
        bool worker = !strcmp(command, "__capture-worker");
        unsigned seconds, ready = 0;
        int first = worker ? 4 : 3;
        if (argc <= first || argc > first + CAPTURE_TRACK_LIMIT || !managed_valid_name(argv[1])
            || !parse_unsigned(argv[2], &seconds) || seconds < 1 || seconds > 120
            || (worker && (!parse_unsigned(argv[3], &ready) || ready > INT_MAX)))
            return usage("start-history NAME SECONDS NODE... (1..120 seconds, 1..8 tracks)");
        for (int i = first; i < argc; i++) if (!valid_target(argv[i]) || strlen(argv[i]) >= 512) return usage("invalid capture source");
        request->command = worker ? CLI_CAPTURE_WORKER : CLI_CAPTURE;
        request->kind = command; request->target = argv[1]; request->index = seconds;
        request->worker_ready_fd = ready; request->sources = (const char *const *)(argv + first); request->n_sources = argc - first;
        return 0;
    } else if (!strcmp(command, "list-history") || !strcmp(command, "history-status") || !strcmp(command, "stop-history")
        || !strcmp(command, "export-history") || !strcmp(command, "record-history") || !strcmp(command, "stop-recording")) {
        bool list = !strcmp(command, "list-history"), record = !strcmp(command, "record-history");
        bool export = !strcmp(command, "export-history") || record; unsigned seconds = 0;
        if (argc < (list ? 1 : export ? 3 : 2) || argc > (list ? 1 : export ? 4 : 2)
            || (!list && !managed_valid_name(argv[1])) || (export && !*argv[2])
            || (export && argc == 4 && (!parse_unsigned(argv[3], &seconds) || (!seconds && !record) || seconds > 120)))
            return usage("history-status/stop-history/stop-recording NAME | export-history/record-history NAME NEW_DIRECTORY [SECONDS] | list-history");
        request->command = CLI_CAPTURE; request->kind = command; request->target = list ? NULL : argv[1];
        request->destination = export ? argv[2] : NULL; request->index = seconds; return 0;
    } else if (!strcmp(command, "diagnostics") || !strcmp(command, "meter")) {
        bool meter = !strcmp(command, "meter"); unsigned duration = 1000;
        if (argc < (meter ? 2 : 1) || argc > (meter ? 3 : 2)) return usage("diagnostics [MILLISECONDS] or meter TARGET [MILLISECONDS]");
        if (argc == (meter ? 3 : 2) && (!parse_unsigned(argv[argc-1], &duration) || duration < 100 || duration > 60000)) return usage("measurement duration must be 100..60000 ms");
        if (meter && !valid_target(argv[1])) return usage("invalid meter target");
        request->command = meter ? CLI_METER : CLI_DIAGNOSTICS; request->target = meter ? argv[1] : NULL; request->index = duration; return 0;
    } else if (!strcmp(command, "list-monitors") || !strcmp(command, "check-monitors")) {
        if (argc != 1) return usage("list-monitors and check-monitors take no arguments");
        request->command = !strcmp(command, "list-monitors") ? CLI_LIST_MONITORS : CLI_CHECK_MONITORS; return 0;
    } else if (!strcmp(command, "create-monitor") || !strcmp(command, "delete-monitor") || !strcmp(command, "enable-monitor")
        || !strcmp(command, "set-monitor-sources") || !strcmp(command, "set-monitor-source")
        || !strcmp(command, "solo-monitor") || !strcmp(command, "clear-monitor-solo")) {
        if (argc < 2 || !monitor_valid_name(argv[1])) return usage("monitor name must contain 1-32 letters, digits, '-' or '_'");
        request->command = CLI_MONITOR_EDIT; request->target = argv[1];
        if (!strcmp(command, "create-monitor")) {
            if (argc < 3 || !monitor_valid_node(argv[2])) return usage("create-monitor NAME DESTINATION_NODE [SOURCE_NODE...]");
            request->monitor_action = MONITOR_CREATE; request->source = argv[2]; request->sources = (const char *const *)(argv + 3); request->n_sources = argc - 3;
        } else if (!strcmp(command, "set-monitor-sources")) {
            if (argc < 3) return usage("set-monitor-sources NAME NODE...|off");
            request->monitor_action = MONITOR_SOURCES;
            if (argc != 3 || strcmp(argv[2], "off")) { request->sources = (const char *const *)(argv + 2); request->n_sources = argc - 2; }
        } else if (!strcmp(command, "set-monitor-source")) {
            if (argc != 3 || (strcmp(argv[2], "mix") && !monitor_valid_node(argv[2]))) return usage("set-monitor-source NAME NODE|mix");
            request->monitor_action = MONITOR_LISTEN; request->source = argv[2];
        } else if (!strcmp(command, "solo-monitor")) {
            if (argc != 4 || !monitor_valid_node(argv[2])) return usage("solo-monitor NAME NODE on|off|toggle");
            if (!strcmp(argv[3], "on")) request->monitor_action = MONITOR_SOLO_ON;
            else if (!strcmp(argv[3], "off")) request->monitor_action = MONITOR_SOLO_OFF;
            else if (!strcmp(argv[3], "toggle")) request->monitor_action = MONITOR_SOLO_TOGGLE;
            else return usage("solo-monitor accepts on, off or toggle");
            request->source = argv[2];
        } else if (!strcmp(command, "enable-monitor")) {
            if (argc != 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) return usage("enable-monitor NAME on|off");
            request->monitor_action = MONITOR_ENABLE; request->source = argv[2];
        } else {
            if (argc != 2) return usage("delete-monitor and clear-monitor-solo require only a name");
            request->monitor_action = !strcmp(command, "delete-monitor") ? MONITOR_DELETE : MONITOR_SOLO_CLEAR;
        }
        if (request->n_sources > MONITOR_SOURCE_LIMIT) return usage("a monitor accepts at most 32 sources");
        for (unsigned i = 0; i < request->n_sources; i++) if (!monitor_valid_node(request->sources[i])) return usage("monitor sources require stable node names");
        return 0;
    } else if (!strcmp(command, "create-route-rule") || !strcmp(command, "delete-route-rule") || !strcmp(command, "enable-route-rule")) {
        int expected = !strcmp(command, "create-route-rule") ? 4 : !strcmp(command, "enable-route-rule") ? 3 : 2;
        bool create = expected == 4;
        if (argc < expected || (!create && argc != expected) || !managed_valid_name(argv[1])) return usage("use a valid routing rule name and the required arguments");
        request->target = argv[1];
        request->command = expected == 4 ? CLI_CREATE_ROUTE_RULE : expected == 3 ? CLI_ENABLE_ROUTE_RULE : CLI_DELETE_ROUTE_RULE;
        if (expected == 4) {
            if (!route_rule_valid_endpoint(argv[2]) || !route_rule_valid_endpoint(argv[3])) return usage("routing rules require node.name:port.name endpoints, not session IDs");
            request->source = argv[2]; request->destination = argv[3];
            unsigned seen = 0;
            for (int i = 4; i < argc; i += 2) {
                if (i + 1 == argc) return usage("routing rule options require values");
                unsigned flag;
                if (!strcmp(argv[i], "--match")) {
                    flag = 1;
                    if (strcmp(argv[i + 1], "exact") && strcmp(argv[i + 1], "glob")) return usage("--match accepts exact or glob");
                    request->route_glob = !strcmp(argv[i + 1], "glob");
                } else if (!strcmp(argv[i], "--priority")) {
                    flag = 2;
                    if (!parse_priority(argv[i + 1], &request->index)) return usage("priority must be -1000000..1000000");
                } else if (!strcmp(argv[i], "--exclusive-group")) {
                    flag = 4;
                    if (!managed_valid_name(argv[i + 1])) return usage("use a valid exclusive group name");
                    request->route_group = argv[i + 1];
                } else if (!strcmp(argv[i], "--fallback")) {
                    flag = 0;
                    if (request->route_fallback_count == ROUTE_FALLBACK_LIMIT || !route_rule_valid_endpoint(argv[i + 1])) return usage("use up to 8 stable fallback input endpoints");
                    request->route_fallbacks[request->route_fallback_count++] = argv[i + 1];
                } else if (!strcmp(argv[i], "--switch-delay")) {
                    flag = 8;
                    if (!parse_unsigned(argv[i + 1], &request->route_switch_delay) || request->route_switch_delay > 60000) return usage("switch delay must be 0..60000 ms");
                    request->route_delay_set = true;
                } else return usage("unknown routing rule option");
                if (seen & flag) return usage("duplicate routing rule option");
                seen |= flag;
            }
        } else if (expected == 3) {
            if (!strcmp(argv[2], "on")) request->mute = CLI_MUTE_ON;
            else if (!strcmp(argv[2], "off")) request->mute = CLI_MUTE_OFF;
            else return usage("enable-route-rule accepts on or off");
        }
        return 0;
    } else if (!strcmp(command, "set-route-fallbacks")) {
        if (argc < 3 || !managed_valid_name(argv[1])) return usage("set-route-fallbacks NAME INPUT...|off [--switch-delay MS]");
        request->target = argv[1]; request->command = CLI_SET_ROUTE_FALLBACKS;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--switch-delay")) {
                if (++i == argc || request->route_delay_set || !parse_unsigned(argv[i], &request->route_switch_delay) || request->route_switch_delay > 60000) return usage("use one switch delay in 0..60000 ms");
                request->route_delay_set = true;
            } else if (!strcmp(argv[i], "off")) {
                if (request->route_clear_fallbacks || request->route_fallback_count) return usage("off cannot be combined with fallback inputs");
                request->route_clear_fallbacks = true;
            } else {
                if (request->route_clear_fallbacks || request->route_fallback_count == ROUTE_FALLBACK_LIMIT || !route_rule_valid_endpoint(argv[i])) return usage("use up to 8 stable fallback input endpoints");
                request->route_fallbacks[request->route_fallback_count++] = argv[i];
            }
        }
        return 0;
    } else if (!strcmp(command, "set-route-priority")) {
        if ((argc != 3 && argc != 4) || !managed_valid_name(argv[1]) || !parse_priority(argv[2], &request->index))
            return usage("set-route-priority NAME PRIORITY [GROUP|off]");
        if (argc == 4 && !managed_valid_name(argv[3])) return usage("use a valid group name or off");
        request->target = argv[1]; request->route_group = argc == 4 ? argv[3] : NULL;
        request->command = CLI_SET_ROUTE_PRIORITY; return 0;
    } else if (!strcmp(command, "list-route-rules") || !strcmp(command, "check-route-rules") || !strcmp(command, "routing-daemon")) {
        if (argc != 1) return usage("this routing rule command takes no arguments");
        request->command = !strcmp(command, "list-route-rules") ? CLI_LIST_ROUTE_RULES : !strcmp(command, "check-route-rules") ? CLI_CHECK_ROUTE_RULES : CLI_ROUTING_DAEMON;
        return 0;
    } else if (!strcmp(command, "recovery-status") || !strcmp(command, "clear-recovery")) {
        if (argc != 1) return usage("recovery-status and clear-recovery take no arguments");
        request->command = !strcmp(command, "recovery-status") ? CLI_RECOVERY_STATUS : CLI_CLEAR_RECOVERY;
        return 0;
    } else if (!strcmp(command, "get-startup-scene") || !strcmp(command, "restore-startup") || !strcmp(command, "set-startup-scene")) {
        if (!strcmp(command, "set-startup-scene")) {
            if (argc != 2 || !managed_valid_name(argv[1])) return usage("set-startup-scene requires a scene name or off");
            request->command = CLI_SET_STARTUP_SCENE; request->target = argv[1];
        } else {
            if (argc != 1) return usage("get-startup-scene and restore-startup take no arguments");
            request->command = !strcmp(command, "get-startup-scene") ? CLI_GET_STARTUP_SCENE : CLI_RESTORE_STARTUP;
        }
        return 0;
    } else if (!strcmp(command, "save-scene") || !strcmp(command, "load-scene") || !strcmp(command, "delete-scene")
        || !strcmp(command, "check-scene") || !strcmp(command, "list-scenes")) {
        if (!strcmp(command, "list-scenes")) {
            if (argc != 1) return usage("list-scenes takes no arguments");
            request->command = CLI_LIST_SCENES;
        } else {
            if (argc != 2 || !managed_valid_name(argv[1])) return usage("scene name must contain 1-48 letters, digits, '-' or '_'");
            request->target = argv[1];
            request->command = !strcmp(command, "save-scene") ? CLI_SAVE_SCENE : !strcmp(command, "load-scene") ? CLI_LOAD_SCENE
                : !strcmp(command, "check-scene") ? CLI_CHECK_SCENE : CLI_DELETE_SCENE;
        }
        return 0;
    } else if (!strcmp(command,"effect-chain") || !strcmp(command,"add-effect-stage") || !strcmp(command,"remove-effect-stage")
        || !strcmp(command,"move-effect-stage") || !strcmp(command,"bypass-effect-stage")) {
        bool list=!strcmp(command,"effect-chain"),add=!strcmp(command,"add-effect-stage"),remove=!strcmp(command,"remove-effect-stage");
        if(argc!=(list?2:remove?3:4) && !(add&&argc==3))return usage("invalid effect chain arguments");
        if(!managed_valid_name(argv[1]))return usage("invalid effect name");
        request->target=argv[1];request->index=-1;
        request->command=list?CLI_EFFECT_CHAIN:add?CLI_ADD_EFFECT_STAGE:remove?CLI_REMOVE_EFFECT_STAGE
            :!strcmp(command,"move-effect-stage")?CLI_MOVE_EFFECT_STAGE:CLI_BYPASS_EFFECT_STAGE;
        if(!list)request->channel=argv[2];
        if((add&&argc==4)||request->command==CLI_MOVE_EFFECT_STAGE) {
            uint32_t position;if(!spa_atou32(argv[3],&position,10)||!position||position>EFFECT_STAGE_LIMIT)return usage("stage position must be 1-16");
            request->index=position-1;
        }
        if(request->command==CLI_BYPASS_EFFECT_STAGE) {
            if(streq(argv[3],"on"))request->mute=CLI_MUTE_ON;
            else if(streq(argv[3],"off"))request->mute=CLI_MUTE_OFF;
            else return usage("stage bypass accepts on or off");
        }
        return 0;
    } else if (!strcmp(command, "effect-params") || !strcmp(command, "set-effect-param") || !strcmp(command, "bypass-effect")) {
        int expected = !strcmp(command, "effect-params") ? 2 : !strcmp(command, "set-effect-param") ? 4 : 3;
        if (argc != expected || !managed_valid_name(argv[1])) return usage("invalid effect arguments");
        request->target = argv[1];
        request->command = expected == 2 ? CLI_EFFECT_PARAMS : expected == 4 ? CLI_SET_EFFECT_PARAM : CLI_BYPASS_EFFECT;
        if (expected == 4) {
            char *end;
            errno = 0; request->value = strtod(argv[3], &end);
            if (errno || end == argv[3] || *end || !isfinite(request->value)) return usage("effect value must be a finite number");
            request->channel = argv[2];
        } else if (expected == 3) {
            if (!strcmp(argv[2], "on")) request->mute = CLI_MUTE_ON;
            else if (!strcmp(argv[2], "off")) request->mute = CLI_MUTE_OFF;
            else return usage("bypass-effect accepts on or off");
        }
        return 0;
    } else if (strcmp(command, "create-bus") == 0 || strcmp(command, "delete-bus") == 0
        || strcmp(command, "create-effect") == 0 || strcmp(command, "delete-effect") == 0
        || strcmp(command, "create-send") == 0 || strcmp(command, "delete-send") == 0) {
        const bool send = strstr(command, "send") != NULL;
        const bool effect = strstr(command, "effect") != NULL;
        const bool create = strncmp(command, "create-", 7) == 0;
        if (argc != (send && create ? 4 : effect && create ? 3 : 2)) return usage("invalid audio path argument count");
        if (!managed_valid_name(argv[1])) return usage("name must contain 1-48 letters, digits, '-' or '_'");
        request->command = effect ? (create ? CLI_CREATE_EFFECT : CLI_DELETE_EFFECT)
                               : send ? (create ? CLI_CREATE_SEND : CLI_DELETE_SEND)
                               : (create ? CLI_CREATE_BUS : CLI_DELETE_BUS);
        request->kind = effect ? "effect" : send ? "send" : "bus";
        request->target = argv[1];
        if (send && create) { request->source = argv[2]; request->destination = argv[3]; }
        if (effect && create) {
            if (strcmp(argv[2], "eq") && strcmp(argv[2], "voice") && strcmp(argv[2],"empty") && (argv[2][0] != '@' || !argv[2][1]))
                return usage("effect preset must be eq, voice, empty, or @CONFIG_FILE");
            request->source = argv[2];
        }
        return 0;
    } else if (strcmp(command, "audio-worker") == 0) {
        request->worker_ready_fd = -1;
        if (argc >= 5 && !strcmp(argv[argc - 2], "--ready-fd")) {
            uint32_t fd;
            if (!spa_atou32(argv[argc - 1], &fd, 10) || fd < 3 || fd > INT_MAX)
                return usage("invalid worker readiness descriptor");
            request->worker_ready_fd = fd;
            argc -= 2;
        }
        if(argc>3 && streq(argv[argc-1],"--private")){request->worker_private=true;argc--;}
        if (argc < 3 || !managed_valid_name(argv[2])) return usage("invalid audio worker arguments");
        if (!strcmp(argv[1], "bus") && argc == 3) request->kind = "bus";
        else if (!strcmp(argv[1], "send") && argc == 5) {
            request->kind = "send"; request->source = argv[3]; request->destination = argv[4];
        } else if (!strcmp(argv[1], "effect") && argc == 4) {
            request->kind = "effect"; request->source = argv[3];
        } else if (!strcmp(argv[1], "monitor") && argc == 4 && monitor_valid_name(argv[2]) && monitor_valid_scope(argv[3])) {
            request->kind = "monitor"; request->source = argv[3];
        } else return usage("invalid audio worker kind or arguments");
        if(request->worker_private && !streq(request->kind,"effect"))return usage("only effect validation workers may be private");
        request->command = CLI_AUDIO_WORKER; request->target = argv[2];
        return 0;
    } else if (strcmp(command, "connect") == 0 || strcmp(command, "disconnect") == 0) {
        if (argc != 3) return usage("connect/disconnect requires an output port and an input port");
        if (!valid_target(argv[1]) || !valid_target(argv[2])) return usage("invalid port target");
        request->command = strcmp(command, "connect") == 0 ? CLI_CONNECT : CLI_DISCONNECT;
        request->target = argv[1];
        request->destination = argv[2];
        return 0;
    } else if (strcmp(command, "list") == 0) {
        request->command = CLI_LIST;
        max_args = 1;
        if (argc == 2) {
            if (strcmp(argv[1], "nodes") == 0) request->list_kind = CLI_LIST_NODES;
            else if (strcmp(argv[1], "devices") == 0) request->list_kind = CLI_LIST_DEVICES;
            else if (strcmp(argv[1], "ports") == 0) request->list_kind = CLI_LIST_PORTS;
            else if (strcmp(argv[1], "links") == 0) request->list_kind = CLI_LIST_LINKS;
            else if (strcmp(argv[1], "graph") == 0) request->list_kind = CLI_LIST_GRAPH;
            else if (strcmp(argv[1], "buses") == 0) request->list_kind = CLI_LIST_BUSES;
            else if (strcmp(argv[1], "sends") == 0) request->list_kind = CLI_LIST_SENDS;
            else if (strcmp(argv[1], "effects") == 0) request->list_kind = CLI_LIST_EFFECTS;
            else return usage("list accepts nodes, devices, ports, links, graph, buses, sends, or effects");
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

static void on_pw_graph(void *data) {
    cli_evaluate(data);
}

static void on_pw_sync(int seq, void *data) {
    struct cli_state *state = data;
    if(!state->done&&state->action_waiting&&seq==state->action_seq){
        state->action_waiting=false;state->action_acked=true;cli_evaluate(state);return;
    }
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
    if(scene_job_error(state->scene_job,code,message))return;
    finish(state, 1, "PipeWire error %d: %s", code, message ?: "unknown error");
}

static const struct pipewire_events pipewire_events = {
    .node = on_pw_node,
    .device = on_pw_device,
    .default_ = on_pw_default,
    .stream_target = on_pw_stream_target,
    .sync = on_pw_sync,
    .error = on_pw_error,
    .graph = on_pw_graph,
};

static void on_timeout(void *data, uint64_t expirations) {
    struct cli_state *state = data;
    if(scene_job_error(state->scene_job,-ETIMEDOUT,"chain edit deadline exceeded")) {
        struct timespec delay={.tv_sec=15};
        pw_loop_update_timer(event_loop,state->timer,&delay,NULL,false);
        return;
    }
    finish(state, 1, "timed out after %u ms waiting for %s",
           state->request->timeout_ms, scene_job_stage(state->scene_job));
}

static void on_worker_poll(void *data, uint64_t expirations) {
    cli_evaluate(data);
}

static void on_cli_signal(void *data, int signal) {
    struct cli_state *state=data;char message[80];snprintf(message,sizeof(message),"interrupted by signal %d",signal);
    if(scene_job_error(state->scene_job,-ECANCELED,message))return;
    finish(state, 1, "%s", message);
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
        if (!state->action_sent) {
            int result=automation_take_control(node_id(entry->node),NULL);
            if(result<0){finish(state,3,"cannot take volume control: %s",strerror(-result));return;}
            state->action_sent = true;
            node_change_volume(entry->node, true, state->desired_volume,
                               state->channel_index < 0 ? ALL_CHANNELS : (uint32_t)state->channel_index);
            state->action_seq=pipewire_sync();state->action_waiting=true;
            if(state->action_seq<0)finish(state,1,"PipeWire sync failed");
        } else if (state->action_acked&&volumes_match(state, entry)) {
            finish(state, 0, NULL);
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
        } else if (entry->default_seen && entry->is_default
            && streq(entry->name, pipewire_get_default(node_media_class(entry->node) == AUDIO_SINK
                ? DEFAULT_CONFIGURED_AUDIO_SINK : DEFAULT_CONFIGURED_AUDIO_SOURCE))) {
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

    if (request->command == CLI_RECOVERY_STATUS) {
        if (!graph_ready()) return;
        char error[256] = {0}; int result = scene_recovery_print(request->json, error, sizeof(error));
        finish(state, result < 0, result < 0 ? "%s" : NULL, error); return;
    }

    if (request->command == CLI_LIST_MONITORS) {
        if (!graph_ready()) return;
        struct monitor *monitors; unsigned count; char error[256] = {0};
        int result = monitors_load(&monitors, &count, error, sizeof(error));
        if (result < 0) finish(state, 1, "%s", error);
        else { monitors_print(monitors, count, request->json); monitors_free(monitors, count); finish(state, 0, NULL); }
        return;
    }
    if (request->command == CLI_LIST_ROUTE_RULES) {
        if (!graph_ready()) return;
        struct route_rule *rules; unsigned count; char error[256] = {0};
        int result = route_rules_load(&rules, &count, error, sizeof(error));
        if (result < 0) finish(state, 1, "%s", error);
        else { route_rules_print(rules, count, request->json); route_rules_free(rules, count); finish(state, 0, NULL); }
        return;
    }

    if (request->command == CLI_SAVE_SCENE || request->command == CLI_LOAD_SCENE) {
        if (!graph_ready()) return;
        char error[256] = {0};
        if (!state->worker_poll) {
            state->worker_poll = pw_loop_add_timer(event_loop, on_worker_poll, state);
            struct timespec interval = {.tv_nsec = 100000000};
            if (!state->worker_poll || pw_loop_update_timer(event_loop, state->worker_poll, &interval, &interval, false) < 0) {
                finish(state, 1, "cannot monitor scene operation"); return;
            }
        }
        int result;
        if (request->command == CLI_SAVE_SCENE) result = scene_save(request->target, error, sizeof(error));
        else {
            if (!state->scene_job) {
                state->scene_job = request->available_only ? scene_load_available(request->target, error, sizeof(error))
                                                          : scene_load(request->target, error, sizeof(error));
                if (!state->scene_job) { finish(state, errno == ENOENT || errno == EEXIST || errno == ENOTUNIQ ? 3 : 1, "%s", error); return; }
            }
            result = scene_step(state->scene_job, error, sizeof(error));
        }
        if (result == -EAGAIN) return;
        if (result < 0) finish(state, request->command == CLI_LOAD_SCENE && (result == -ENOENT || result == -EEXIST || result == -ENOTUNIQ) ? 3 : 1, "%s", error[0] ? error : strerror(-result));
        else {
            unsigned skipped = scene_job_skipped(state->scene_job);
            if (skipped) fprintf(stderr, "pipemixer: restored available paths; skipped %u nodes with absent devices/applications\n", skipped);
            finish(state, 0, NULL);
        }
        return;
    }

    if(request->command>=CLI_EFFECT_CHAIN && request->command<=CLI_BYPASS_EFFECT_STAGE) {
        if(!graph_ready())return;
        char error[256]={0};
        if(!state->worker_poll) {
            state->worker_poll=pw_loop_add_timer(event_loop,on_worker_poll,state);struct timespec interval={.tv_nsec=100000000};
            if(!state->worker_poll||pw_loop_update_timer(event_loop,state->worker_poll,&interval,&interval,false)<0){finish(state,1,"cannot monitor chain editing");return;}
        }
        if(state->scene_job) {
            int result=scene_step(state->scene_job,error,sizeof(error));
            if(result!=-EAGAIN)finish(state,result<0?1:0,result<0?"%s":NULL,error[0]?error:strerror(-result));
            return;
        }
        const struct graph_node *node=managed_find("effect",request->target,"input");struct effect_chain chain;
        int result=effect_chain_read(node,&chain);
        if(result==-EAGAIN)return;
        if(result<0){finish(state,3,"cannot edit chain: %s",result==-ENOTSUP?"custom graph has no chain description":strerror(-result));return;}
        if(request->command==CLI_EFFECT_CHAIN){effect_chain_print(&chain,request->json);finish(state,0,NULL);return;}
        if(request->command==CLI_ADD_EFFECT_STAGE)result=effect_chain_add(&chain,request->channel,request->index<0?chain.count:(unsigned)request->index);
        else {
            unsigned index=chain.count;for(unsigned i=0;i<chain.count;i++)if(streq(chain.stages[i].id,request->channel))index=i;
            if(index==chain.count){finish(state,3,"stage '%s' not found",request->channel);return;}
            if(request->command==CLI_REMOVE_EFFECT_STAGE) {
                memmove(chain.stages+index,chain.stages+index+1,(chain.count-index-1)*sizeof(chain.stages[0]));chain.count--;
            } else if(request->command==CLI_MOVE_EFFECT_STAGE) {
                if((unsigned)request->index>=chain.count)result=-EINVAL;
                else {
                    struct effect_stage stage=chain.stages[index];
                    memmove(chain.stages+index,chain.stages+index+1,(chain.count-index-1)*sizeof(stage));
                    memmove(chain.stages+request->index+1,chain.stages+request->index,(chain.count-request->index-1)*sizeof(stage));chain.stages[request->index]=stage;
                }
            } else {
                bool bypass=request->mute==CLI_MUTE_ON;
                if(chain.stages[index].bypass==bypass){finish(state,0,NULL);return;}
                char wet[96],dry[96];snprintf(wet,sizeof(wet),"pm_%s_wet:Mult",chain.stages[index].id);snprintf(dry,sizeof(dry),"pm_%s_dry:Mult",chain.stages[index].id);
                if(graph_control_find(node,wet)) {
                    if(!state->action_sent){const char *names[]={wet,dry};const double values[]={!bypass,bypass};result=graph_set_controls(node->id,names,values,2);state->action_sent=true;}
                    if(result<0)finish(state,3,"cannot bypass stage: %s",strerror(-result));
                    return;
                }
                chain.stages[index].bypass=bypass;
            }
        }
        if(result<0){finish(state,3,"cannot edit chain: %s",strerror(-result));return;}
        state->scene_job=scene_edit_effect(request->target,&chain,error,sizeof(error));
        if(!state->scene_job && errno!=EAGAIN)finish(state,1,"%s",error[0]?error:strerror(errno));
        return;
    }

    if (request->command == CLI_EFFECT_PARAMS || request->command == CLI_SET_EFFECT_PARAM
        || request->command == CLI_BYPASS_EFFECT) {
        if (!graph_ready()) return;
        const struct graph_node *node = managed_find("effect", request->target, "input");
        if (!node) { finish(state, 3, "effect '%s' not found", request->target); return; }
        if (!node->controls_ready) return;
        if (request->command == CLI_EFFECT_PARAMS) {
            if (request->json) fputc('[', stdout);
            bool first = true;
            for (unsigned i = 0; i < node->n_controls; i++) {
                const struct graph_control *control = &node->controls[i];
                if (!control->has_value || !control->has_info || !control->visible) continue;
                if (request->json) {
                    if (!first) fputc(',', stdout);
                    fputs("{\"name\":", stdout); print_json_string(control->name);
                    printf(",\"value\":%.9g,\"minimum\":%.9g,\"maximum\":%.9g,\"default\":%.9g,\"writable\":%s}",
                           control->value, control->minimum, control->maximum, control->default_value,
                           control->writable ? "true" : "false");
                } else printf("%s\t%.9g\t[%.9g, %.9g]\t%s\n", control->name, control->value,
                              control->minimum, control->maximum, control->writable ? "writable" : "read-only");
                first = false;
            }
            if (request->json) fputs("]\n", stdout);
            finish(state, 0, NULL); return;
        }
        const char *names[2] = {request->channel, NULL};
        double values[2] = {request->value, 0};
        unsigned count = 1;
        if (request->command == CLI_BYPASS_EFFECT) {
            count = 2; names[0] = "wet:Mult"; names[1] = "dry:Mult";
            values[0] = request->mute == CLI_MUTE_ON ? 0 : 1; values[1] = 1 - values[0];
        }
        bool matches = true;
        for (unsigned i = 0; i < count; i++) {
            const struct graph_control *control = graph_control_find(node, names[i]);
            if (!control || !control->has_info || !control->has_value || !control->visible) {
                finish(state, 3, "effect parameter '%s' not found", names[i]); return;
            }
            if (fabs(control->value - values[i]) > 1e-6 * fmax(1, fabs(values[i]))) matches = false;
        }
        if (!state->action_sent) {
            int result=graph_validate_controls(node->id,names,values,count);
            for(unsigned i=0;result>=0&&i<count;i++)result=automation_take_control(node->id,names[i]);
            if(result>=0)result=graph_set_controls(node->id,names,values,count);
            if (result < 0) finish(state, 3, "cannot set effect parameter: %s", strerror(-result));
            else{
                state->action_sent=true;state->action_seq=pipewire_sync();state->action_waiting=true;
                if(state->action_seq<0)finish(state,1,"PipeWire sync failed");
            }
        } else if(state->action_acked&&matches){
            finish(state,0,NULL);
        }
        return;
    }

    if (request->command == CLI_AUDIO_WORKER) {
        if (state->action_sent || !graph_ready()) return;
        int result = managed_worker_load(request->kind, request->target, request->source, request->destination,
                                         request->worker_ready_fd, request->worker_private);
        if (result < 0) { finish(state, 1, "cannot start audio worker: %s", strerror(-result)); return; }
        state->action_sent = true;
        state->status = 0;
        if (state->timer) { pw_loop_destroy_source(event_loop, state->timer); state->timer = NULL; }
        return;
    }

    if (request->command == CLI_CREATE_BUS || request->command == CLI_CREATE_SEND || request->command == CLI_CREATE_EFFECT
        || request->command == CLI_DELETE_BUS || request->command == CLI_DELETE_SEND || request->command == CLI_DELETE_EFFECT) {
        if (!graph_ready()) return;
        bool create = request->command == CLI_CREATE_BUS || request->command == CLI_CREATE_SEND || request->command == CLI_CREATE_EFFECT;
        if (!create) {
            if (!managed_find(request->kind, request->target, NULL)) { finish(state, 0, NULL); return; }
            if (!state->action_sent) {
                int result = managed_remove(request->kind, request->target);
                if (result < 0) finish(state, 1, "cannot remove %s: %s", request->kind, strerror(-result));
                else state->action_sent = true;
            }
            return;
        }
        if (state->action_sent) {
            if (managed_child_status(state->child_pid) != -EAGAIN)
                finish(state, 1, "audio worker exited before becoming ready; see %s/pipemixer-%s-%s.log",
                       getenv("XDG_RUNTIME_DIR") ?: "/tmp", request->kind, request->target);
            else if (managed_child_ready(state->child_pid) && managed_ready(request->kind, request->target))
                finish(state, 0, NULL);
            return;
        }
        if (managed_find(request->kind, request->target, NULL)) {
            finish(state, 3, "%s '%s' already exists", request->kind, request->target); return;
        }
        char *source = NULL, *destination = NULL;
        if (request->command == CLI_CREATE_EFFECT) source = xstrdup(request->source);
        if (request->command == CLI_CREATE_SEND) {
            uint32_t src, dest;
            int result = graph_resolve_node(request->source, &src);
            if (!result) result = graph_resolve_node(request->destination, &dest);
            if (!result && (!graph_node_has_ports(src, PW_DIRECTION_OUTPUT)
                || !graph_node_has_ports(dest, PW_DIRECTION_INPUT))) result = -EINVAL;
            if (result < 0) { finish(state, 3, "send endpoint unavailable: %s", strerror(-result)); return; }
            if (graph_would_cycle(src, dest)) { finish(state, 1, "send would create a feedback loop"); return; }
            const struct graph_node *src_node = graph_node_find(src), *dst_node = graph_node_find(dest);
            xasprintf(&source, "serial:%s", dict_get(&src_node->props, PW_KEY_OBJECT_SERIAL));
            xasprintf(&destination, "serial:%s", dict_get(&dst_node->props, PW_KEY_OBJECT_SERIAL));
        }
        state->child_pid = managed_spawn(request->kind, request->target, source, destination);
        free(source); free(destination);
        if (state->child_pid < 0) {
            finish(state, 1, "cannot create %s: %s", request->kind, strerror(-state->child_pid)); return;
        }
        state->action_sent = true;
        state->worker_poll = pw_loop_add_timer(event_loop, on_worker_poll, state);
        struct timespec interval = { .tv_nsec = 100000000 };
        if (!state->worker_poll || pw_loop_update_timer(event_loop, state->worker_poll, &interval, &interval, false) < 0)
            finish(state, 1, "cannot start audio worker monitor");
        return;
    }

    if (request->command == CLI_CONNECT || request->command == CLI_DISCONNECT) {
        if (!graph_ready()) return;
        uint32_t output, input;
        int result = graph_resolve_port(request->target, PW_DIRECTION_OUTPUT, &output);
        if (!result) result = graph_resolve_port(request->destination, PW_DIRECTION_INPUT, &input);
        if (result < 0) { finish(state, 3, "port unavailable: %s", strerror(-result)); return; }
        const struct graph_link *link = graph_link_between(output, input);
        if (request->command == CLI_CONNECT && link) {
            if (link->info->state == PW_LINK_STATE_ERROR) {
                finish(state, 1, "link failed: %s", link->info->error ?: "unknown error");
            } else if (link->info->state >= PW_LINK_STATE_PAUSED) {
                finish(state, 0, NULL);
            }
        } else if (request->command == CLI_DISCONNECT && !link) {
            finish(state, 0, NULL);
        } else if (!state->action_sent) {
            result = request->command == CLI_CONNECT ? graph_connect(output, input) : graph_disconnect(output, input);
            if (result < 0) finish(state, 1, "cannot change connection: %s",
                                   result == -ELOOP ? "connection would create a feedback loop" : strerror(-result));
            else state->action_sent = true;
        }
        return;
    }

    if (request->command == CLI_LIST) {
        if (request->list_kind >= CLI_LIST_PORTS) {
            if (!graph_ready()) return;
            cli_graph_print(request->list_kind, request->json);
        } else if (request->list_kind == CLI_LIST_DEVICES) {
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
    for (unsigned i = 0; i < 2; i++) if (state->signals[i]) pw_loop_destroy_source(event_loop, state->signals[i]);
    scene_job_free(state->scene_job, state->status != 0);
    if (state->child_pid > 0 && state->status != 0) managed_cancel(state->child_pid);
    if (state->worker_poll) pw_loop_destroy_source(event_loop, state->worker_poll);
    managed_worker_cleanup();
    managed_cleanup();
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
    if (request->command == CLI_AUTOMATION_DAEMON) return automation_run(request->worker_ready_fd);
    if (request->command == CLI_CAPTURE_WORKER)
        return capture_worker(request->target, request->index, request->sources, request->n_sources, request->worker_ready_fd);
    if (request->command == CLI_CAPTURE)
        return capture_cli(request->kind, request->target, request->destination, request->index, request->sources, request->n_sources, request->json, request->timeout_ms);
    if (request->command == CLI_DIAGNOSTICS || request->command == CLI_METER)
        return diagnostics_cli(request->command == CLI_METER ? request->target : NULL, request->index, request->json, request->timeout_ms);
    if (request->command == CLI_ROUTING_DAEMON) return route_rules_run();
    char *startup = NULL;
    struct cli_request resolved;
    if (request->command == CLI_RESTORE_STARTUP) {
        startup = scene_startup_get();
        if (!startup) { fprintf(stderr, "pipemixer: cannot read startup scene: %s\n", strerror(errno)); return 1; }
        if (streq(startup, "off")) { free(startup); return 0; }
        resolved = *request; resolved.command = CLI_LOAD_SCENE; resolved.target = startup; resolved.available_only = true;
        request = &resolved;
    }
    struct cli_state state = {
        .request = request,
        .status = 1,
        .channel_index = -1,
        .desired_volume = request->volume / 100.0f,
    };
    if (request->command != CLI_AUDIO_WORKER) {
        state.signals[0] = pw_loop_add_signal(event_loop, SIGINT, on_cli_signal, &state);
        state.signals[1] = pw_loop_add_signal(event_loop, SIGTERM, on_cli_signal, &state);
    }
    state.timer = pw_loop_add_timer(event_loop, on_timeout, &state);
    if (!state.timer) {
        fprintf(stderr, "pipemixer: failed to create CLI timeout timer\n");
        cleanup(&state);
        free(startup); return 1;
    }
    struct timespec delay = {
        .tv_sec = request->timeout_ms / 1000,
        .tv_nsec = (request->timeout_ms % 1000) * 1000000L,
    };
    if (pw_loop_update_timer(event_loop, state.timer, &delay, NULL, false) < 0) {
        fprintf(stderr, "pipemixer: failed to start CLI timeout timer\n");
        cleanup(&state);
        free(startup); return 1;
    }
    state.hook = pipewire_add_listener(&pipewire_events, &state);
    state.sync_seq = pipewire_sync();
    if (state.sync_seq < 0) {
        fprintf(stderr, "pipemixer: PipeWire sync failed\n");
        cleanup(&state);
        free(startup); return 1;
    }
    pw_main_loop_run(main_loop);
    cleanup(&state);
    free(startup);
    return state.status;
}

int cli_offline(const struct cli_request *request) {
    if(request->command==CLI_AUTOMATION&&(streq(request->kind,"import-automation")||streq(request->kind,"check-automation")||streq(request->kind,"enable-automation"))){
        char error[256]={0};int result;
        if(streq(request->kind,"import-automation"))result=automation_config_import(request->target,error,sizeof(error));
        else if(streq(request->kind,"enable-automation"))result=automation_config_enable(request->target,request->mute,error,sizeof(error));
        else{char *path=request->target?strdup(request->target):automation_config_path();struct auto_config *config=NULL;result=path?automation_config_load(path,&config,error,sizeof(error)):-ENOENT;automation_config_free(config);free(path);}
        if(result<0)fprintf(stderr,"pipemixer: %s\n",*error?error:strerror(-result));
        return result<0?3:0;
    }
    if (request->command == CLI_AUTOMATION) return automation_cli(request->kind, request->target, request->channel,
        request->value, request->index, request->mute, request->json, request->timeout_ms);
    if (request->command == CLI_CAPTURE && strcmp(request->kind, "start-history"))
        return capture_cli(request->kind, request->target, request->destination, request->index, request->sources, request->n_sources, request->json, request->timeout_ms);
    if (request->command == CLI_CLEAR_RECOVERY) {
        int result = scene_recovery_clear();
        if (result < 0) fprintf(stderr, "pipemixer: cannot clear recovery: %s\n", strerror(-result));
        return result < 0;
    }
    if (request->command == CLI_MONITOR_EDIT || request->command == CLI_CHECK_MONITORS) {
        char error[256] = {0}; int result;
        if (request->command == CLI_MONITOR_EDIT) result = monitor_edit(request->target, request->monitor_action,
            request->source, request->sources, request->n_sources, error, sizeof(error));
        else { struct monitor *monitors; unsigned count; result = monitors_load(&monitors, &count, error, sizeof(error)); monitors_free(monitors, count); }
        if (result < 0) fprintf(stderr, "pipemixer: %s\n", error);
        return result < 0;
    }
    if (request->command == CLI_CREATE_ROUTE_RULE || request->command == CLI_DELETE_ROUTE_RULE || request->command == CLI_ENABLE_ROUTE_RULE || request->command == CLI_CHECK_ROUTE_RULES || request->command == CLI_SET_ROUTE_PRIORITY || request->command == CLI_SET_ROUTE_FALLBACKS) {
        char error[256] = {0}; int result;
        if (request->command == CLI_CREATE_ROUTE_RULE) result = route_rule_create_config(&(struct route_rule){
            .name = (char *)request->target, .output = (char *)request->source, .input = (char *)request->destination,
            .enabled = true, .glob = request->route_glob, .priority = request->index, .group = (char *)request->route_group,
            .fallbacks = (char **)request->route_fallbacks, .n_fallbacks = request->route_fallback_count,
            .switch_delay_ms = request->route_delay_set ? request->route_switch_delay : 1000,
            .version = request->route_glob || request->index || request->route_group || request->route_fallback_count || request->route_delay_set ? 2 : 1}, error, sizeof(error));
        else if (request->command == CLI_SET_ROUTE_PRIORITY) result = route_rule_set_priority(request->target, request->index, request->route_group, error, sizeof(error));
        else if (request->command == CLI_SET_ROUTE_FALLBACKS) result = route_rule_set_fallbacks(request->target,
            request->route_fallback_count || request->route_clear_fallbacks ? request->route_fallbacks : NULL,
            request->route_fallback_count, request->route_delay_set ? (int)request->route_switch_delay : -1, error, sizeof(error));
        else if (request->command == CLI_DELETE_ROUTE_RULE) result = route_rule_delete(request->target, error, sizeof(error));
        else if (request->command == CLI_ENABLE_ROUTE_RULE) result = route_rule_enable(request->target, request->mute == CLI_MUTE_ON, error, sizeof(error));
        else {
            struct route_rule *rules; unsigned count;
            result = route_rules_load(&rules, &count, error, sizeof(error)); route_rules_free(rules, count);
        }
        if (result < 0) fprintf(stderr, "pipemixer: %s\n", error);
        return result < 0;
    }
    if (request->command == CLI_SET_STARTUP_SCENE) {
        char error[256] = {0}; int result = scene_startup_set(request->target, error, sizeof(error));
        if (result < 0) fprintf(stderr, "pipemixer: %s\n", error);
        return result < 0;
    }
    if (request->command == CLI_GET_STARTUP_SCENE || request->command == CLI_RESTORE_STARTUP) {
        char *startup = scene_startup_get();
        if (!startup) { fprintf(stderr, "pipemixer: cannot read startup scene: %s\n", strerror(errno)); return 1; }
        int result = streq(startup, "off") ? 0 : -1;
        if (request->command == CLI_RESTORE_STARTUP && !result) result = scene_recovery_clear() < 0;
        if (request->command == CLI_GET_STARTUP_SCENE) {
            if (request->json) { print_json_string(startup); fputc('\n', stdout); }
            else puts(startup);
            result = 0;
        }
        free(startup); return result;
    }
    if (request->command == CLI_LIST_SCENES) {
        char **names; unsigned count;
        int result = scene_list(&names, &count);
        if (result < 0) { fprintf(stderr, "pipemixer: cannot list scenes: %s\n", strerror(-result)); return 1; }
        if (request->json) fputc('[', stdout);
        for (unsigned i = 0; i < count; i++) {
            if (request->json) { if (i) fputc(',', stdout); print_json_string(names[i]); }
            else puts(names[i]);
        }
        if (request->json) fputs("]\n", stdout);
        scene_list_free(names, count); return 0;
    }
    if (request->command == CLI_DELETE_SCENE || request->command == CLI_CHECK_SCENE) {
        char error[256] = {0};
        int result = request->command == CLI_DELETE_SCENE ? scene_delete(request->target) : scene_validate(request->target, error, sizeof(error));
        if (result < 0) { fprintf(stderr, "pipemixer: %s\n", error[0] ? error : strerror(-result)); return 1; }
        return 0;
    }
    return -1;
}
