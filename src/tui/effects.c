#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tui/effects.h"
#include "tui/tui.h"
#include "tui/menu.h"
#include "pw/effect-chain.h"
#include "pw/managed.h"
#include "scene.h"
#include "automation.h"
#include "utils.h"

enum view { CLOSED, CHAIN, ADD, PARAMETERS, REMOVE };
static enum view view;
static char group[49],stage_id[25];
static struct effect_chain chain;
static struct scene_job *pending;
static bool reopen;
static unsigned selection;
static char opened_serial[32];
static bool quit_pending;

bool effect_ui_active(void){return view!=CLOSED||pending;}
bool effect_ui_quit(void){if(!pending)return false;quit_pending=true;tui_notice("Completing the chain edit before exit...");return true;}
void effect_ui_cancel(void){view=CLOSED;}
void effect_ui_cleanup(void){effect_ui_cancel();scene_job_free(pending,true);pending=NULL;}
static const struct graph_node *current(void){return managed_find("effect",group,"input");}
static bool read_chain(void){int r=effect_chain_read(current(),&chain);if(r<0){tui_notice(r==-EAGAIN?"Effect parameters are loading":r==-ENOENT?"Effect disappeared":strerror(-r));return false;}return true;}
static int selected_stage(void){for(unsigned i=0;i<chain.count;i++)if(streq(stage_id,chain.stages[i].id))return i;return -1;}
static void open_chain(void);
static void update(void);
static int manual_controls(const struct graph_node *node,const char *names[],const double values[],unsigned count){
    int result=graph_validate_controls(node->id,names,values,count);
    for(unsigned i=0;result>=0&&i<count;i++)result=automation_take_control(node->id,names[i]);
    return result<0?result:graph_set_controls(node->id,names,values,count);
}
static void close_menu(void){tui_bind_cancel_selection((union tui_bind_data){0});}
static void install(enum view next,unsigned count,tui_menu_callback_t callback){view=next;tui.menu=tui_menu_create(count);tui.menu->callback=callback;tui.menu_active=true;tui_menu_resize(tui.menu,tui.term_width,tui.term_height);}
static void apply(void){char error[256]={0};pending=scene_edit_effect(group,&chain,error,sizeof(error));if(!pending)tui_notice(error[0]?error:strerror(errno));else tui_notice("Applying chain; restoring its connections...");}
static void bypass(bool whole){
    if(!read_chain())return;
    const struct graph_node *node=current();char wet[96]="wet:Mult",dry[96]="dry:Mult";bool bypass;
    if(whole)bypass=chain.wet==0&&chain.dry==1;
    else{int i=selected_stage();if(i<0)return;bypass=chain.stages[i].bypass;snprintf(wet,sizeof(wet),"pm_%s_wet:Mult",stage_id);snprintf(dry,sizeof(dry),"pm_%s_dry:Mult",stage_id);
        if(!graph_control_find(node,wet)){chain.stages[i].bypass=!bypass;apply();return;}}
    const char *names[]={wet,dry};double values[]={bypass?1:0,bypass?0:1};int r=manual_controls(node,names,values,2);if(r<0)tui_notice(strerror(-r));
}
static void choose_add(struct tui_menu *menu,struct tui_menu_item *pick){if(pending)return;unsigned n;const struct effect_processor *p=effect_processors(&n);if(pick->data.uint>=n||!read_chain())return;
    int r=effect_chain_add(&chain,p[pick->data.uint].name,chain.count);if(r<0){tui_notice(strerror(-r));return;}selection=chain.count-1;apply();close_menu();open_chain();}
