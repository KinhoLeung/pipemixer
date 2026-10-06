#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <wctype.h>

#include "tui/routing.h"
#include "tui/routing-view.h"
#include "tui/routing-batch.h"
#include "pw/graph.h"
#include "xmalloc.h"
#include "utils.h"
#include "macros.h"

static struct {
    WINDOW *win;
    struct routing_axis outputs, inputs;
    struct routing_filter filter;
    struct routing_marks marked_outputs, marked_inputs;
    struct routing_batch batch;
    bool searching;
    char previous_filter[128];
    int height, width, left, columns, visible_rows;
    int pending;
    char monitor[128];
    uint32_t pending_output, pending_input;
    char message[256];
} routing = { .pending = -1 };

static void rebuild(void) {
    bool removed = routing_marks_prune(&routing.marked_outputs);
    removed |= routing_marks_prune(&routing.marked_inputs);
    if (removed) {
        snprintf(routing.message, sizeof(routing.message), "Removed marks for changed or missing ports");
        routing_batch_abort(&routing.batch, "selected endpoint changed or disappeared");
    }
    routing_view_build(&routing.outputs, PW_DIRECTION_OUTPUT, &routing.filter);
    routing_view_build(&routing.inputs, PW_DIRECTION_INPUT, &routing.filter);
}
static void clear_marks(void) {
    routing_marks_clear(&routing.marked_outputs); routing_marks_clear(&routing.marked_inputs);
}
static void mark(bool input, bool node) {
    if (routing.batch.active) { snprintf(routing.message, sizeof(routing.message), "Wait for batch completion or Esc to cancel"); return; }
    uint32_t *ids;
    unsigned count = routing_view_members(input ? &routing.inputs : &routing.outputs,
        input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, &routing.filter, node, &ids);
    if (!routing_marks_toggle(input ? &routing.marked_inputs : &routing.marked_outputs, ids, count))
        snprintf(routing.message, sizeof(routing.message), "Selection limit is 256 ports per side");
    else snprintf(routing.message, sizeof(routing.message), "Marked output:%u input:%u | c connect / d disconnect / u clear",
                  routing.marked_outputs.count, routing.marked_inputs.count);
    free(ids);
}
static bool item_marked(const struct routing_axis *axis, unsigned index, bool input) {
    const struct routing_marks *marks = input ? &routing.marked_inputs : &routing.marked_outputs;
    if (axis->items[index].port != PW_ID_ANY) return routing_marks_has(marks, axis->items[index].port);
    struct routing_axis focused = *axis; focused.cursor = index;
    uint32_t *ids; unsigned count = routing_view_members(&focused, input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, &routing.filter, false, &ids);
    bool marked = false;
    for (unsigned i = 0; i < count; i++) marked |= routing_marks_has(marks, ids[i]);
    free(ids); return marked;
}

static void text(int y, int x, int columns, const char *value) {
    if (columns <= 0 || y < 0 || y >= routing.height || x >= routing.width) return;
    if (wmove(routing.win, y, x) != OK) return;
    mbstate_t state = {0};
    const char *cursor = value ?: "";
    while (*cursor && columns > 0) {
        wchar_t wc;
        size_t bytes = mbrtowc(&wc, cursor, strlen(cursor), &state);
        if (bytes == (size_t)-1 || bytes == (size_t)-2) {
            wc = L'?'; bytes = 1; memset(&state, 0, sizeof(state));
        }
        int width = MAX(0, wcwidth(wc));
        if (width > columns) break;
        if (wc < L' ') wc = L'?';
        wchar_t string[2] = {wc, L'\0'};
        waddwstr(routing.win, string);
        columns -= width;
        cursor += bytes;
    }
}

static void endpoint_text(int y, const char *prefix, const struct routing_axis *axis) {
    const struct routing_item *item = routing_view_selected(axis);
    if (!item) return;
    uint32_t id = item->port;
    if (id == PW_ID_ANY) {
        char *value; xasprintf(&value, "%s group: %s (%s)", prefix, item->label, item->folded ? "folded" : "expanded");
        text(y, 0, routing.width, value); free(value); return;
    }
    const struct graph_port *port = graph_port_find(id);
    char *value;
    xasprintf(&value, "%s %s:%s (id:%u)", prefix, port ? graph_node_name(port->node_id) ?: "?" : "?",
              port ? dict_get(&port->props, PW_KEY_PORT_NAME) ?: "?" : "?", id);
    text(y, 0, routing.width, value);
    free(value);
}

