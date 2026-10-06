/* SPDX-License-Identifier: MIT
 * Deterministic PCM generator/capture for board integration tests.
 * The realtime callback only processes preallocated memory; the main thread
 * writes captured samples after the stream has stopped.
 */
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/iec958-utils.h>

struct probe {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_source *done;
    bool capture;
    bool failed;
    float *samples;
    size_t count, capacity;
    double phase, frequency;
};

static void stop(void *data, uint64_t count) { pw_main_loop_quit(((struct probe *)data)->loop); }
static void signal_stop(void *data, int signal) { pw_main_loop_quit(((struct probe *)data)->loop); }

static void process(void *data) {
    struct probe *probe = data;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(probe->stream);
    if (!buffer) return;
    struct spa_data *audio = &buffer->buffer->datas[0];
    if (!audio->data || !audio->chunk) { pw_stream_queue_buffer(probe->stream, buffer); return; }
    if (probe->capture) {
        size_t offset = audio->chunk->offset;
        size_t bytes = offset < audio->maxsize ? SPA_MIN(audio->chunk->size, audio->maxsize - offset) : 0;
        size_t count = SPA_MIN(bytes / sizeof(float), probe->capacity - probe->count);
        memcpy(probe->samples + probe->count, (char *)audio->data + offset, count * sizeof(float));
        probe->count += count;
        if (probe->count == probe->capacity) pw_loop_signal_event(pw_main_loop_get_loop(probe->loop), probe->done);
    } else {
        unsigned frames = audio->maxsize / (2 * sizeof(float));
        if (buffer->requested) frames = SPA_MIN(frames, buffer->requested);
        float *pcm = audio->data;
        for (unsigned i = 0; i < frames; i++) {
            float sample = .125f * sin(probe->phase);
            probe->phase += 2 * M_PI * probe->frequency / 48000;
            if (probe->phase >= 2 * M_PI) probe->phase -= 2 * M_PI;
            pcm[2 * i] = pcm[2 * i + 1] = sample;
        }
        audio->chunk->offset = 0;
        audio->chunk->stride = 2 * sizeof(float);
        audio->chunk->size = frames * 2 * sizeof(float);
    }
    pw_stream_queue_buffer(probe->stream, buffer);
}

static void state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    struct probe *probe = data;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "PCM stream failed: %s\n", error ?: "unknown error");
        probe->failed = true; pw_main_loop_quit(probe->loop);
    }
}
static const struct pw_stream_events events = {
    .version = PW_VERSION_STREAM_EVENTS, .process = process, .state_changed = state_changed,
};

static void reject_stop(void *data, int signal) { pw_main_loop_quit(data); }

/* Expose raw S16 ports that cannot negotiate with the DSP F32 monitor ports.
 * This exercises real link errors, including two failed fallback candidates. */
