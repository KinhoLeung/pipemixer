#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <pipewire/impl.h>
#include <spa/param/audio/raw-utils.h>

#include "pw/managed.h"
#include "pw/common.h"
#include "pw/effects.h"
#include "monitor.h"
#include "eventloop.h"
#include "utils.h"
#include "xmalloc.h"

struct child {
    pid_t pid;
    int status;
    bool exited;
    bool ready;
    int ready_fd;
    struct child *next;
};
static struct child *children;
static struct pw_impl_module *worker_module;
static struct spa_hook worker_listener;
static int worker_lock = -1;
static int worker_ready_fd = -1;
static struct pw_stream *worker_prime;
static struct spa_source *worker_prime_timer;

static void worker_notify_ready(void) {
    if (worker_ready_fd < 0) return;
    char ready = 'R';
    ssize_t result;
    do { result = write(worker_ready_fd, &ready, 1); } while (result < 0 && errno == EINTR);
    close(worker_ready_fd); worker_ready_fd = -1;
    if (result != 1) pw_main_loop_quit(main_loop);
}

static void finish_prime(void *data, uint64_t expirations) {
    if (worker_prime) { pw_stream_destroy(worker_prime); worker_prime = NULL; }
    pw_loop_destroy_source(event_loop, worker_prime_timer); worker_prime_timer = NULL;
    worker_notify_ready();
}

static void prime_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "effect initialization failed: %s\n", error ?: "stream error");
        pw_main_loop_quit(main_loop); return;
    }
    if (state != PW_STREAM_STATE_STREAMING) return;
    /* Filter-Chain only instantiates its DSP controls on first activation.
     * A brief private capture makes subsequent idle changes observable too. */
    struct timespec delay = { .tv_nsec = 200000000 };
    if (pw_loop_update_timer(event_loop, worker_prime_timer, &delay, NULL, false) < 0)
        pw_main_loop_quit(main_loop);
}

static void prime_process(void *data) {
    struct pw_stream *stream = worker_prime;
    if (!stream) return;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(stream);
    if (buffer) pw_stream_queue_buffer(stream, buffer);
}

static const struct pw_stream_events prime_events = {
    .version = PW_VERSION_STREAM_EVENTS, .state_changed = prime_state, .process = prime_process,
};

static int prime_effect(const char *target) {
    worker_prime_timer = pw_loop_add_timer(event_loop, finish_prime, NULL);
    if (!worker_prime_timer) return -ENOMEM;
    struct pw_properties *props = pw_properties_new(
        PW_KEY_NODE_NAME, "pipemixer.internal.effect-init", "pipemixer.internal", "true",
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_TARGET_OBJECT, target, PW_KEY_NODE_DONT_RECONNECT, "true",
        "node.dont-fallback", "true", PW_KEY_NODE_VIRTUAL, "true", NULL);
    if (!props) return -ENOMEM;
    worker_prime = pw_stream_new_simple(event_loop, "PipeMixer effect initialization", props, &prime_events, NULL);
    if (!worker_prime) return -(errno ?: ENOMEM);
    uint8_t buffer[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *format = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = 48000, .channels = 2,
                                .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }));
    return pw_stream_connect(worker_prime, PW_DIRECTION_INPUT, PW_ID_ANY,
                             PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                             &format, 1);
}

bool managed_valid_name(const char *name) {
    if (!name || !*name || strlen(name) > 48) return false;
    for (const char *p = name; *p; p++) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= 'A' && *p <= 'Z')
            && !(*p >= '0' && *p <= '9') && *p != '-' && *p != '_') return false;
    }
    return true;
}

struct find_data { const char *kind, *name, *role; const struct graph_node *node; };
static void find_node(const struct graph_node *node, void *data) {
    struct find_data *find = data;
    if (streq(dict_get(&node->props, "pipemixer.managed"), "1")
        && streq(dict_get(&node->props, "pipemixer.kind"), find->kind)
        && streq(dict_get(&node->props, "pipemixer.group"), find->name)
        && (!find->role || streq(dict_get(&node->props, "pipemixer.role"), find->role))) find->node = node;
}
const struct graph_node *managed_find(const char *kind, const char *name, const char *role) {
    struct find_data find = {kind, name, role, NULL};
    graph_foreach_node(find_node, &find);
    return find.node;
}

