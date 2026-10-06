#pragma once

#include <stdbool.h>
#include <stdint.h>

enum cli_command {
    CLI_LIST,
    CLI_GET_VOLUME,
    CLI_SET_VOLUME,
    CLI_GET_MUTE,
    CLI_SET_MUTE,
    CLI_GET_DEFAULT,
    CLI_SET_DEFAULT,
    CLI_LIST_ROUTES,
    CLI_SET_ROUTE,
    CLI_SET_TARGET,
    CLI_LIST_PROFILES,
    CLI_SET_PROFILE,
    CLI_CONNECT,
    CLI_DISCONNECT,
    CLI_CREATE_BUS,
    CLI_DELETE_BUS,
    CLI_CREATE_SEND,
    CLI_DELETE_SEND,
    CLI_AUDIO_WORKER,
    CLI_CREATE_EFFECT,
    CLI_DELETE_EFFECT,
    CLI_EFFECT_PARAMS,
    CLI_SET_EFFECT_PARAM,
    CLI_BYPASS_EFFECT,
    CLI_EFFECT_CHAIN,
    CLI_ADD_EFFECT_STAGE,
    CLI_REMOVE_EFFECT_STAGE,
    CLI_MOVE_EFFECT_STAGE,
    CLI_BYPASS_EFFECT_STAGE,
    CLI_SAVE_SCENE,
    CLI_LOAD_SCENE,
    CLI_LIST_SCENES,
    CLI_DELETE_SCENE,
    CLI_CHECK_SCENE,
    CLI_GET_STARTUP_SCENE,
    CLI_SET_STARTUP_SCENE,
    CLI_RESTORE_STARTUP,
    CLI_CREATE_ROUTE_RULE,
    CLI_DELETE_ROUTE_RULE,
    CLI_ENABLE_ROUTE_RULE,
    CLI_LIST_ROUTE_RULES,
    CLI_CHECK_ROUTE_RULES,
    CLI_ROUTING_DAEMON,
    CLI_SET_ROUTE_PRIORITY,
    CLI_SET_ROUTE_FALLBACKS,
    CLI_MONITOR_EDIT,
    CLI_LIST_MONITORS,
    CLI_CHECK_MONITORS,
    CLI_RECOVERY_STATUS,
    CLI_CLEAR_RECOVERY,
    CLI_DIAGNOSTICS,
    CLI_METER,
    CLI_CAPTURE,
    CLI_CAPTURE_WORKER,
    CLI_AUTOMATION,
    CLI_AUTOMATION_DAEMON,
};

enum cli_mute_value {
    CLI_MUTE_OFF,
    CLI_MUTE_ON,
    CLI_MUTE_TOGGLE,
};

enum cli_list_kind {
    CLI_LIST_NODES,
    CLI_LIST_DEVICES,
    CLI_LIST_PORTS,
    CLI_LIST_LINKS,
    CLI_LIST_GRAPH,
    CLI_LIST_BUSES,
    CLI_LIST_SENDS,
    CLI_LIST_EFFECTS,
};

struct cli_request {
    enum cli_command command;
    const char *target;
    const char *destination;
    const char *source;
    const char *kind;
    const char *channel;
    const char *route_group;
    const char *route_fallbacks[8];
    unsigned route_fallback_count, route_switch_delay;
    bool route_delay_set, route_clear_fallbacks;
    enum cli_list_kind list_kind;
    bool json;
    bool available_only;
    bool route_glob;
    unsigned timeout_ms;
    float volume;
    double value;
    int worker_ready_fd;
    bool worker_private;
    int32_t index;
    enum cli_mute_value mute;
    unsigned monitor_action, n_sources;
    const char *const *sources;
};

/* Return 0 for a valid command, 2 for a usage error. */
int cli_parse(int argc, char **argv, struct cli_request *request);

/* Run one request against the already connected PipeWire event loop. */
int cli_run(const struct cli_request *request);
/* -1 means this request needs PipeWire. */
int cli_offline(const struct cli_request *request);
