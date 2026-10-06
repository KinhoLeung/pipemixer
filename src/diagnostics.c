#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <pipewire/extensions/profiler.h>
#include <spa/param/profiler.h>
#include <spa/pod/parser.h>
#include <spa/pod/iter.h>
#include <spa/param/audio/raw-types.h>
#include "diagnostics.h"
#include "pw/common.h"
#include "pw/graph.h"
#include "pw/peak.h"
#include "eventloop.h"
#include "utils.h"

struct process_sample { int pid; uint64_t ticks; };
static void node_count(const struct graph_node *n,void *data);
static void link_count(const struct graph_link *l,void *data);
struct private {
    struct event_hook *hook;
    struct pw_core *core;
    struct pw_registry *registry;
    struct pw_profiler *profiler;
    struct spa_hook registry_hook, profiler_hook, core_hook;
    uint32_t profiler_id;
    uint64_t last_ns, cpu_ticks, idle_ticks, self_ticks;
    struct process_sample processes[64]; unsigned n_processes;
    char last_error[192];
};
uint64_t diagnostics_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void event(struct diagnostics *d,const char *message) {
    struct private *p=d->private; if (!strcmp(p->last_error,message)) return;
    for (unsigned i=0;i<d->n_events;i++) if (!strcmp(d->events[i].message,message)) return;
    snprintf(p->last_error,sizeof(p->last_error),"%s",message);
    if(d->n_events==DIAGNOSTIC_EVENTS){memmove(d->events,d->events+1,(DIAGNOSTIC_EVENTS-1)*sizeof(d->events[0]));d->n_events--;}
    struct diagnostic_event *e=&d->events[d->n_events++];e->time=diagnostics_now_ns()/1e9;snprintf(e->message,sizeof(e->message),"%s",message);
}
static struct diagnostic_node *slot(struct diagnostics *d,uint32_t id) {
    for(unsigned i=0;i<DIAGNOSTIC_NODES;i++)if(d->nodes[i].used&&d->nodes[i].id==id)return &d->nodes[i];
    for(unsigned i=0;i<DIAGNOSTIC_NODES;i++)if(!d->nodes[i].used){d->nodes[i]=(struct diagnostic_node){.used=true,.id=id};return &d->nodes[i];}
    return NULL;
}
static void block(struct diagnostics *d,const struct spa_pod *pod,uint32_t driver,uint32_t rate,uint32_t quantum,int fallback_xruns) {
    int32_t id,status,xruns=-1; const char *name; int64_t prev,signal,awake,finish;struct spa_fraction latency;
    if(spa_pod_parse_struct(pod,SPA_POD_Int(&id),SPA_POD_String(&name),SPA_POD_Long(&prev),SPA_POD_Long(&signal),SPA_POD_Long(&awake),SPA_POD_Long(&finish),SPA_POD_Int(&status),SPA_POD_Fraction(&latency),SPA_POD_OPT_Int(&xruns))<0)return;
    struct diagnostic_node *n=slot(d,id);if(!n)return;
    unsigned old=n->xruns;bool was=n->xrun_known;
    n->driver=driver==PW_ID_ANY?(uint32_t)id:driver;n->rate=rate;n->quantum=quantum;
    n->cycle_ms=rate?1000.0*quantum/rate:0;n->wait_us=awake>=signal?(awake-signal)/1000.0:0;n->busy_us=finish>=awake?(finish-awake)/1000.0:0;
    n->load_percent=n->cycle_ms?n->busy_us/(n->cycle_ms*10):0;n->measured_ns=diagnostics_now_ns();
    if(xruns<0)xruns=fallback_xruns;
    n->xrun_known=xruns>=0;n->xruns=xruns>=0?(unsigned)xruns:0;
    if(was&&n->xrun_known&&n->xruns>old){char message[192];snprintf(message,sizeof(message),"xrun: node %u (%s), +%u",n->id,name,n->xruns-old);event(d,message);}
}
static void profile(void *data,const struct spa_pod *pod) {
    struct diagnostics *d=data;struct spa_pod *o;
    SPA_POD_STRUCT_FOREACH(pod,o){
        if(!spa_pod_is_object_type(o,SPA_TYPE_OBJECT_Profiler))continue;
        struct spa_pod_prop *p;uint32_t rate=0,quantum=0,driver=PW_ID_ANY;int32_t xruns=-1;
        SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)o,p){
            if(p->key==SPA_PROFILER_info){int64_t counter;float fast,medium,slow;spa_pod_parse_struct(&p->value,SPA_POD_Long(&counter),SPA_POD_Float(&fast),SPA_POD_Float(&medium),SPA_POD_Float(&slow),SPA_POD_OPT_Int(&xruns));}
            if(p->key==SPA_PROFILER_clock){int32_t flags,id;const char *name;int64_t nsec,pos,duration,delay;struct spa_fraction frac;double diff;
                if(spa_pod_parse_struct(&p->value,SPA_POD_Int(&flags),SPA_POD_Int(&id),SPA_POD_String(&name),SPA_POD_Long(&nsec),SPA_POD_Fraction(&frac),SPA_POD_Long(&pos),SPA_POD_Long(&duration),SPA_POD_Long(&delay),SPA_POD_Double(&diff))>=0){rate=frac.num?frac.denom/frac.num:0;quantum=duration;}}
            if(p->key==SPA_PROFILER_driverBlock){int32_t id;const char *name; if(spa_pod_parse_struct(&p->value,SPA_POD_Int(&id),SPA_POD_String(&name))>=0)driver=id;}
        }
        SPA_POD_OBJECT_FOREACH((struct spa_pod_object*)o,p)if(p->key==SPA_PROFILER_driverBlock||p->key==SPA_PROFILER_followerBlock)block(d,&p->value,driver,rate,quantum,xruns);
    }
}
static const struct pw_profiler_events profiler_events={.version=PW_VERSION_PROFILER_EVENTS,.profile=profile};
static void global(void *data,uint32_t id,uint32_t permissions,const char *type,uint32_t version,const struct spa_dict *props){
    struct diagnostics *d=data;struct private *p=d->private;
    if(!strcmp(type,PW_TYPE_INTERFACE_Profiler)&&!p->profiler){p->profiler=pw_registry_bind(p->registry,id,type,PW_VERSION_PROFILER,0);if(p->profiler){p->profiler_id=id;pw_profiler_add_listener(p->profiler,&p->profiler_hook,&profiler_events,d);d->profiler_available=true;}}
}
static void removed(void *data,uint32_t id){struct diagnostics *d=data;struct private *p=d->private;
    for(unsigned i=0;i<DIAGNOSTIC_NODES;i++)if(d->nodes[i].used&&d->nodes[i].id==id)d->nodes[i].used=false;
    if(p->profiler&&id==p->profiler_id){spa_hook_remove(&p->profiler_hook);pw_proxy_destroy((struct pw_proxy*)p->profiler);p->profiler=NULL;d->profiler_available=false;event(d,"PipeWire profiler removed");}}
