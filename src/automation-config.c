#define _GNU_SOURCE
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pipewire/keys.h>
#include <spa/utils/json.h>
#include "automation-config.h"
#include "scene.h"
#include "pw/common.h"
#include "pw/graph.h"
#include "pw/managed.h"
#include "pw/peak.h"
#include "utils.h"
#define FILE_LIMIT (256*1024)
#define CONDITION_LIMIT 256

enum jt { OBJECT, ARRAY, STRING, NUMBER, BOOLEAN, NIL };
struct json {enum jt type;char *key,*text;double number;bool boolean;unsigned count;struct json **items;};
struct parser {const char *p,*end;unsigned count;};
static int fail(char *error,size_t size,int code,const char *format,...) {va_list ap;va_start(ap,format);vsnprintf(error,size,format,ap);va_end(ap);return -code;}
static void json_free(struct json *j){if(!j)return;for(unsigned i=0;i<j->count;i++)json_free(j->items[i]);free(j->items);free(j->key);free(j->text);free(j);}
static void whitespace(struct parser *p){while(p->p<p->end&&strchr(" \n\r\t",*p->p))p->p++;}
static char *string_token(struct parser *p){const char *start=p->p;if(p->p==p->end||*p->p++!='"')return NULL;bool closed=false;
    while(p->p<p->end){char c=*p->p++;if(c=='"'){closed=true;break;}if((unsigned char)c<32)return NULL;if(c=='\\'){if(p->p==p->end)return NULL;if(p->end-p->p>=5&&!strncmp(p->p,"u0000",5))return NULL;p->p++;}}
    size_t len=p->p-start;if(!closed||len>4096)return NULL;char *out=calloc(len+1,1);if(!out)return NULL;if(spa_json_parse_stringn(start,len,out,len+1)<=0){free(out);return NULL;}return out;
}
static struct json *get(const struct json *j,const char *key){if(j&&j->type==OBJECT)for(unsigned i=0;i<j->count;i++)if(!strcmp(j->items[i]->key,key))return j->items[i];return NULL;}
static struct json *parse_value(struct parser *p,unsigned depth){whitespace(p);if(depth>12||++p->count>8192||p->p==p->end)return NULL;
    struct json *j=calloc(1,sizeof(*j));if(!j)return NULL;char c=*p->p;
    if(c=='{'||c=='['){j->type=c=='{'?OBJECT:ARRAY;p->p++;whitespace(p);char close=c=='{'?'}':']';if(p->p<p->end&&*p->p==close){p->p++;return j;}
        for(;;){char *key=NULL;if(j->type==OBJECT){key=string_token(p);if(!key||get(j,key)){free(key);goto bad;}whitespace(p);if(p->p==p->end||*p->p++!=':'){free(key);goto bad;}}
            struct json *child=parse_value(p,depth+1);if(!child){free(key);goto bad;}child->key=key;struct json **items=realloc(j->items,(j->count+1)*sizeof(*items));if(!items){json_free(child);goto bad;}j->items=items;j->items[j->count++]=child;
            whitespace(p);if(p->p==p->end)goto bad;if(*p->p==close){p->p++;break;}if(*p->p++!=',')goto bad;whitespace(p);
        }
    }else if(c=='"'){j->type=STRING;j->text=string_token(p);if(!j->text)goto bad;}
    else if(p->end-p->p>=4&&!strncmp(p->p,"true",4)){j->type=BOOLEAN;j->boolean=true;p->p+=4;}
    else if(p->end-p->p>=5&&!strncmp(p->p,"false",5)){j->type=BOOLEAN;p->p+=5;}
    else if(p->end-p->p>=4&&!strncmp(p->p,"null",4)){j->type=NIL;p->p+=4;}
    else {const char *start=p->p;if(*p->p=='-')p->p++;if(p->p==p->end||*p->p<'0'||*p->p>'9')goto bad;
        if(*p->p=='0')p->p++;else while(p->p<p->end&&*p->p>='0'&&*p->p<='9')p->p++;
        if(p->p<p->end&&*p->p=='.'){p->p++;const char *first=p->p;while(p->p<p->end&&*p->p>='0'&&*p->p<='9')p->p++;if(p->p==first)goto bad;}
        if(p->p<p->end&&(*p->p=='e'||*p->p=='E')){p->p++;if(p->p<p->end&&(*p->p=='+'||*p->p=='-'))p->p++;const char *first=p->p;while(p->p<p->end&&*p->p>='0'&&*p->p<='9')p->p++;if(p->p==first)goto bad;}
        char number[80];size_t len=p->p-start;if(len>=sizeof(number))goto bad;memcpy(number,start,len);number[len]=0;errno=0;j->number=strtod(number,NULL);if(errno||!isfinite(j->number))goto bad;j->type=NUMBER;
    }return j;
bad:json_free(j);return NULL;
}
static int read_json(const char *path,struct json **out){*out=NULL;int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -errno;struct stat st;
    if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>FILE_LIMIT){close(fd);return -EINVAL;}
    char *text=calloc(st.st_size+1,1);if(!text){close(fd);return -ENOMEM;}size_t at=0;while(at<(size_t)st.st_size){ssize_t n=read(fd,text+at,st.st_size-at);if(n<0&&errno==EINTR)continue;if(n<=0){free(text);close(fd);return -EIO;}at+=n;}close(fd);
    struct parser p={.p=text,.end=text+at};struct json *j=parse_value(&p,0);whitespace(&p);bool valid=j&&p.p==p.end;free(text);if(!valid){json_free(j);return -EINVAL;}*out=j;return 0;
}
static void write_json(FILE *out,const struct json *j){switch(j->type){case OBJECT:case ARRAY:fputc(j->type==OBJECT?'{':'[',out);for(unsigned i=0;i<j->count;i++){if(i)fputc(',',out);if(j->type==OBJECT){char *key=json_quote(j->items[i]->key);fprintf(out,"%s:",key);free(key);}write_json(out,j->items[i]);}fputc(j->type==OBJECT?'}':']',out);break;case STRING:{char *text=json_quote(j->text);fputs(text,out);free(text);break;}case NUMBER:fprintf(out,"%.17g",j->number);break;case BOOLEAN:fputs(j->boolean?"true":"false",out);break;case NIL:fputs("null",out);break;}}
static const char *text(const struct json *j,const char *key){struct json *v=get(j,key);return v&&v->type==STRING?v->text:NULL;}
static bool keys(const struct json *j,const char *const allowed[]){if(!j||j->type!=OBJECT)return false;for(unsigned i=0;i<j->count;i++){bool known=false;for(unsigned k=0;allowed[k];k++)if(!strcmp(j->items[i]->key,allowed[k]))known=true;if(!known)return false;}return true;}
static bool integer(const struct json *j,const char *key,int low,int high,int def,int *out){struct json *v=get(j,key);*out=def;if(!v)return true;if(v->type!=NUMBER||v->number<low||v->number>high||floor(v->number)!=v->number)return false;*out=v->number;return true;}
static bool boolean(const struct json *j,const char *key,bool def,bool *out){struct json *v=get(j,key);*out=def;if(!v)return true;if(v->type!=BOOLEAN)return false;*out=v->boolean;return true;}
static bool number(const struct json *j,const char *key,double low,double high,double def,double *out){struct json *v=get(j,key);*out=def;if(!v)return true;if(v->type!=NUMBER||v->number<low||v->number>high)return false;*out=v->number;return true;}
static bool target_valid(const char *target){return target&&*target&&strlen(target)<512&&strncmp(target,"id:",3)&&strncmp(target,"serial:",7);}
enum ct { C_ALL,C_ANY,C_NOT,C_PRESENT,C_MUTE,C_VOLUME,C_PARAMETER,C_LEVEL,C_LINK,C_DEFAULT,C_ELAPSED,C_ALWAYS };
struct auto_condition {
    enum ct kind;unsigned count;struct auto_condition **children;
    char target[512],parameter[128],destination[512],serial[32];
    double threshold,hysteresis;bool above,expected,high;
    struct peak_meter *meter;uint64_t last_meter;
};
static void condition_free(struct auto_condition *c){if(!c)return;if(c->children)for(unsigned i=0;i<c->count;i++)condition_free(c->children[i]);peak_meter_destroy(c->meter);free(c->children);free(c);}
static struct auto_condition *condition_parse(const struct json *j,unsigned depth,unsigned *total){if(!j||j->type!=OBJECT||depth>8||++*total>CONDITION_LIMIT)return NULL;
    struct auto_condition *c=calloc(1,sizeof(*c));if(!c)return NULL;
    const char *operators[]={"all","any","not",NULL};
    for(unsigned k=0;k<3;k++){struct json *v=get(j,operators[k]);if(!v)continue;if(j->count!=1)goto bad;c->kind=k==0?C_ALL:k==1?C_ANY:C_NOT;
        if(k==2){c->count=1;c->children=calloc(1,sizeof(*c->children));if(!c->children||(c->children[0]=condition_parse(v,depth+1,total))==NULL)goto bad;}
        else {if(v->type!=ARRAY||!v->count||v->count>16)goto bad;c->count=v->count;c->children=calloc(c->count,sizeof(*c->children));if(!c->children)goto bad;for(unsigned i=0;i<c->count;i++)if(!(c->children[i]=condition_parse(v->items[i],depth+1,total)))goto bad;}return c;
    }
    if(get(j,"elapsed_ms")){if(j->count!=1||!number(j,"elapsed_ms",0,86400000,0,&c->threshold))goto bad;c->kind=C_ELAPSED;return c;}
    const char *type=text(j,"type"),*target=text(j,"target"),*parameter=text(j,"parameter");
    static const char *const allowed[]={"type","target","parameter","above","below","hysteresis","value","input",NULL};
    if(!keys(j,allowed)||!type)goto bad;
    if(!strcmp(type,"default")){if(!target_valid(target)||!parameter||(strcmp(parameter,"sink")&&strcmp(parameter,"source")))goto bad;c->kind=C_DEFAULT;}
    else if(!strcmp(type,"link")){const char *input=text(j,"input");if(!target_valid(target)||!target_valid(input))goto bad;c->kind=C_LINK;snprintf(c->destination,sizeof(c->destination),"%s",input);}
    else {if(!target_valid(target))goto bad;
        if(!strcmp(type,"present"))c->kind=C_PRESENT;else if(!strcmp(type,"mute"))c->kind=C_MUTE;
        else if(!strcmp(type,"volume"))c->kind=C_VOLUME;else if(!strcmp(type,"parameter"))c->kind=C_PARAMETER;
        else if(!strcmp(type,"level"))c->kind=C_LEVEL;else goto bad;
        if(c->kind==C_PARAMETER&&(!managed_valid_name(target)||!parameter||!*parameter||strlen(parameter)>=128))goto bad;
    }
    snprintf(c->target,sizeof(c->target),"%s",target);if(parameter)snprintf(c->parameter,sizeof(c->parameter),"%s",parameter);
    if(c->kind==C_PRESENT||c->kind==C_MUTE||c->kind==C_LINK){if(!get(j,"value")||!boolean(j,"value",true,&c->expected))goto bad;}
    else if(c->kind==C_VOLUME||c->kind==C_PARAMETER||c->kind==C_LEVEL){bool above=get(j,"above")!=NULL,below=get(j,"below")!=NULL;if(above==below)goto bad;c->above=above;
        double low=c->kind==C_VOLUME?0:c->kind==C_LEVEL?-120:-1000000,high=c->kind==C_VOLUME?150:c->kind==C_LEVEL?24:1000000;
        if(!number(j,above?"above":"below",low,high,0,&c->threshold)||!number(j,"hysteresis",0,high-low,0,&c->hysteresis))goto bad;
    }return c;
bad:condition_free(c);return NULL;
}
static int compare_rule(const void *a,const void *b){const struct auto_rule *x=a,*y=b;if(x->priority!=y->priority)return x->priority>y->priority?-1:1;return strcmp(x->name,y->name);}
static int actions_parse(const struct json *j,struct auto_action *out,unsigned *count){*count=0;if(!j)return 0;if(j->type!=ARRAY||j->count>AUTOMATION_ACTION_LIMIT)return -EINVAL;
    static const char *const allowed[]={"type","target","parameter","value","duration_ms","curve","min","max",NULL};
    const char *types[]={"volume","mute","parameter","fade-volume","fade-parameter","scene","route-rule","monitor-source","monitor-solo","wait"};
    for(unsigned i=0;i<j->count;i++){const struct json *v=j->items[i];if(!keys(v,allowed))return -EINVAL;const char *type=text(v,"type"),*target=text(v,"target"),*parameter=text(v,"parameter");unsigned k;
        for(k=0;k<sizeof(types)/sizeof(*types);k++)if(streq(type,types[k]))break;
        if(k==sizeof(types)/sizeof(*types))return -EINVAL;
        struct auto_action *a=&out[i];a->kind=k;int duration;
        if(!integer(v,"duration_ms",0,3600000,0,&duration))return -EINVAL;
        a->duration=duration;
        if(k==AUTO_WAIT){if(!a->duration||target||get(v,"value")||parameter)return -EINVAL;(*count)++;continue;}
        if(!target_valid(target))return -EINVAL;
        snprintf(a->target,sizeof(a->target),"%s",target);
        bool managed=k==AUTO_PARAMETER||k==AUTO_FADE_PARAMETER||k==AUTO_SCENE||k==AUTO_ROUTE_RULE||k==AUTO_MONITOR_SOURCE||k==AUTO_MONITOR_SOLO;
        if(managed&&!managed_valid_name(target))return -EINVAL;
        if(k==AUTO_PARAMETER||k==AUTO_FADE_PARAMETER||k==AUTO_MONITOR_SOURCE||k==AUTO_MONITOR_SOLO){if(!parameter||!*parameter||strlen(parameter)>=sizeof(a->parameter))return -EINVAL;snprintf(a->parameter,sizeof(a->parameter),"%s",parameter);}
        if(k==AUTO_VOLUME||k==AUTO_PARAMETER||k==AUTO_FADE_VOLUME||k==AUTO_FADE_PARAMETER){double low=k==AUTO_VOLUME||k==AUTO_FADE_VOLUME?0:-1000000,high=k==AUTO_VOLUME||k==AUTO_FADE_VOLUME?150:1000000;
            if(streq(text(v,"value"),"input")){a->input=true;if(!get(v,"min")||!get(v,"max")||!number(v,"min",low,high,0,&a->minimum)||!number(v,"max",low,high,0,&a->maximum))return -EINVAL;}
            else if(!get(v,"value")||!number(v,"value",low,high,0,&a->value))return -EINVAL;}
        if(k==AUTO_MUTE||k==AUTO_ROUTE_RULE){bool value;if(streq(text(v,"value"),"input")&&k==AUTO_MUTE){a->input=true;a->maximum=1;}else{if(!get(v,"value")||!boolean(v,"value",false,&value))return -EINVAL;a->value=value;}}
        if(k==AUTO_FADE_VOLUME||k==AUTO_FADE_PARAMETER){if(a->duration<20)return -EINVAL;}
        const char *curve=text(v,"curve");if(curve&&strcmp(curve,"linear")&&strcmp(curve,"smooth"))return -EINVAL;a->smooth=streq(curve,"smooth");(*count)++;
    }return 0;
}
static int decode(struct json *j,struct auto_config **out,char *error,size_t size){*out=NULL;static const char *const allowed[]={"format","version","rules","osc","midi",NULL};struct json *rules=get(j,"rules"),*version=get(j,"version");
    if(!keys(j,allowed)||!streq(text(j,"format"),"pipemixer.automation")||!version||version->type!=NUMBER||version->number!=1||!rules||rules->type!=ARRAY||rules->count>AUTOMATION_RULE_LIMIT)return fail(error,size,EINVAL,"Expected pipemixer.automation version 1 with at most 32 rules");
    struct auto_config *c=calloc(1,sizeof(*c));if(!c)return -ENOMEM;c->count=rules->count;unsigned total=0;
    snprintf(c->osc_bind,sizeof(c->osc_bind),"127.0.0.1");struct json *osc=get(j,"osc"),*midi=get(j,"midi");
    if(osc){static const char *const fields[]={"bind","port","direct",NULL};int port;const char *bind=text(osc,"bind");struct in_addr addr;
        if(!keys(osc,fields)||!integer(osc,"port",0,65535,9000,&port)||!boolean(osc,"direct",false,&c->osc_direct)||(get(osc,"bind")&&!bind)||(bind&&(strlen(bind)>=sizeof(c->osc_bind)||inet_pton(AF_INET,bind,&addr)!=1)))goto bad;
        c->osc_port=port;if(bind)snprintf(c->osc_bind,sizeof(c->osc_bind),"%s",bind);
    }
    if(midi){static const char *const fields[]={"device",NULL};const char *device=text(midi,"device");if(!keys(midi,fields)||!device||*device!='/'||strlen(device)>=sizeof(c->midi_device))goto bad;snprintf(c->midi_device,sizeof(c->midi_device),"%s",device);}

    static const char *const fields[]={"name","enabled","priority","on_start","hold_ms","release_ms","cooldown_ms","when","actions","otherwise","trigger",NULL};
    for(unsigned i=0;i<c->count;i++){struct json *v=rules->items[i];struct auto_rule *r=&c->rules[i];const char *name=text(v,"name");int hold,release,cooldown,priority;
        if(!keys(v,fields)||!managed_valid_name(name)||!boolean(v,"enabled",true,&r->enabled)||!boolean(v,"on_start",false,&r->on_start)||!integer(v,"priority",-1000000,1000000,0,&priority)||!integer(v,"hold_ms",0,3600000,0,&hold)||!integer(v,"release_ms",0,3600000,hold,&release)||!integer(v,"cooldown_ms",0,3600000,0,&cooldown))goto bad;
        snprintf(r->name,sizeof(r->name),"%s",name);for(unsigned k=0;k<i;k++)if(streq(c->rules[k].name,name))goto bad;
        r->priority=priority;r->hold_ms=hold;r->release_ms=release;r->cooldown_ms=cooldown;
        struct json *trigger=get(v,"trigger");
        if(trigger){if(trigger->type!=OBJECT||trigger->count!=1||r->on_start)goto bad;
            struct json *osc=get(trigger,"osc"),*midi=get(trigger,"midi"),*interval=get(trigger,"interval_ms");
            if(osc){if(osc->type!=STRING||osc->text[0]!='/'||strlen(osc->text)>=sizeof(r->osc_address)||strpbrk(osc->text," *?[]{}#,\t\n\r"))goto bad;r->trigger=TRIGGER_OSC;snprintf(r->osc_address,sizeof(r->osc_address),"%s",osc->text);}
            else if(midi){static const char *const fields[]={"type","channel","number","edge",NULL};const char *kind=text(midi,"type"),*edge=text(midi,"edge");int channel,number;
                if(!keys(midi,fields)||!integer(midi,"channel",1,16,1,&channel)||!integer(midi,"number",0,127,0,&number)||!get(midi,"number")||(!streq(kind,"cc")&&!streq(kind,"note")&&!streq(kind,"program"))||(edge&&strcmp(edge,"press")&&strcmp(edge,"release")&&strcmp(edge,"any")))goto bad;
                r->trigger=TRIGGER_MIDI;r->midi_kind=streq(kind,"cc")?0:streq(kind,"note")?1:2;r->midi_channel=channel;r->midi_number=number;r->midi_edge=streq(edge,"release")?1:streq(edge,"any")?2:0;
            }else if(interval){int ms;if(!integer(trigger,"interval_ms",100,86400000,0,&ms))goto bad;r->trigger=TRIGGER_INTERVAL;r->interval_ms=ms;}else goto bad;
        }
        if(!get(v,"when")&&trigger){r->when=calloc(1,sizeof(*r->when));if(r->when)r->when->kind=C_ALWAYS;}else r->when=condition_parse(get(v,"when"),0,&total);
        if(!r->when||actions_parse(get(v,"actions"),r->actions,&r->n_actions)<0||!r->n_actions||actions_parse(get(v,"otherwise"),r->otherwise,&r->n_otherwise)<0)goto bad;
        if(r->trigger!=TRIGGER_OSC&&r->trigger!=TRIGGER_MIDI){for(unsigned a=0;a<r->n_actions;a++)if(r->actions[a].input)goto bad;for(unsigned a=0;a<r->n_otherwise;a++)if(r->otherwise[a].input)goto bad;}
        if(r->trigger!=TRIGGER_CONDITION&&r->n_otherwise)goto bad;
        r->observed=-1;snprintf(r->state,sizeof(r->state),"%s",r->enabled?"arming":"disabled");
    }
    qsort(c->rules,c->count,sizeof(c->rules[0]),compare_rule);*out=c;return 0;
bad:automation_config_free(c);return fail(error,size,EINVAL,"Invalid automation rule %u; check names, conditions, action values and bounds",total);
}
char *automation_config_path(void){char *path=scene_directory();if(!path)return NULL;*strrchr(path,'/')=0;char *file=NULL;if(asprintf(&file,"%s/automation.json",path)<0)file=NULL;free(path);return file;}
int automation_config_load(const char *path,struct auto_config **out,char *error,size_t size){*out=NULL;struct json *j=NULL;int result=read_json(path,&j);if(!result)result=decode(j,out,error,size);json_free(j);if(result<0&&!*error)fail(error,size,-result,"Cannot read automation configuration: %s",strerror(-result));return result;}
void automation_config_free(struct auto_config *c){if(!c)return;for(unsigned i=0;i<c->count;i++)condition_free(c->rules[i].when);free(c);}
static int mkdirs(char *path){for(char *p=path+1;;p++){if(*p&&*p!='/')continue;char saved=*p;*p=0;if(mkdir(path,0700)<0&&errno!=EEXIST){*p=saved;return -errno;}*p=saved;if(!saved)return 0;}}
static int config_lock(char **path){*path=automation_config_path();if(!*path)return -ENOENT;char *directory=strdup(*path);if(!directory)return -ENOMEM;*strrchr(directory,'/')=0;int r=mkdirs(directory);free(directory);if(r<0)return r;char *lock=NULL;if(asprintf(&lock,"%s.lock",*path)<0)return -ENOMEM;int fd=open(lock,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);free(lock);if(fd<0)return -errno;if(flock(fd,LOCK_EX)<0){r=-errno;close(fd);return r;}return fd;}
static int save_json(const char *path,const struct json *j){char *temp=NULL;if(asprintf(&temp,"%s.tmp-XXXXXX",path)<0)return -ENOMEM;int fd=mkostemp(temp,O_CLOEXEC);int r=0;if(fd<0){free(temp);return -errno;}FILE *out=fdopen(fd,"w");if(!out){r=-errno;close(fd);goto done;}write_json(out,j);fputc('\n',out);if(ferror(out)||fflush(out)||fsync(fd))r=-(errno?:EIO);if(fclose(out)&&!r)r=-errno;if(!r&&rename(temp,path)<0)r=-errno;
    if(!r){char *dir=strdup(path);if(!dir){r=-ENOMEM;goto done;}*strrchr(dir,'/')=0;int d=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);free(dir);if(d<0)r=-errno;else{if(fsync(d)<0)r=-errno;close(d);}}