struct ready_links { uint32_t input, output; bool incoming, outgoing; };
static void check_ready_link(const struct graph_link *link, void *data) {
    struct ready_links *ready = data;
    if (!link->info || link->info->state < PW_LINK_STATE_PAUSED) return;
    if (link->info->input_node_id == ready->input) ready->incoming = true;
    if (link->info->output_node_id == ready->output) ready->outgoing = true;
}

bool managed_ready(const char *kind, const char *name) {
    const struct graph_node *in = managed_find(kind, name, "input");
    const struct graph_node *out = managed_find(kind, name, "output");
    if (!in || !out) return false;
    bool private = streq(dict_get(&in->props,"pipemixer.internal"),"true");
    if (!private && (!graph_node_has_ports(in->id,PW_DIRECTION_INPUT)
        || !graph_node_has_ports(out->id,PW_DIRECTION_OUTPUT))) return false;
    if (streq(kind, "effect")) return in->controls_ready;
    if (!streq(kind, "send")) return true;
    struct ready_links ready = { .input = in->id, .output = out->id };
    graph_foreach_link(check_ready_link, &ready);
    return ready.incoming && ready.outgoing;
}

int managed_remove(const char *kind, const char *name) {
    const struct graph_node *in = managed_find(kind, name, "input");
    const struct graph_node *out = managed_find(kind, name, "output");
    /* Removing either end makes the module tear down both streams. */
    return in ? graph_destroy_node(in->id) : out ? graph_destroy_node(out->id) : 0;
}

static pid_t spawn_worker(const char *kind, const char *name, const char *source, const char *destination, bool private) {
    if (!managed_valid_name(name)) return -EINVAL;
    if (managed_find(kind, name, NULL)) return -EEXIST;
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0) return -errno;
    executable[length] = '\0';
    char *logpath;
    xasprintf(&logpath, "%s/pipemixer-%s-%s.log", getenv("XDG_RUNTIME_DIR") ?: "/tmp", kind, name);
    int logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    free(logpath);
    int nullfd = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (logfd < 0 || nullfd < 0) {
        int error = errno;
        if (logfd >= 0) close(logfd);
        if (nullfd >= 0) close(nullfd);
        return -error;
    }
    int ready_pipe[2];
    if (pipe2(ready_pipe, O_NONBLOCK | O_CLOEXEC) < 0) {
        int error = errno; close(nullfd); close(logfd); return -error;
    }
    char descriptor[16];
    snprintf(descriptor, sizeof(descriptor), "%d", ready_pipe[1]);
    char *arguments[11] = {executable, "audio-worker", (char *)kind, (char *)name};
    unsigned count = 4;
    if (source) arguments[count++] = (char *)source;
    if (destination) arguments[count++] = (char *)destination;
    if (private) arguments[count++] = "--private";
    arguments[count++] = "--ready-fd"; arguments[count++] = descriptor;
    arguments[count] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        close(ready_pipe[0]);
        fcntl(ready_pipe[1], F_SETFD, 0);
        dup2(nullfd, STDIN_FILENO); dup2(nullfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO);
        close(nullfd); close(logfd);
        execv("/proc/self/exe", arguments);
        _exit(127);
    }
    int error = errno;
    close(nullfd); close(logfd);
    close(ready_pipe[1]);
    if (pid < 0) { close(ready_pipe[0]); return -error; }
    struct child *child = xcalloc(1, sizeof(*child));
    child->pid = pid; child->ready_fd = ready_pipe[0]; child->next = children; children = child;
    return pid;
}
pid_t managed_spawn(const char *kind,const char *name,const char *source,const char *destination) {
    return spawn_worker(kind,name,source,destination,false);
}
pid_t managed_spawn_private(const char *kind,const char *name,const char *source,const char *destination) {
    return spawn_worker(kind,name,source,destination,true);
}

