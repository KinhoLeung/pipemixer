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
    CLI_LIST_PROFILES,
    CLI_SET_PROFILE,
};

enum cli_mute_value {
    CLI_MUTE_OFF,
    CLI_MUTE_ON,
    CLI_MUTE_TOGGLE,
};

struct cli_request {
    enum cli_command command;
    const char *target;
    const char *channel;
    bool devices;
    bool json;
    unsigned timeout_ms;
    float volume;
    int32_t index;
    enum cli_mute_value mute;
};

/* Return 0 for a valid command, 2 for a usage error. */
int cli_parse(int argc, char **argv, struct cli_request *request);

/* Run one request against the already connected PipeWire event loop. */
int cli_run(const struct cli_request *request);
