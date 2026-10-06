#pragma once
#include <stdbool.h>
#include <stddef.h>

struct scene_job;

char *scene_directory(void);
char *scene_filename(const char *name);
int scene_list(char ***names, unsigned *count);
void scene_list_free(char **names, unsigned count);
int scene_delete(const char *name);
int scene_validate(const char *name, char *error, size_t size);
int scene_save(const char *name, char *error, size_t size);
/* step: 0 complete, -EAGAIN waiting, other negative values are failures.
 * No cached PipeWire pointers survive between calls. */
struct scene_job *scene_load(const char *name, char *error, size_t size);
/* Restore independent paths when external devices/applications are absent. */
struct scene_job *scene_load_available(const char *name, char *error, size_t size);
struct effect_chain;
/* Rebuild one chain and its dependent Sends, preserving the current state. */
struct scene_job *scene_edit_effect(const char *name, const struct effect_chain *chain, char *error, size_t size);
unsigned scene_job_skipped(const struct scene_job *job);
const char *scene_job_stage(const struct scene_job *job);
int scene_step(struct scene_job *job, char *error, size_t size);
/* A committed chain edit rolls back asynchronously after a PipeWire error. */
bool scene_job_error(struct scene_job *job, int code, const char *message);
void scene_job_free(struct scene_job *job, bool cancel);
char *scene_startup_get(void);
int scene_startup_set(const char *name, char *error, size_t size);

/* The routing engine calls tick while holding the exclusive scene lock.
 * True delays routing until newly appearing endpoints have their state. */
struct scene_recovery;
struct scene_recovery *scene_recovery_new(void);
bool scene_recovery_tick(struct scene_recovery *recovery);
bool scene_recovery_error(struct scene_recovery *recovery, int code, const char *message);
void scene_recovery_free(struct scene_recovery *recovery);
int scene_recovery_clear(void);
int scene_recovery_print(bool json, char *error, size_t size);
