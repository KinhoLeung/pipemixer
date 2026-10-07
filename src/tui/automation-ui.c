#define _GNU_SOURCE
#include "i18n.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <spa/utils/json.h>
#include "tui/tui.h"
#include "tui/routing.h"
#include "tui/automation-ui.h"
#include "automation.h"
#include "automation-config.h"
#include "diagnostics.h"
#include "pw/graph.h"
#include "pw/managed.h"
#include "utils.h"
enum view { CLOSED,ROOT,FADE,PARAMETERS,RULE,JOBS };
static enum view view;
static char target[512],effect[49],parameter[128],chosen_rule[49];
static char rules[32][49],states[32][160],params[128][128];
static char job_targets[32][512],job_params[32][128],job_rows[32][864];
static bool enabled[32],running,quit_pending,smooth;
static unsigned n_rules,n_params,n_jobs,duration=1000;
static double value=100,minimum=0,maximum=150,step=5;
static uint64_t last;
static pid_t child;static int error_fd=-1;static char error[256];static size_t error_size;
static struct automation_reply *reply;
static void update(void);static void open_root(void);
void automation_ui_cancel(void){view=CLOSED;free(reply);reply=NULL;}
bool automation_ui_quit(void){if(!child)return false;quit_pending=true;tui_notice(tr("Completing automation command before exit..."));return true;}
void automation_ui_cleanup(void){automation_ui_cancel();if(error_fd>=0)close(error_fd);error_fd=-1;}
static void install(enum view next,unsigned count,tui_menu_callback_t callback){if(tui.menu)tui_bind_cancel_selection((union tui_bind_data){0});view=next;tui.menu=tui_menu_create(count);tui.menu->callback=callback;tui.menu_active=true;tui_menu_resize(tui.menu,tui.term_width,tui.term_height);last=0;}
static void command(const char *name,const char *a,const char *b,const char *c,const char *d,const char *f){if(child)return;int fds[2];if(pipe2(fds,O_CLOEXEC|O_NONBLOCK)<0){tui_notice(strerror(errno));return;}
    char *args[]={"pipemixer",(char*)name,(char*)a,(char*)b,(char*)c,(char*)d,(char*)f,NULL};pid_t pid=fork();if(pid<0){close(fds[0]);close(fds[1]);tui_notice(strerror(errno));return;}
    if(!pid){close(fds[0]);dup2(fds[1],STDERR_FILENO);close(fds[1]);int fd=open("/dev/null",O_RDWR);dup2(fd,STDIN_FILENO);dup2(fd,STDOUT_FILENO);close(fd);execv("/proc/self/exe",args);_exit(127);}
    close(fds[1]);child=pid;error_fd=fds[0];error_size=0;error[0]=0;tui_notice(tr("Automation command in progress..."));
}
static double number(struct spa_json *object,const char *key){const char *v;int len=spa_json_object_find(object,key,&v);double value=0;if(len>0){char text[64];if(len<64){memcpy(text,v,len);text[len]=0;value=strtod(text,NULL);}}return value;}
static bool flag(struct spa_json *object,const char *key){const char *v;int len=spa_json_object_find(object,key,&v);bool value=false;if(len>0)spa_json_parse_bool(v,len,&value);return value;}
static void string(struct spa_json *object,const char *key,char *out,unsigned size){const char *v;int len=spa_json_object_find(object,key,&v);out[0]=0;if(len>0)spa_json_parse_stringn(v,len,out,size);}
static void choose_rule(struct tui_menu *m,struct tui_menu_item *pick){if(child)return;if(pick->data.uint==0){bool on=false;for(unsigned i=0;i<n_rules;i++)if(streq(rules[i],chosen_rule))on=enabled[i];command("enable-automation",chosen_rule,on?"off":"on",NULL,NULL,NULL);}else if(pick->data.uint==1)command("trigger-automation",chosen_rule,NULL,NULL,NULL,NULL);else if(pick->data.uint==4)open_root();}
static void choose_fade(struct tui_menu *m,struct tui_menu_item *pick){if(child)return;if(pick->data.uint==0){char goal[48],ms[24];snprintf(goal,sizeof(goal),"%.9g",value);snprintf(ms,sizeof(ms),"%u",duration);
    if(*parameter)command("fade-effect-param",effect,parameter,goal,ms,smooth?"smooth":"linear");else command("fade-volume",target,goal,ms,smooth?"smooth":"linear",NULL);
    }else if(pick->data.uint==4)smooth=!smooth;update();}