static void check_pending(void) {
    if (routing.pending < 0) return;
    const struct graph_link *link = graph_link_between(routing.pending_output, routing.pending_input);
    if (!graph_port_find(routing.pending_output) || !graph_port_find(routing.pending_input)) {
        routing_error("Selected port disappeared");
    } else if (link && link->info->state == PW_LINK_STATE_ERROR) {
        routing_error(link->info->error ?: "Connection failed");
    } else if ((!routing.pending && !link) ||
               (routing.pending && link && link->info->state >= PW_LINK_STATE_PAUSED)) {
        snprintf(routing.message, sizeof(routing.message), "%s", routing.pending ? "Connected" : "Disconnected");
        routing.pending = -1;
    }
}

void routing_draw(int height, int width) {
    routing.height = MAX(1, height - 1);
    routing.width = MAX(1, width);
    if (!routing.win) routing.win = newwin(routing.height, routing.width, 1, 0);
    else wresize(routing.win, routing.height, routing.width);
    if (!routing.win) return;
    rebuild();
    check_pending();
    werase(routing.win);
    char header[256];
    snprintf(header, sizeof(header), "Routing matrix%s%s | M:monitor S:Solo L:listen U:clear Solo | ?:help",
             *routing.monitor ? " | " : "", routing.monitor);
    text(0, 0, width, header);
    if (height < 8 || width < 28) {
        text(2, 0, width, "Increase terminal size to see the matrix");
        wnoutrefresh(routing.win); return;
    }
    routing.left = MIN(40, MAX(12, width / 2));
    routing.columns = MAX(1, (width - routing.left) / 7);
    routing.visible_rows = routing.height - 6;
    routing.outputs.scroll = MIN(routing.outputs.scroll, routing.outputs.cursor);
    if (routing.outputs.cursor >= routing.outputs.scroll + routing.visible_rows) routing.outputs.scroll = routing.outputs.cursor - routing.visible_rows + 1;
    routing.inputs.scroll = MIN(routing.inputs.scroll, routing.inputs.cursor);
    if (routing.inputs.cursor >= routing.inputs.scroll + routing.columns) routing.inputs.scroll = routing.inputs.cursor - routing.columns + 1;
    char context[96]; snprintf(context, sizeof(context), "%s/%s O:%u I:%u", routing_grouping_name(routing.filter.grouping), routing_kind_name(routing.filter.kind), routing.marked_outputs.count, routing.marked_inputs.count);
    text(1, 0, routing.left, context);
    for (int c = 0; c < routing.columns && c + routing.inputs.scroll < (int)routing.inputs.count; c++) {
        const struct routing_item *item = &routing.inputs.items[c + routing.inputs.scroll];
        bool marked = item_marked(&routing.inputs, c + routing.inputs.scroll, true);
        char label[32]; snprintf(label, sizeof(label), "#%u%s", item->port, marked ? "*" : "");
        if (c + routing.inputs.scroll == routing.inputs.cursor) wattron(routing.win, A_REVERSE);
        if (item->port == PW_ID_ANY) text(1, routing.left + c * 7, 6, item->label);
        else
        text(1, routing.left + c * 7, 6, label);
        wattroff(routing.win, A_REVERSE);
    }
    if (!routing.outputs.count || !routing.inputs.count) text(3, 0, width, "No matching audio ports available");
    for (int r = 0; r < routing.visible_rows && r + routing.outputs.scroll < (int)routing.outputs.count; r++) {
        const struct routing_item *item = &routing.outputs.items[r + routing.outputs.scroll];
        uint32_t id = item->port;
        if (r + routing.outputs.scroll == routing.outputs.cursor) wattron(routing.win, A_BOLD);
        char *label; xasprintf(&label, "%s%s", item_marked(&routing.outputs, r + routing.outputs.scroll, false) ? "* " : "", item->label);
        text(r + 2, 0, routing.left - 1, label); free(label);
        wattroff(routing.win, A_BOLD);
        for (int c = 0; c < routing.columns && c + routing.inputs.scroll < (int)routing.inputs.count; c++) {
            uint32_t input = routing.inputs.items[c + routing.inputs.scroll].port;
            const struct graph_link *link = id != PW_ID_ANY && input != PW_ID_ANY ? graph_link_between(id, input) : NULL;
            const char *cell = id == PW_ID_ANY || input == PW_ID_ANY ? "  :  " : !link ? "  .  " : link->info->state == PW_LINK_STATE_ERROR ? "  !  "
                               : link->info->state >= PW_LINK_STATE_PAUSED ? "  +  " : "  ~  ";
            if (r + routing.outputs.scroll == routing.outputs.cursor && c + routing.inputs.scroll == routing.inputs.cursor)
                wattron(routing.win, A_REVERSE);
            text(r + 2, routing.left + c * 7, 6, cell);
            wattroff(routing.win, A_REVERSE);
        }
    }
    endpoint_text(routing.height - 3, "Output:", &routing.outputs);
    endpoint_text(routing.height - 2, "Input: ", &routing.inputs);
    char search[512]; snprintf(search, sizeof(search), "Find: %s%s | %s", routing.filter.text, routing.searching ? "_" : "", routing.searching ? "Enter apply / Esc cancel" : routing.message);
    text(routing.height - 1, 0, width, routing.searching || *routing.filter.text ? search : routing.message[0] ? routing.message : "+ connected  . disconnected  ! error  ~ negotiating");
    wnoutrefresh(routing.win);
}

