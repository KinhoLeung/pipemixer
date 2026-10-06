#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spa/utils/json.h>
#include "pw/effect-chain.h"
#include "utils.h"
#include "xmalloc.h"

static const struct effect_processor processors[] = {
    {"lowshelf", "Low shelf EQ", "bq_lowshelf", 3, {{"Freq",20,20000,120,0}, {"Q",.1,10,.707,.1}, {"Gain",-120,20,0,.5}}},
    {"peaking", "Parametric EQ", "bq_peaking", 3, {{"Freq",20,20000,1000,0}, {"Q",.1,10,1,.1}, {"Gain",-120,20,0,.5}}},
    {"highshelf", "High shelf EQ", "bq_highshelf", 3, {{"Freq",20,20000,6000,0}, {"Q",.1,10,.707,.1}, {"Gain",-120,20,0,.5}}},
    {"highpass", "High-pass filter", "bq_highpass", 2, {{"Freq",20,20000,80,0}, {"Q",.1,10,.707,.1}}},
    {"lowpass", "Low-pass filter", "bq_lowpass", 2, {{"Freq",20,20000,12000,0}, {"Q",.1,10,.707,.1}}},
    {"gain", "Gain", "linear", 1, {{"Mult",0,4,1,.05}}},
    {"gate", "Noise gate", "noisegate", 2, {{"Open Threshold",0,1,.006,.001}, {"Close Threshold",0,1,.003,.001}}},
    {"compressor", "Compressor", "pm_compressor", 7, {{"Threshold dB",-60,0,-18,1},{"Ratio",1,20,4,.5},{"Attack ms",.1,200,10,1},{"Release ms",10,2000,150,10},{"Knee dB",0,24,6,1},{"Makeup dB",0,24,0,.5},{"Mix",0,1,1,.05}}},
    {"limiter", "Peak limiter", "pm_limiter", 3, {{"Ceiling dB",-24,0,-1,.5},{"Release ms",10,2000,100,10},{"Input dB",0,24,0,.5}}},
};
const struct effect_processor *effect_processors(unsigned *count) { *count = SPA_N_ELEMENTS(processors); return processors; }
static int processor_index(const char *name) {
    for (unsigned i = 0; i < SPA_N_ELEMENTS(processors); i++) if (streq(name, processors[i].name)) return i;
    return -1;
}
static bool valid_id(const char *id) {
    if (!id || !*id || strlen(id) > 24) return false;
    for (const char *p = id; *p; p++) if ((*p < 'a' || *p > 'z') && (*p < '0' || *p > '9') && *p != '_') return false;
    return !streq(id, "input") && !streq(id, "wet") && !streq(id, "dry") && !streq(id, "mix") && strncmp(id, "pm_", 3);
}
static void stage_defaults(struct effect_stage *stage) {
    const struct effect_processor *p = &processors[stage->processor];
    for (unsigned i = 0; i < p->count; i++) stage->values[i] = p->params[i].value;
    if (streq(stage->id, "presence") && streq(p->name, "peaking")) { stage->values[0] = 2500; stage->values[2] = 2; }
}
int effect_chain_parse(const char *spec, struct effect_chain *chain) {
    *chain = (struct effect_chain){.wet = 1};
    if (!spec || strlen(spec) > 4096) return -EINVAL;
    struct spa_json root, array; spa_json_init(&root, spec, strlen(spec));
    if (spa_json_enter_array(&root, &array) <= 0) return -EINVAL;
    const char *value; int length;
    while ((length = spa_json_next(&array, &value)) > 0) {
        if (!spa_json_is_object(value, length) || chain->count == EFFECT_STAGE_LIMIT) return -EINVAL;
        length = spa_json_container_len(&array, value, length);
        struct pw_properties *props = pw_properties_new(NULL, NULL); struct spa_error_location location;
        int result = pw_properties_update_string_checked(props, value, length, &location);
        const char *id = pw_properties_get(props, "id"), *type = pw_properties_get(props, "type");
        int index = processor_index(type);
        if (result < 0 || props->dict.n_items != 2 || !valid_id(id) || index < 0) { pw_properties_free(props); return -EINVAL; }
        for (unsigned i = 0; i < chain->count; i++) if (streq(chain->stages[i].id, id)) { pw_properties_free(props); return -EINVAL; }
        struct effect_stage *stage = &chain->stages[chain->count++];
        snprintf(stage->id, sizeof(stage->id), "%s", id); stage->processor = index; stage_defaults(stage);
        pw_properties_free(props);
    }
    return length < 0 || spa_json_next(&root, &value) > 0 ? -EINVAL : 0;
}
int effect_chain_read(const struct graph_node *node, struct effect_chain *chain) {
    if (!node) return -ENOENT;
    const char *spec = dict_get(&node->props, "pipemixer.chain");
    if (!spec) {
        const char *preset = dict_get(&node->props, "pipemixer.preset");
        spec = streq(preset,"eq") ? "[{id=bass type=lowshelf},{id=mid type=peaking},{id=treble type=highshelf}]"
            : streq(preset,"voice") ? "[{id=highpass type=highpass},{id=gate type=gate},{id=presence type=peaking}]" : NULL;
        if (!spec) return -ENOTSUP;
    }
    int result = effect_chain_parse(spec, chain); if (result < 0) return result;
    if (!node->controls_ready) return -EAGAIN;
    for (unsigned i = 0; i < chain->count; i++) {
        struct effect_stage *s = &chain->stages[i]; const struct effect_processor *p = &processors[s->processor];
        for (unsigned j = 0; j < p->count; j++) {
            char name[96]; snprintf(name,sizeof(name),"%s:%s",s->id,p->params[j].name);
            const struct graph_control *c = graph_control_find(node,name);
            if (!c || !c->has_value) return -EAGAIN;
            s->values[j] = c->value;
        }
        char name[96]; snprintf(name,sizeof(name),"pm_%s_wet:Mult",s->id);
        const struct graph_control *c = graph_control_find(node,name); s->bypass = c && c->has_value && c->value == 0;
    }
    const struct graph_control *wet = graph_control_find(node,"wet:Mult"), *dry = graph_control_find(node,"dry:Mult");
    if (wet && wet->has_value) chain->wet = wet->value;
    if (dry && dry->has_value) chain->dry = dry->value;
    return 0;
}
char *effect_chain_spec(const struct effect_chain *chain) {
    char *text = NULL; size_t size; FILE *f = open_memstream(&text,&size); if (!f) return NULL;
    fputc('[',f);
    for (unsigned i = 0; i < chain->count; i++) fprintf(f,"%s{\"id\":\"%s\",\"type\":\"%s\"}", i ? "," : "",chain->stages[i].id,processors[chain->stages[i].processor].name);
    fputc(']',f); fclose(f); return text;
}
static void link_write(FILE *f, const char *out, const char *in) { fprintf(f,"{output=\"%s\" input=\"%s\"}\n",out,in); }
char *effect_chain_graph(const struct effect_chain *chain) {
    char *text = NULL; size_t size; FILE *f = open_memstream(&text,&size); if (!f) return NULL;
    fputs("{nodes=[{type=builtin name=input label=copy}\n",f);
    for (unsigned i = 0; i < chain->count; i++) {
        const struct effect_stage *s = &chain->stages[i]; const struct effect_processor *p = &processors[s->processor];
        bool dynamics=strncmp(p->plugin,"pm_",3)==0;
        fprintf(f,"{type=%s %sname=%s label=%s control={",dynamics?"ladspa":"builtin",dynamics?"plugin=pipemixer-dynamics ":"",s->id,p->plugin);
        for (unsigned j = 0; j < p->count; j++) fprintf(f,"\"%s\"=%.9g ",p->params[j].name,s->values[j]);
        if (streq(p->name,"gain")) fputs("Add=0 ",f);
        fputs("}}\n",f);
        if (streq(p->name,"gate")) fprintf(f,"{type=builtin name=pm_%s_level label=log control={M2=0}}\n",s->id);
        fprintf(f,"{type=builtin name=pm_%s_wet label=linear control={Mult=%d Add=0}}\n"
                  "{type=builtin name=pm_%s_dry label=linear control={Mult=%d Add=0}}\n"
                  "{type=builtin name=pm_%s_mix label=mixer control={\"Gain 1\"=1 \"Gain 2\"=1}}\n",s->id,!s->bypass,s->id,s->bypass,s->id);
    }
    fprintf(f,"{type=builtin name=wet label=linear control={Mult=%.9g Add=0}}\n"
              "{type=builtin name=dry label=linear control={Mult=%.9g Add=0}}\n"
              "{type=builtin name=mix label=mixer control={\"Gain 1\"=1 \"Gain 2\"=1}}] links=[\n",chain->wet,chain->dry);
    char previous[96] = "input:Out", out[96], in[96];
    for (unsigned i = 0; i < chain->count; i++) {
        const struct effect_stage *s = &chain->stages[i];
        snprintf(in,sizeof(in),"%s:In",s->id); link_write(f,previous,in);
        snprintf(out,sizeof(out),"%s:Out",s->id); snprintf(in,sizeof(in),"pm_%s_wet:In",s->id); link_write(f,out,in);
        snprintf(in,sizeof(in),"pm_%s_dry:In",s->id); link_write(f,previous,in);
        snprintf(out,sizeof(out),"pm_%s_wet:Out",s->id); snprintf(in,sizeof(in),"pm_%s_mix:In 1",s->id); link_write(f,out,in);
        snprintf(out,sizeof(out),"pm_%s_dry:Out",s->id); snprintf(in,sizeof(in),"pm_%s_mix:In 2",s->id); link_write(f,out,in);
        if (streq(processors[s->processor].name,"gate")) {
            snprintf(out,sizeof(out),"pm_%s_level:Notify",s->id); snprintf(in,sizeof(in),"%s:Level",s->id); link_write(f,out,in);
        }
        snprintf(previous,sizeof(previous),"pm_%s_mix:Out",s->id);
    }
    link_write(f,previous,"wet:In"); link_write(f,"input:Out","dry:In");
    link_write(f,"wet:Out","mix:In 1"); link_write(f,"dry:Out","mix:In 2");
    fputs("] inputs=[\"input:In\"] outputs=[\"mix:Out\"]}",f); fclose(f); return text;
}
int effect_chain_add(struct effect_chain *chain, const char *processor, unsigned position) {
    int index = processor_index(processor);
    if (index < 0 || position > chain->count) return -EINVAL;
    if (chain->count == EFFECT_STAGE_LIMIT) return -ENOSPC;
    struct effect_stage stage = {.processor = index};
    for (unsigned n = 1;; n++) {
        snprintf(stage.id,sizeof(stage.id),"%s%u",processor,n); bool exists = false;
        for (unsigned i=0;i<chain->count;i++) if (streq(chain->stages[i].id,stage.id)) exists = true;
        if (!exists) break;
    }
    stage_defaults(&stage);
    memmove(chain->stages+position+1,chain->stages+position,(chain->count-position)*sizeof(stage));
    chain->stages[position]=stage; chain->count++; return 0;
}
bool effect_chain_stage_control(const struct effect_stage *stage, const char *name) {
    size_t n = strlen(stage->id); return strncmp(stage->id,name,n)==0 && name[n]==':';
}
bool effect_chain_control(const struct effect_chain *chain, const char *name, double *min, double *max, double *def) {
    if (streq(name,"wet:Mult") || streq(name,"dry:Mult")) { *min=0;*max=1;*def=streq(name,"wet:Mult");return true; }
    for (unsigned i=0;i<chain->count;i++) {
        const struct effect_stage *s=&chain->stages[i]; const struct effect_processor *p=&processors[s->processor];
        if (effect_chain_stage_control(s,name)) for (unsigned j=0;j<p->count;j++) if (streq(strchr(name,':')+1,p->params[j].name)) {
            *min=p->params[j].minimum;*max=p->params[j].maximum;*def=p->params[j].value;
            if(streq(s->id,"presence")){if(j==0)*def=2500;if(j==2)*def=2;}return true;
        }
        char wet[96],dry[96];snprintf(wet,sizeof(wet),"pm_%s_wet:Mult",s->id);snprintf(dry,sizeof(dry),"pm_%s_dry:Mult",s->id);
        if(streq(name,wet)||streq(name,dry)){*min=0;*max=1;*def=streq(name,wet);return true;}
    }
    return false;
}
void effect_chain_print(const struct effect_chain *chain, bool json) {
    if(json)fputc('[',stdout);
    for(unsigned i=0;i<chain->count;i++) {
        const struct effect_stage *s=&chain->stages[i];const struct effect_processor *p=&processors[s->processor];
        if(json)printf("%s{\"position\":%u,\"id\":\"%s\",\"type\":\"%s\",\"bypass\":%s}",i?",":"",i+1,s->id,p->name,s->bypass?"true":"false");
        else printf("%u\t%s\t%s\t%s\n",i+1,s->id,p->label,s->bypass?"bypassed":"active");
    }
    if(json)fputs("]\n",stdout);
}
