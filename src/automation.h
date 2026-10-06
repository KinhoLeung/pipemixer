#pragma once
#include <stdbool.h>
#include <stdint.h>

#define AUTOMATION_JOB_LIMIT 32
#define AUTOMATION_TEXT_LIMIT 196608
enum automation_operation {
    AUTOMATION_STATUS, AUTOMATION_FADE_VOLUME, AUTOMATION_FADE_PARAMETER,
    AUTOMATION_CANCEL, AUTOMATION_STOP, AUTOMATION_TRIGGER, AUTOMATION_TAKEOVER,
};
struct automation_request {
    uint32_t magic, operation, duration_ms, smooth;
    double value;
    char target[512], parameter[128];
};
struct automation_reply { int result; char text[AUTOMATION_TEXT_LIMIT]; };
int automation_request(const struct automation_request *request, struct automation_reply *reply, unsigned timeout_ms);
int automation_start(unsigned timeout_ms);
/* Stop this scope's fade and drain its in-flight writes before a manual edit. */
int automation_take_control(uint32_t id, const char *parameter);
int automation_run(int ready_fd);
int automation_cli(const char *command, const char *target, const char *parameter,
                   double value, unsigned duration, bool smooth, bool json, unsigned timeout);
