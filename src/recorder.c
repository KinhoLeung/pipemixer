#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/utils/json.h>
#include "recorder.h"
#include "diagnostics.h"
#include "pw/common.h"
#include "pw/graph.h"
#include "pw/managed.h"
#include "eventloop.h"
#include "utils.h"
#include "xmalloc.h"

#define RATE 48000u
#define CHANNELS 2u
#define BLOCK 2048u
#define QUEUE 128u
#define MAGIC 0x504d4831u
#define LAG 4800u
struct request { unsigned magic,op,seconds; char directory[4096]; };
struct chunk { uint64_t frame;unsigned frames;float pcm[BLOCK*CHANNELS]; };
struct recording {
    bool active;
    char directory[4096],error[256];
    unsigned part;
    uint64_t start,next,part_frames,part_limit,bytes,checkpoint_ns,space_ns;
    uint64_t gaps[CAPTURE_TRACK_LIMIT];
    FILE *files[CAPTURE_TRACK_LIMIT];
};
struct session;
struct track {
    struct session *session;char name[512],serial[32];
    struct pw_stream *stream;
    atomic_bool format_ok;
    atomic_uint write_index,read_index;
    atomic_uint_fast64_t received,dropped,invalid,last_audio,first_end;
    struct chunk *queue;
    uint64_t next_frame;
    float *history;unsigned char *valid;
    uint64_t end,gaps;double sum;float peak;
};
struct session {
    char name[49],socket_path[108];
    unsigned count,seconds,capacity,reserve_mb;
    uint64_t origin,common_end;
    uint64_t cpu_at,cpu_ns;
    double cpu_percent;
    struct event_hook *hook;
    int server,lock,ready_fd,reply_fd;
    atomic_bool ready;pthread_t writer;
    struct spa_source *io,*timer,*signals[2];
    atomic_bool shutdown;
    atomic_uint request_pending;
    struct request request;
    struct recording recording;
    char error[256];
    struct track tracks[CAPTURE_TRACK_LIMIT];
};
static pid_t children[16];
static uint64_t frame_now(const struct session *s){uint64_t elapsed=diagnostics_now_ns()-s->origin;return elapsed/1000000000*RATE+elapsed%1000000000*RATE/1000000000;}
uint64_t capture_memory_budget(unsigned seconds,unsigned tracks){return (uint64_t)tracks*((uint64_t)(seconds*RATE+LAG*2)*CHANNELS*sizeof(float)+(uint64_t)(seconds*RATE+LAG*2)+QUEUE*sizeof(struct chunk))+8u*1024u*1024u;}
static int runtime_path(const char *name,char *path,size_t size,const char *suffix){
    if(name&&!managed_valid_name(name))return -EINVAL;
    const char *runtime=getenv("XDG_RUNTIME_DIR");if(!runtime||!*runtime)return -ENOENT;
    struct stat st;if(lstat(runtime,&st)<0)return -errno;if(!S_ISDIR(st.st_mode)||st.st_uid!=getuid())return -EPERM;
    const char *config=getenv("XDG_CONFIG_HOME");if(!config)config=getenv("HOME");uint64_t hash=1469598103934665603ull;
    for(const char *p=config?:"";*p;p++){hash^=(unsigned char)*p;hash*=1099511628211ull;}
    int length=snprintf(path,size,"%s/pipemixer-history-%016"PRIx64"-%s%s",runtime,hash,name?:"",suffix);
    return length<0||(size_t)length>=size?-ENAMETOOLONG:0;
}
static void error_text(char *error,unsigned size,int code){snprintf(error,size,"%s",strerror(code<0?-code:code));}
static uint64_t memory_available(void){FILE *f=fopen("/proc/meminfo","r");char line[160];uint64_t kb=0;if(f){while(fgets(line,sizeof(line),f))if(sscanf(line,"MemAvailable: %"SCNu64,&kb)==1)break;fclose(f);}return kb*1024;}
static int send_reply(int fd,int result,unsigned flags,const char *text){struct capture_reply *r=calloc(1,sizeof(*r));if(!r)return -ENOMEM;r->result=result;r->flags=flags;snprintf(r->text,sizeof(r->text),"%s",text?:"");ssize_t n=send(fd,r,sizeof(*r),MSG_NOSIGNAL);free(r);return n==(ssize_t)sizeof(struct capture_reply)?0:-EIO;}
int capture_request(const char *name,enum capture_operation op,const char *directory,unsigned seconds,unsigned timeout,struct capture_reply *reply){
    *reply=(struct capture_reply){0};
    char path[108];int result=runtime_path(name,path,sizeof(path),".sock");if(result<0)return result;
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);if(fd<0)return -errno;
    struct timeval tv={.tv_sec=timeout/1000,.tv_usec=timeout%1000*1000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    struct sockaddr_un address={.sun_family=AF_UNIX};snprintf(address.sun_path,sizeof(address.sun_path),"%s",path);
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))<0){result=-errno;close(fd);return result;}
    struct request request={.magic=MAGIC,.op=op,.seconds=seconds};
    if(directory){if(*directory=='/')snprintf(request.directory,sizeof(request.directory),"%s",directory);else{char cwd[2048];if(!getcwd(cwd,sizeof(cwd))){close(fd);return -errno;}if(snprintf(request.directory,sizeof(request.directory),"%s/%s",cwd,directory)>=(int)sizeof(request.directory)){close(fd);return -ENAMETOOLONG;}}if(strlen(directory)>=sizeof(request.directory)){close(fd);return -ENAMETOOLONG;}}
    if(send(fd,&request,sizeof(request),MSG_NOSIGNAL)!=(ssize_t)sizeof(request)){close(fd);return -EIO;}
    ssize_t bytes=recv(fd,reply,sizeof(*reply),MSG_TRUNC);result=bytes==(ssize_t)sizeof(*reply)?reply->result:-EIO;reply->text[sizeof(reply->text)-1]=0;close(fd);return result;
}
unsigned capture_list(char names[][49],unsigned limit){char prefix[108];if(runtime_path(NULL,prefix,sizeof(prefix),"")<0)return 0;char *slash=strrchr(prefix,'/');if(!slash)return 0;*slash=0;DIR *dir=opendir(prefix);if(!dir)return 0;const char *base=slash+1;size_t length=strlen(base);unsigned count=0;struct dirent *de;
    while(count<limit&&(de=readdir(dir))){size_t n=strlen(de->d_name);if(n<=length+5||strncmp(de->d_name,base,length)||strcmp(de->d_name+n-5,".sock"))continue;size_t size=n-length-5;if(size>48)continue;memcpy(names[count],de->d_name+length,size);names[count][size]=0;if(managed_valid_name(names[count]))count++;}closedir(dir);return count;
}
void capture_reap(void){for(unsigned i=0;i<16;i++)if(children[i]>0&&waitpid(children[i],NULL,WNOHANG)>0)children[i]=0;}
int capture_child_poll(struct capture_child *child){if(child->ready_fd<0)return 0;unsigned char status;ssize_t n=read(child->ready_fd,&status,1);if(n<0&&(errno==EAGAIN||errno==EINTR))return -EAGAIN;close(child->ready_fd);child->ready_fd=-1;return n==1&&status==1?0:-EIO;}
void capture_child_cancel(struct capture_child *child){if(child->ready_fd>=0)close(child->ready_fd);child->ready_fd=-1;if(child->pid>0){kill(child->pid,SIGTERM);waitpid(child->pid,NULL,0);child->pid=0;}}
int capture_spawn(const char *name,unsigned seconds,const char *const *sources,unsigned count,struct capture_child *child,char *error,unsigned size){
    if(!managed_valid_name(name)||!count||count>CAPTURE_TRACK_LIMIT||!seconds||seconds>120){error_text(error,size,EINVAL);return -EINVAL;}
    if(capture_memory_budget(seconds,count)>CAPTURE_MEMORY_LIMIT){snprintf(error,size,"History and capture queues exceed the 64 MiB session budget; reduce duration or tracks");return -E2BIG;}
    if(memory_available()<capture_memory_budget(seconds,count)+64u*1024u*1024u){snprintf(error,size,"Insufficient available RAM for the requested cache and 64 MiB system reserve");return -ENOMEM;}
    char targets[CAPTURE_TRACK_LIMIT][512];for(unsigned i=0;i<count;i++){uint32_t id;int r=graph_resolve_node(sources[i],&id);if(r<0){snprintf(error,size,"Source unavailable: %s",sources[i]);return r;}const char *name=graph_node_name(id);if(!name||strlen(name)>=512){error_text(error,size,ENAMETOOLONG);return -ENAMETOOLONG;}snprintf(targets[i],512,"%s",name);for(unsigned j=0;j<i;j++)if(!strcmp(targets[i],targets[j])){snprintf(error,size,"Duplicate track source");return -EINVAL;}}
    struct capture_reply reply;if(capture_request(name,CAPTURE_STATUS,NULL,0,200,&reply)==0){snprintf(error,size,"History session already exists");return -EEXIST;}
    char path[108];int r=runtime_path(name,path,sizeof(path),".sock");if(r<0){error_text(error,size,r);return r;}
    int fds[2];if(pipe2(fds,O_CLOEXEC|O_NONBLOCK)<0)return -errno;
    pid_t pid=fork();if(pid<0){close(fds[0]);close(fds[1]);return -errno;}
    if(!pid){close(fds[0]);setsid();umask(077);int null=open("/dev/null",O_RDWR);dup2(null,STDIN_FILENO);dup2(null,STDOUT_FILENO);
        char log[160];snprintf(log,sizeof(log),"%s.log",path);int fd=open(log,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);if(fd>=0){dup2(fd,STDERR_FILENO);close(fd);}else dup2(null,STDERR_FILENO);close(null);
        fcntl(fds[1],F_SETFD,0);char duration[24],ready[24];snprintf(duration,sizeof(duration),"%u",seconds);snprintf(ready,sizeof(ready),"%d",fds[1]);char *args[14]={"pipemixer","__capture-worker",(char*)name,duration,ready};for(unsigned i=0;i<count;i++)args[5+i]=targets[i];execv("/proc/self/exe",args);_exit(127);
    }
    close(fds[1]);*child=(struct capture_child){pid,fds[0]};capture_reap();for(unsigned i=0;i<16;i++)if(!children[i]){children[i]=pid;break;}return 0;
}
static void process(void *data){struct track *t=data;struct session *s=t->session;struct pw_buffer *buffer;
    while((buffer=pw_stream_dequeue_buffer(t->stream))){struct spa_buffer *b=buffer->buffer;
        if(atomic_load_explicit(&t->format_ok,memory_order_relaxed)&&b->n_datas){struct spa_data *a=&b->datas[0];struct spa_chunk *c=a->chunk;if(a->data&&c&&c->offset<a->maxsize){unsigned bytes=c->size<a->maxsize-c->offset?c->size:a->maxsize-c->offset;unsigned frames=bytes/(CHANNELS*sizeof(float));const float *pcm=(const float*)((char*)a->data+c->offset);
            uint64_t ns=buffer->time?buffer->time:pw_stream_get_nsec(t->stream);uint64_t elapsed=ns>s->origin?ns-s->origin:0,frame=elapsed/1000000000*RATE+elapsed%1000000000*RATE/1000000000;
            if(t->next_frame&&llabs((long long)frame-(long long)t->next_frame)<RATE/20)frame=t->next_frame;
            for(unsigned offset=0;offset<frames;){unsigned n=frames-offset>BLOCK?BLOCK:frames-offset;unsigned w=atomic_load_explicit(&t->write_index,memory_order_relaxed),r=atomic_load_explicit(&t->read_index,memory_order_acquire);
                if(w-r==QUEUE){atomic_fetch_add_explicit(&t->dropped,n,memory_order_relaxed);}else{struct chunk *chunk=&t->queue[w%QUEUE];chunk->frame=frame+offset;chunk->frames=n;
                    for(unsigned j=0;j<n*CHANNELS;j++){float v=pcm[offset*CHANNELS+j];if(!isfinite(v)){v=0;atomic_fetch_add_explicit(&t->invalid,1,memory_order_relaxed);}chunk->pcm[j]=v;}
                    atomic_store_explicit(&t->write_index,w+1,memory_order_release);}
                offset+=n;
            }t->next_frame=frame+frames;if(frames&&!atomic_load_explicit(&t->first_end,memory_order_relaxed))atomic_store_explicit(&t->first_end,frame+frames,memory_order_release);atomic_fetch_add_explicit(&t->received,frames,memory_order_relaxed);atomic_store_explicit(&t->last_audio,diagnostics_now_ns(),memory_order_relaxed);
        }}pw_stream_queue_buffer(t->stream,buffer);
    }
}
static void format(void *data,uint32_t id,const struct spa_pod *pod){if(id!=SPA_PARAM_Format)return;struct track *t=data;struct spa_audio_info_raw info={0};bool valid=pod&&spa_format_audio_raw_parse(pod,&info)>=0&&info.format==SPA_AUDIO_FORMAT_F32&&info.rate==RATE&&info.channels==CHANNELS;atomic_store(&t->format_ok,valid);}
static const struct pw_stream_events stream_events={.version=PW_VERSION_STREAM_EVENTS,.param_changed=format,.process=process};
static int connect_track(struct track *t,const struct graph_node *node){char name[128];snprintf(name,sizeof(name),"pipemixer.history.%s.%u",t->session->name,(unsigned)(t-t->session->tracks)+1);const char *cls=dict_get(&node->props,PW_KEY_MEDIA_CLASS);
    struct pw_properties *props=pw_properties_new(PW_KEY_NODE_NAME,name,PW_KEY_MEDIA_TYPE,"Audio",PW_KEY_MEDIA_CATEGORY,"Capture",PW_KEY_TARGET_OBJECT,dict_get(&node->props,PW_KEY_OBJECT_SERIAL),"pipemixer.internal","true","stream.monitor","true","node.dont-fallback","true",PW_KEY_NODE_DONT_RECONNECT,"true","state.restore-props","false","state.restore-target","false",NULL);
    if(!strcmp(cls?:"","Audio/Sink")||!strcmp(cls?:"","Stream/Input/Audio"))pw_properties_set(props,PW_KEY_STREAM_CAPTURE_SINK,"true");
    t->stream=pw_stream_new_simple(event_loop,"PipeMixer history / multitrack capture",props,&stream_events,t);if(!t->stream)return -errno;
    snprintf(t->serial,sizeof(t->serial),"%s",dict_get(&node->props,PW_KEY_OBJECT_SERIAL)?:"");uint8_t storage[512];struct spa_pod_builder builder=SPA_POD_BUILDER_INIT(storage,sizeof(storage));const struct spa_pod *pod=spa_format_audio_raw_build(&builder,SPA_PARAM_EnumFormat,&SPA_AUDIO_INFO_RAW_INIT(.format=SPA_AUDIO_FORMAT_F32,.rate=RATE,.channels=CHANNELS,.position={SPA_AUDIO_CHANNEL_FL,SPA_AUDIO_CHANNEL_FR}));
    return pw_stream_connect(t->stream,PW_DIRECTION_INPUT,PW_ID_ANY,PW_STREAM_FLAG_AUTOCONNECT|PW_STREAM_FLAG_MAP_BUFFERS|PW_STREAM_FLAG_RT_PROCESS|PW_STREAM_FLAG_DONT_RECONNECT,&pod,1);
}
static void history_zero(struct track *t,uint64_t end){struct session *s=t->session;if(end<=t->end)return;uint64_t from=t->end;if(end-from>s->capacity)from=end-s->capacity;t->gaps+=end-t->end;
    while(from<end){unsigned position=from%s->capacity,n=end-from>s->capacity-position?s->capacity-position:end-from;memset(t->history+position*CHANNELS,0,n*CHANNELS*sizeof(float));memset(t->valid+position,0,n);from+=n;}t->end=end;
}
static void history_write(struct track *t,const struct chunk *c){struct session *s=t->session;history_zero(t,c->frame);unsigned offset=0;
    while(offset<c->frames){uint64_t frame=c->frame+offset;if(t->end>frame+s->capacity){unsigned skip=t->end-s->capacity-frame;offset+=skip<c->frames-offset?skip:c->frames-offset;continue;}unsigned pos=frame%s->capacity,n=c->frames-offset>s->capacity-pos?s->capacity-pos:c->frames-offset;memcpy(t->history+pos*CHANNELS,c->pcm+offset*CHANNELS,n*CHANNELS*sizeof(float));memset(t->valid+pos,1,n);offset+=n;}
    if(c->frame+c->frames>t->end)t->end=c->frame+c->frames;
    for(unsigned i=0;i<c->frames*CHANNELS;i++){float v=fabsf(c->pcm[i]);t->peak=fmaxf(t->peak,v);t->sum+=(double)v*v;}
}
static uint64_t history_read(struct track *t,uint64_t from,unsigned frames,float *pcm){struct session *s=t->session;uint64_t missing=0;
    for(unsigned offset=0;offset<frames;){uint64_t f=from+offset;unsigned pos=f%s->capacity,n=frames-offset>s->capacity-pos?s->capacity-pos:frames-offset;
        if(f>=t->end||t->end-f>s->capacity){if(f<t->end&&n>t->end-s->capacity-f)n=t->end-s->capacity-f;memset(pcm+offset*CHANNELS,0,n*CHANNELS*sizeof(float));missing+=n;}
        else{if(n>t->end-f)n=t->end-f;memcpy(pcm+offset*CHANNELS,t->history+pos*CHANNELS,n*CHANNELS*sizeof(float));for(unsigned i=0;i<n;i++)if(!t->valid[pos+i])missing++;}offset+=n;
    }return missing;
}
static uint64_t disk_free(const char *directory){struct statvfs st;if(statvfs(directory,&st)<0)return 0;return (uint64_t)st.f_bavail*st.f_frsize;}
static int new_directory(struct session *s,const char *path,uint64_t bytes){if(!*path)return -EINVAL;if(mkdir(path,0700)<0)return -errno;
    if(disk_free(path)<bytes+(uint64_t)s->reserve_mb*1024*1024){rmdir(path);return -ENOSPC;}return 0;
}
static void u16(unsigned char *p,unsigned v){p[0]=v;p[1]=v>>8;}
static void u32(unsigned char *p,unsigned v){for(unsigned i=0;i<4;i++)p[i]=v>>(i*8);}
static int wav_header(FILE *f,uint64_t frames){unsigned char h[56]={0};memcpy(h,"RIFF",4);u32(h+4,frames*8+48);memcpy(h+8,"WAVEfmt ",8);u32(h+16,16);u16(h+20,3);u16(h+22,2);u32(h+24,RATE);u32(h+28,RATE*8);u16(h+32,8);u16(h+34,32);memcpy(h+36,"fact",4);u32(h+40,4);u32(h+44,frames);memcpy(h+48,"data",4);u32(h+52,frames*8);return pwrite(fileno(f),h,sizeof(h),0)==sizeof(h)?0:-errno;}
static FILE *wav_open(const char *directory,unsigned track,unsigned part){char path[4096];if(snprintf(path,sizeof(path),"%s/track%02u-part%04u.wav",directory,track+1,part)>=(int)sizeof(path)){errno=ENAMETOOLONG;return NULL;}
    int fd=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return NULL;FILE *f=fdopen(fd,"wb");if(!f){close(fd);return NULL;}if(wav_header(f,0)<0||fseek(f,56,SEEK_SET)<0){fclose(f);return NULL;}return f;
}
static int wav_close(FILE *f){if(!f)return 0;int result=fflush(f)==0?0:-EIO;struct stat st;
    if(fstat(fileno(f),&st)==0&&st.st_size>=56){uint64_t frames=(st.st_size-56)/8;
        if(ftruncate(fileno(f),56+frames*8)<0)result=-errno;
        int r=wav_header(f,frames);if(r<0)result=r;
    }else result=-EIO;
    if(fsync(fileno(f))<0)result=-errno;
    if(fclose(f)<0)result=-errno;
    return result;
}
/* Publish a complete metadata document alongside recoverable WAV headers. */
static int manifest(struct session *s,const char *directory,const char *mode,uint64_t from,uint64_t end,const uint64_t *gaps){
    char path[4096],temporary[4096];
    if(snprintf(path,sizeof(path),"%s/session.json",directory)>=(int)sizeof(path)||snprintf(temporary,sizeof(temporary),"%s/.session.json.tmp",directory)>=(int)sizeof(temporary))return -ENAMETOOLONG;
    int fd=open(temporary,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return -errno;
    FILE *f=fdopen(fd,"w");if(!f){close(fd);return -errno;}
    fprintf(f,"{\"version\":1,\"mode\":\"%s\",\"rate\":%u,\"channels_per_track\":2,\"sample_format\":\"float32-le\",\"origin_monotonic_ns\":%"PRIu64",\"start_frame\":%"PRIu64",\"end_frame\":%"PRIu64",\"frames_per_track\":%"PRIu64,mode,RATE,s->origin,from,end,end-from);
    if(!strcmp(mode,"recording")){struct recording *r=&s->recording;char *error=json_quote(r->error);
        fprintf(f,",\"complete\":%s,\"segment_frames\":%"PRIu64",\"parts\":%u,\"last_part_frames\":%"PRIu64",\"bytes_written\":%"PRIu64",\"error\":%s",r->active?"false":"true",r->part_limit,r->part,r->part_frames,r->bytes,error);free(error);
    }
    fputs(",\"tracks\":[",f);
    for(unsigned i=0;i<s->count;i++){char *q=json_quote(s->tracks[i].name);fprintf(f,"%s{\"index\":%u,\"source\":%s,\"file_pattern\":\"track%02u-part%%04u.wav\",\"gap_frames\":%"PRIu64",\"dropped_capture_frames\":%"PRIuFAST64",\"invalid_samples\":%"PRIuFAST64"}",i?",":"",i+1,q,i+1,gaps?gaps[i]:0,atomic_load(&s->tracks[i].dropped),atomic_load(&s->tracks[i].invalid));free(q);}fputs("]}\n",f);
    int result=fflush(f)==0?0:-EIO;if(!result&&fsync(fd)<0)result=-errno;if(fclose(f)<0)result=-errno;
    if(!result&&rename(temporary,path)<0)result=-errno;
    if(!result){int dirfd=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(dirfd<0)result=-errno;else{if(fsync(dirfd)<0)result=-errno;close(dirfd);}}
    if(result)unlink(temporary);
    return result;
}
static int export_history(struct session *s,const char *directory,unsigned seconds){if(s->recording.active)return -EBUSY;if(seconds>s->seconds)return -EINVAL;uint64_t end=s->common_end,frames=(uint64_t)(seconds?:s->seconds)*RATE;if(frames>end)frames=end;if(!frames)return -EAGAIN;uint64_t from=end-frames;
    int result=new_directory(s,directory,frames*8*s->count+4096);if(result<0)return result;FILE *files[CAPTURE_TRACK_LIMIT]={0};uint64_t gaps[CAPTURE_TRACK_LIMIT]={0};float pcm[BLOCK*CHANNELS];
    for(unsigned i=0;i<s->count;i++)if(!(files[i]=wav_open(directory,i,1))){result=-errno;goto out;}
    for(uint64_t at=from;at<end;){unsigned n=end-at>BLOCK?BLOCK:end-at;for(unsigned i=0;i<s->count;i++){gaps[i]+=history_read(&s->tracks[i],at,n,pcm);if(fwrite(pcm,8,n,files[i])!=n){result=-EIO;goto out;}}at+=n;}
out:for(unsigned i=0;i<s->count;i++){int r=wav_close(files[i]);if(r<0)result=r;}
    if(!result)result=manifest(s,directory,"history",from,end,gaps);
    return result;
}
static int close_part(struct session *s){int result=0;for(unsigned i=0;i<s->count;i++){int r=wav_close(s->recording.files[i]);s->recording.files[i]=NULL;if(r<0)result=r;}return result;}
static int finish_recording(struct session *s,int reason){struct recording *r=&s->recording;if(!r->active)return 0;
    int result=close_part(s);if(reason<0)result=reason;
    r->active=false;if(result<0){snprintf(r->error,sizeof(r->error),"%s",strerror(-result));snprintf(s->error,sizeof(s->error),"Recording stopped: %.230s",r->error);}
    int meta=manifest(s,r->directory,"recording",r->start,r->next,r->gaps);if(meta<0&&!result)result=meta;
    if(result<0)snprintf(s->error,sizeof(s->error),"Recording stopped: %.230s",strerror(-result));
    return result;
}
static int open_part(struct session *s){struct recording *r=&s->recording;
    for(unsigned i=0;i<s->count;i++)if(!(r->files[i]=wav_open(r->directory,i,r->part))){int result=-errno;close_part(s);return result;}
    return 0;
}
static int begin_recording(struct session *s,const char *directory,unsigned pre_seconds){if(s->recording.active)return -EEXIST;if(pre_seconds>s->seconds)return -EINVAL;
    uint64_t pre=(uint64_t)pre_seconds*RATE;if(pre>s->common_end)pre=s->common_end;
    int result=new_directory(s,directory,(pre+RATE)*8*s->count+4096);if(result<0)return result;
    unsigned segment_seconds=60;const char *segment=getenv("PIPEMIXER_RECORD_SEGMENT_SECONDS");if(segment){char *end;unsigned long v=strtoul(segment,&end,10);if(*end||v<1||v>3600){rmdir(directory);return -EINVAL;}segment_seconds=v;}
    struct recording *r=&s->recording;memset(r,0,sizeof(*r));r->active=true;r->part=1;r->start=r->next=s->common_end-pre;r->part_limit=(uint64_t)segment_seconds*RATE;snprintf(r->directory,sizeof(r->directory),"%s",directory);s->error[0]=0;
    result=open_part(s);if(result<0){finish_recording(s,result);return result;}
    result=manifest(s,directory,"recording",r->start,r->next,r->gaps);if(result<0)finish_recording(s,result);
    return result;
}
static int checkpoint(struct session *s){struct recording *r=&s->recording;
    for(unsigned i=0;i<s->count;i++)if(r->files[i]){if(fflush(r->files[i])<0)return -errno;int result=wav_header(r->files[i],r->part_frames);if(result<0)return result;if(fsync(fileno(r->files[i]))<0)return -errno;}
    return manifest(s,r->directory,"recording",r->start,r->next,r->gaps);
}
static void record_step(struct session *s){struct recording *r=&s->recording;if(!r->active)return;
    uint64_t now=diagnostics_now_ns();int result=0;float pcm[BLOCK*CHANNELS];
    if(now-r->space_ns>=250000000ull){r->space_ns=now;if(disk_free(r->directory)<(uint64_t)s->reserve_mb*1024*1024+RATE/2*8*s->count){finish_recording(s,-ENOSPC);return;}}
    while(r->next<s->common_end){if(r->part_frames==r->part_limit){result=close_part(s);if(result<0)break;r->part++;r->part_frames=0;}if(!r->files[0]){result=open_part(s);if(result<0)break;}
        uint64_t remaining=s->common_end-r->next;if(remaining>r->part_limit-r->part_frames)remaining=r->part_limit-r->part_frames;unsigned n=remaining>BLOCK?BLOCK:remaining;
        for(unsigned i=0;i<s->count;i++){r->gaps[i]+=history_read(&s->tracks[i],r->next,n,pcm);size_t written=fwrite(pcm,8,n,r->files[i]);r->bytes+=written*8;if(written!=n){result=-EIO;break;}}
        if(result<0)break;
        r->next+=n;r->part_frames+=n;
        if(r->part_frames==r->part_limit&&r->next<s->common_end){result=close_part(s);if(result<0)break;r->part++;r->part_frames=0;}
        else if(r->part_frames==r->part_limit)break;
    }
    if(!result&&r->part_frames==r->part_limit&&r->next<s->common_end){result=close_part(s);if(!result){r->part++;r->part_frames=0;}}
    if(!result&&now-r->checkpoint_ns>=1000000000ull){r->checkpoint_ns=now;result=checkpoint(s);}
    if(result<0)finish_recording(s,result);
}
static char *status_json(struct session *s){char *text=NULL;size_t size=0;FILE *f=open_memstream(&text,&size);if(!f)return NULL;unsigned long pages=0,rss=0;FILE *stat=fopen("/proc/self/statm","r");if(stat){if(fscanf(stat,"%lu %lu",&pages,&rss)!=2)rss=0;fclose(stat);}char *q=json_quote(s->name),*error=json_quote(s->error),*directory=json_quote(s->recording.directory);
    fprintf(f,"{\"name\":%s,\"pid\":%d,\"ready\":%s,\"history_seconds\":%u,\"buffered_seconds\":%.3f,\"memory_budget_bytes\":%"PRIu64",\"rss_kb\":%lu,\"cpu_percent_one_core\":%.2f,\"rate\":%u,\"channels_per_track\":2,\"recording\":%s,\"recording_directory\":%s,\"recorded_seconds\":%.3f,\"recorded_bytes\":%"PRIu64",\"disk_free_bytes\":%"PRIu64",\"disk_reserve_mb\":%u,\"last_error\":%s,\"tracks\":[",q,getpid(),s->ready?"true":"false",s->seconds,fmin(s->seconds,s->common_end/(double)RATE),capture_memory_budget(s->seconds,s->count),rss*(unsigned long)sysconf(_SC_PAGESIZE)/1024,s->cpu_percent,RATE,s->recording.active?"true":"false",directory,(s->recording.next-s->recording.start)/(double)RATE,s->recording.bytes,*s->recording.directory?disk_free(s->recording.directory):0,s->reserve_mb,error);free(q);free(error);free(directory);
    for(unsigned i=0;i<s->count;i++){struct track *t=&s->tracks[i];q=json_quote(t->name);uint64_t last=atomic_load(&t->last_audio);fprintf(f,"%s{\"source\":%s,\"available\":%s,\"received_frames\":%"PRIuFAST64",\"dropped_frames\":%"PRIuFAST64",\"gap_frames\":%"PRIu64",\"invalid_samples\":%"PRIuFAST64",\"peak\":%.8g}",i?",":"",q,last&&diagnostics_now_ns()-last<500000000ull?"true":"false",atomic_load(&t->received),atomic_load(&t->dropped),t->gaps,atomic_load(&t->invalid),t->peak);free(q);}fputs("]}",f);fclose(f);return text;
}
static void sample_cpu(struct session *s){uint64_t now=diagnostics_now_ns();if(now-s->cpu_at<250000000ull)return;struct rusage u;if(getrusage(RUSAGE_SELF,&u)<0)return;
    uint64_t ns=(uint64_t)(u.ru_utime.tv_sec+u.ru_stime.tv_sec)*1000000000ull+(u.ru_utime.tv_usec+u.ru_stime.tv_usec)*1000ull;
    if(s->cpu_at)s->cpu_percent=100.0*(ns-s->cpu_ns)/(now-s->cpu_at);
    s->cpu_at=now;s->cpu_ns=ns;
}
static void *writer(void *data){struct session *s=data;
    while(!atomic_load(&s->shutdown)){
        sample_cpu(s);
        for(unsigned i=0;i<s->count;i++){struct track *t=&s->tracks[i];unsigned r=atomic_load_explicit(&t->read_index,memory_order_relaxed),w=atomic_load_explicit(&t->write_index,memory_order_acquire);while(r!=w){history_write(t,&t->queue[r%QUEUE]);r++;atomic_store_explicit(&t->read_index,r,memory_order_release);w=atomic_load_explicit(&t->write_index,memory_order_acquire);}}
        uint64_t now=frame_now(s);s->common_end=now>LAG?now-LAG:0;for(unsigned i=0;i<s->count;i++)history_zero(&s->tracks[i],s->common_end);
        record_step(s);
        if(atomic_load_explicit(&s->request_pending,memory_order_acquire)==1){int result=0;struct request *r=&s->request;char *text=NULL;
            if(r->op==CAPTURE_STATUS)text=status_json(s);else if(r->op==CAPTURE_EXPORT)result=export_history(s,r->directory,r->seconds);else if(r->op==CAPTURE_RECORD)result=begin_recording(s,r->directory,r->seconds);else if(r->op==CAPTURE_FINISH)result=finish_recording(s,0);else if(r->op==CAPTURE_STOP){result=finish_recording(s,0);atomic_store(&s->shutdown,true);}else result=-ENOTSUP;
            if(result<0){snprintf(s->error,sizeof(s->error),"%s",result==-EBUSY&&r->op==CAPTURE_EXPORT?"Stop continuous recording before exporting the cache":strerror(-result));if(!text)text=strdup(s->error);}send_reply(s->reply_fd,result,s->ready?1:0,text?:"ok");free(text);close(s->reply_fd);s->reply_fd=-1;atomic_store_explicit(&s->request_pending,0,memory_order_release);
        }
        struct timespec pause={.tv_nsec=5000000};nanosleep(&pause,NULL);
    }finish_recording(s,0);return NULL;
}
static void accept_request(void *data,int fd,uint32_t mask){struct session *s=data;int client;
    while((client=accept4(fd,NULL,NULL,SOCK_CLOEXEC))>=0){struct timeval tv={.tv_usec=100000};setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));setsockopt(client,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));struct ucred cred;socklen_t size=sizeof(cred);struct request r;ssize_t n=recv(client,&r,sizeof(r),MSG_TRUNC);
        if(getsockopt(client,SOL_SOCKET,SO_PEERCRED,&cred,&size)<0||cred.uid!=getuid()||n!=sizeof(r)||r.magic!=MAGIC||r.op>CAPTURE_FINISH||!memchr(r.directory,0,sizeof(r.directory))){send_reply(client,-EINVAL,0,"Invalid capture request");close(client);continue;}
        if(atomic_load_explicit(&s->request_pending,memory_order_acquire)){send_reply(client,-EBUSY,0,"Another export or recording operation is in progress");close(client);continue;}
        s->request=r;s->reply_fd=client;atomic_store_explicit(&s->request_pending,1,memory_order_release);
    }
}
static void tick(void *data,uint64_t count){struct session *s=data;if(atomic_load(&s->shutdown)){pw_main_loop_quit(main_loop);return;}bool ready=true;
    uint64_t elapsed=frame_now(s),guarded=elapsed>LAG?elapsed-LAG:0;
    for(unsigned i=0;i<s->count;i++){struct track *t=&s->tracks[i];uint32_t id;bool exists=graph_resolve_node(t->name,&id)==0;const struct graph_node *node=exists?graph_node_find(id):NULL;const char *serial=node?dict_get(&node->props,PW_KEY_OBJECT_SERIAL):NULL;
        if(t->stream&&(!exists||strcmp(t->serial,serial?:""))){pw_stream_destroy(t->stream);t->stream=NULL;t->next_frame=0;atomic_store(&t->format_ok,false);atomic_store(&t->last_audio,0);}
        if(!t->stream&&node)connect_track(t,node);
        /* Do not acknowledge readiness before a full audio buffer reaches the
         * common guarded timeline; immediate recording would include startup silence. */
        if(!atomic_load(&t->received)||!atomic_load(&t->format_ok)||guarded<atomic_load_explicit(&t->first_end,memory_order_acquire))ready=false;
    }
    if(!s->ready&&ready){s->ready=true;if(s->ready_fd>=0){unsigned char ok=1;ssize_t written=write(s->ready_fd,&ok,1);(void)written;close(s->ready_fd);s->ready_fd=-1;}}
    if(!s->ready&&diagnostics_now_ns()-s->origin>10000000000ull){atomic_store(&s->shutdown,true);pw_main_loop_quit(main_loop);}
}
static void stop(void *data,int signal){struct session *s=data;atomic_store(&s->shutdown,true);pw_main_loop_quit(main_loop);}
static void server_error(int code,const char *message,void *data){if(code==-EPIPE||code==-ECONNRESET||code==-ENOTCONN)stop(data,SIGTERM);}
static const struct pipewire_events capture_events={.error=server_error};
static void touch_memory(void *memory,size_t bytes){volatile unsigned char *p=memory;for(size_t i=0;i<bytes;i+=4096)p[i]=0;}
int capture_worker(const char *name,unsigned seconds,const char *const *sources,unsigned count,int ready_fd){
    if(!managed_valid_name(name)||!seconds||seconds>120||!count||count>CAPTURE_TRACK_LIMIT||capture_memory_budget(seconds,count)>CAPTURE_MEMORY_LIMIT)return 2;
    if(memory_available()<capture_memory_budget(seconds,count)+64u*1024u*1024u)return 1;
    struct session *s=calloc(1,sizeof(*s));if(!s)return 1;s->server=s->lock=s->reply_fd=-1;s->ready_fd=ready_fd;s->count=count;s->seconds=seconds;s->capacity=seconds*RATE+LAG*2;s->reserve_mb=16;
    const char *reserve=getenv("PIPEMIXER_RECORD_RESERVE_MB");if(reserve){char *end;unsigned long mb=strtoul(reserve,&end,10);if(*end||mb<16||mb>1024){free(s);return 2;}s->reserve_mb=mb;}
    snprintf(s->name,sizeof(s->name),"%s",name);int result=1;char lock[128];if(runtime_path(name,s->socket_path,sizeof(s->socket_path),".sock")<0||runtime_path(name,lock,sizeof(lock),".lock")<0)goto out;
    s->lock=open(lock,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(s->lock<0||flock(s->lock,LOCK_EX|LOCK_NB)<0)goto out;
    s->server=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(s->server<0)goto out;unlink(s->socket_path);struct sockaddr_un address={.sun_family=AF_UNIX};snprintf(address.sun_path,sizeof(address.sun_path),"%s",s->socket_path);if(bind(s->server,(struct sockaddr*)&address,sizeof(address))<0||chmod(s->socket_path,0600)<0||listen(s->server,8)<0)goto out;
    for(unsigned i=0;i<count;i++){struct track *t=&s->tracks[i];t->session=s;snprintf(t->name,sizeof(t->name),"%s",sources[i]);t->history=calloc((size_t)s->capacity*CHANNELS,sizeof(float));t->valid=calloc(s->capacity,1);t->queue=calloc(QUEUE,sizeof(struct chunk));if(!t->history||!t->valid||!t->queue)goto out;touch_memory(t->history,(size_t)s->capacity*8);touch_memory(t->valid,s->capacity);touch_memory(t->queue,QUEUE*sizeof(struct chunk));}
    s->origin=diagnostics_now_ns();s->hook=pipewire_add_listener(&capture_events,s);s->io=pw_loop_add_io(event_loop,s->server,SPA_IO_IN,false,accept_request,s);s->timer=pw_loop_add_timer(event_loop,tick,s);s->signals[0]=pw_loop_add_signal(event_loop,SIGINT,stop,s);s->signals[1]=pw_loop_add_signal(event_loop,SIGTERM,stop,s);
    if(!s->io||!s->timer||pthread_create(&s->writer,NULL,writer,s)!=0)goto out;
    struct timespec interval={.tv_nsec=50000000};pw_loop_update_timer(event_loop,s->timer,&interval,&interval,false);pw_main_loop_run(main_loop);atomic_store(&s->shutdown,true);pthread_join(s->writer,NULL);result=s->ready?0:1;
out:if(s->hook)event_hook_release(s->hook);for(unsigned i=0;i<count;i++){if(s->tracks[i].stream)pw_stream_destroy(s->tracks[i].stream);free(s->tracks[i].history);free(s->tracks[i].valid);free(s->tracks[i].queue);}if(s->io)pw_loop_destroy_source(event_loop,s->io);if(s->timer)pw_loop_destroy_source(event_loop,s->timer);for(unsigned i=0;i<2;i++)if(s->signals[i])pw_loop_destroy_source(event_loop,s->signals[i]);if(s->server>=0){close(s->server);unlink(s->socket_path);}if(s->lock>=0)close(s->lock);if(s->ready_fd>=0)close(s->ready_fd);if(s->reply_fd>=0)close(s->reply_fd);free(s);return result;
}
struct starter {const char *name;unsigned seconds,count;const char *const *sources;struct capture_child child;uint64_t deadline;int result;bool sent;};
static void start_tick(void *data,uint64_t ticks){struct starter *s=data;if(!s->sent&&graph_ready()){char error[256]={0};int r=capture_spawn(s->name,s->seconds,s->sources,s->count,&s->child,error,sizeof(error));if(r<0){fprintf(stderr,"pipemixer: %s\n",error);s->result=r==-ENOENT||r==-EEXIST?3:1;pw_main_loop_quit(main_loop);return;}s->sent=true;}
    if(s->sent){int r=capture_child_poll(&s->child);if(r!=-EAGAIN){s->result=r?1:0;pw_main_loop_quit(main_loop);return;}}
    if(diagnostics_now_ns()>=s->deadline){fprintf(stderr,"pipemixer: history source did not become ready\n");s->result=1;pw_main_loop_quit(main_loop);}
}
static void start_signal(void *data,int sig){struct starter *s=data;s->result=1;pw_main_loop_quit(main_loop);}
static void print_status(const char *text){
    struct spa_json root=SPA_JSON_INIT(text,strlen(text)),object;const char *value;char name[512],directory[4096],error[256];
    if(spa_json_enter_object(&root,&object)<=0){puts(text);return;}
    int n=spa_json_object_find(&object,"name",&value);name[0]=0;if(n>0)spa_json_parse_stringn(value,n,name,sizeof(name));
    n=spa_json_object_find(&object,"recording_directory",&value);directory[0]=0;if(n>0)spa_json_parse_stringn(value,n,directory,sizeof(directory));
    n=spa_json_object_find(&object,"last_error",&value);error[0]=0;if(n>0)spa_json_parse_stringn(value,n,error,sizeof(error));
    bool recording=false;n=spa_json_object_find(&object,"recording",&value);if(n>0)spa_json_parse_bool(value,n,&recording);
    double seconds=0,cpu=0,rss=0;n=spa_json_object_find(&object,"buffered_seconds",&value);if(n>0)seconds=strtod(value,NULL);n=spa_json_object_find(&object,"cpu_percent_one_core",&value);if(n>0)cpu=strtod(value,NULL);n=spa_json_object_find(&object,"rss_kb",&value);if(n>0)rss=strtod(value,NULL);
    printf("%s | cache %.1f s | recording %s | CPU %.1f%% of one core | RSS %.0f KiB\n",name,seconds,recording?"active":"stopped",cpu,rss);
    if(*directory)printf("  files: %s\n",directory);
    if(*error)printf("  last error: %s\n",error);
    char key[64];while((n=spa_json_object_next(&object,key,sizeof(key),&value))>0)if(!strcmp(key,"tracks")){struct spa_json tracks;spa_json_enter(&object,&tracks);while((n=spa_json_next(&tracks,&value))>0){struct spa_json track;spa_json_enter(&tracks,&track);bool available=false;n=spa_json_object_find(&track,"available",&value);if(n>0)spa_json_parse_bool(value,n,&available);n=spa_json_object_find(&track,"source",&value);name[0]=0;if(n>0)spa_json_parse_stringn(value,n,name,sizeof(name));printf("  %s %s\n",available?"audio":"missing",name);}break;}
}
int capture_cli(const char *command,const char *name,const char *directory,unsigned seconds,const char *const *sources,unsigned count,bool json,unsigned timeout){
    if(!strcmp(command,"start-history")){struct starter s={.name=name,.seconds=seconds,.count=count,.sources=sources,.child={.ready_fd=-1},.deadline=diagnostics_now_ns()+(uint64_t)timeout*1000000,.result=1};
        struct spa_source *timer=pw_loop_add_timer(event_loop,start_tick,&s),*a=pw_loop_add_signal(event_loop,SIGINT,start_signal,&s),*b=pw_loop_add_signal(event_loop,SIGTERM,start_signal,&s);struct timespec interval={.tv_nsec=50000000};if(timer){pw_loop_update_timer(event_loop,timer,&interval,&interval,false);pw_main_loop_run(main_loop);pw_loop_destroy_source(event_loop,timer);}if(a)pw_loop_destroy_source(event_loop,a);if(b)pw_loop_destroy_source(event_loop,b);if(s.result&&s.sent)capture_child_cancel(&s.child);return s.result;
    }
    if(!strcmp(command,"list-history")){char names[16][49];unsigned n=capture_list(names,16);if(json)putchar('[');bool first=true;for(unsigned i=0;i<n;i++){struct capture_reply reply;if(capture_request(names[i],CAPTURE_STATUS,NULL,0,timeout,&reply)<0)continue;if(json){if(!first)putchar(',');fputs(reply.text,stdout);}else print_status(reply.text);first=false;}if(json)puts("]");return 0;}
    enum capture_operation op=!strcmp(command,"history-status")?CAPTURE_STATUS:!strcmp(command,"export-history")?CAPTURE_EXPORT:!strcmp(command,"record-history")?CAPTURE_RECORD:!strcmp(command,"stop-recording")?CAPTURE_FINISH:CAPTURE_STOP;
    struct capture_reply reply;int r=capture_request(name,op,directory,seconds,timeout,&reply);if(r<0){fprintf(stderr,"pipemixer: %s\n",reply.text[0]?reply.text:strerror(-r));return r==-ENOENT||r==-ECONNREFUSED?3:1;}if(op==CAPTURE_STATUS){if(json)puts(reply.text);else print_status(reply.text);}return 0;
}
char *capture_default_directory(void){const char *base=getenv("XDG_STATE_HOME");char *directory=NULL;if(base&&*base)xasprintf(&directory,"%s/pipemixer/recordings",base);else{xasprintf(&directory,"%s/.local/state/pipemixer/recordings",getenv("HOME")?:"/tmp");}return directory;}
