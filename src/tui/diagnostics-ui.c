#include "i18n.h"
#include <math.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <spa/param/audio/raw-types.h>
#include "tui/tui.h"
#include "tui/routing.h"
#include "tui/diagnostics-ui.h"
#include "diagnostics.h"
#include "pw/graph.h"
#include "pw/peak.h"
#include "pw/node.h"
#include "utils.h"
static struct diagnostics *state;
static struct peak_meter *meter;
static char target[512];
static char target_serial[32];
static uint64_t last;
static char rows[128][256];static unsigned n_rows;
static void row(const char *format,...){if(n_rows==128)return;va_list ap;va_start(ap,format);vsnprintf(rows[n_rows++],256,tr(format),ap);va_end(ap);}
void diagnostic_ui_cancel(void){peak_meter_destroy(meter);meter=NULL;diagnostics_destroy(state);state=NULL;target[0]=target_serial[0]=0;}
static void reset(struct tui_menu *m,struct tui_menu_item *i){peak_meter_reset(meter);diagnostics_reset(state);tui_notice(tr("Meter overloads, peak hold and recent events reset"));}
static double db(float x){return x>0?20*log10(x):-120;}
static void node_row(const struct graph_node *node,void *data){if(!graph_node_is_audio(node))return;
    const struct diagnostic_node *n=NULL;for(unsigned i=0;i<DIAGNOSTIC_NODES;i++)if(state->nodes[i].used&&state->nodes[i].id==node->id)n=&state->nodes[i];
    if(n&&diagnostics_now_ns()-n->measured_ns<2000000000)row(tr("%u %s %u/%u %.2f ms | wait %.1fus busy %.1fus load %.1f%% xrun %u | %s"),node->id,tr(pw_node_state_as_string(node->state)),n->quantum,n->rate,n->cycle_ms,n->wait_us,n->busy_us,n->load_percent,n->xruns,graph_node_name(node->id));
    else row(tr("%u %s | profiler: no current cycle | %s%s%s"),node->id,tr(pw_node_state_as_string(node->state)),graph_node_name(node->id),node->error?" | ":"",node->error?:"");
}
bool diagnostic_ui_poll(void){if(!state||!tui.menu_active)return false;if(meter)peak_meter_step(meter);uint64_t now=diagnostics_now_ns();if(now-last<250000000)return false;last=now;diagnostics_sample(state);n_rows=0;
    row("CPU %.1f%% | audio %.1f%% of one core | self %.1f%% | RSS %llu KiB",state->cpu_percent,state->audio_cpu_percent,state->self_cpu_percent,(unsigned long long)state->rss_kb);
    row("Memory available %llu / %llu KiB | profiler %s | %u nodes %u links %u errors",(unsigned long long)state->available_kb,(unsigned long long)state->total_kb,tr(state->profiler_available?"available":"unavailable"),state->node_count,state->link_count,state->error_count);
    if(*target){uint32_t id;bool exists=graph_resolve_node(target,&id)==0;
        const struct graph_node *live=exists?graph_node_find(id):NULL;
        const char *serial=live?dict_get(&live->props,PW_KEY_OBJECT_SERIAL):NULL;
        if(!exists){peak_meter_destroy(meter);meter=NULL;target_serial[0]=0;}
        else if(!meter || !streq(target_serial,serial)){
            peak_meter_destroy(meter);const char *cls=dict_get(&live->props,PW_KEY_MEDIA_CLASS);
            meter=peak_meter_create(serial,!strcmp(cls?:"","Audio/Sink")||!strcmp(cls?:"","Stream/Input/Audio"),false);
            snprintf(target_serial,sizeof(target_serial),"%s",serial?:"");
        }
        row(tr("Meter: %s%s"),target,exists?"":tr(" [source missing]"));struct meter_snapshot s;peak_meter_snapshot(meter,&s,false);
        for(unsigned i=0;i<s.channels;i++)row("%s peak %.1f dBFS RMS %.1f dBFS hold %.1f dBFS | clips %llu invalid %llu %s",spa_type_audio_channel_to_short_name(s.positions[i])?:"?",db(s.channel[i].peak),db(s.channel[i].rms),db(s.channel[i].hold),(unsigned long long)s.channel[i].clipped,(unsigned long long)s.channel[i].invalid,s.active?"":tr("[no samples]"));
        if(!s.channels)row(tr("Meter: %s"),s.failed?tr("unavailable"):tr("waiting for audio"));}
    row(tr("PipeWire nodes: quantum/rate, scheduling wait, processing time, DSP load, server xrun count"));graph_foreach_node(node_row,NULL);
    row(tr("Recent anomalies (Enter resets meter counters and events)"));if(!state->n_events)row(tr("No anomalies observed"));
    unsigned first=state->n_events>8?state->n_events-8:0;for(unsigned i=first;i<state->n_events;i++)row("%.3f %s",state->events[i].time,state->events[i].message);
    if(!tui.menu||tui.menu->n_items!=n_rows){unsigned selection=tui.menu?tui.menu->selected:0;if(tui.menu)tui_menu_free(tui.menu);tui.menu=tui_menu_create(n_rows);tui.menu->callback=reset;tui.menu->selected=selection<n_rows?selection:n_rows-1;tui_menu_resize(tui.menu,tui.term_width,tui.term_height);}
    wstring_clear(&tui.menu->header);
    wstring_printf(&tui.menu->header,trw(L"Diagnostics | Enter: reset counters  Esc: close"));for(unsigned i=0;i<n_rows;i++){wstring_clear(&tui.menu->items[i].wstr);wstring_printf(&tui.menu->items[i].wstr,L"%s",rows[i]);}return true;
}
void tui_bind_diagnostics(union tui_bind_data data){tui_bind_cancel_selection((union tui_bind_data){0});state=diagnostics_create();if(!state){tui_notice(tr("Cannot start diagnostics"));return;}
    uint32_t id=tui.routing_active?routing_selected_node(false):PW_ID_ANY;struct tui_tab_item *item=tui.tabs[tui.tab_index].focused;if(!tui.routing_active&&item&&item->type==TUI_TAB_ITEM_TYPE_NODE)id=item->as.node.id;
    const struct graph_node *node=graph_node_find(id);if(node)snprintf(target,sizeof(target),"%s",graph_node_name(id));
    tui.menu_active=true;last=0;diagnostic_ui_poll();
}
