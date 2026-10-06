#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pipewire/pipewire.h>

#include "pw/effects.h"
#include "pw/effect-chain.h"
#include "utils.h"
#include "xmalloc.h"

extern const char eq_config[], voice_config[];

char *effect_graph_load_chain(const char *preset, char **chain_spec) {
    *chain_spec = NULL;
    if (streq(preset, "empty")) {
        struct effect_chain chain = {.wet = 1};
        *chain_spec = effect_chain_spec(&chain); return effect_chain_graph(&chain);
    }
    const char *config = streq(preset, "eq") ? eq_config : streq(preset, "voice") ? voice_config : NULL;
    char *owned = NULL;
    if (!config) {
        if (!preset || preset[0] != '@' || !preset[1]) { errno = EINVAL; return NULL; }
        FILE *file = fopen(preset + 1, "rb");
        if (!file) return NULL;
        const size_t limit = 1024 * 1024;
        owned = xmalloc(limit + 1);
        size_t count = fread(owned, 1, limit, file);
        bool invalid = ferror(file) || (count == limit && fgetc(file) != EOF) || memchr(owned, '\0', count);
        fclose(file);
        if (invalid) { free(owned); errno = EINVAL; return NULL; }
        owned[count] = '\0'; config = owned;
    }
    struct pw_properties *props = pw_properties_new(NULL, NULL);
    if (!props) { free(owned); errno = ENOMEM; return NULL; }
    struct spa_error_location location;
    int result = pw_properties_update_string_checked(props, config, strlen(config), &location);
    const char *value = result >= 0 ? pw_properties_get(props, "filter.graph") : NULL;
    char *graph = value && value[0] == '{' ? xstrdup(value) : NULL;
    const char *spec = pw_properties_get(props, "pipemixer.chain");
    if (spec && graph) {
        struct effect_chain chain;
        if (effect_chain_parse(spec, &chain) < 0) { free(graph); graph = NULL; }
        else *chain_spec = effect_chain_spec(&chain);
    }
    pw_properties_free(props);
    free(owned);
    if (!graph) errno = EINVAL;
    return graph;
}
char *effect_graph_load(const char *preset) {
    char *spec, *graph = effect_graph_load_chain(preset, &spec); free(spec); return graph;
}

bool effect_control_config(const char *preset, const char *name,
                           double *minimum, double *maximum, double *default_value) {
    const char *label = strchr(name, ':');
    if (!label) return false;
    if (!streq(preset, "eq") && !streq(preset, "voice")) return true;
    label++;
    bool biquad = (streq(preset, "eq") && (strncmp(name, "bass:", 5) == 0
                   || strncmp(name, "mid:", 4) == 0 || strncmp(name, "treble:", 7) == 0))
                  || (streq(preset, "voice") && (strncmp(name, "highpass:", 9) == 0
                      || strncmp(name, "presence:", 9) == 0));
    if (biquad) {
        if (streq(label, "Freq")) {
            *minimum = 20; *maximum = 20000;
            *default_value = strncmp(name, "bass:", 5) == 0 ? 120
                             : strncmp(name, "mid:", 4) == 0 ? 1000
                             : strncmp(name, "treble:", 7) == 0 ? 6000
                             : strncmp(name, "highpass:", 9) == 0 ? 80 : 2500;
        } else if (streq(label, "Q")) {
            *minimum = .1; *default_value = strncmp(name, "bass:", 5) == 0
                             || strncmp(name, "treble:", 7) == 0
                             || strncmp(name, "highpass:", 9) == 0 ? .707 : 1;
        } else if (streq(label, "Gain") && strncmp(name, "highpass:", 9) != 0) {
            *default_value = strncmp(name, "presence:", 9) == 0 ? 2 : 0;
        } else return false;
        return true;
    }
    if (strncmp(name, "gate:", 5) == 0) {
        if (streq(label, "Open Threshold")) *default_value = .006;
        else if (streq(label, "Close Threshold")) *default_value = .003;
        return true;
    }
    if (streq(name, "wet:Mult") || streq(name, "dry:Mult")) {
        *minimum = 0; *maximum = 1;
        *default_value = streq(name, "wet:Mult") ? 1 : 0;
        return true;
    }
    return false;
}
