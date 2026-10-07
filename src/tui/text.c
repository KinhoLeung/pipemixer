#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "tui/text.h"
#include "xmalloc.h"

static wchar_t *decode(const char *text) {
    if (!text) text = "";
    size_t bytes = strlen(text), count = 0;
    wchar_t *result = xmalloc((bytes + 1) * sizeof(*result));
    mbstate_t state = {0};
    while (bytes) {
        wchar_t wc;
        size_t consumed = mbrtowc(&wc, text, bytes, &state);
        if (consumed == (size_t)-1 || consumed == (size_t)-2) {
            wc = L'?'; consumed = 1; memset(&state, 0, sizeof(state));
        }
        if (!consumed) break;
        result[count++] = wcwidth(wc) < 0 ? L'?' : wc;
        text += consumed; bytes -= consumed;
    }
    result[count] = 0;
    return result;
}

int tui_wide_width(const wchar_t *text) {
    int width = 0;
    if (text) for (; *text; text++) {
        int columns = wcwidth(*text);
        width += columns < 0 ? 1 : columns;
    }
    return width;
}

int tui_text_width(const char *text) {
    wchar_t *wide = decode(text);
    int width = tui_wide_width(wide);
    free(wide);
    return width;
}

int tui_write_wide(WINDOW *win, int y, int x, const wchar_t *text, int columns) {
    if (!win || !text || columns <= 0 || wmove(win, y, x) == ERR) return 0;
    int remaining = getmaxx(win) - x;
    if (columns > remaining) columns = remaining;
    int used = 0, length = 0;
    bool clipped = tui_wide_width(text) > columns;
    int limit = columns - (clipped ? 1 : 0);
    while (text[length]) {
        int width = wcwidth(text[length]);
        if (width < 0) width = 1;
        if (used + width > limit) break;
        used += width; length++;
    }
    if (length) {
        wchar_t *safe = xmalloc((length + 1) * sizeof(*safe));
        for (int i = 0; i < length; i++) safe[i] = wcwidth(text[i]) < 0 ? L'?' : text[i];
        safe[length] = 0;
        waddnwstr(win, safe, length);
        free(safe);
    }
    if (clipped) { waddwstr(win, wcwidth(L'…') < 0 ? L"." : L"…"); used++; }
    return used;
}

int tui_write_text(WINDOW *win, int y, int x, const char *text, int columns) {
    wchar_t *wide = decode(text);
    int width = tui_write_wide(win, y, x, wide, columns);
    free(wide);
    return width;
}
