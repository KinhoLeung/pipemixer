#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spa/utils/json.h>
#include "automation.h"
#include "automation-config.h"
#include "external-control.h"
#include "scene.h"
#include "monitor.h"
#include "diagnostics.h"
#include "eventloop.h"
#include "pw/common.h"
#include "pw/graph.h"
#include "pw/managed.h"
#include "route-rules.h"
#include "utils.h"

#define MAGIC 0x504d4132u
#define PEERS 8
/* A control write also makes the session manager refresh its parameter cache.
 * Bound aggregate traffic as more controls fade, instead of multiplying the
 * 50 Hz single-control rate by the number of active jobs. */
#define FADE_WRITES_PER_TICK 1
struct engine;
struct fade {
    bool used, active, parameter, pending, final, smooth;
    uint32_t id;
    char target[512], control[128], serial[32], reason[96], owner[49];
    unsigned channels, duration;
    uint64_t started, sent_at;
    double start, end, verified, sent, progress;
    float from[SPA_AUDIO_MAX_CHANNELS], to[SPA_AUDIO_MAX_CHANNELS];
    float confirmed[SPA_AUDIO_MAX_CHANNELS], written[SPA_AUDIO_MAX_CHANNELS];
    struct node *node;
    struct event_hook *hook;
};
struct peer { struct engine *engine; int fd, sync_seq; bool waiting; uint64_t opened; struct spa_source *io; };
struct batch {
    bool used,waiting_fade,pending_write,write_acked;
    int write_seq;
    char owner[49], fade_target[512], fade_parameter[128];
    struct auto_action actions[AUTOMATION_ACTION_LIMIT];
    unsigned count,index;
    uint64_t order,due,action_started;
    double input;
};
struct engine {
    struct fade jobs[AUTOMATION_JOB_LIMIT];
    struct peer peers[PEERS];
    struct auto_config *config;
    struct external_control *external;
    struct batch batches[16];
    struct scene_job *scene_job;
    char config_error[256];
    uint64_t started,last_rules,last_config,order,scene_order,scene_blocked;
    struct stat config_stat;
    bool config_seen;
    unsigned queued, fade_cursor;
    int server, lock, ready_fd, sync_seq, rounds, status;
    bool ready, stopping;
    char path[108];
    struct spa_source *io, *timer, *signals[2];
    struct event_hook *hook;
};
static int path_for(char *path, size_t size, const char *suffix) {
    char *scope = route_rules_scope();
    if (!scope) return -ENOENT;
    int n = snprintf(path, size, "%s/pipemixer-automation-%s%s", getenv("XDG_RUNTIME_DIR") ?: "/tmp", scope, suffix);
    free(scope); return n < 0 || n >= (int)size ? -ENAMETOOLONG : 0;
}
int automation_request(const struct automation_request *request, struct automation_reply *reply, unsigned timeout) {
    memset(reply, 0, sizeof(*reply));
    char path[108]; int r = path_for(path, sizeof(path), ".sock"); if (r < 0) return r;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); if (fd < 0) return -errno;
    struct timeval tv = {.tv_sec = timeout / 1000, .tv_usec = timeout % 1000 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_un address = {.sun_family = AF_UNIX}; snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) { r = -errno; close(fd); return r; }
    struct automation_request copy = *request; copy.magic = MAGIC;
    if (send(fd, &copy, sizeof(copy), MSG_NOSIGNAL) != sizeof(copy)) r = -EIO;
    else { ssize_t got = recv(fd, reply, sizeof(*reply), MSG_TRUNC);
        r=got>=(ssize_t)offsetof(struct automation_reply,text)+1&&got<=(ssize_t)sizeof(*reply)&&memchr(reply->text,0,got-(ssize_t)offsetof(struct automation_reply,text))?reply->result:-EIO;
    }
    reply->text[sizeof(reply->text)-1] = 0; close(fd); return r;
}
int automation_take_control(uint32_t id,const char *parameter) {
    struct automation_request request={.operation=AUTOMATION_TAKEOVER};
    snprintf(request.target,sizeof(request.target),"id:%u",id);
    snprintf(request.parameter,sizeof(request.parameter),"%s",parameter?:"");
    struct automation_reply *reply=calloc(1,sizeof(*reply));if(!reply)return -ENOMEM;
    int result=automation_request(&request,reply,500);free(reply);
    return result==-ENOENT||result==-ECONNREFUSED?0:result;
}
static int wait_ready(struct automation_reply *reply, unsigned timeout) {
    uint64_t deadline = diagnostics_now_ns() + timeout * 1000000ull;
    for (;;) {
        int r = automation_request(&(struct automation_request){.operation=AUTOMATION_STATUS}, reply, 100);
        if (r < 0) return r;
        struct spa_json parser=SPA_JSON_INIT(reply->text,strlen(reply->text)),object;
        const char *token;bool ready=false;
        if(spa_json_enter_object(&parser,&object)>0){int n=spa_json_object_find(&object,"ready",&token);if(n>0)spa_json_parse_bool(token,n,&ready);}
        if (ready) return 0;
        if (diagnostics_now_ns() >= deadline) return -ETIMEDOUT;
        poll(NULL, 0, 20);
    }
}
int automation_start(unsigned timeout) {
    struct automation_reply *reply = calloc(1, sizeof(*reply)); if (!reply) return -ENOMEM;
    int r = wait_ready(reply, timeout); free(reply);
    if (!r) return 0;
    if (r != -ENOENT && r != -ECONNREFUSED) return r;
    int fds[2]; if (pipe2(fds, O_CLOEXEC) < 0) return -errno;
    pid_t pid = fork();
    if (pid < 0) { r = -errno; close(fds[0]); close(fds[1]); return r; }
    if (!pid) {
        close(fds[0]);
        if (setsid() < 0) _exit(127);
        pid_t daemon = fork(); if (daemon < 0) _exit(127); if (daemon) _exit(0);
        int ready = fcntl(fds[1], F_DUPFD, 3); if (ready < 0) _exit(127);
        close(fds[1]);
        int input = open("/dev/null", O_RDWR);
        char log[160]; path_for(log, sizeof(log), ".log");
        int output = open(log, O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_CLOEXEC, 0600);
        if (input < 0 || output < 0) _exit(127);
        dup2(input, STDIN_FILENO); dup2(output, STDOUT_FILENO); dup2(output, STDERR_FILENO);
        close(input); close(output);
        /* No PipeWire, terminal or supervisor descriptors cross into the worker. */
        long limit = sysconf(_SC_OPEN_MAX); if (limit < 0 || limit > 65536) limit = 65536;
        for (int fd=3; fd<limit; fd++) if (fd != ready) close(fd);
        char number[24]; snprintf(number, sizeof(number), "%d", ready);
        char *argv[]={"pipemixer", "__automation-worker", number, NULL};
        execv("/proc/self/exe", argv); _exit(127);
    }
    close(fds[1]); int status; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    struct pollfd poller = {.fd=fds[0], .events=POLLIN};
    int got; do { got = poll(&poller, 1, timeout); } while (got < 0 && errno == EINTR);
    unsigned char ok=0; if (got>0) { ssize_t n=read(fds[0], &ok, 1); if(n!=1)ok=0; } close(fds[0]);
    if (ok==1) return 0;
    /* A concurrent starter may have won the exclusive worker lock. */
    reply = calloc(1,sizeof(*reply)); if(!reply)return -ENOMEM;
    r=wait_ready(reply,timeout);free(reply);
    return !r ? 0 : -ETIMEDOUT;
}
static void end_fade(struct fade *f, const char *reason) {
    f->progress=fmin(1,(diagnostics_now_ns()-f->started)/(f->duration*1000000.0));
    f->active=false; f->pending=false; snprintf(f->reason,sizeof(f->reason),"%s",reason);
}
static bool close_value(double a,double b) { return fabs(a-b)<=0.00003*fmax(1,fmax(fabs(a),fabs(b))); }
static bool same_volumes(const float *a,const float *b,unsigned n) { for(unsigned i=0;i<n;i++)if(!close_value(a[i],b[i]))return false;return true; }
static void volume_changed(struct node *node,const float *volumes,unsigned count,void *data) {
    struct fade *f=data;if(!f->active)return;
    if(count!=f->channels){end_fade(f,"channel layout changed");return;}
    if(f->pending && same_volumes(volumes,f->written,count)) {
        memcpy(f->confirmed,volumes,count*sizeof(float));f->pending=false;
        if(f->final)end_fade(f,"completed");
    } else if(!same_volumes(volumes,f->confirmed,count))end_fade(f,"manual volume change");
}
static void removed(struct node *node,void *data) { struct fade *f=data;if(f->active)end_fade(f,"target disappeared"); }
static void clear_fade(struct fade *f) { event_hook_release(f->hook);if(f->node)node_unref(&f->node);memset(f,0,sizeof(*f)); }
static int new_fade(struct engine *e,const struct automation_request *r) {
    bool parameter=r->operation==AUTOMATION_FADE_PARAMETER;
    if(!*r->target || !isfinite(r->value) || r->duration_ms<20 || r->duration_ms>3600000 || r->smooth>1
        || (!parameter&&(r->value<0||r->value>150)) || (parameter&&!*r->parameter))return -EINVAL;
    uint32_t id;const struct graph_node *g=NULL;
    if(parameter){g=managed_find("effect",r->target,"input");if(!g)return -ENOENT;id=g->id;}
    else {int result=graph_resolve_node(r->target,&id);if(result<0)return result;g=graph_node_find(id);}
    struct node *node=parameter?NULL:node_lookup(id);
    const struct param_props *p=node_get_params(node);
    if(!parameter&&(!p||!p->n_channels||p->n_channels>SPA_AUDIO_MAX_CHANNELS))return -EAGAIN;
    const struct graph_control *c=parameter?graph_control_find(g,r->parameter):NULL;
    if(parameter){const char *names[]={r->parameter};double values[]={r->value};int result=graph_validate_controls(id,names,values,1);if(result<0)return result;if(!c||!c->has_value)return -EAGAIN;if(c->type!=SPA_TYPE_Float&&c->type!=SPA_TYPE_Double)return -ENOTSUP;}
    const char *target=parameter?r->target:graph_node_name(id), *serial=dict_get(&g->props,PW_KEY_OBJECT_SERIAL);
    if(!target||strlen(target)>=512||!serial)return -EINVAL;
    struct fade *slot=NULL;uint64_t oldest=UINT64_MAX;
    for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++){struct fade *f=&e->jobs[i];
        if(f->used&&f->parameter==parameter&&streq(f->target,target)&&(!parameter||streq(f->control,r->parameter))){if(f->active)end_fade(f,"superseded");slot=f;break;}
        if(!f->used){slot=f;oldest=0;}else if(!f->active&&f->started<oldest){slot=f;oldest=f->started;}
    }
    if(!slot)return -ENOSPC;
    clear_fade(slot);struct fade *f=slot;
    f->used=f->active=true;f->parameter=parameter;f->smooth=r->smooth;f->id=id;f->duration=r->duration_ms;f->started=diagnostics_now_ns();
    snprintf(f->target,sizeof(f->target),"%s",target);snprintf(f->serial,sizeof(f->serial),"%s",serial);snprintf(f->control,sizeof(f->control),"%s",r->parameter);snprintf(f->reason,sizeof(f->reason),"running");
    if(parameter){f->start=f->verified=f->sent=c->value;f->end=r->value;}
    else {f->node=node_ref(node);f->channels=p->n_channels;float maximum=0;
        for(unsigned i=0;i<f->channels;i++){if(!isfinite(p->channel_volumes[i])||p->channel_volumes[i]<0){clear_fade(f);return -EINVAL;}maximum=fmaxf(maximum,p->channel_volumes[i]);}
        for(unsigned i=0;i<f->channels;i++){f->from[i]=f->confirmed[i]=p->channel_volumes[i];f->to[i]=r->value/100*(maximum>0?p->channel_volumes[i]/maximum:1);}
        static const struct node_events ev={.volume=volume_changed,.removed=removed};f->hook=node_add_listener(node,&ev,f);
    }
    return 0;
}
static void fade_tick(struct engine *e,uint64_t now) {
    bool active=false;for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++)active|=e->jobs[i].active;if(!active)return;
    char path[256];snprintf(path,sizeof(path),"%s/pipemixer-scene.lock",getenv("XDG_RUNTIME_DIR")?:"/tmp");
    int lock=open(path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    if(lock<0||flock(lock,LOCK_SH|LOCK_NB)<0){
        /* The recovery engine briefly takes the same exclusive lock every
         * tick. Skip writes during that contention; a sustained scene edit
         * cancels fades, while changed controls already cancel via listeners. */
        bool busy=lock>=0&&(errno==EWOULDBLOCK||errno==EINTR);
        if(!e->scene_blocked)e->scene_blocked=now;
        if(!busy||now-e->scene_blocked>=100000000ull)
            for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++)if(e->jobs[i].active)end_fade(&e->jobs[i],busy?"scene operation in progress":"scene lock unavailable");
        if(lock>=0)close(lock);
        return;
    }
    e->scene_blocked=0;
    unsigned first=e->fade_cursor,writes=0;
    for(unsigned offset=0;offset<AUTOMATION_JOB_LIMIT;offset++){
        unsigned i=(first+offset)%AUTOMATION_JOB_LIMIT;
        struct fade *f=&e->jobs[i];if(!f->active)continue;
        const struct graph_node *g=graph_node_find(f->id);
        if(!g||!streq(dict_get(&g->props,PW_KEY_OBJECT_SERIAL),f->serial)){end_fade(f,"target disappeared");continue;}
        if(f->parameter){const struct graph_control *c=graph_control_find(g,f->control);
            if(!c||!c->has_value){end_fade(f,"parameter disappeared");continue;}
            if(f->pending&&close_value(c->value,f->sent)){f->verified=c->value;f->pending=false;if(f->final){end_fade(f,"completed");continue;}}
            else if(!close_value(c->value,f->verified)){end_fade(f,"manual parameter change");continue;}}
        if(f->pending){if(now-f->sent_at>2000000000ull)end_fade(f,"parameter acknowledgement timeout");continue;}
        if(writes>=FADE_WRITES_PER_TICK)continue;
        double t=fmin(1,(now-f->started)/(f->duration*1000000.0));double weight=f->smooth?t*t*(3-2*t):t;f->final=t>=1;
        int result;
        if(f->parameter){f->sent=f->final?f->end:(1-weight)*f->start+weight*f->end;
            if(close_value(f->sent,f->verified)){if(f->final)end_fade(f,"completed");continue;}
            const char *names[]={f->control};double values[]={f->sent};result=graph_set_controls(f->id,names,values,1);}
        else {for(unsigned ch=0;ch<f->channels;ch++){double a=f->from[ch],b=f->to[ch];f->written[ch]=f->final?b:cbrt((1-weight)*a*a*a+weight*b*b*b);}
            if(same_volumes(f->written,f->confirmed,f->channels)){if(f->final)end_fade(f,"completed");continue;}
            result=node_set_volumes(f->node,f->written,f->channels);}
        if(result<0){end_fade(f,strerror(-result));continue;}f->pending=true;f->sent_at=now;
        writes++;e->fade_cursor=(i+1)%AUTOMATION_JOB_LIMIT;
    }
    close(lock);
}
static struct auto_rule *find_rule(struct engine *e,const char *name) {
    if(e->config)for(unsigned i=0;i<e->config->count;i++)if(streq(e->config->rules[i].name,name))return &e->config->rules[i];
    return NULL;
}
static struct fade *find_fade(struct engine *e,const char *target,const char *parameter) {
    for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++){struct fade *f=&e->jobs[i];if(f->used&&streq(f->target,target)&&streq(f->control,parameter?:""))return f;}return NULL;
}
static void cancel_parameter(struct engine *e,uint32_t id,const char *parameter) {
    for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++){
        struct fade *f=&e->jobs[i];
        if(f->active&&f->parameter&&f->id==id&&streq(f->control,parameter))end_fade(f,"manual parameter change");
    }
}
static void finish_batch(struct engine *e,struct batch *b,int result,const char *message) {
    struct auto_rule *r=find_rule(e,b->owner);
    if(r){snprintf(r->state,sizeof(r->state),"%s",result<0?"failed":r->trigger!=TRIGGER_CONDITION?"armed":r->latched?"active":"idle");snprintf(r->error,sizeof(r->error),"%s",result<0?(message&&*message?message:strerror(-result)):"");}
    if(r&&r->trigger!=TRIGGER_CONDITION)r->latched=false;
    b->used=false;e->queued--;
}
static bool same_control(const struct auto_action *a,const struct auto_action *b) {
    bool av=a->kind==AUTO_VOLUME||a->kind==AUTO_FADE_VOLUME,bv=b->kind==AUTO_VOLUME||b->kind==AUTO_FADE_VOLUME;
    bool ap=a->kind==AUTO_PARAMETER||a->kind==AUTO_FADE_PARAMETER,bp=b->kind==AUTO_PARAMETER||b->kind==AUTO_FADE_PARAMETER;
    return streq(a->target,b->target)&&((av&&bv)||(ap&&bp&&streq(a->parameter,b->parameter))||(a->kind==b->kind&&a->kind!=AUTO_WAIT));
}
static bool suppressed(struct engine *e,const struct auto_rule *rule,const struct auto_action *actions,unsigned count) {
    if(!e->config)return false;
    for(unsigned i=0;i<e->config->count;i++){const struct auto_rule *high=&e->config->rules[i];if(high==rule)break;
        if(!high->enabled||!high->latched||high->priority<=rule->priority)continue;
        for(unsigned a=0;a<count;a++)for(unsigned b=0;b<high->n_actions;b++)if(same_control(&actions[a],&high->actions[b]))return true;
    }return false;
}
static int enqueue(struct engine *e,struct auto_rule *r,bool rising,double input) {
    const struct auto_action *actions=rising?r->actions:r->otherwise;unsigned count=rising?r->n_actions:r->n_otherwise;
    if(!count)return 0;
    if(suppressed(e,r,actions,count))return -EBUSY;
    /* New state replaces stale waits/fades from the same rule. A scene already
     * loading finishes before its successor, using the existing scene lock. */
    for(unsigned i=0;i<16;i++)if(e->batches[i].used&&streq(e->batches[i].owner,r->name)){
        struct batch *old=&e->batches[i];if(e->scene_job&&old->order==e->scene_order)continue;
        for(unsigned j=0;j<AUTOMATION_JOB_LIMIT;j++)if(e->jobs[j].active&&streq(e->jobs[j].owner,r->name))end_fade(&e->jobs[j],"rule state changed");
        old->used=false;e->queued--;
    }
    struct batch *b=NULL;for(unsigned i=0;i<16;i++)if(!e->batches[i].used){b=&e->batches[i];break;}if(!b)return -ENOSPC;
    *b=(struct batch){.used=true,.count=count,.order=++e->order,.input=input};snprintf(b->owner,sizeof(b->owner),"%s",r->name);memcpy(b->actions,actions,count*sizeof(*actions));e->queued++;
    snprintf(r->state,sizeof(r->state),"queued");r->error[0]=0;return 0;
}
static void supersede_write(struct engine *e,struct batch *b,const struct auto_action *action) {
    for(unsigned i=0;i<16;i++){
        struct batch *old=&e->batches[i];
        if(old==b||!old->used||old->order>=b->order||old->index>=old->count||!same_control(action,&old->actions[old->index]))continue;
        for(unsigned j=0;j<AUTOMATION_JOB_LIMIT;j++)if(e->jobs[j].active&&streq(e->jobs[j].owner,old->owner))end_fade(&e->jobs[j],"superseded");
        finish_batch(e,old,-ECANCELED,"superseded by a newer action");
    }
}
static int confirm_write(struct batch *b) {
    b->write_seq=pipewire_sync();
    if(b->write_seq<0)return b->write_seq;
    b->pending_write=true;b->write_acked=false;return -EAGAIN;
}
static int execute_action(struct engine *e,struct batch *b,const struct auto_action *a,char *error,size_t size) {
    if(a->kind==AUTO_WAIT){b->due=diagnostics_now_ns()+(uint64_t)a->duration*1000000;return -EAGAIN;}
    if(a->kind==AUTO_SCENE){
        if(!e->scene_job){for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++)if(e->jobs[i].active)end_fade(&e->jobs[i],"scene loading");
            e->scene_job=scene_load(a->target,error,size);if(!e->scene_job)return -errno;e->scene_order=b->order;}
        int result=scene_step(e->scene_job,error,size);if(result!=-EAGAIN){scene_job_free(e->scene_job,result<0);e->scene_job=NULL;}return result;
    }
    if(a->kind==AUTO_ROUTE_RULE)return route_rule_enable(a->target,a->value!=0,error,size);
    if(a->kind==AUTO_MONITOR_SOURCE||a->kind==AUTO_MONITOR_SOLO)return monitor_edit(a->target,a->kind==AUTO_MONITOR_SOURCE?MONITOR_LISTEN:streq(a->parameter,"off")?MONITOR_SOLO_CLEAR:MONITOR_SOLO_ON,a->kind==AUTO_MONITOR_SOURCE&&streq(a->parameter,"off")?"mix":a->parameter,NULL,0,error,size);
    if(a->kind==AUTO_MUTE){uint32_t id;int result=graph_resolve_node(a->target,&id);if(result<0)return result;struct node *node=node_lookup(id);if(!node_get_params(node))return -EAGAIN;bool mute=a->input?b->input>=.5:a->value!=0;
        if(!b->pending_write){supersede_write(e,b,a);node_set_mute(node,mute);return confirm_write(b);}
        if(!b->write_acked)return -EAGAIN;
        return node_get_params(node)->mute==mute?0:-ECANCELED;}
    if(a->kind==AUTO_PARAMETER){const struct graph_node *node=managed_find("effect",a->target,"input");if(!node)return -ENOENT;const struct graph_control *control=graph_control_find(node,a->parameter);if(!control||!control->has_value)return -EAGAIN;
        double value=a->input?a->minimum+(a->maximum-a->minimum)*b->input:a->value;const char *names[]={a->parameter};double values[]={value};int result=graph_validate_controls(node->id,names,values,1);if(result<0)return result;
        if(!b->pending_write){supersede_write(e,b,a);cancel_parameter(e,node->id,a->parameter);result=graph_set_controls(node->id,names,values,1);return result<0?result:confirm_write(b);}
        if(!b->write_acked)return -EAGAIN;
        return close_value(control->value,value)?0:-ECANCELED;}
    struct automation_request request={.operation=a->kind==AUTO_VOLUME||a->kind==AUTO_FADE_VOLUME?AUTOMATION_FADE_VOLUME:AUTOMATION_FADE_PARAMETER,
        .duration_ms=a->kind==AUTO_VOLUME||a->kind==AUTO_PARAMETER?20:a->duration,.smooth=a->smooth,.value=a->input?a->minimum+(a->maximum-a->minimum)*b->input:a->value};
    snprintf(request.target,sizeof(request.target),"%s",a->target);snprintf(request.parameter,sizeof(request.parameter),"%s",a->parameter);
    int result=new_fade(e,&request);if(result<0)return result;
    struct fade *f=find_fade(e,a->target,a->parameter);if(!f)return -EIO;snprintf(f->owner,sizeof(f->owner),"%s",b->owner);
    b->waiting_fade=true;snprintf(b->fade_target,sizeof(b->fade_target),"%s",f->target);snprintf(b->fade_parameter,sizeof(b->fade_parameter),"%s",f->control);return -EAGAIN;
}
static void batch_tick(struct engine *e,struct batch *b,uint64_t now) {
    if(e->scene_job&&b->order!=e->scene_order)return;
    struct auto_rule *rule=find_rule(e,b->owner);if(rule)snprintf(rule->state,sizeof(rule->state),"applying");
    if(b->due){if(now<b->due)return;b->due=0;b->index++;b->action_started=0;}
    if(b->waiting_fade){struct fade *f=find_fade(e,b->fade_target,b->fade_parameter);if(f&&f->active&&streq(f->owner,b->owner))return;
        if(!f||!streq(f->owner,b->owner)||!streq(f->reason,"completed")){finish_batch(e,b,-ECANCELED,f?f->reason:"fade disappeared");return;}b->waiting_fade=false;b->index++;b->action_started=0;}
    for(unsigned n=0;n<AUTOMATION_ACTION_LIMIT&&b->index<b->count;n++){
        if(!b->action_started)b->action_started=now;
        if(now-b->action_started>((uint64_t)b->actions[b->index].duration+30000)*1000000){scene_job_free(e->scene_job,true);e->scene_job=NULL;finish_batch(e,b,-ETIMEDOUT,"automation action timed out");return;}
        char error[256]={0};int result=execute_action(e,b,&b->actions[b->index],error,sizeof(error));if(result==-EAGAIN)return;if(result<0){finish_batch(e,b,result,error);return;}b->index++;b->action_started=0;b->pending_write=b->write_acked=false;}
    if(b->index==b->count)finish_batch(e,b,0,NULL);
}
static void batches_tick(struct engine *e,uint64_t now) {
    /* Independent batches advance together; waits and long fades only suspend
     * their own sequence. Scene restoration remains exclusive. */
    uint64_t previous=0;
    for(unsigned n=0;n<16;n++){struct batch *b=NULL;for(unsigned i=0;i<16;i++)if(e->batches[i].used&&e->batches[i].order>previous&&(!b||e->batches[i].order<b->order))b=&e->batches[i];if(!b)break;previous=b->order;batch_tick(e,b,now);}
}
static void reload_config(struct engine *e,uint64_t now) {
    if(now-e->last_config<250000000ull)return;
    e->last_config=now;char *path=automation_config_path();if(!path)return;struct stat st;
    bool exists=lstat(path,&st)==0;
    if(!exists&&errno!=ENOENT){snprintf(e->config_error,sizeof(e->config_error),"Cannot stat automation configuration: %s",strerror(errno));free(path);return;}
    bool changed=exists!=e->config_seen||(exists&&(st.st_ino!=e->config_stat.st_ino||st.st_size!=e->config_stat.st_size||st.st_mtim.tv_sec!=e->config_stat.st_mtim.tv_sec||st.st_mtim.tv_nsec!=e->config_stat.st_mtim.tv_nsec));
    if(!changed){free(path);return;}struct auto_config *config=NULL;char error[256]={0};int result=exists?automation_config_load(path,&config,error,sizeof(error)):0;free(path);
    e->config_seen=exists;if(exists)e->config_stat=st;
    if(result<0){snprintf(e->config_error,sizeof(e->config_error),"%s",error);return;}
    if(e->scene_job){automation_config_free(config);e->config_seen=!exists;return;}
    for(unsigned i=0;i<16;i++)e->batches[i].used=false;
    e->queued=0;
    for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++)if(e->jobs[i].active&&*e->jobs[i].owner)end_fade(&e->jobs[i],"automation configuration reloaded");
    automation_config_free(e->config);e->config=config;e->config_error[0]=0;external_control_configure(e->external,config);
}
static int fire_rule(struct engine *e,struct auto_rule *r,double input,bool gate) {
    uint64_t now=diagnostics_now_ns();if(!r->enabled)return -EACCES;
    if(gate){int condition=automation_condition_eval(r->when,e->started,now);if(condition<=0)return -EAGAIN;
        if(!r->since||!r->candidate){r->candidate=true;r->since=now;}
        if(now-r->since<(uint64_t)r->hold_ms*1000000)return -EAGAIN;
    }
    if(r->last_fire&&now-r->last_fire<(uint64_t)r->cooldown_ms*1000000)return -EAGAIN;
    int result=enqueue(e,r,true,input);if(!result){r->last_fire=now;r->fired++;if(r->trigger!=TRIGGER_CONDITION)r->latched=true;}return result;
}
static int external_event(void *data,const struct control_event *event,char *error,size_t size) {
    struct engine *e=data;
    if(event->kind==CONTROL_OSC||event->kind==CONTROL_MIDI){bool matched=false;int result=-ENOENT;
        if(e->config)for(unsigned i=0;i<e->config->count;i++){struct auto_rule *r=&e->config->rules[i];bool match=event->kind==CONTROL_OSC?r->trigger==TRIGGER_OSC&&streq(r->osc_address,event->address)
                :r->trigger==TRIGGER_MIDI&&r->midi_kind==event->midi_kind&&r->midi_channel==event->channel&&r->midi_number==event->number&&(event->midi_kind!=1||r->midi_edge==2||r->midi_edge==(unsigned)event->release);
            if(match){matched=true;int sent=fire_rule(e,r,event->value,true);if(result<0||sent<0)result=sent;}}
        return matched?result:-ENOENT;
    }
    if(event->kind==CONTROL_RULE){struct auto_rule *r=find_rule(e,event->target);return r?fire_rule(e,r,event->value,true):-ENOENT;}
    if(event->kind==CONTROL_PARAMETER&&!event->duration){const struct graph_node *node=managed_find("effect",event->target,"input");if(!node)return -ENOENT;const char *names[]={event->parameter};double values[]={event->value};int result=graph_validate_controls(node->id,names,values,1);if(result<0)return result;cancel_parameter(e,node->id,event->parameter);result=graph_set_controls(node->id,names,values,1);return result<0?result:0;}
    if(event->kind==CONTROL_VOLUME||event->kind==CONTROL_PARAMETER){struct automation_request request={.operation=event->kind==CONTROL_VOLUME?AUTOMATION_FADE_VOLUME:AUTOMATION_FADE_PARAMETER,.value=event->value,.duration_ms=event->duration?:20,.smooth=event->smooth};
        snprintf(request.target,sizeof(request.target),"%s",event->target);snprintf(request.parameter,sizeof(request.parameter),"%s",event->parameter);return new_fade(e,&request);}
    if(event->kind==CONTROL_MUTE){uint32_t id;int result=graph_resolve_node(event->target,&id);if(result<0)return result;struct node *node=node_lookup(id);if(!node_get_params(node))return -EAGAIN;node_set_mute(node,event->value!=0);return 0;}
    if(event->kind==CONTROL_SCENE){struct batch *b=NULL;for(unsigned i=0;i<16;i++)if(!e->batches[i].used){b=&e->batches[i];break;}if(!b)return -ENOSPC;int result=scene_validate(event->target,error,size);if(result<0)return result;
        *b=(struct batch){.used=true,.count=1,.order=++e->order};b->actions[0].kind=AUTO_SCENE;snprintf(b->actions[0].target,sizeof(b->actions[0].target),"%s",event->target);e->queued++;return 0;}
    return -EINVAL;
}
static void rules_tick(struct engine *e,uint64_t now) {
    if(now-e->last_rules<50000000ull)return;
    e->last_rules=now;if(!e->config)return;
    for(unsigned i=0;i<e->config->count;i++){struct auto_rule *r=&e->config->rules[i];if(!r->enabled)continue;
        int value=automation_condition_eval(r->when,e->started,now);r->observed=value;
        if(value<0){r->since=0;snprintf(r->state,sizeof(r->state),"unavailable");continue;}
        bool raw=value!=0;
        if(!r->initialized){r->initialized=true;r->candidate=raw;r->since=now;r->latched=r->trigger!=TRIGGER_CONDITION||r->on_start?false:raw;snprintf(r->state,sizeof(r->state),"%s",r->latched?"active":"idle");}
        if(!r->since||r->candidate!=raw){r->candidate=raw;r->since=now;}
        if(r->trigger!=TRIGGER_CONDITION){
            if(r->trigger==TRIGGER_INTERVAL){if(!r->next_interval)r->next_interval=now+(uint64_t)r->interval_ms*1000000;if(now>=r->next_interval){r->next_interval=now+(uint64_t)r->interval_ms*1000000;fire_rule(e,r,0,true);}}
            if(!r->latched&&!*r->error)snprintf(r->state,sizeof(r->state),"%s",raw?"armed":"gated");
            continue;
        }
        if(raw==r->latched){if(streq(r->state,"unavailable")||streq(r->state,"holding")||streq(r->state,"cooldown")||streq(r->state,"suppressed"))snprintf(r->state,sizeof(r->state),"%s",raw?"active":"idle");continue;}
        if(now-r->since<(uint64_t)(raw?r->hold_ms:r->release_ms)*1000000){snprintf(r->state,sizeof(r->state),"holding");continue;}
        if(r->last_fire&&now-r->last_fire<(uint64_t)r->cooldown_ms*1000000){snprintf(r->state,sizeof(r->state),"cooldown");continue;}
        int result=enqueue(e,r,raw,0);
        if(result==-EBUSY){snprintf(r->state,sizeof(r->state),"suppressed");continue;}
        if(result<0){snprintf(r->state,sizeof(r->state),"queue full");continue;}
        r->latched=raw;r->last_fire=now;r->fired++;
        if(!(raw?r->n_actions:r->n_otherwise))snprintf(r->state,sizeof(r->state),"%s",raw?"active":"idle");
    }
}
static char *status_text(struct engine *e) {
    char *text=NULL;size_t size=0;FILE *out=open_memstream(&text,&size);if(!out)return NULL;
    unsigned active=0;for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++)active+=e->jobs[i].active;
    fprintf(out,"{\"running\":true,\"ready\":%s,\"pid\":%ld,\"active_fades\":%u,\"jobs\":[",e->ready?"true":"false",(long)getpid(),active);bool first=true;
    for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++){struct fade *f=&e->jobs[i];if(!f->used)continue;
        char *target=json_quote(f->target),*parameter=json_quote(f->control),*reason=json_quote(f->reason);
        fprintf(out,"%s{\"target\":%s,\"parameter\":%s,\"active\":%s,\"reason\":%s,\"duration_ms\":%u,\"progress\":%.6f}",first?"":",",target,parameter,f->active?"true":"false",reason,f->duration,f->active?fmin(1,(diagnostics_now_ns()-f->started)/(f->duration*1000000.0)):f->progress);
        free(target);free(parameter);free(reason);first=false;}
    char *error=json_quote(e->config_error);fprintf(out,"],\"config_error\":%s,\"queued_batches\":%u,\"rules\":[",error,e->queued);free(error);
    if(e->config)for(unsigned i=0;i<e->config->count;i++){struct auto_rule *r=&e->config->rules[i];char *name=json_quote(r->name),*state=json_quote(r->state),*message=json_quote(r->error);
        fprintf(out,"%s{\"name\":%s,\"enabled\":%s,\"priority\":%d,\"condition\":%d,\"latched\":%s,\"state\":%s,\"fired\":%"PRIu64",\"error\":%s}",i?",":"",name,r->enabled?"true":"false",r->priority,r->observed,r->latched?"true":"false",state,r->fired,message);free(name);free(state);free(message);}
    fputs("],\"external\":",out);external_control_status(e->external,out);fputs("}",out);fclose(out);return text;
}
static void close_peer(struct peer *p) { if(p->io)pw_loop_destroy_source(event_loop,p->io);p->io=NULL;if(p->fd>=0)close(p->fd);p->fd=-1;p->waiting=false; }
static void peer_request(void *data,int fd,uint32_t mask) {
    struct peer *p=data;struct engine *e=p->engine;struct automation_request r;
    if(p->waiting){if(mask&(SPA_IO_HUP|SPA_IO_ERR))close_peer(p);return;}
    ssize_t got=recv(fd,&r,sizeof(r),MSG_TRUNC);if(got<0&&(errno==EAGAIN||errno==EINTR))return;
    struct automation_reply *reply=calloc(1,sizeof(*reply));if(!reply){close_peer(p);return;}
    if(got!=sizeof(r)||r.magic!=MAGIC||r.operation>AUTOMATION_TAKEOVER||!memchr(r.target,0,sizeof(r.target))||!memchr(r.parameter,0,sizeof(r.parameter)))reply->result=-EINVAL;
    else if(r.operation==AUTOMATION_STATUS){char *text=status_text(e);if(text){if(strlen(text)>=sizeof(reply->text))reply->result=-E2BIG;else snprintf(reply->text,sizeof(reply->text),"%s",text);free(text);}else reply->result=-ENOMEM;}
    else if(r.operation==AUTOMATION_STOP)e->stopping=true;
    else if(!e->ready)reply->result=-EAGAIN;
    else if(r.operation==AUTOMATION_TAKEOVER){
        uint32_t id;bool cancelled=false;reply->result=graph_resolve_node(r.target,&id);
        if(!reply->result)for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++){
            struct fade *f=&e->jobs[i];
            if(f->active&&f->id==id&&f->parameter==!!*r.parameter&&(!f->parameter||streq(f->control,r.parameter))){
                end_fade(f,f->parameter?"manual parameter change":"manual volume change");cancelled=true;
            }
        }
        if(cancelled){
            /* The reply is a barrier across connections: the caller writes
             * only after all earlier worker writes reached the server. */
            p->sync_seq=pipewire_sync();
            if(p->sync_seq>=0){p->waiting=true;pw_loop_update_io(event_loop,p->io,SPA_IO_HUP|SPA_IO_ERR);free(reply);return;}
            reply->result=p->sync_seq;
        }
    }
    else if(r.operation==AUTOMATION_CANCEL){uint32_t id;const char *target=r.target;if(graph_resolve_node(target,&id)==0)target=graph_node_name(id);
        for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++){struct fade *f=&e->jobs[i];if(f->active&&(!*r.target||streq(f->target,target))&&(!*r.parameter||streq(f->control,r.parameter)))end_fade(f,"cancelled");}}
    else if(r.operation==AUTOMATION_TRIGGER){struct auto_rule *rule=find_rule(e,r.target);reply->result=rule?fire_rule(e,rule,0,false):-ENOENT;}
    else reply->result=new_fade(e,&r);
    if(reply->result<0)snprintf(reply->text,sizeof(reply->text),"%s",strerror(-reply->result));
    ssize_t sent=send(fd,reply,offsetof(struct automation_reply,text)+strlen(reply->text)+1,MSG_NOSIGNAL);(void)sent;free(reply);close_peer(p);
}
static void accept_peer(void *data,int fd,uint32_t mask) {
    struct engine *e=data;for(unsigned attempt=0;attempt<PEERS;attempt++){int client=accept4(fd,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);if(client<0)return;
        int buffer=262144;setsockopt(client,SOL_SOCKET,SO_SNDBUF,&buffer,sizeof(buffer));
        struct ucred cred; socklen_t size=sizeof(cred);if(getsockopt(client,SOL_SOCKET,SO_PEERCRED,&cred,&size)<0||cred.uid!=getuid()){close(client);continue;}
        struct peer *slot=NULL;for(unsigned i=0;i<PEERS;i++)if(e->peers[i].fd<0){slot=&e->peers[i];break;}if(!slot){close(client);continue;}
        slot->engine=e;slot->fd=client;slot->opened=diagnostics_now_ns();slot->io=pw_loop_add_io(event_loop,client,SPA_IO_IN|SPA_IO_HUP|SPA_IO_ERR,false,peer_request,slot);if(!slot->io)close_peer(slot);
    }
}
static void synced(int seq,void *data) {
    struct engine *e=data;if(seq==e->sync_seq){if(++e->rounds<3)e->sync_seq=pipewire_sync();}
    for(unsigned i=0;i<16;i++)if(e->batches[i].used&&e->batches[i].pending_write&&e->batches[i].write_seq==seq)e->batches[i].write_acked=true;
    for(unsigned i=0;i<PEERS;i++)if(e->peers[i].waiting&&e->peers[i].sync_seq==seq){
        struct {int result;char text[1];} reply={0};
        ssize_t sent=send(e->peers[i].fd,&reply,offsetof(struct automation_reply,text)+1,MSG_NOSIGNAL);(void)sent;
        close_peer(&e->peers[i]);
    }
}
static void failed(int code,const char *message,void *data) {struct engine *e=data;fprintf(stderr,"pipemixer automation: %s\n",message);if(scene_job_error(e->scene_job,code,message))return;if(code==-EPIPE||code==-ENOTCONN||code==-ECONNRESET){e->status=1;e->stopping=true;}}
static void stop_signal(void *data,int signal) {((struct engine*)data)->stopping=true;}
static void tick(void *data,uint64_t count) {
    struct engine *e=data;uint64_t now=diagnostics_now_ns();
    if(!e->ready&&e->rounds>=3&&graph_ready()){e->ready=true;if(e->ready_fd>=0){unsigned char ok=1;ssize_t n=write(e->ready_fd,&ok,1);(void)n;close(e->ready_fd);e->ready_fd=-1;}}
    if(!e->ready&&now-e->started>10000000000ull){fprintf(stderr,"pipemixer automation: initialization timed out\n");e->status=1;e->stopping=true;}
    for(unsigned i=0;i<PEERS;i++)if(e->peers[i].fd>=0&&now-e->peers[i].opened>500000000ull)close_peer(&e->peers[i]);
    if(e->ready){reload_config(e,now);external_control_tick(e->external,now);rules_tick(e,now);fade_tick(e,now);batches_tick(e,now);}
    if(e->stopping)pw_main_loop_quit(main_loop);
}
int automation_run(int ready_fd) {
    struct engine *e=calloc(1,sizeof(*e));if(!e)return 1;e->server=e->lock=-1;e->ready_fd=ready_fd;e->started=diagnostics_now_ns();for(unsigned i=0;i<PEERS;i++)e->peers[i].fd=-1;
    int result=1;e->external=external_control_create(external_event,e);if(!e->external)goto out;
    char path[108];if(path_for(e->path,sizeof(e->path),".sock")<0||path_for(path,sizeof(path),".lock")<0)goto out;
    e->lock=open(path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);if(e->lock<0||flock(e->lock,LOCK_EX|LOCK_NB)<0)goto out;
    e->server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(e->server<0)goto out;
    unlink(e->path);struct sockaddr_un address={.sun_family=AF_UNIX};snprintf(address.sun_path,sizeof(address.sun_path),"%s",e->path);
    if(bind(e->server,(struct sockaddr*)&address,sizeof(address))<0||chmod(e->path,0600)<0||listen(e->server,PEERS)<0)goto out;
    static const struct pipewire_events events={.sync=synced,.error=failed};e->hook=pipewire_add_listener(&events,e);
    e->io=pw_loop_add_io(event_loop,e->server,SPA_IO_IN,false,accept_peer,e);e->timer=pw_loop_add_timer(event_loop,tick,e);
    e->signals[0]=pw_loop_add_signal(event_loop,SIGINT,stop_signal,e);e->signals[1]=pw_loop_add_signal(event_loop,SIGTERM,stop_signal,e);
    struct timespec interval={.tv_nsec=20000000};e->sync_seq=pipewire_sync();
    if(!e->io||!e->timer||!e->signals[0]||!e->signals[1]||e->sync_seq<0||pw_loop_update_timer(event_loop,e->timer,&interval,&interval,false)<0)goto out;
    pw_main_loop_run(main_loop);result=e->status;
out:
    for(unsigned i=0;i<AUTOMATION_JOB_LIMIT;i++)clear_fade(&e->jobs[i]);
    for(unsigned i=0;i<PEERS;i++)close_peer(&e->peers[i]);
    scene_job_free(e->scene_job,true);automation_config_free(e->config);external_control_destroy(e->external);
    event_hook_release(e->hook);if(e->io)pw_loop_destroy_source(event_loop,e->io);if(e->timer)pw_loop_destroy_source(event_loop,e->timer);for(unsigned i=0;i<2;i++)if(e->signals[i])pw_loop_destroy_source(event_loop,e->signals[i]);
    if(e->server>=0){close(e->server);unlink(e->path);}if(e->lock>=0)close(e->lock);if(e->ready_fd>=0)close(e->ready_fd);free(e);return result;
}
static void print_status(const char *text) {
    struct spa_json parser=SPA_JSON_INIT(text,strlen(text)),object;
    if(spa_json_enter_object(&parser,&object)<=0)return;
    const char *token;int len=spa_json_object_find(&object,"active_fades",&token);unsigned fades=len>0?strtoul(token,NULL,10):0;
    len=spa_json_object_find(&object,"queued_batches",&token);unsigned queued=len>0?strtoul(token,NULL,10):0;
    printf("Automation running: %u active fades, %u queued batches\n",fades,queued);
    char key[64];struct spa_json iter=object;
    while((len=spa_json_object_next(&iter,key,sizeof(key),&token))>0){
        if(streq(key,"rules")||streq(key,"jobs")){bool jobs=streq(key,"jobs");struct spa_json array;spa_json_enter(&iter,&array);
            while((len=spa_json_next(&array,&token))>0){struct spa_json item;spa_json_enter(&array,&item);char name[512]={0},state[160]={0};const char *v;int n;
                n=spa_json_object_find(&item,jobs?"target":"name",&v);if(n>0)spa_json_parse_stringn(v,n,name,sizeof(name));n=spa_json_object_find(&item,jobs?"reason":"state",&v);if(n>0)spa_json_parse_stringn(v,n,state,sizeof(state));printf("%s %s: %s\n",jobs?"Fade":"Rule",name,state);
            }
        }
    }
}
int automation_cli(const char *command,const char *target,const char *parameter,double value,unsigned duration,bool smooth,bool json,unsigned timeout) {
    bool start=streq(command,"start-automation"),status=streq(command,"automation-status"),stop=streq(command,"stop-automation"),cancel=streq(command,"cancel-fade");
    int r=0;
    if(start||stop){char error[256]={0};r=automation_startup_set(start,error,sizeof(error));if(r<0){fprintf(stderr,"pipemixer: %s\n",error);return 1;}}
    if(start||(!status&&!stop&&!cancel))r=automation_start(timeout);
    if(start){if(r<0)fprintf(stderr,"pipemixer: cannot start automation: %s\n",strerror(-r));return r<0?1:0;}
    struct automation_request request={.operation=status?AUTOMATION_STATUS:stop?AUTOMATION_STOP:cancel?AUTOMATION_CANCEL:streq(command,"trigger-automation")?AUTOMATION_TRIGGER:parameter?AUTOMATION_FADE_PARAMETER:AUTOMATION_FADE_VOLUME,.value=value,.duration_ms=duration,.smooth=smooth};
    snprintf(request.target,sizeof(request.target),"%s",target?:"");snprintf(request.parameter,sizeof(request.parameter),"%s",parameter?:"");
    struct automation_reply *reply=calloc(1,sizeof(*reply));if(!reply)return 1;
    if(!r)r=automation_request(&request,reply,timeout);
    if((status||stop||cancel)&&(r==-ENOENT||r==-ECONNREFUSED)){if(status)puts(json?"{\"running\":false,\"active_fades\":0,\"jobs\":[]}":"Automation is stopped");r=0;}
    else if(r<0)fprintf(stderr,"pipemixer: automation: %s\n",*reply->text?reply->text:strerror(-r));
    else if(status){if(json)puts(reply->text);else print_status(reply->text);}
    free(reply);return r<0?3:0;
}