void managed_reap(void) {
    for (struct child *child = children; child; child = child->next) {
        if (child->exited) continue;
        int status;
        if (waitpid(child->pid, &status, WNOHANG) == child->pid) {
            child->exited = true;
            child->status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
    }
}

int managed_child_status(pid_t pid) {
    managed_reap();
    for (struct child *child = children; child; child = child->next) {
        if (child->pid == pid) return child->exited ? child->status : -EAGAIN;
    }
    return -ECHILD;
}

bool managed_child_ready(pid_t pid) {
    for (struct child *child = children; child; child = child->next) {
        if (child->pid != pid) continue;
        if (child->ready_fd >= 0) {
            char ready;
            ssize_t result = read(child->ready_fd, &ready, 1);
            if (result >= 0) {
                child->ready = result == 1 && ready == 'R';
                close(child->ready_fd); child->ready_fd = -1;
            }
        }
        return child->ready;
    }
    return false;
}

void managed_cancel(pid_t pid) {
    if (pid > 0 && managed_child_status(pid) == -EAGAIN) kill(pid, SIGTERM);
}

void managed_cleanup(void) {
    managed_reap();
    while (children) {
        struct child *child = children;
        children = child->next;
        if (child->ready_fd >= 0) close(child->ready_fd);
        free(child);
    }
}

static void on_module_destroy(void *data) {
    worker_module = NULL;
    spa_hook_remove(&worker_listener);
    pw_main_loop_quit(main_loop);
}
static const struct pw_impl_module_events module_events = {
    .version = PW_VERSION_IMPL_MODULE_EVENTS, .destroy = on_module_destroy,
};
static void worker_quit(void *data, int signal) { pw_main_loop_quit(main_loop); }

int managed_worker_load(const char *kind, const char *name, const char *source, const char *destination, int ready_fd, bool private) {
    worker_ready_fd = ready_fd;
    signal(SIGPIPE, SIG_IGN);
    if (managed_find(kind, name, NULL)) return -EEXIST;
    char *lockpath;
    xasprintf(&lockpath, "%s/pipemixer-%s-%s.lock", getenv("XDG_RUNTIME_DIR") ?: "/tmp", kind, name);
    worker_lock = open(lockpath, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    free(lockpath);
    if (worker_lock < 0) return -errno;
    if (flock(worker_lock, LOCK_EX | LOCK_NB) < 0) return -EEXIST;
    char *input_name, *output_name, *common, *args;
    xasprintf(&input_name, "pipemixer.%s.%s.input", kind, name);
    xasprintf(&output_name, "pipemixer.%s.%s.output", kind, name);
    xasprintf(&common, "pipemixer.managed=1 pipemixer.kind=%s pipemixer.group=%s "
              "node.virtual=true priority.session=0 node.dont-fallback=true node.dont-reconnect=true "
              "audio.channels=2 audio.position=[FL FR]", kind, name);
    if (private) {
        char *tagged; xasprintf(&tagged,"%s pipemixer.internal=true",common);free(common);common=tagged;
    }
    if (streq(kind, "effect")) {
        char *tagged;
        xasprintf(&tagged, "%s pipemixer.preset=%s", common,
                  streq(source, "eq") ? "eq" : streq(source, "voice") ? "voice" : "custom");
        free(common); common = tagged;
    }
    if (streq(kind, "monitor")) {
        if (!monitor_valid_scope(source)) { free(input_name); free(output_name); free(common); return -EINVAL; }
        char *tagged; xasprintf(&tagged, "%s pipemixer.monitor-set=%s", common, source); free(common); common = tagged;
    }
    const char *input_class = "Audio/Sink", *output_class = "Audio/Source";
    char *targets_in = xstrdup("node.autoconnect=false"), *targets_out = xstrdup("node.autoconnect=false");
    if (streq(kind, "send")) {
        uint32_t src, dest;
        int result = graph_resolve_node(source, &src);
        if (!result) result = graph_resolve_node(destination, &dest);
        if (!result && (!graph_node_has_ports(src, PW_DIRECTION_OUTPUT)
                         || !graph_node_has_ports(dest, PW_DIRECTION_INPUT))) result = -EINVAL;
        if (!result && graph_would_cycle(src, dest)) result = -ELOOP;
        if (result < 0) {
            free(input_name); free(output_name); free(common); free(targets_in); free(targets_out);
            return result;
        }
        const struct graph_node *src_node = graph_node_find(src), *dst_node = graph_node_find(dest);
        char *source_name = json_quote(graph_node_name(src));
        char *destination_name = json_quote(graph_node_name(dest));
        char *tagged;
        xasprintf(&tagged, "%s pipemixer.source=%s pipemixer.destination=%s", common,
                  source_name, destination_name);
        free(source_name); free(destination_name); free(common); common = tagged;
        const char *src_serial = dict_get(&src_node->props, PW_KEY_OBJECT_SERIAL);
        const char *dst_serial = dict_get(&dst_node->props, PW_KEY_OBJECT_SERIAL);
        char *quoted_src = json_quote(src_serial ?: graph_node_name(src));
        char *quoted_dest = json_quote(dst_serial ?: graph_node_name(dest));
        const char *class = dict_get(&src_node->props, PW_KEY_MEDIA_CLASS);
        bool capture_sink = streq(class, "Audio/Sink") || streq(class, "Stream/Output/Audio");
        free(targets_in); free(targets_out);
        xasprintf(&targets_in, "target.object=%s stream.monitor=true stream.capture.sink=%s", quoted_src, capture_sink ? "true" : "false");
        xasprintf(&targets_out, "target.object=%s", quoted_dest);
        free(quoted_src); free(quoted_dest);
        input_class = "Stream/Input/Audio"; output_class = "Stream/Output/Audio";
    } else if (!streq(kind, "bus") && !streq(kind, "effect") && !streq(kind, "monitor")) {
        free(input_name); free(output_name); free(common); free(targets_in); free(targets_out);
        return -EINVAL;
    }
    char *filter_graph = NULL;
    if (streq(kind, "effect")) {
        char *chain_spec = NULL;
        filter_graph = effect_graph_load_chain(source, &chain_spec);
        if (!filter_graph) {
            int result = -errno;
            free(input_name); free(output_name); free(common); free(targets_in); free(targets_out);
            return result;
        }
        char *quoted_graph = json_quote(filter_graph), *tagged;
        xasprintf(&tagged, "%s pipemixer.filter.graph=%s", targets_in, quoted_graph);
        free(quoted_graph); free(targets_in); targets_in = tagged;
        if (chain_spec) {
            char *quoted = json_quote(chain_spec);
            xasprintf(&tagged, "%s pipemixer.chain=%s", common, quoted);
            free(common); common = tagged; free(quoted); free(chain_spec);
        }
    }
    xasprintf(&args, "audio.channels=2 audio.position=[FL FR] "
              "capture.props={ %s %s node.name=%s node.description=\"%s %s input\" "
              "media.class=%s pipemixer.role=input } "
              "playback.props={ %s %s node.name=%s node.description=\"%s %s output\" "
              "media.class=%s pipemixer.role=output }",
              common, targets_in, input_name, name, kind, input_class,
              common, targets_out, output_name, name, kind, output_class);
    if (streq(kind, "effect")) {
        char *full_args;
        xasprintf(&full_args, "%s filter.graph=%s", args, filter_graph);
        free(args); free(filter_graph); args = full_args;
        char executable[PATH_MAX]; ssize_t length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
        if(length>0) {
            executable[length]=0;char *slash=strrchr(executable,'/');if(slash)*slash=0;
            const char *existing=getenv("LADSPA_PATH");char *paths;
            xasprintf(&paths,"%s:%s:%s:%s",executable,existing?:"/usr/lib64/ladspa:/usr/lib/ladspa","/usr/local/lib/pipemixer",PIPEMIXER_DSP_DIR);
            setenv("LADSPA_PATH",paths,1);free(paths);
        }
    }
    worker_module = pw_context_load_module(pipewire_context(), streq(kind, "effect")
                                          ? "libpipewire-module-filter-chain" : "libpipewire-module-loopback", args, NULL);
    int result = worker_module ? 0 : -(errno ?: EIO);
    if (worker_module && streq(kind, "effect")) result = prime_effect(output_name);
    free(args); free(input_name); free(output_name); free(common); free(targets_in); free(targets_out);
    if (worker_module) {
        pw_impl_module_add_listener(worker_module, &worker_listener, &module_events, NULL);
        pw_loop_add_signal(event_loop, SIGTERM, worker_quit, NULL);
        pw_loop_add_signal(event_loop, SIGINT, worker_quit, NULL);
    }
    if (result >= 0 && !streq(kind, "effect")) worker_notify_ready();
    return result;
}

void managed_worker_cleanup(void) {
    if (worker_prime) { pw_stream_destroy(worker_prime); worker_prime = NULL; }
    if (worker_prime_timer) { pw_loop_destroy_source(event_loop, worker_prime_timer); worker_prime_timer = NULL; }
    if (worker_ready_fd >= 0) { close(worker_ready_fd); worker_ready_fd = -1; }
    if (worker_module) {
        spa_hook_remove(&worker_listener);
        pw_impl_module_destroy(worker_module);
        worker_module = NULL;
    }
    if (worker_lock >= 0) { close(worker_lock); worker_lock = -1; }
}
