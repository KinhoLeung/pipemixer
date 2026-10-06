#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sound/asound.h>
#include "external-control.h"
#include "diagnostics.h"
#include "eventloop.h"
#include "utils.h"
#define PACKET_LIMIT 4096
#define MESSAGE_LIMIT 16
struct external_control {
    control_handler handler;void *data;
    int osc,midi;struct spa_source *osc_io,*midi_io;
    char bind[16],device[512],osc_error[160],midi_error[160];unsigned port;
    bool direct,sysex;
    unsigned char status,bytes[2];unsigned count,system_remaining;
    uint64_t retry,rate_at,osc_received,midi_received,rejected,dropped;
    unsigned tokens;
};
static uint32_t read32(const unsigned char *p){uint32_t v;memcpy(&v,p,4);return ntohl(v);}
static bool osc_string(const unsigned char *packet,size_t size,size_t *at,char *out,size_t capacity){if(*at>=size)return false;const unsigned char *start=packet+*at,*nul=memchr(start,0,size-*at);if(!nul)return false;
    size_t length=nul-start,padded=(length+4)&~3u;if(length>=capacity||padded>size-*at)return false;for(size_t i=length;i<padded;i++)if(start[i])return false;
    memcpy(out,start,length);out[length]=0;*at+=padded;return true;
}
static bool osc_number(const unsigned char *packet,size_t size,size_t *at,char type,double *number){if(*at+4>size||(type!='i'&&type!='f'))return false;uint32_t bits=read32(packet+*at);*at+=4;
    if(type=='i')*number=(int32_t)bits;else{float f;memcpy(&f,&bits,4);*number=f;}return isfinite(*number);
}
static bool osc_duration(double value,unsigned *out){if(value<0||value>3600000||floor(value)!=value||(value>0&&value<20))return false;*out=value;return true;}
static int decode_message(const unsigned char *packet,size_t size,struct control_event *event){size_t at=0;char tags[16];*event=(struct control_event){0};
    if(!osc_string(packet,size,&at,event->address,sizeof(event->address))||event->address[0]!='/'||strpbrk(event->address," *?[]{}#,\t\r\n")||!osc_string(packet,size,&at,tags,sizeof(tags))||tags[0]!=',')return -EINVAL;
    event->kind=CONTROL_OSC;unsigned count=strlen(tags)-1;double duration=0;
    if(streq(event->address,"/pipemixer/volume")){event->kind=CONTROL_VOLUME;
        if(count<2||count>4||tags[1]!='s'||(tags[2]!='f'&&tags[2]!='i')||!osc_string(packet,size,&at,event->target,sizeof(event->target))||!osc_number(packet,size,&at,tags[2],&event->value)||event->value<0||event->value>150)return -EINVAL;
        if(count>=3&&(tags[3]!='i'||!osc_number(packet,size,&at,'i',&duration)||!osc_duration(duration,&event->duration)))return -EINVAL;
        if(count==4){char curve[16];if(tags[4]!='s'||!osc_string(packet,size,&at,curve,sizeof(curve))||(!streq(curve,"linear")&&!streq(curve,"smooth")))return -EINVAL;event->smooth=streq(curve,"smooth");}
    }else if(streq(event->address,"/pipemixer/mute")){event->kind=CONTROL_MUTE;
        if(count!=2||tags[1]!='s'||tags[2]!='i'||!osc_string(packet,size,&at,event->target,sizeof(event->target))||!osc_number(packet,size,&at,'i',&event->value)||(event->value!=0&&event->value!=1))return -EINVAL;
    }else if(streq(event->address,"/pipemixer/parameter")){event->kind=CONTROL_PARAMETER;
        if(count<3||count>5||tags[1]!='s'||tags[2]!='s'||(tags[3]!='f'&&tags[3]!='i')||!osc_string(packet,size,&at,event->target,sizeof(event->target))||!osc_string(packet,size,&at,event->parameter,sizeof(event->parameter))||!osc_number(packet,size,&at,tags[3],&event->value))return -EINVAL;
        if(count>=4&&(tags[4]!='i'||!osc_number(packet,size,&at,'i',&duration)||!osc_duration(duration,&event->duration)))return -EINVAL;
        if(count==5){char curve[16];if(tags[5]!='s'||!osc_string(packet,size,&at,curve,sizeof(curve))||(!streq(curve,"linear")&&!streq(curve,"smooth")))return -EINVAL;event->smooth=streq(curve,"smooth");}
    }else if(streq(event->address,"/pipemixer/scene")||streq(event->address,"/pipemixer/rule")){event->kind=streq(event->address,"/pipemixer/scene")?CONTROL_SCENE:CONTROL_RULE;
        if(count!=1||tags[1]!='s'||!osc_string(packet,size,&at,event->target,sizeof(event->target)))return -EINVAL;
    }else {if(count>1||(count==1&&!osc_number(packet,size,&at,tags[1],&event->value))||event->value<0||event->value>1)return -EINVAL;}
    if(event->kind!=CONTROL_OSC&&!*event->target)return -EINVAL;
    return at==size?0:-EINVAL;
}
static int decode_packet(const unsigned char *packet,size_t size,struct control_event events[],unsigned *count,unsigned depth){if(!size||size%4||depth>4)return -EINVAL;
    if(size>=8&&!memcmp(packet,"#bundle\0",8)){if(size<16)return -EINVAL;if(read32(packet+8)||read32(packet+12)!=1)return -ENOTSUP;size_t at=16;
        while(at<size){if(at+4>size)return -EINVAL;uint32_t length=read32(packet+at);at+=4;if(!length||length>size-at)return -EINVAL;int result=decode_packet(packet+at,length,events,count,depth+1);if(result<0)return result;at+=length;}return 0;
    }
    if(*count==MESSAGE_LIMIT)return -E2BIG;
    int result=decode_message(packet,size,&events[*count]);if(!result)(*count)++;return result;
}
static size_t put_string(unsigned char *packet,size_t at,const char *text){size_t len=strlen(text)+1,padded=(len+3)&~3u;memset(packet+at,0,padded);memcpy(packet+at,text,len);return at+padded;}
static void reply_osc(struct external_control *c,const struct sockaddr_in *peer,socklen_t size,const char *address,int result,const char *message){unsigned char packet[1400];size_t at=put_string(packet,0,"/pipemixer/reply");at=put_string(packet,at,",sis");at=put_string(packet,at,address);uint32_t code=htonl((uint32_t)result);memcpy(packet+at,&code,4);at+=4;char text[1024];snprintf(text,sizeof(text),"%s",message?:"");at=put_string(packet,at,text);ssize_t n=sendto(c->osc,packet,at,MSG_NOSIGNAL,(const struct sockaddr*)peer,size);(void)n;}
static void osc_ready(void *data,int fd,uint32_t mask){struct external_control *c=data;
    for(unsigned n=0;n<16;n++){unsigned char packet[PACKET_LIMIT];struct sockaddr_in peer;socklen_t size=sizeof(peer);ssize_t bytes=recvfrom(fd,packet,sizeof(packet),MSG_TRUNC,(struct sockaddr*)&peer,&size);if(bytes<0)return;c->osc_received++;
        if(!c->tokens){c->dropped++;continue;}c->tokens--;
        struct control_event events[MESSAGE_LIMIT];unsigned count=0;int result=bytes>PACKET_LIMIT?-E2BIG:decode_packet(packet,bytes,events,&count,0);char message[1024]={0};
        if(!result){for(unsigned i=0;i<count;i++){if(events[i].kind!=CONTROL_OSC&&!c->direct){result=-EACCES;snprintf(message,sizeof(message),"Direct OSC controls are disabled; use a configured mapping");break;}
                result=c->handler(c->data,&events[i],message,sizeof(message));if(result<0)break;}}
        if(result<0){c->rejected++;if(!*message)snprintf(message,sizeof(message),"%s",strerror(-result));}
        reply_osc(c,&peer,size,count==1?events[0].address:"#bundle",result,message);
    }
}
static void midi_message(struct external_control *c){unsigned type=c->status&0xf0;struct control_event event={.kind=CONTROL_MIDI,.channel=(c->status&15)+1,.number=c->bytes[0]};
    if(type==0xb0){event.midi_kind=0;event.value=c->bytes[1]/127.0;}else if(type==0x90||type==0x80){event.midi_kind=1;event.release=type==0x80||c->bytes[1]==0;event.value=event.release?0:c->bytes[1]/127.0;}else if(type==0xc0){event.midi_kind=2;event.value=c->bytes[0]/127.0;}else return;
    c->midi_received++;char error[160]={0};int result=c->handler(c->data,&event,error,sizeof(error));if(result<0&&result!=-ENOENT&&result!=-EAGAIN){c->rejected++;snprintf(c->midi_error,sizeof(c->midi_error),"%s",*error?error:strerror(-result));}
}
static void midi_byte(struct external_control *c,unsigned char byte){if(byte>=0xf8)return;
    if(byte&0x80){c->count=0;c->system_remaining=0;if(byte>=0xf0){c->status=0;c->sysex=byte==0xf0;if(byte==0xf1||byte==0xf3)c->system_remaining=1;else if(byte==0xf2)c->system_remaining=2;return;}c->sysex=false;c->status=byte;return;}
    if(c->sysex)return;
    if(c->system_remaining){c->system_remaining--;return;}if(!c->status)return;
    unsigned length=(c->status&0xe0)==0xc0?1:2;c->bytes[c->count++]=byte;if(c->count==length){midi_message(c);c->count=0;}
}
static void close_midi(struct external_control *c){if(c->midi_io)pw_loop_destroy_source(event_loop,c->midi_io);c->midi_io=NULL;if(c->midi>=0)close(c->midi);c->midi=-1;c->status=c->count=c->system_remaining=0;c->sysex=false;}
static void close_osc(struct external_control *c){if(c->osc_io)pw_loop_destroy_source(event_loop,c->osc_io);c->osc_io=NULL;if(c->osc>=0)close(c->osc);c->osc=-1;}
static void midi_ready(void *data,int fd,uint32_t mask){struct external_control *c=data;unsigned char bytes[512];ssize_t count=read(fd,bytes,sizeof(bytes));
    if(count<0&&(errno==EAGAIN||errno==EINTR)&&!(mask&(SPA_IO_HUP|SPA_IO_ERR)))return;
    if(count<=0){snprintf(c->midi_error,sizeof(c->midi_error),"MIDI disconnected; retrying configured device");close_midi(c);return;}
    for(ssize_t i=0;i<count;i++)midi_byte(c,bytes[i]);
}
struct external_control *external_control_create(control_handler handler,void *data){struct external_control *c=calloc(1,sizeof(*c));if(c){c->handler=handler;c->data=data;c->osc=c->midi=-1;c->tokens=200;}return c;}
void external_control_configure(struct external_control *c,const struct auto_config *config){if(!c)return;const char *bind=config?config->osc_bind:"127.0.0.1",*device=config?config->midi_device:"";unsigned port=config?config->osc_port:0;
    if(!streq(bind,c->bind)||port!=c->port)close_osc(c);
    if(!streq(device,c->device))close_midi(c);
    snprintf(c->bind,sizeof(c->bind),"%s",bind);snprintf(c->device,sizeof(c->device),"%s",device);c->port=port;c->direct=config&&config->osc_direct;c->retry=0;
    if(!port)c->osc_error[0]=0;
    if(!*device)c->midi_error[0]=0;
}
void external_control_tick(struct external_control *c,uint64_t now){if(!c)return;if(now-c->rate_at>=1000000000ull){c->tokens=200;c->rate_at=now;}
    if(now-c->retry<1000000000ull)return;
    c->retry=now;
    if(c->port&&c->osc<0){int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(c->port)};inet_pton(AF_INET,c->bind,&addr.sin_addr);
        if(fd<0||bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0){snprintf(c->osc_error,sizeof(c->osc_error),"%s",strerror(errno));if(fd>=0)close(fd);}
        else{c->osc=fd;c->osc_io=pw_loop_add_io(event_loop,fd,SPA_IO_IN,false,osc_ready,c);if(!c->osc_io){close_osc(c);snprintf(c->osc_error,sizeof(c->osc_error),"Cannot watch OSC socket");}else c->osc_error[0]=0;}
    }
    if(*c->device&&c->midi<0){int fd=open(c->device,O_RDONLY|O_NONBLOCK|O_CLOEXEC);struct stat st;int result=fd<0?-errno:fstat(fd,&st)<0?-errno:!S_ISCHR(st.st_mode)?-EINVAL:0;
        if(!result){struct snd_rawmidi_params params={.stream=SNDRV_RAWMIDI_STREAM_INPUT,.buffer_size=4096,.avail_min=1,.no_active_sensing=1};if(ioctl(fd,SNDRV_RAWMIDI_IOCTL_PARAMS,&params)<0&&errno!=ENOTTY)result=-errno;}
        if(result<0){snprintf(c->midi_error,sizeof(c->midi_error),"%s",strerror(-result));if(fd>=0)close(fd);}
        else{c->midi=fd;c->midi_io=pw_loop_add_io(event_loop,fd,SPA_IO_IN|SPA_IO_HUP|SPA_IO_ERR,false,midi_ready,c);if(!c->midi_io){close_midi(c);snprintf(c->midi_error,sizeof(c->midi_error),"Cannot watch MIDI device");}else c->midi_error[0]=0;}
    }
}
void external_control_destroy(struct external_control *c){if(!c)return;close_osc(c);close_midi(c);free(c);}
void external_control_status(struct external_control *c,FILE *out){if(!c){fputs("null",out);return;}char *bind=json_quote(c->bind),*device=json_quote(c->device),*osc_error=json_quote(c->osc_error),*midi_error=json_quote(c->midi_error);
    fprintf(out,"{\"osc_bind\":%s,\"osc_port\":%u,\"osc_listening\":%s,\"osc_direct\":%s,\"midi_device\":%s,\"midi_connected\":%s,\"osc_received\":%"PRIu64",\"midi_received\":%"PRIu64",\"rejected\":%"PRIu64",\"dropped\":%"PRIu64",\"osc_error\":%s,\"midi_error\":%s}",bind,c->port,c->osc>=0?"true":"false",c->direct?"true":"false",device,c->midi>=0?"true":"false",c->osc_received,c->midi_received,c->rejected,c->dropped,osc_error,midi_error);
    free(bind);free(device);free(osc_error);free(midi_error);
}
