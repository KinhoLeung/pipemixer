#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include "automation-config.h"
enum control_event_kind { CONTROL_OSC, CONTROL_MIDI, CONTROL_VOLUME, CONTROL_MUTE, CONTROL_PARAMETER, CONTROL_SCENE, CONTROL_RULE };
struct control_event {
    enum control_event_kind kind;
    char address[128],target[512],parameter[128];
    double value;
    unsigned duration,channel,number,midi_kind;
    bool smooth,release;
};
struct external_control;
typedef int (*control_handler)(void *data,const struct control_event *event,char *error,size_t size);
struct external_control *external_control_create(control_handler handler,void *data);
void external_control_configure(struct external_control *control,const struct auto_config *config);
void external_control_tick(struct external_control *control,uint64_t now);
void external_control_destroy(struct external_control *control);
void external_control_status(struct external_control *control,FILE *out);