static void open_add(void){if(!read_chain())return;if(chain.count==EFFECT_STAGE_LIMIT){tui_notice("A chain supports at most 16 processors");return;}close_menu();unsigned n;effect_processors(&n);install(ADD,n,choose_add);for(unsigned i=0;i<n;i++)tui.menu->items[i].data.uint=i;update();}
static void parameter_reset(struct tui_menu *menu,struct tui_menu_item *pick){if(pending)return;const struct graph_node *node=current();if(!node||pick->data.uint>=node->n_controls)return;const struct graph_control *c=&node->controls[pick->data.uint];const char *names[]={c->name};int r=manual_controls(node,names,&c->default_value,1);if(r<0)tui_notice(strerror(-r));}
static void open_parameters(void){if(!read_chain())return;int index=selected_stage();if(index<0)return;const struct graph_node *node=current();unsigned count=0;
    for(unsigned i=0;i<node->n_controls;i++)if(node->controls[i].visible&&node->controls[i].writable&&effect_chain_stage_control(&chain.stages[index],node->controls[i].name))count++;
    if(!count){tui_notice("No editable parameters");return;}close_menu();install(PARAMETERS,count,parameter_reset);count=0;
    for(unsigned i=0;i<node->n_controls;i++)if(node->controls[i].visible&&node->controls[i].writable&&effect_chain_stage_control(&chain.stages[index],node->controls[i].name))tui.menu->items[count++].data.uint=i;
    update();}
static void choose_remove(struct tui_menu *menu,struct tui_menu_item *pick){if(pending)return;bool remove=pick->data.uint==1;if(!read_chain())return;int index=selected_stage();close_menu();
    if(remove&&index>=0){memmove(chain.stages+index,chain.stages+index+1,(chain.count-index-1)*sizeof(chain.stages[0]));chain.count--;apply();open_chain();}else open_chain();}
static void open_remove(void){if(!read_chain()||selected_stage()<0)return;close_menu();install(REMOVE,2,choose_remove);tui.menu->items[0].data.uint=0;tui.menu->items[1].data.uint=1;update();}
static void choose_chain(struct tui_menu *menu,struct tui_menu_item *pick){if(pending)return;unsigned index=pick->data.uint;if(index<chain.count){snprintf(stage_id,sizeof(stage_id),"%s",chain.stages[index].id);selection=index;open_parameters();}
    else if(index==chain.count)open_add();else bypass(true);}
static void open_chain(void){if(!read_chain())return;snprintf(opened_serial,sizeof(opened_serial),"%s",dict_get(&current()->props,PW_KEY_OBJECT_SERIAL)?:"");install(CHAIN,chain.count+2,choose_chain);for(unsigned i=0;i<tui.menu->n_items;i++)tui.menu->items[i].data.uint=i;tui.menu->selected=selection<chain.count?selection:chain.count;update();}
bool effect_ui_open(uint32_t id){const struct graph_node *node=graph_node_find(id);struct effect_chain candidate;int r=effect_chain_read(node,&candidate);if(r==-ENOTSUP)return false;
    if(r<0){tui_notice(r==-EAGAIN?"Effect parameters are loading; press e again":strerror(-r));return true;}
    snprintf(group,sizeof(group),"%s",dict_get(&node->props,"pipemixer.group"));selection=0;*stage_id=0;open_chain();return true;}
