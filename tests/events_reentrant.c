/* Event callbacks can grow the queue while their event is being dispatched. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "events.h"

static struct event_emitter *emitter;
static unsigned callbacks, completed;

static void after(union event_data data) {
    assert(data.u == 1234);
    completed++;
}

static void dispatch(uint64_t id, union event_data data, const void *table,
                     void *user, void *private) {
    callbacks++;
    if (id == 0) {
        for (unsigned i = 0; i < 100; i++)
            event_emit(emitter, NULL, 1, NULL, 'u', (uint64_t)i);
    }
}

int main(void) {
    assert(events_global_init() >= 0);
    emitter = event_emitter_create(dispatch);
    struct event_hook *hook = event_emitter_add_hook(emitter, NULL, NULL, NULL, NULL);
    event_emit(emitter, NULL, 0, after, 'u', (uint64_t)1234);
    for (unsigned i = 0; i < 14; i++)
        event_emit(emitter, NULL, 1, NULL, 'u', (uint64_t)i);
    events_dispatch();
    assert(callbacks == 115 && completed == 1);
    event_hook_release(hook);
    event_emitter_release(emitter);
    puts("PASS callbacks grow the event queue without invalidating the dispatched event");
    return 0;
}
