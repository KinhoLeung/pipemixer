#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#define CAPTURE_TRACK_LIMIT 8
#define CAPTURE_MEMORY_LIMIT (64u * 1024u * 1024u)
#define CAPTURE_REPLY_SIZE 16384
struct capture_child { pid_t pid; int ready_fd; };
enum capture_operation { CAPTURE_STATUS, CAPTURE_EXPORT, CAPTURE_STOP, CAPTURE_RECORD, CAPTURE_FINISH };
struct capture_reply { int result; unsigned flags; char text[CAPTURE_REPLY_SIZE]; };
int capture_spawn(const char *name,unsigned seconds,const char *const *sources,unsigned count,struct capture_child *child,char *error,unsigned size);
int capture_child_poll(struct capture_child *child);
void capture_child_cancel(struct capture_child *child);
void capture_reap(void);
int capture_request(const char *name,enum capture_operation op,const char *directory,unsigned seconds,unsigned timeout,struct capture_reply *reply);
unsigned capture_list(char names[][49],unsigned limit);
uint64_t capture_memory_budget(unsigned seconds,unsigned tracks);
int capture_worker(const char *name,unsigned seconds,const char *const *sources,unsigned count,int ready_fd);
int capture_cli(const char *command,const char *name,const char *directory,unsigned seconds,const char *const *sources,unsigned count,bool json,unsigned timeout);
char *capture_default_directory(void);
