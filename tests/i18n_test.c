#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "i18n.h"
#include "tui/text.h"
#include "collections/wstring.h"

static const struct { const char *english, *chinese; } source_messages[] = {
#define I18N(english, chinese) {english, chinese},
#include "i18n-messages.h"
#undef I18N
};

static wchar_t cell(WINDOW *window, int row, int column) {
    cchar_t character;
    wchar_t value[CCHARW_MAX];
    attr_t attributes;
    short pair;
    assert(mvwin_wch(window, row, column, &character) == OK);
    assert(getcchar(&character, value, &attributes, &pair, NULL) == OK);
    return value[0];
}

int main(void) {
    assert(setlocale(LC_CTYPE, "C.UTF-8") || setlocale(LC_CTYPE, "en_US.UTF-8")
           || setlocale(LC_CTYPE, "zh_CN.utf8"));
    unsetenv("PIPEMIXER_LANGUAGE");
    unsetenv("LC_ALL");
    unsetenv("LC_MESSAGES");
    setenv("LANG", "zh_CN.UTF-8", 1);
    i18n_init(UI_LANGUAGE_AUTO);
    assert(i18n_is_chinese());
    assert(!strcmp(tr("Scenes | startup: %s"), "场景 | 开机恢复：%s"));
    assert(!strcmp(tr_parameter("compressor1:Threshold dB"), "compressor1:阈值 dB"));
    assert(!strcmp(tr_parameter("custom:Untranslated"), "custom:Untranslated"));
    wchar_t selection[128];
    assert(swprintf(selection, 128, trw(L"Select %ls for %ls"), L"输出", L"中文流") > 0);
    assert(!wcscmp(selection, L"为 中文流 选择输出"));
    for (unsigned i = 0; i < sizeof(source_messages) / sizeof(source_messages[0]); i++) {
        if (i) assert(strcmp(source_messages[i - 1].english, source_messages[i].english) < 0);
        assert(!strcmp(tr(source_messages[i].english), source_messages[i].chinese));
    }

    setenv("PIPEMIXER_LANGUAGE", "en", 1);
    i18n_init(UI_LANGUAGE_ZH_CN);
    assert(!i18n_is_chinese());
    assert(!strcmp(tr("Playback"), "Playback"));
    unsetenv("PIPEMIXER_LANGUAGE");
    setenv("LC_MESSAGES", "en_US.UTF-8", 1);
    i18n_init(UI_LANGUAGE_AUTO);
    assert(!i18n_is_chinese());
    setenv("LC_ALL", "zh_CN.UTF-8", 1);
    i18n_init(UI_LANGUAGE_AUTO);
    assert(i18n_is_chinese());
    assert(i18n_toggle_language() && !i18n_is_chinese());
    assert(i18n_toggle_language() && i18n_is_chinese());
    enum ui_language language;
    assert(i18n_parse_language("zh-CN", &language) && language == UI_LANGUAGE_ZH_CN);
    assert(!i18n_parse_language("bad-language", &language));

    assert(tui_text_width("中文AB") == 6);
    assert(tui_text_width("e\xcc\x81中A") == 4);
    struct wstring text = {0};
    assert(wstring_appendsz(&text, "中文"));
    assert(text.len == 2);
    const char invalid[] = {'x', (char)0xff, 'y', 0};
    assert(!wstring_appendsz(&text, invalid));
    assert(text.len == 2 && !wcscmp(text.data, L"中文"));
    wstring_free(&text);

    FILE *input = tmpfile(), *output = tmpfile();
    assert(input && output);
    SCREEN *screen = newterm("xterm", output, input);
    assert(screen);
    WINDOW *window = newwin(5, 12, 0, 0);
    assert(window);
    for (int y = 0; y < 5; y++) for (int x = 0; x < 12; x++) mvwaddch(window, y, x, '.');
    assert(tui_write_text(window, 1, 1, "中文AB中文", 7) == 7);
    assert(cell(window, 1, 7) == L'…');
    assert(cell(window, 1, 8) == L'.');
    assert(cell(window, 2, 1) == L'.');
    assert(tui_write_wide(window, 2, 8, L"中文ABCDE", 8) == 3);
    assert(cell(window, 2, 10) == L'…');
    assert(cell(window, 2, 11) == L'.');
    assert(tui_write_text(window, 3, 1, "A\nB", 8) == 3);
    assert(cell(window, 3, 2) == L'?');
    assert(tui_write_text(window, 4, 1, invalid, 8) == 3);
    assert(cell(window, 4, 2) == L'?');
    delwin(window);
    endwin();
    delscreen(screen);
    fclose(input); fclose(output);

    assert(setlocale(LC_CTYPE, "C"));
    i18n_init(UI_LANGUAGE_ZH_CN);
    assert(!i18n_is_chinese() && !i18n_toggle_language());
    assert(!strcmp(tr("Playback"), "Playback"));
    puts("PASS i18n selection, catalog lookup, parameters, UTF-8 errors and CJK cell clipping");
    return 0;
}
