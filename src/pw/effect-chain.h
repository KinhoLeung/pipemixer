#pragma once
#include "pw/graph.h"

#define EFFECT_STAGE_LIMIT 16
#define EFFECT_PARAM_LIMIT 8
struct effect_parameter { const char *name; double minimum, maximum, value, step; };
struct effect_processor { const char *name, *label, *plugin; unsigned count; struct effect_parameter params[EFFECT_PARAM_LIMIT]; };
struct effect_stage { char id[25]; unsigned processor; double values[EFFECT_PARAM_LIMIT]; bool bypass; };
struct effect_chain { unsigned count; struct effect_stage stages[EFFECT_STAGE_LIMIT]; double wet, dry; };

const struct effect_processor *effect_processors(unsigned *count);
int effect_chain_parse(const char *spec, struct effect_chain *chain);
int effect_chain_read(const struct graph_node *node, struct effect_chain *chain);
char *effect_chain_spec(const struct effect_chain *chain);
char *effect_chain_graph(const struct effect_chain *chain);
int effect_chain_add(struct effect_chain *chain, const char *processor, unsigned position);
bool effect_chain_control(const struct effect_chain *chain, const char *name,
                          double *minimum, double *maximum, double *def);
bool effect_chain_stage_control(const struct effect_stage *stage, const char *name);
void effect_chain_print(const struct effect_chain *chain, bool json);