static void toggle(void) {
    const struct routing_item *out = routing_view_selected(&routing.outputs), *in = routing_view_selected(&routing.inputs);
    if (!out || !in) return;
    if (out->port == PW_ID_ANY || in->port == PW_ID_ANY) {
        routing_view_fold(out->port == PW_ID_ANY ? &routing.outputs : &routing.inputs, &routing.filter);
        return;
    }
    if (routing.pending >= 0 || routing.batch.active) return;
    uint32_t output = out->port, input = in->port;
    bool connected = graph_link_between(output, input) != NULL;
    int result = connected ? graph_disconnect(output, input) : graph_connect(output, input);
    if (result < 0) {
        routing_error(result == -ELOOP ? "Connection would create a feedback loop" : strerror(-result));
    } else {
        routing.pending = !connected;
        routing.pending_output = output;
        routing.pending_input = input;
        snprintf(routing.message, sizeof(routing.message), "%s", connected ? "Disconnecting..." : "Connecting...");
    }
}

bool routing_key(wint_t key, bool special) {
    rebuild();
    if (routing.batch.active && (key == 27 || key == 'q' || key == 'r' || key == 'x' || key == 'X'
        || key == 'n' || key == 'N' || key == 'u' || key == 'c' || key == 'd' || key == 'f' || key == '/'
        || key == 'M' || key == 'S' || key == 'L' || key == 'U')) {
        if (key == 27) routing_batch_abort(&routing.batch, "cancelled");
        else snprintf(routing.message, sizeof(routing.message), "Wait for batch completion or Esc to cancel");
        return true;
    }
    if (routing.searching && key != KEY_RESIZE) {
        size_t length = strlen(routing.filter.text);
        if (key == 27) { strcpy(routing.filter.text, routing.previous_filter); routing.searching = false; }
        else if (key == '\n' || key == KEY_ENTER) {
            if (!streq(routing.filter.text, routing.previous_filter)) clear_marks();
            routing.searching = false;
        }
        else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (length) { do { length--; } while (length && ((unsigned char)routing.filter.text[length] & 0xc0) == 0x80); routing.filter.text[length] = 0; }
        } else if (key == 21) routing.filter.text[0] = 0;
        else if (!special && iswprint(key)) {
            char encoded[MB_LEN_MAX]; mbstate_t state = {0}; size_t count = wcrtomb(encoded, key, &state);
            if (count != (size_t)-1 && length + count < sizeof(routing.filter.text)) {
                memcpy(routing.filter.text + length, encoded, count); routing.filter.text[length + count] = 0;
            }
        }
        return true;
    }
    switch (key) {
    case KEY_UP: case 'k': routing.outputs.cursor = MAX(0, routing.outputs.cursor - 1); break;
    case KEY_DOWN: case 'j': routing.outputs.cursor = MIN(MAX(0, (int)routing.outputs.count - 1), routing.outputs.cursor + 1); break;
    case KEY_LEFT: case 'h': routing.inputs.cursor = MAX(0, routing.inputs.cursor - 1); break;
    case KEY_RIGHT: case 'l': routing.inputs.cursor = MIN(MAX(0, (int)routing.inputs.count - 1), routing.inputs.cursor + 1); break;
    case 'g': routing.outputs.cursor = routing.inputs.cursor = 0; break;
    case 'G': routing.outputs.cursor = MAX(0, (int)routing.outputs.count - 1); break;
    case 'v': routing.filter.grouping = (routing.filter.grouping + 1) % 3; break;
    case 'f': clear_marks(); routing.filter.kind = (routing.filter.kind + 1) % ROUTING_KIND_COUNT; break;
    case 'x': case 'X': mark(key == 'X', false); break;
    case 'n': case 'N': mark(key == 'N', true); break;
    case 'u': clear_marks(); snprintf(routing.message, sizeof(routing.message), "Selection cleared"); break;
    case 'c': case 'd': {
        int result = routing.pending >= 0 ? -EBUSY : routing_batch_start(&routing.batch, &routing.marked_outputs, &routing.marked_inputs, key == 'd');
        if (result == 0) snprintf(routing.message, sizeof(routing.message), "%s", routing.batch.message);
        else snprintf(routing.message, sizeof(routing.message), "%s", result == -ELOOP ? "Batch rejected: feedback loop; no links changed"
            : result == -EINVAL ? "Mark output ports with x/n and inputs with X/N first"
            : result == -ENOTSUP ? "Marked ports have no compatible audio channels" : strerror(-result));
        break;
    }
    case '?': snprintf(routing.message, sizeof(routing.message), "hjkl move; Enter/Space link; z/Z fold; x/X mark; n/N node; c/d batch; u clear; a/A rule; r back"); break;
    case '/': strcpy(routing.previous_filter, routing.filter.text); routing.searching = true; break;
    case 'z': case 'Z':
        if (!routing_view_fold(key == 'z' ? &routing.outputs : &routing.inputs, &routing.filter))
            snprintf(routing.message, sizeof(routing.message), "Press v to group ports before folding");
        break;
    case ' ': case '\n': case KEY_ENTER: toggle(); break;
    default: return false;
    }
    return true;
}