done:if(r<0)unlink(temp);free(temp);return r;}
int automation_config_import(const char *source,char *error,size_t size){struct json *j=NULL;struct auto_config *c=NULL;int r=read_json(source,&j);if(!r)r=decode(j,&c,error,size);automation_config_free(c);char *path=NULL;int lock=-1;if(!r){lock=config_lock(&path);if(lock<0)r=lock;else r=save_json(path,j);}if(lock>=0)close(lock);free(path);json_free(j);if(r<0&&!*error)fail(error,size,-r,"Cannot import automation: %s",strerror(-r));return r;}
int automation_config_enable(const char *name,bool enabled,char *error,size_t size){char *path=NULL;int lock=config_lock(&path),r=lock<0?lock:0;struct json *j=NULL;struct auto_config *c=NULL;
    if(!r)r=read_json(path,&j);
    if(!r)r=decode(j,&c,error,size);
    automation_config_free(c);
    if(!r){struct json *rules=get(j,"rules");r=-ENOENT;for(unsigned i=0;i<rules->count;i++)if(streq(text(rules->items[i],"name"),name)){struct json *v=get(rules->items[i],"enabled");if(!v){v=calloc(1,sizeof(*v));if(!v){r=-ENOMEM;break;}struct json **items=realloc(rules->items[i]->items,(rules->items[i]->count+1)*sizeof(*items));if(!items){free(v);r=-ENOMEM;break;}v->key=strdup("enabled");rules->items[i]->items=items;rules->items[i]->items[rules->items[i]->count++]=v;}v->type=BOOLEAN;v->boolean=enabled;r=save_json(path,j);break;}}
    if(lock>=0)close(lock);
    free(path);json_free(j);if(r<0&&!*error)fail(error,size,-r,"Cannot enable automation rule: %s",strerror(-r));return r;}
