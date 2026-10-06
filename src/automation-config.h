#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AUTOMATION_RULE_LIMIT 32
#define AUTOMATION_ACTION_LIMIT 8

enum auto_action_kind { AUTO_VOLUME, AUTO_MUTE, AUTO_PARAMETER, AUTO_FADE_VOLUME,
    AUTO_FADE_PARAMETER, AUTO_SCENE, AUTO_ROUTE_RULE, AUTO_MONITOR_SOURCE, AUTO_MONITOR_SOLO, AUTO_WAIT };
struct auto_action {
    enum auto_action_kind kind;
    char target[512], parameter[128];
    double value;
    unsigned duration;
    bool smooth, input;
    double minimum, maximum;
};
struct auto_condition;
enum auto_trigger { TRIGGER_CONDITION, TRIGGER_OSC, TRIGGER_MIDI, TRIGGER_INTERVAL };
struct auto_rule {
    char name[49];bool enabled,on_start;
    int priority;
    unsigned hold_ms, release_ms, cooldown_ms;
    struct auto_condition *when;
    enum auto_trigger trigger;
    char osc_address[128];
    unsigned midi_kind,midi_channel,midi_number,midi_edge,interval_ms;
    uint64_t next_interval;
    struct auto_action actions[AUTOMATION_ACTION_LIMIT], otherwise[AUTOMATION_ACTION_LIMIT];
    unsigned n_actions,n_otherwise;
    /* Runtime state is deliberately absent from the saved configuration. */
    bool initialized, latched, candidate;
    int observed;
    uint64_t since,last_fire,fired;
    char state[32], error[160];
};
struct auto_config {
    unsigned count;struct auto_rule rules[AUTOMATION_RULE_LIMIT];
    char osc_bind[16],midi_device[512];
    unsigned osc_port;
    bool osc_direct;
};
char *automation_config_path(void);
int automation_config_load(const char *path,struct auto_config **out,char *error,size_t size);
void automation_config_free(struct auto_config *config);
int automation_config_import(const char *path,char *error,size_t size);
int automation_config_enable(const char *name,bool enabled,char *error,size_t size);
/* Three-valued logic: -1 means unavailable, 0 false, 1 true. */
int automation_condition_eval(struct auto_condition *condition,uint64_t started,uint64_t now);
/* Persistent supervisor preference; transient fade progress is never saved. */
int automation_startup_set(bool enabled,char *error,size_t size);