void routing_mouse(const MEVENT *event) {
    if (routing.searching) return;
    rebuild();
    if (event->bstate & BUTTON4_PRESSED) routing_key(KEY_UP, true);
    else if (event->bstate & BUTTON5_PRESSED) routing_key(KEY_DOWN, true);
    else if (event->bstate & BUTTON1_PRESSED) {
        int row = event->y - 3 + routing.outputs.scroll;
        int column = (event->x - routing.left) / 7 + routing.inputs.scroll;
        if (event->x >= routing.left && column >= 0 && column < (int)routing.inputs.count && event->y == 2) {
            routing.inputs.cursor = column;
            if (event->bstate & BUTTON_SHIFT) mark(true, false);
            else if (routing.inputs.items[column].port == PW_ID_ANY) routing_view_fold(&routing.inputs, &routing.filter);
        } else if (row >= 0 && row < (int)routing.outputs.count && event->y >= 3 && event->y < routing.height - 2) {
            routing.outputs.cursor = row;
            if (event->x < routing.left) {
                if (event->bstate & BUTTON_SHIFT) mark(false, false);
                else if (routing.outputs.items[row].port == PW_ID_ANY) routing_view_fold(&routing.outputs, &routing.filter);
            } else if (column >= 0 && column < (int)routing.inputs.count) {
                routing.inputs.cursor = column; toggle();
            }
        }
    }
}

void routing_error(const char *message) {
    snprintf(routing.message, sizeof(routing.message), "%s", message ?: "PipeWire error");
    routing.pending = -1;
    routing_batch_abort(&routing.batch, message ?: "PipeWire error");
}
bool routing_poll(void) {
    bool removed = routing_marks_prune(&routing.marked_outputs);
    removed |= routing_marks_prune(&routing.marked_inputs);
    if (removed) {
        routing_batch_abort(&routing.batch, "selected endpoint changed or disappeared");
        snprintf(routing.message, sizeof(routing.message), "Removed marks for changed or missing ports");
    }
    bool changed = routing_batch_step(&routing.batch);
    if (changed) snprintf(routing.message, sizeof(routing.message), "%s", routing.batch.message);
    return removed || changed;
}

bool routing_selected(uint32_t *output, uint32_t *input) {
    rebuild();
    const struct routing_item *out = routing_view_selected(&routing.outputs), *in = routing_view_selected(&routing.inputs);
    if (!out || !in || out->port == PW_ID_ANY || in->port == PW_ID_ANY) return false;
    *output = out->port; *input = in->port; return true;
}
uint32_t routing_selected_node(bool input) {
    rebuild(); uint32_t *ports = NULL;
    unsigned count = routing_view_members(input ? &routing.inputs : &routing.outputs,
        input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, &routing.filter, false, &ports);
    uint32_t id = PW_ID_ANY;
    for (unsigned i = 0; i < count; i++) {
        const struct graph_port *port = graph_port_find(ports[i]);
        if (!port || (id != PW_ID_ANY && id != port->node_id)) { id = PW_ID_ANY; break; }
        id = port->node_id;
    }
    free(ports); return id;
}
void routing_monitor_status(const char *status) { snprintf(routing.monitor, sizeof(routing.monitor), "%s", status ?: ""); }

void routing_cleanup(void) {
    if (routing.win) delwin(routing.win);
    routing_view_clear(&routing.outputs); routing_view_clear(&routing.inputs); routing_filter_clear(&routing.filter);
    clear_marks(); routing_batch_clear(&routing.batch);
    memset(&routing, 0, sizeof(routing));
    routing.pending = -1;
}