static void update(void){if(view==CLOSED||!tui.menu)return;
    wstring_clear(&tui.menu->header);
    if(pending){wstring_printf(&tui.menu->header,L"Chain %s | applying...",group);return;}
    const struct graph_node *node=current();if(!node){wstring_printf(&tui.menu->header,L"Effect disappeared | Esc to close");return;}
    for(unsigned i=0;i<tui.menu->n_items;i++)wstring_clear(&tui.menu->items[i].wstr);
    if(view==ADD){unsigned count;const struct effect_processor *p=effect_processors(&count);wstring_printf(&tui.menu->header,L"Add processor | Enter: add  Esc: back");for(unsigned i=0;i<count;i++)wstring_printf(&tui.menu->items[i].wstr,L"%s (%s)",p[i].label,p[i].name);return;}
    if(view==REMOVE){wstring_printf(&tui.menu->header,L"Remove %s?",stage_id);wstring_printf(&tui.menu->items[0].wstr,L"Cancel");wstring_printf(&tui.menu->items[1].wstr,L"Remove processor and its parameters");return;}
    if(effect_chain_read(node,&chain)<0)return;
    if(view==PARAMETERS){wstring_printf(&tui.menu->header,L"%s / %s | h/l: adjust  Enter: reset  Space: bypass  Esc: back",group,stage_id);
        for(unsigned i=0;i<tui.menu->n_items;i++){unsigned n=tui.menu->items[i].data.uint;if(n>=node->n_controls)continue;const struct graph_control *c=&node->controls[n];const char *label=strchr(c->name,':');wstring_printf(&tui.menu->items[i].wstr,L"%s = %.6g [%.6g .. %.6g]",label?label+1:c->name,c->value,c->minimum,c->maximum);}return;}
    unsigned n;const struct effect_processor *p=effect_processors(&n);wstring_printf(&tui.menu->header,L"Chain %s | Enter: edit  a: add  d: remove  K/J: move  Space: bypass  B: all",group);
    for(unsigned i=0;i<chain.count&&i<tui.menu->n_items-2;i++)wstring_printf(&tui.menu->items[i].wstr,L"%u. %s: %s [%s]",i+1,chain.stages[i].id,p[chain.stages[i].processor].label,chain.stages[i].bypass?"bypassed":"active");
    if(tui.menu->n_items==chain.count+2){wstring_printf(&tui.menu->items[chain.count].wstr,L"Add processor...");wstring_printf(&tui.menu->items[chain.count+1].wstr,L"Whole chain [%s]",chain.wet==0&&chain.dry==1?"bypassed":"active");}
}
bool effect_ui_key(wint_t key){
    if(pending){if(key==27){close_menu();return true;}if(key==KEY_RESIZE)return false;tui_notice("Chain edit in progress");return true;}
    if(view==CLOSED)return false;
    const struct graph_node *live=current();
    if(live&&!streq(opened_serial,dict_get(&live->props,PW_KEY_OBJECT_SERIAL))){close_menu();open_chain();return true;}
    if(key==27){if(view==CHAIN)close_menu();else{close_menu();open_chain();}return true;}
    if(key=='B'){bypass(true);return true;}
    if(view==CHAIN){unsigned index=tui.menu->selected;if(index<chain.count)snprintf(stage_id,sizeof(stage_id),"%s",chain.stages[index].id);
        if(key=='a'){open_add();return true;}if(key=='d'&&index<chain.count){open_remove();return true;}
        if(key==' '&&index<chain.count){bypass(false);return true;}
        if((key=='K'||key=='J')&&index<chain.count){if(!read_chain())return true;int other=(int)index+(key=='K'?-1:1);if(other<0||(unsigned)other>=chain.count)return true;
            struct effect_stage tmp=chain.stages[index];chain.stages[index]=chain.stages[other];chain.stages[other]=tmp;selection=other;apply();return true;}
        return false;
    }
    if(view!=PARAMETERS)return false;
    if(key==' '){bypass(false);return true;}if(key!='h'&&key!='l'&&key!=KEY_LEFT&&key!=KEY_RIGHT)return false;
    const struct graph_node *node=current();if(!node)return true;unsigned index=tui.menu->items[tui.menu->selected].data.uint;if(index>=node->n_controls)return true;
    const struct graph_control *c=&node->controls[index];double step=(c->maximum-c->minimum)/100;int s=selected_stage();unsigned n;const struct effect_processor *p=effect_processors(&n);
    if(s>=0)for(unsigned i=0;i<p[chain.stages[s].processor].count;i++)if(streq(strchr(c->name,':')+1,p[chain.stages[s].processor].params[i].name))step=p[chain.stages[s].processor].params[i].step;
    if(!step)step=fmax(1,fabs(c->value)*.1);
    double value=fmin(c->maximum,fmax(c->minimum,c->value+((key=='h'||key==KEY_LEFT)?-step:step)));
    const char *names[]={c->name};int r=manual_controls(node,names,&value,1);if(r<0)tui_notice(strerror(-r));return true;
}
bool effect_ui_poll(void){if(!effect_ui_active())return false;
    if(pending){char error[256]={0};int r=scene_step(pending,error,sizeof(error));if(r==-EAGAIN){update();return true;}
        scene_job_free(pending,r<0);pending=NULL;tui_notice(r<0?(error[0]?error:strerror(-r)):"Chain updated; connections restored");
        reopen=view!=CLOSED;if(reopen){close_menu();open_chain();}}
    if(!pending&&quit_pending){quit_pending=false;tui_bind_quit((union tui_bind_data){0});return true;}
    const struct graph_node *live=current();
    if(!pending&&view!=CLOSED&&live&&!streq(opened_serial,dict_get(&live->props,PW_KEY_OBJECT_SERIAL))){close_menu();open_chain();}
    update();return true;
}