static int reject_target(const char *name, bool digital) {
    pw_init(NULL, NULL);
    struct pw_main_loop *main = pw_main_loop_new(NULL);
    struct pw_loop *loop = pw_main_loop_get_loop(main);
    pw_loop_add_signal(loop, SIGINT, reject_stop, main);
    pw_loop_add_signal(loop, SIGTERM, reject_stop, main);
    static const struct pw_filter_events filter_events = {.version = PW_VERSION_FILTER_EVENTS};
    struct pw_filter *filter = pw_filter_new_simple(loop, "PipeMixer incompatible test sink",
        pw_properties_new(PW_KEY_NODE_NAME, name, PW_KEY_MEDIA_CLASS, "Audio/Sink",
                          PW_KEY_NODE_VIRTUAL, "true", "node.autoconnect", "false", "priority.session", "0", NULL),
        &filter_events, NULL);
    if (!filter) return 1;
    for (unsigned i = 0; i < 2; i++) {
        uint8_t storage[512]; struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const struct spa_pod *format = digital ? spa_format_audio_iec958_build(&builder, SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_IEC958_INIT(.codec = SPA_AUDIO_IEC958_CODEC_DTS, .rate = 48000))
            : spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
            &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16, .rate = 48000, .channels = 1,
                                    .position = {i ? SPA_AUDIO_CHANNEL_FR : SPA_AUDIO_CHANNEL_FL}));
        if (!pw_filter_add_port(filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
                pw_properties_new(PW_KEY_PORT_NAME, i ? "playback_FR" : "playback_FL",
                                  PW_KEY_AUDIO_CHANNEL, i ? "FR" : "FL", NULL), &format, 1)) return 1;
    }
    if (pw_filter_connect(filter, PW_FILTER_FLAG_NONE, NULL, 0) < 0) return 1;
    pw_main_loop_run(main);
    pw_filter_destroy(filter); pw_main_loop_destroy(main); pw_deinit(); return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "reject")) return reject_target(argv[2], false);
    if (argc == 3 && !strcmp(argv[1], "reject-digital")) return reject_target(argv[2], true);
    bool capture_sink = argc > 1 && !strcmp(argv[1], "capture-sink");
    bool reconnect = argc > 1 && !strcmp(argv[1], "play-reconnect");
    if (argc < 4 || (strcmp(argv[1], "play") && strcmp(argv[1], "capture") && !capture_sink && !reconnect)) return 2;
    struct probe probe = { .capture = strcmp(argv[1], "play") != 0 && !reconnect, .frequency = argc > 4 ? atof(argv[4]) : 440 };
    pw_init(NULL, NULL);
    probe.loop = pw_main_loop_new(NULL);
    struct pw_loop *loop = pw_main_loop_get_loop(probe.loop);
    pw_loop_add_signal(loop, SIGTERM, signal_stop, &probe);
    pw_loop_add_signal(loop, SIGINT, signal_stop, &probe);
    probe.done = pw_loop_add_event(loop, stop, &probe);
    if (probe.capture) {
        probe.capacity = 48000 * 2 * 2;
        probe.samples = calloc(probe.capacity, sizeof(float));
    }
    struct pw_properties *props = pw_properties_new(
        PW_KEY_NODE_NAME, probe.capture ? "pipemixer.board-test.capture" : argv[2],
        /* WirePlumber initializes playback channel controls through its state
         * hook. Give each fixture its own key instead of sharing saved gains. */
        PW_KEY_MEDIA_NAME, argv[2],
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, probe.capture ? "Capture" : "Playback",
        PW_KEY_TARGET_OBJECT, probe.capture ? argv[2] : argv[3],
        PW_KEY_NODE_DONT_RECONNECT, reconnect ? "false" : "true", "node.dont-fallback", "true",
        "state.restore-props", probe.capture ? "false" : "true", "state.restore-target", "false",
        PW_KEY_NODE_VIRTUAL, "true", NULL);
    if (capture_sink) { pw_properties_set(props, "stream.capture.sink", "true"); pw_properties_set(props, "stream.monitor", "true"); }
    probe.stream = pw_stream_new_simple(loop, reconnect ? "PipeMixer reconnect regression" : "PipeMixer PCM regression", props, &events, &probe);
    uint8_t storage[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod *format = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = 48000, .channels = 2,
                                .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }));
    if (pw_stream_connect(probe.stream, probe.capture ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                          PW_ID_ANY, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
                          &format, 1) < 0) return 1;
    pw_main_loop_run(probe.loop);
    pw_stream_destroy(probe.stream);
    int status = probe.failed;
    if (probe.capture) {
        FILE *file = fopen(argv[3], "wb");
        if (!file || fwrite(probe.samples, sizeof(float), probe.count, file) != probe.count) status = 1;
        if (file) fclose(file);
        if (probe.count != probe.capacity) status = 1;
    }
    free(probe.samples);
    pw_main_loop_destroy(probe.loop);
    pw_deinit();
    return status;
}
