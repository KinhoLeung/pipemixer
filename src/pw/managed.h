#pragma once

#include <sys/types.h>
#include "pw/graph.h"

bool managed_valid_name(const char *name);
const struct graph_node *managed_find(const char *kind, const char *name, const char *role);
bool managed_ready(const char *kind, const char *name);
int managed_remove(const char *kind, const char *name);

/* Creates a detached audio worker. source/destination are serial:N selectors. */
pid_t managed_spawn(const char *kind, const char *name, const char *source, const char *destination);
pid_t managed_spawn_private(const char *kind, const char *name, const char *source, const char *destination);
int managed_child_status(pid_t pid);
bool managed_child_ready(pid_t pid);
void managed_cancel(pid_t pid);
void managed_reap(void);
void managed_cleanup(void);

/* Called only by the internal audio-worker CLI command. */
int managed_worker_load(const char *kind, const char *name, const char *source, const char *destination, int ready_fd, bool private);
void managed_worker_cleanup(void);
