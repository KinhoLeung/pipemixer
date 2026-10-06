#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <spa/utils/json.h>
#include "tui/tui.h"
#include "tui/recording-ui.h"
#include "tui/routing.h"
#include "recorder.h"
#include "diagnostics.h"
#include "pw/graph.h"
#include "utils.h"

enum view { CLOSED, LIST, CREATE, DETAIL, STOP };
static enum view view;
static char selected[49],sessions[16][49],nodes[256][512];
static bool marked[256];
static unsigned n_sessions,n_nodes;
static struct capture_child starting={.ready_fd=-1};
static bool starting_pending,quit_pending;
static uint64_t deadline,last_poll;
static pid_t command_pid;
static int error_fd=-1;
static char command_error[256],command_path[4096];
static size_t error_size;
static bool recording;
static unsigned history_seconds;

void recording_ui_cancel(void){view=CLOSED;}
bool recording_ui_quit(void){if(!command_pid)return false;quit_pending=true;tui_notice("Completing the recording operation before exit...");return true;}
void recording_ui_cleanup(void){recording_ui_cancel();if(starting_pending)capture_child_cancel(&starting);starting_pending=false;if(error_fd>=0)close(error_fd);error_fd=-1;}
static bool pending(void){return starting_pending||command_pid;}
static void install(enum view next,unsigned count,tui_menu_callback_t callback){if(tui.menu)tui_bind_cancel_selection((union tui_bind_data){0});view=next;tui.menu=tui_menu_create(count);tui.menu->callback=callback;tui.menu_active=true;tui_menu_resize(tui.menu,tui.term_width,tui.term_height);last_poll=0;}
static void open_list(void);
static void open_detail(void);
static void update(void);
static int mkdirs(char *path){for(char *p=path+1;*p;p++)if(*p=='/'){*p=0;if(mkdir(path,0700)<0&&errno!=EEXIST){*p='/';return -errno;}*p='/';}return mkdir(path,0700)<0&&errno!=EEXIST?-errno:0;}
static bool new_take(void){char *base=capture_default_directory();if(!base)return false;int r=mkdirs(base);if(r<0){tui_notice(strerror(-r));free(base);return false;}time_t now=time(NULL);struct tm tm;localtime_r(&now,&tm);char stamp[32];strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",&tm);int length=snprintf(command_path,sizeof(command_path),"%s/take-%s-%09u",base,stamp,(unsigned)(diagnostics_now_ns()%1000000000));free(base);if(length>=(int)sizeof(command_path)){tui_notice("Recording path is too long");return false;}return true;}
/* Export and file finalization run in another process; the menu keeps responding. */
static void run_command(const char *command,const char *directory,const char *seconds){if(pending())return;
    int fds[2];if(pipe2(fds,O_CLOEXEC|O_NONBLOCK)<0){tui_notice(strerror(errno));return;}
    char *args[]={"pipemixer","--timeout","30000",(char*)command,selected,(char*)directory,(char*)seconds,NULL};
    pid_t pid=fork();if(pid<0){close(fds[0]);close(fds[1]);tui_notice(strerror(errno));return;}
    if(!pid){close(fds[0]);dup2(fds[1],STDERR_FILENO);close(fds[1]);int fd=open("/dev/null",O_RDWR);dup2(fd,STDIN_FILENO);dup2(fd,STDOUT_FILENO);close(fd);execv("/proc/self/exe",args);_exit(127);}
    close(fds[1]);command_pid=pid;error_fd=fds[0];error_size=0;command_error[0]=0;tui_notice("Recording operation in progress...");
}
static void choose_stop(struct tui_menu *menu,struct tui_menu_item *pick){if(pending())return;if(pick->data.uint==1)run_command("stop-history",NULL,NULL);open_detail();}
static void choose_detail(struct tui_menu *menu,struct tui_menu_item *pick){if(pending())return;unsigned action=pick->data.uint;
    if(action==0||action==1){if(recording){tui_notice("Stop continuous recording before exporting the cache");return;}char seconds[16];snprintf(seconds,sizeof(seconds),"%u",history_seconds>=5?5:history_seconds?:1);if(new_take())run_command("export-history",command_path,action==0?seconds:NULL);}
    else if(action==2){if(recording){command_path[0]=0;run_command("stop-recording",NULL,NULL);}else if(new_take())run_command("record-history",command_path,NULL);}
    else if(action==3){install(STOP,2,choose_stop);wstring_printf(&tui.menu->header,L"Stop cache %s and finalize any recording?",selected);wstring_printf(&tui.menu->items[0].wstr,L"Cancel");wstring_printf(&tui.menu->items[1].wstr,L"Stop cache");tui.menu->items[0].data.uint=0;tui.menu->items[1].data.uint=1;}
    else if(action==4)open_list();
}
static void source_node(const struct graph_node *node,void *data){if(n_nodes==256||!graph_node_is_audio(node)||!graph_node_has_ports(node->id,PW_DIRECTION_OUTPUT))return;const char *name=graph_node_name(node->id);if(name&&strlen(name)<512)snprintf(nodes[n_nodes++],512,"%s",name);}
static void choose_create(struct tui_menu *menu,struct tui_menu_item *pick){if(pending())return;unsigned index=pick->data.uint;
    if(index>=2){marked[index-2]=!marked[index-2];update();return;}
    const char *sources[CAPTURE_TRACK_LIMIT];unsigned count=0;for(unsigned i=0;i<n_nodes;i++)if(marked[i]){if(count==CAPTURE_TRACK_LIMIT){tui_notice("Select at most 8 stereo tracks");return;}sources[count++]=nodes[i];}
    if(!count){tui_notice("Select at least one source");return;}
    for(unsigned n=1;;n++){bool used=false;snprintf(selected,sizeof(selected),"history%u",n);for(unsigned i=0;i<n_sessions;i++)if(!strcmp(selected,sessions[i]))used=true;if(!used)break;}
    char error[256]={0};int result=capture_spawn(selected,index?30:10,sources,count,&starting,error,sizeof(error));if(result<0){tui_notice(error[0]?error:strerror(-result));return;}
    starting_pending=true;deadline=diagnostics_now_ns()+10000000000ull;open_detail();tui_notice("Starting audio history...");
}
static void open_create(void){n_nodes=0;memset(marked,0,sizeof(marked));graph_foreach_node(source_node,NULL);
    uint32_t focus=tui.routing_active?routing_selected_node(false):PW_ID_ANY;struct tui_tab_item *item=tui.tabs[tui.tab_index].focused;if(!tui.routing_active&&item&&item->type==TUI_TAB_ITEM_TYPE_NODE)focus=item->as.node.id;const char *name=graph_node_name(focus);for(unsigned i=0;i<n_nodes;i++)if(name&&!strcmp(name,nodes[i]))marked[i]=true;
    install(CREATE,n_nodes+2,choose_create);update();
}
static void choose_list(struct tui_menu *menu,struct tui_menu_item *pick){if(pending())return;unsigned index=pick->data.uint;if(index==n_sessions){open_create();return;}if(index<n_sessions){snprintf(selected,sizeof(selected),"%s",sessions[index]);open_detail();}}
static void open_list(void){n_sessions=capture_list(sessions,16);install(LIST,n_sessions+1,choose_list);update();}
static void open_detail(void){install(DETAIL,17,choose_detail);update();}
static double number(struct spa_json *object,const char *key){const char *v;return spa_json_object_find(object,key,&v)>0?strtod(v,NULL):0;}
static void string(struct spa_json *object,const char *key,char *out,unsigned size){const char *v;int len=spa_json_object_find(object,key,&v);out[0]=0;if(len>0)spa_json_parse_stringn(v,len,out,size);}
static void update(void){if(view==CLOSED||view==STOP||!tui.menu)return;struct tui_menu *m=tui.menu;wstring_clear(&m->header);for(unsigned i=0;i<m->n_items;i++){wstring_clear(&m->items[i].wstr);m->items[i].data.uint=i;}
    if(view==LIST){wstring_printf(&m->header,L"Audio history / multitrack recording | Enter: open  Esc: close");for(unsigned i=0;i<n_sessions;i++)wstring_printf(&m->items[i].wstr,L"%s",sessions[i]);wstring_printf(&m->items[n_sessions].wstr,L"Create audio history...");return;}
    if(view==CREATE){unsigned count=0;for(unsigned i=0;i<n_nodes;i++)count+=marked[i];wstring_printf(&m->header,L"Select sources (%u/8) | Enter/Space: toggle  Esc: cancel",count);wstring_printf(&m->items[0].wstr,L"Start 10-second cache");wstring_printf(&m->items[1].wstr,L"Start 30-second cache");for(unsigned i=0;i<n_nodes;i++)wstring_printf(&m->items[i+2].wstr,L"[%c] %s",marked[i]?'x':' ',nodes[i]);return;}
    wstring_printf(&m->header,L"History %s | %s | Enter: action  Esc: close",selected,pending()?"operation pending":"live");
    wstring_printf(&m->items[0].wstr,L"Export latest %u seconds",history_seconds>=5?5:history_seconds?:1);wstring_printf(&m->items[1].wstr,L"Export entire cache");wstring_printf(&m->items[2].wstr,L"%s continuous multitrack recording",recording?"Stop":"Start");wstring_printf(&m->items[3].wstr,L"Stop cache...");wstring_printf(&m->items[4].wstr,L"Back to sessions");
    if(pending()){wstring_printf(&m->items[5].wstr,L"Waiting for operation to finish...");return;}
    struct capture_reply reply;int result=capture_request(selected,CAPTURE_STATUS,NULL,0,80,&reply);if(result<0){wstring_printf(&m->items[5].wstr,L"Session unavailable: %s",strerror(-result));return;}
    struct spa_json root=SPA_JSON_INIT(reply.text,strlen(reply.text)),object;if(spa_json_enter_object(&root,&object)<=0)return;
    recording=number(&object,"recording")!=0;const char *v;int len=spa_json_object_find(&object,"recording",&v);if(len>0)spa_json_parse_bool(v,len,&recording);history_seconds=number(&object,"history_seconds");
    wstring_clear(&m->items[2].wstr);wstring_printf(&m->items[2].wstr,L"%s continuous multitrack recording",recording?"Stop":"Start");
    wstring_printf(&m->items[5].wstr,L"Cache %.1f / %u s | RSS %.0f KiB | CPU %.1f%% of one core",number(&object,"buffered_seconds"),history_seconds,number(&object,"rss_kb"),number(&object,"cpu_percent_one_core"));
    wstring_printf(&m->items[6].wstr,L"Recording %s | %.1f s | %.2f MiB | disk free %.1f MiB",recording?"active":"stopped",number(&object,"recorded_seconds"),number(&object,"recorded_bytes")/1048576,number(&object,"disk_free_bytes")/1048576);
    char value[4096];string(&object,"recording_directory",value,sizeof(value));wstring_printf(&m->items[7].wstr,L"Files: %s",*value?value:"no recording yet");string(&object,"last_error",value,sizeof(value));wstring_printf(&m->items[8].wstr,L"Last error: %s",*value?value:"none");
    char key[64];while((len=spa_json_object_next(&object,key,sizeof(key),&v))>0)if(!strcmp(key,"tracks")){struct spa_json tracks;spa_json_enter(&object,&tracks);unsigned index=0;while((len=spa_json_next(&tracks,&v))>0&&index<8){struct spa_json track;spa_json_enter(&tracks,&track);bool available=false;len=spa_json_object_find(&track,"available",&v);if(len>0)spa_json_parse_bool(v,len,&available);string(&track,"source",value,sizeof(value));wstring_printf(&m->items[9+index].wstr,L"%u %s | drops %.0f gaps %.0f | %s",index+1,available?"audio":"missing",number(&track,"dropped_frames"),number(&track,"gap_frames"),value);index++;}break;}
}
bool recording_ui_poll(void){capture_reap();bool changed=false;
    if(starting_pending){int result=capture_child_poll(&starting);if(result!=-EAGAIN||diagnostics_now_ns()>deadline){starting_pending=false;if(result||diagnostics_now_ns()>deadline){capture_child_cancel(&starting);tui_notice("Cannot start history; check source availability and the history worker log");}else tui_notice("Audio history started; cache continues after closing the TUI");changed=true;}}
    if(command_pid){if(error_fd>=0&&error_size<sizeof(command_error)-1){ssize_t n=read(error_fd,command_error+error_size,sizeof(command_error)-1-error_size);if(n>0){error_size+=n;command_error[error_size]=0;}}int status;pid_t result=waitpid(command_pid,&status,WNOHANG);if(result>0){command_pid=0;if(error_fd>=0)close(error_fd);error_fd=-1;if(!WIFEXITED(status)||WEXITSTATUS(status))tui_notice(*command_error?command_error:"Recording operation failed");else{char text[4352];snprintf(text,sizeof(text),"Recording operation completed%s%s",*command_path?": ":"",command_path);tui_notice(text);}changed=true;if(quit_pending){quit_pending=false;tui_bind_quit((union tui_bind_data){0});}}}
    if(view!=CLOSED&&tui.menu_active&&diagnostics_now_ns()-last_poll>=250000000ull){last_poll=diagnostics_now_ns();update();changed=true;}return changed;
}
bool recording_ui_key(wint_t key){if(view==CLOSED)return false;if(pending()&&key!=27&&key!=KEY_RESIZE&&key!='q'){tui_notice("Recording operation in progress");return true;}if(key==' '&&view==CREATE){choose_create(tui.menu,&tui.menu->items[tui.menu->selected]);return true;}return false;}
void tui_bind_manage_recording(union tui_bind_data data){tui_bind_cancel_selection((union tui_bind_data){0});open_list();}
