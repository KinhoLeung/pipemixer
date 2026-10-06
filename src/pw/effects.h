#pragma once
#include <stdbool.h>

char *effect_graph_load(const char *preset);
char *effect_graph_load_chain(const char *preset, char **chain_spec);
/* Apply useful preset defaults/ranges and select the public DSP controls. */
bool effect_control_config(const char *preset, const char *name,
                           double *minimum, double *maximum, double *default_value);