static void choose_parameter(struct tui_menu *m,struct tui_menu_item *pick){unsigned index=pick->data.uint;if(index>=n_params)return;const struct graph_node *node=managed_find("effect",effect,"input");const struct graph_control *p=node?graph_control_find(node,params[index]):NULL;if(!p||!p->has_info)return;
    snprintf(parameter,sizeof(parameter),"%s",p->name);value=p->value;minimum=p->minimum;maximum=p->maximum;step=(maximum-minimum)<=2?.01:(maximum-minimum)<=100?1:100;install(FADE,5,choose_fade);update();}
static void choose_job(struct tui_menu *m,struct tui_menu_item *pick){if(child)return;unsigned index=pick->data.uint;if(index<n_jobs)command("cancel-fade",job_targets[index],*job_params[index]?job_params[index]:NULL,NULL,NULL,NULL);else open_root();}
static void choose_root(struct tui_menu *m,struct tui_menu_item *pick){if(child)return;unsigned index=pick->data.uint;
    if(index==0)command(running?"stop-automation":"start-automation",NULL,NULL,NULL,NULL,NULL);
    else if(index==1){if(!*target){tui_notice(tr("Select an audio node first"));return;}parameter[0]=0;value=100;minimum=0;maximum=150;step=5;install(FADE,5,choose_fade);update();}
    else if(index==2){if(!*effect){tui_notice(tr("Select an effect input or output node first"));return;}const struct graph_node *node=managed_find("effect",effect,"input");n_params=0;
        if(node)for(unsigned i=0;i<node->n_controls&&n_params<128;i++){const struct graph_control *p=&node->controls[i];if(p->has_value&&p->has_info&&p->visible&&p->writable&&(p->type==SPA_TYPE_Float||p->type==SPA_TYPE_Double)&&strlen(p->name)<128)snprintf(params[n_params++],128,"%s",p->name);}
        if(!n_params){tui_notice(tr("No editable effect parameters available"));return;}install(PARAMETERS,n_params,choose_parameter);update();}
    else if(index==3){if(*target)command("cancel-fade",target,NULL,NULL,NULL,NULL);}
    else if(index==4)command("cancel-fade",NULL,NULL,NULL,NULL,NULL);
    else if(index==5){install(JOBS,33,choose_job);update();}
    else if(index>=10&&index<10+n_rules){snprintf(chosen_rule,sizeof(chosen_rule),"%s",rules[index-10]);install(RULE,5,choose_rule);update();}
}
static void open_root(void){install(ROOT,42,choose_root);update();}
static void update(void){if(view==CLOSED||!tui.menu)return;struct tui_menu *m=tui.menu;
    wstring_clear(&m->header);for(unsigned i=0;i<m->n_items;i++){wstring_clear(&m->items[i].wstr);m->items[i].data.uint=i;}
    if(view==FADE){wstring_printf(&m->header,trw(L"Fade editor | h/l: adjust  Enter: run  Esc: close"));wstring_printf(&m->items[0].wstr,trw(L"Run fade"));wstring_printf(&m->items[1].wstr,trw(L"Target: %s%s%s"),*parameter?effect:target,*parameter?" | ":"",tr_parameter(parameter));wstring_printf(&m->items[2].wstr,trw(L"Value: %.3f [%g..%g]"),value,minimum,maximum);wstring_printf(&m->items[3].wstr,trw(L"Duration: %u ms"),duration);wstring_printf(&m->items[4].wstr,trw(L"Curve: %s"),tr(smooth?"smooth":"linear"));return;}
    if(view==PARAMETERS){wstring_printf(&m->header,trw(L"Select parameter to fade | %s"),effect);for(unsigned i=0;i<n_params;i++)wstring_printf(&m->items[i].wstr,L"%s",tr_parameter(params[i]));return;}
    if(!reply)reply=calloc(1,sizeof(*reply));
    if(!reply)return;
    int result=automation_request(&(struct automation_request){.operation=AUTOMATION_STATUS},reply,80);
    struct spa_json root=SPA_JSON_INIT(reply->text,strlen(reply->text)),object;bool available=result==0&&spa_json_enter_object(&root,&object)>0;
    running=available;n_rules=0;n_jobs=0;
    if(available){char key[64];const char *v;int len;struct spa_json iter=object;
        while((len=spa_json_object_next(&iter,key,sizeof(key),&v))>0){if(streq(key,"rules")||streq(key,"jobs")){bool jobs=streq(key,"jobs");struct spa_json array;spa_json_enter(&iter,&array);
                while((len=spa_json_next(&array,&v))>0){struct spa_json entry;spa_json_enter(&array,&entry);
                    if(jobs&&n_jobs<32){char reason[128],name[512],param[128];string(&entry,"target",name,sizeof(name));string(&entry,"parameter",param,sizeof(param));string(&entry,"reason",reason,sizeof(reason));snprintf(job_targets[n_jobs],512,"%s",name);snprintf(job_params[n_jobs],128,"%s",param);
                        snprintf(job_rows[n_jobs],864,"%s %.0f%% | %s %s | %s",tr(flag(&entry,"active")?"running":"ended"),number(&entry,"progress")*100,name,tr_parameter(param),tr(reason));n_jobs++;}
                    else if(!jobs&&n_rules<32){string(&entry,"name",rules[n_rules],49);enabled[n_rules]=flag(&entry,"enabled");char state[32],error[160];string(&entry,"state",state,sizeof(state));string(&entry,"error",error,sizeof(error));snprintf(states[n_rules],160,tr("%s | %.0f runs%s%.100s"),tr(state),number(&entry,"fired"),*error?" | ":"",error);n_rules++;}
                }
            }}
    }else {char *path=automation_config_path();struct auto_config *c=NULL;char error[256]={0};if(path&&automation_config_load(path,&c,error,sizeof(error))==0){n_rules=c->count;for(unsigned i=0;i<n_rules;i++){snprintf(rules[i],49,"%s",c->rules[i].name);enabled[i]=c->rules[i].enabled;snprintf(states[i],160,tr("engine stopped"));}}automation_config_free(c);free(path);}
    if(view==ROOT||view==JOBS){unsigned count=view==ROOT?10+n_rules:n_jobs+1;if(m->n_items!=count){unsigned selected=m->selected;tui_menu_free(m);tui.menu=m=tui_menu_create(count);m->callback=view==ROOT?choose_root:choose_job;m->selected=selected<count?selected:count-1;tui_menu_resize(m,tui.term_width,tui.term_height);for(unsigned i=0;i<count;i++)m->items[i].data.uint=i;}}
    if(view==JOBS){for(unsigned i=0;i<n_jobs;i++)wstring_printf(&m->items[i].wstr,L"%s",job_rows[i]);wstring_printf(&m->header,trw(L"Active / recent fades | Enter: cancel selected fade  Esc: close"));wstring_printf(&m->items[n_jobs].wstr,trw(L"Back to automation"));return;}
    if(view==RULE){wstring_printf(&m->header,trw(L"Automation rule %s | Enter: action  Esc: close"),chosen_rule);unsigned index;for(index=0;index<n_rules;index++)if(streq(chosen_rule,rules[index]))break;bool on=index<n_rules&&enabled[index];wstring_printf(&m->items[0].wstr,trw(L"%s rule"),tr(on?"Disable":"Enable"));wstring_printf(&m->items[1].wstr,trw(L"Run actions now (manual trigger; cooldown applies)"));wstring_printf(&m->items[2].wstr,trw(L"State: %s"),index<n_rules?states[index]:tr("rule absent"));wstring_printf(&m->items[3].wstr,trw(L"Condition definitions: automation.json"));wstring_printf(&m->items[4].wstr,trw(L"Back to automation"));return;}
    wstring_printf(&m->header,trw(L"Automation / MIDI / OSC | Enter: action  Esc: close"));wstring_printf(&m->items[0].wstr,trw(L"%s automation engine"),tr(running?"Stop":"Start"));wstring_printf(&m->items[1].wstr,trw(L"Fade focused volume..."));wstring_printf(&m->items[2].wstr,trw(L"Fade focused effect parameter..."));wstring_printf(&m->items[3].wstr,trw(L"Cancel focused fades: %s"),target);wstring_printf(&m->items[4].wstr,trw(L"Cancel all fades"));wstring_printf(&m->items[5].wstr,trw(L"Active / recent fades..."));wstring_printf(&m->items[6].wstr,trw(L"Engine %s | %.0f active fades | %.0f queued batches"),tr(running?"running":"stopped"),available?number(&object,"active_fades"):0,available?number(&object,"queued_batches"):0);
    if(available){const char *v;int len=spa_json_object_find(&object,"external",&v);struct spa_json parser=SPA_JSON_INIT(v,len),external;char address[32],device[512],message[256];if(len>0&&spa_json_enter_object(&parser,&external)>0){string(&external,"osc_bind",address,sizeof(address));string(&external,"osc_error",message,sizeof(message));wstring_printf(&m->items[7].wstr,L"OSC %s:%g | %s | %s",address,number(&external,"osc_port"),tr(flag(&external,"osc_listening")?"listening":"off"),message);string(&external,"midi_device",device,sizeof(device));string(&external,"midi_error",message,sizeof(message));wstring_printf(&m->items[8].wstr,L"MIDI %s | %s | %s",device,tr(flag(&external,"midi_connected")?"connected":"off/missing"),message);}string(&object,"config_error",message,sizeof(message));wstring_printf(&m->items[9].wstr,trw(L"Config: %s"),*message?tr(message):tr("valid"));}
    for(unsigned i=0;i<n_rules;i++)wstring_printf(&m->items[10+i].wstr,L"[%s] %s | %s",tr(enabled[i]?"on":"off"),rules[i],states[i]);
}
bool automation_ui_poll(void){bool changed=false;if(child){if(error_fd>=0&&error_size<sizeof(error)-1){ssize_t n=read(error_fd,error+error_size,sizeof(error)-1-error_size);if(n>0){error_size+=n;error[error_size]=0;}}int status;if(waitpid(child,&status,WNOHANG)>0){child=0;close(error_fd);error_fd=-1;tui_notice(WIFEXITED(status)&&WEXITSTATUS(status)==0?tr("Automation command completed"):*error?error:tr("Automation command failed"));changed=true;if(quit_pending){quit_pending=false;tui_bind_quit((union tui_bind_data){0});}}}
    if(view!=CLOSED&&tui.menu_active&&diagnostics_now_ns()-last>=250000000ull){last=diagnostics_now_ns();update();changed=true;}return changed;}