static const struct pw_registry_events registry_events={.version=PW_VERSION_REGISTRY_EVENTS,.global=global,.global_remove=removed};
static void core_error(void *data,uint32_t id,int seq,int res,const char *message){struct diagnostics *d=data;event(d,message?message:"PipeWire error");if(id==PW_ID_CORE&&(res==-EPIPE||res==-ECONNRESET))d->failed=true;}
static const struct pw_core_events core_events={.version=PW_VERSION_CORE_EVENTS,.error=core_error};
static void graph_changed(void *data){struct diagnostics *d=data;d->node_count=d->link_count=d->error_count=0;graph_foreach_node(node_count,d);graph_foreach_link(link_count,d);}
static void server_error(int code,const char *message,void *data){struct diagnostics *d=data;event(d,message?:"PipeWire error");if(code==-EPIPE||code==-ECONNRESET)d->failed=true;}
static const struct pipewire_events graph_events={.graph=graph_changed,.error=server_error};
struct diagnostics *diagnostics_create(void){
    static bool protocol_loaded;
    if (!protocol_loaded) protocol_loaded = pw_context_load_module(pipewire_context(), "libpipewire-module-profiler", NULL, NULL) != NULL;
    struct diagnostics *d=calloc(1,sizeof(*d));struct private *p=calloc(1,sizeof(*p));if(!d||!p){free(d);free(p);return NULL;}d->private=p;
    p->core=pw_context_connect(pipewire_context(),pw_properties_new(PW_KEY_APP_NAME,"PipeMixer diagnostics",NULL),0);
    if(!p->core){free(p);free(d);return NULL;}pw_core_add_listener(p->core,&p->core_hook,&core_events,d);
    p->registry=pw_core_get_registry(p->core,PW_VERSION_REGISTRY,0);if(!p->registry){diagnostics_destroy(d);return NULL;}
    pw_registry_add_listener(p->registry,&p->registry_hook,&registry_events,d);p->hook=pipewire_add_listener(&graph_events,d);diagnostics_sample(d);return d;
}
void diagnostics_destroy(struct diagnostics *d){if(!d)return;struct private *p=d->private;
    if(p->hook)event_hook_release(p->hook);
    if(p->profiler){spa_hook_remove(&p->profiler_hook);pw_proxy_destroy((struct pw_proxy*)p->profiler);}
    if(p->registry){spa_hook_remove(&p->registry_hook);pw_proxy_destroy((struct pw_proxy*)p->registry);}
    if(p->core){spa_hook_remove(&p->core_hook);pw_core_disconnect(p->core);}free(p);free(d);
}
static uint64_t proc_ticks(int pid,uint64_t *rss){char path[64],buffer[2048];snprintf(path,sizeof(path),"/proc/%d/stat",pid);FILE *f=fopen(path,"r");if(!f)return 0;
    char *r=fgets(buffer,sizeof(buffer),f);fclose(f);if(!r)return 0;char *end=strrchr(buffer,')');if(!end)return 0;char *save,*token=strtok_r(end+2," ",&save);unsigned field=3;uint64_t ticks=0;
    while(token){if(field==14||field==15)ticks+=strtoull(token,NULL,10);if(field==24&&rss)*rss=strtoull(token,NULL,10)*sysconf(_SC_PAGESIZE)/1024;token=strtok_r(NULL," ",&save);field++;}return ticks;
}
static void node_count(const struct graph_node *n,void *data){struct diagnostics *d=data;if(!graph_node_is_audio(n))return;d->node_count++;if(n->state==PW_NODE_STATE_ERROR){d->error_count++;char text[192];snprintf(text,sizeof(text),"node %u: %s",n->id,n->error?:"error");event(d,text);}}
static void link_count(const struct graph_link *l,void *data){struct diagnostics *d=data;if(!graph_link_is_audio(l))return;d->link_count++;if(l->info&&l->info->state==PW_LINK_STATE_ERROR){d->error_count++;char text[192];snprintf(text,sizeof(text),"link %u: %s",l->id,l->info->error?:"error");event(d,text);}}
void diagnostics_sample(struct diagnostics *d){if(!d)return;struct private *p=d->private;uint64_t now=diagnostics_now_ns(),total=0,idle=0,v[10]={0};FILE *f=fopen("/proc/stat","r");
    if(p->last_ns&&now-p->last_ns<100000000){if(f)fclose(f);graph_changed(d);return;}
    if(f){if(fscanf(f,"cpu %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64,v,v+1,v+2,v+3,v+4,v+5,v+6,v+7)==8){for(unsigned i=0;i<8;i++)total+=v[i];idle=v[3]+v[4];}fclose(f);}
    double seconds=p->last_ns?(now-p->last_ns)/1e9:0;if(total>p->cpu_ticks&&p->last_ns)d->cpu_percent=100.0*(total-p->cpu_ticks-(idle-p->idle_ticks))/(total-p->cpu_ticks);
    uint64_t self=proc_ticks(getpid(),&d->rss_kb);if(seconds>0&&self>=p->self_ticks)d->self_cpu_percent=100.0*(self-p->self_ticks)/sysconf(_SC_CLK_TCK)/seconds;
    struct process_sample next[64];unsigned count=0;uint64_t delta=0;DIR *dir=opendir("/proc");struct dirent *de;
    if(dir){while((de=readdir(dir))&&count<64){int pid=atoi(de->d_name);if(!pid)continue;char path[64],name[64];snprintf(path,sizeof(path),"/proc/%d/comm",pid);f=fopen(path,"r");if(!f)continue;name[0]=0;if (!fgets(name,sizeof(name),f)) name[0]=0;fclose(f);
        if(strcmp(name,"pipewire\n")&&strcmp(name,"wireplumber\n")&&strcmp(name,"pipemixer\n"))continue;
        uint64_t ticks=proc_ticks(pid,NULL);next[count++]=(struct process_sample){pid,ticks};for(unsigned i=0;i<p->n_processes;i++)if(p->processes[i].pid==pid&&ticks>=p->processes[i].ticks)delta+=ticks-p->processes[i].ticks;
    }closedir(dir);}if(seconds>0)d->audio_cpu_percent=100.0*delta/sysconf(_SC_CLK_TCK)/seconds;memcpy(p->processes,next,count*sizeof(next[0]));p->n_processes=count;
    f=fopen("/proc/meminfo","r");if(f){char line[160];while(fgets(line,sizeof(line),f)){sscanf(line,"MemAvailable: %"SCNu64,&d->available_kb);sscanf(line,"MemTotal: %"SCNu64,&d->total_kb);}fclose(f);}
    p->last_ns=now;p->cpu_ticks=total;p->idle_ticks=idle;p->self_ticks=self;d->node_count=d->link_count=d->error_count=0;graph_foreach_node(node_count,d);graph_foreach_link(link_count,d);
}
void diagnostics_reset(struct diagnostics *d){if(d){d->n_events=0;((struct private*)d->private)->last_error[0]=0;}}
static void quoted(const char *s){char *q=json_quote(s?:"");fputs(q,stdout);free(q);}
struct print_data {struct diagnostics *d;bool json;unsigned count;};
static void print_node(const struct graph_node *n,void *data){struct print_data *p=data;if(!graph_node_is_audio(n))return;struct diagnostic_node *m=NULL;for(unsigned i=0;i<DIAGNOSTIC_NODES;i++)if(p->d->nodes[i].used&&p->d->nodes[i].id==n->id)m=&p->d->nodes[i];
    bool fresh=m&&diagnostics_now_ns()-m->measured_ns<2000000000;
    if(p->json){if(p->count++)putchar(',');printf("{\"id\":%u,\"name\":",n->id);quoted(graph_node_name(n->id));fputs(",\"state\":",stdout);quoted(pw_node_state_as_string(n->state));fputs(",\"error\":",stdout);if(n->error)quoted(n->error);else fputs("null",stdout);
        printf(",\"measured\":%s",fresh?"true":"false");if(fresh)printf(",\"driver_id\":%u,\"rate\":%u,\"quantum\":%u,\"cycle_ms\":%.6f,\"wait_us\":%.3f,\"busy_us\":%.3f,\"load_percent\":%.4f",m->driver,m->rate,m->quantum,m->cycle_ms,m->wait_us,m->busy_us,m->load_percent);
        fputs(",\"xruns\":",stdout);if(m&&m->xrun_known)printf("%u",m->xruns);else fputs("null",stdout);putchar('}');
    }else{printf("%u %s %s",n->id,pw_node_state_as_string(n->state),graph_node_name(n->id));if(fresh)printf(" | %u/%u %.2f ms wait %.1f us busy %.1f us load %.2f%% xruns %u",m->quantum,m->rate,m->cycle_ms,m->wait_us,m->busy_us,m->load_percent,m->xruns);if(n->error)printf(" | %s",n->error);putchar('\n');}}
void diagnostics_print(struct diagnostics *d,bool json){diagnostics_sample(d);if(json)printf("{\"profiler_available\":%s,\"system_cpu_percent\":%.3f,\"process_cpu_percent_one_core\":%.3f,\"audio_cpu_percent_one_core\":%.3f,\"rss_kb\":%"PRIu64",\"memory_available_kb\":%"PRIu64",\"memory_total_kb\":%"PRIu64",\"error_count\":%u,\"nodes\":[",d->profiler_available?"true":"false",d->cpu_percent,d->self_cpu_percent,d->audio_cpu_percent,d->rss_kb,d->available_kb,d->total_kb,d->error_count);
    else printf("CPU %.1f%% | audio processes %.1f%% of one core | self %.1f%% RSS %"PRIu64" KiB | available %"PRIu64" KiB | profiler %s | %u nodes %u links %u errors\n",d->cpu_percent,d->audio_cpu_percent,d->self_cpu_percent,d->rss_kb,d->available_kb,d->profiler_available?"available":"unavailable",d->node_count,d->link_count,d->error_count);
    struct print_data p={d,json,0};graph_foreach_node(print_node,&p);if(json)fputs("],\"events\":[",stdout);for(unsigned i=0;i<d->n_events;i++){if(json){if(i)putchar(',');printf("{\"monotonic_seconds\":%.3f,\"message\":",d->events[i].time);quoted(d->events[i].message);putchar('}');}else printf("event %.3f %s\n",d->events[i].time,d->events[i].message);}if(json)fputs("]}\n",stdout);
}
struct cli_meter { struct diagnostics *d;struct peak_meter *meter;const char *target;uint64_t start,end,deadline;unsigned duration;bool json;int result;};
static void print_meter(struct cli_meter *c){struct meter_snapshot s;peak_meter_snapshot(c->meter,&s,true);if(c->json){fputs("{\"target\":",stdout);quoted(c->target);printf(",\"rate\":%u,\"frames\":%"PRIu64",\"active\":%s,\"channels\":[",s.rate,s.frames,s.active?"true":"false");}
    for(unsigned i=0;i<s.channels;i++){struct meter_channel *m=&s.channel[i];const char *name=spa_type_audio_channel_to_short_name(s.positions[i]);if(c->json){if(i)putchar(',');fputs("{\"name\":",stdout);quoted(name);printf(",\"peak\":%.9g,\"rms\":%.9g,\"hold\":%.9g,\"clipped_samples\":%"PRIu64",\"invalid_samples\":%"PRIu64"}",m->peak,m->rms,m->hold,m->clipped,m->invalid);}else printf("%s peak %.6f (%.2f dBFS) RMS %.6f (%.2f dBFS) hold %.6f clips %"PRIu64" invalid %"PRIu64"\n",name?:"?",m->peak,m->peak>0?20*log10(m->peak):-120,m->rms,m->rms>0?20*log10(m->rms):-120,m->hold,m->clipped,m->invalid);}
    if(c->json)fputs("]}\n",stdout);
}
static void cli_tick(void *data,uint64_t count){struct cli_meter *c=data;uint64_t now=diagnostics_now_ns();
    if(c->target&&!c->meter){uint32_t id;if(graph_resolve_node(c->target,&id)==0){const struct graph_node *n=graph_node_find(id);const char *cls=dict_get(&n->props,PW_KEY_MEDIA_CLASS);c->meter=peak_meter_create(dict_get(&n->props,PW_KEY_OBJECT_SERIAL),!strcmp(cls?:"","Audio/Sink")||!strcmp(cls?:"","Stream/Input/Audio"),false);if(!c->meter){c->result=1;pw_main_loop_quit(main_loop);return;}}}
    if(c->meter){peak_meter_step(c->meter);struct meter_snapshot s;peak_meter_snapshot(c->meter,&s,true);if(s.failed){c->result=1;pw_main_loop_quit(main_loop);return;}if(s.frames&&!c->end)c->end=now+(uint64_t)c->duration*1000000;}
    if(!c->target&&!c->end)c->end=now+(uint64_t)c->duration*1000000;
    if(c->end&&now>=c->end){if(c->target)print_meter(c);else diagnostics_print(c->d,c->json);c->result=0;pw_main_loop_quit(main_loop);}
    else if(now>=c->deadline||c->d->failed){fprintf(stderr,"pipemixer: monitoring timed out or source unavailable\n");c->result=c->meter?1:3;pw_main_loop_quit(main_loop);}
}
static void cli_signal(void *data,int sig){struct cli_meter *c=data;c->result=1;pw_main_loop_quit(main_loop);}
int diagnostics_cli(const char *target,unsigned duration,bool json,unsigned timeout){struct cli_meter c={.target=target,.duration=duration,.json=json,.result=1,.deadline=diagnostics_now_ns()+(uint64_t)timeout*1000000};c.d=diagnostics_create();if(!c.d)return 1;
    struct spa_source *timer=pw_loop_add_timer(event_loop,cli_tick,&c),*s1=pw_loop_add_signal(event_loop,SIGINT,cli_signal,&c),*s2=pw_loop_add_signal(event_loop,SIGTERM,cli_signal,&c);struct timespec interval={.tv_nsec=50000000};if(timer)pw_loop_update_timer(event_loop,timer,&interval,&interval,false);if(timer)pw_main_loop_run(main_loop);
    if(s1)pw_loop_destroy_source(event_loop,s1);
    if(s2)pw_loop_destroy_source(event_loop,s2);
    if(timer)pw_loop_destroy_source(event_loop,timer);
    peak_meter_destroy(c.meter);diagnostics_destroy(c.d);return c.result;
}