int automation_condition_eval(struct auto_condition *c,uint64_t started,uint64_t now){if(!c)return -1;
    if(c->kind==C_ALL||c->kind==C_ANY){bool unknown=false;int result=c->kind==C_ALL?1:0;for(unsigned i=0;i<c->count;i++){int v=automation_condition_eval(c->children[i],started,now);if(v<0)unknown=true;else if(c->kind==C_ALL&&!v)result=0;else if(c->kind==C_ANY&&v)result=1;}return (c->kind==C_ALL&&result==0)||(c->kind==C_ANY&&result==1)?result:unknown?-1:result;}
    if(c->kind==C_NOT){int v=automation_condition_eval(c->children[0],started,now);return v<0?-1:!v;}
    if(c->kind==C_ALWAYS)return 1;
    if(c->kind==C_ELAPSED)return (now-started)/1000000>=c->threshold;
    if(c->kind==C_DEFAULT){const char *name=pipewire_get_default(streq(c->parameter,"sink")?DEFAULT_AUDIO_SINK:DEFAULT_AUDIO_SOURCE);return name?streq(name,c->target):-1;}
    if(c->kind==C_LINK){uint32_t output,input;bool linked=graph_resolve_port(c->target,PW_DIRECTION_OUTPUT,&output)==0&&graph_resolve_port(c->destination,PW_DIRECTION_INPUT,&input)==0&&graph_link_between(output,input);return linked==c->expected;}
    uint32_t id;const struct graph_node *g;if(c->kind==C_PARAMETER){g=managed_find("effect",c->target,"input");id=g?g->id:PW_ID_ANY;}else{g=graph_resolve_node(c->target,&id)==0?graph_node_find(id):NULL;}
    if(c->kind==C_PRESENT)return (g!=NULL)==c->expected;
    if(!g){peak_meter_destroy(c->meter);c->meter=NULL;c->serial[0]=0;c->high=false;return -1;}
    double value;
    if(c->kind==C_MUTE||c->kind==C_VOLUME){const struct param_props *p=node_get_params(node_lookup(id));if(!p)return -1;if(c->kind==C_MUTE)return p->mute==c->expected;value=0;for(unsigned i=0;i<p->n_channels;i++)value=fmax(value,p->channel_volumes[i]*100);}
    else if(c->kind==C_PARAMETER){const struct graph_control *v=graph_control_find(g,c->parameter);if(!v||!v->has_value)return -1;value=v->value;}
    else {const char *serial=dict_get(&g->props,PW_KEY_OBJECT_SERIAL);if(!c->meter||!streq(c->serial,serial)){peak_meter_destroy(c->meter);const char *cls=dict_get(&g->props,PW_KEY_MEDIA_CLASS);c->meter=peak_meter_create(serial,streq(cls,"Audio/Sink")||streq(cls,"Stream/Input/Audio"),false);snprintf(c->serial,sizeof(c->serial),"%s",serial?:"");c->high=false;}
        if(!c->meter)return -1;
        if(now-c->last_meter>=50000000ull){peak_meter_step(c->meter);c->last_meter=now;}struct meter_snapshot s;peak_meter_snapshot(c->meter,&s,false);if(!s.active||!s.channels)return -1;float rms=0;for(unsigned i=0;i<s.channels;i++)rms=fmaxf(rms,s.channel[i].rms);value=rms>0?20*log10(rms):-120;
    }
    if(!isfinite(value))return -1;
    double threshold=c->threshold+(c->high?(c->above?-c->hysteresis:c->hysteresis):0);c->high=c->above?value>threshold:value<threshold;return c->high;
}
int automation_startup_set(bool enabled,char *error,size_t size){char *config=NULL;int lock=config_lock(&config),result=lock<0?lock:0;char *path=NULL,*temp=NULL;
    if(!result){*strrchr(config,'/')=0;if(asprintf(&path,"%s/automation-enabled",config)<0||asprintf(&temp,"%s.tmp-XXXXXX",path)<0)result=-ENOMEM;}
    if(!result){int fd=mkostemp(temp,O_CLOEXEC);if(fd<0)result=-errno;else{const char *text=enabled?"on\n":"off\n";size_t size=strlen(text);if(write(fd,text,size)!=(ssize_t)size||fsync(fd)<0)result=-(errno?:EIO);if(close(fd)<0&&!result)result=-errno;
            if(!result&&rename(temp,path)<0)result=-errno;
            if(!result){int directory=open(config,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(directory<0)result=-errno;else{if(fsync(directory)<0)result=-errno;close(directory);}}}}
    if(temp&&result<0)unlink(temp);
    if(lock>=0)close(lock);
    free(path);free(temp);free(config);if(result<0)fail(error,size,-result,"Cannot update automation startup preference: %s",strerror(-result));return result;
}