bool automation_ui_key(wint_t key){if(view==CLOSED)return false;if(child&&key!=27&&key!=KEY_RESIZE&&key!='q'){tui_notice(tr("Automation command in progress"));return true;}
    if(view!=FADE||(key!='h'&&key!='l'&&key!=KEY_LEFT&&key!=KEY_RIGHT))return false;
    int direction=key=='l'||key==KEY_RIGHT?1:-1;unsigned selection=tui.menu->selected;if(selection==2)value=fmax(minimum,fmin(maximum,value+direction*step));else if(selection==3)duration=fmax(20,fmin(60000,(int)duration+direction*250));else if(selection==4)smooth=!smooth;update();return true;}
void tui_bind_manage_automation(union tui_bind_data data){uint32_t id=tui.routing_active?routing_selected_node(false):PW_ID_ANY;struct tui_tab_item *item=tui.tabs[tui.tab_index].focused;if(!tui.routing_active&&item&&item->type==TUI_TAB_ITEM_TYPE_NODE)id=item->as.node.id;const struct graph_node *node=graph_node_find(id);target[0]=effect[0]=0;if(node){snprintf(target,sizeof(target),"%s",graph_node_name(id));if(streq(dict_get(&node->props,"pipemixer.kind"),"effect"))snprintf(effect,sizeof(effect),"%s",dict_get(&node->props,"pipemixer.group"));}tui_bind_cancel_selection((union tui_bind_data){0});open_root();}
