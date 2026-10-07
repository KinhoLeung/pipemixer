#include <langinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "i18n.h"

struct translation {
    const char *english, *chinese;
    const wchar_t *wide_english, *wide_chinese;
};

/* Sorted by the English text; the catalog is compiled into the binary. */
static const struct translation messages[] = {
#define I18N(english, chinese) {english, chinese, L##english, L##chinese},
#include "i18n-messages.h"
#undef I18N
};

static bool chinese;

static bool utf8_locale(void) {
    const char *codeset = nl_langinfo(CODESET);
    return codeset && (!strcasecmp(codeset, "UTF-8") || !strcasecmp(codeset, "UTF8"));
}

bool i18n_parse_language(const char *name, enum ui_language *language) {
    if (!name || !language) return false;
    if (!strcmp(name, "auto")) *language = UI_LANGUAGE_AUTO;
    else if (!strcmp(name, "en")) *language = UI_LANGUAGE_EN;
    else if (!strcmp(name, "zh_CN") || !strcmp(name, "zh-CN") || !strcmp(name, "zh"))
        *language = UI_LANGUAGE_ZH_CN;
    else return false;
    return true;
}

void i18n_init(enum ui_language language) {
    const char *override = getenv("PIPEMIXER_LANGUAGE");
    if (override && *override) i18n_parse_language(override, &language);
    if (language == UI_LANGUAGE_AUTO) {
        const char *locale = getenv("LC_ALL");
        if (!locale || !*locale) locale = getenv("LC_MESSAGES");
        if (!locale || !*locale) locale = getenv("LANG");
        language = locale && !strncasecmp(locale, "zh", 2)
            && (!locale[2] || locale[2] == '_' || locale[2] == '-' || locale[2] == '.')
            ? UI_LANGUAGE_ZH_CN : UI_LANGUAGE_EN;
    }
    /* Respect explicitly selected byte locales and keep curses usable there. */
    chinese = language == UI_LANGUAGE_ZH_CN && utf8_locale();
}

bool i18n_toggle_language(void) {
    if (!chinese && !utf8_locale()) return false;
    chinese = !chinese;
    return true;
}

bool i18n_is_chinese(void) { return chinese; }

const char *tr(const char *message) {
    if (!chinese || !message) return message;
    size_t low = 0, high = sizeof(messages) / sizeof(messages[0]);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(message, messages[middle].english);
        if (!order) return messages[middle].chinese;
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return message;
}

const wchar_t *trw(const wchar_t *message) {
    if (!chinese || !message) return message;
    size_t low = 0, high = sizeof(messages) / sizeof(messages[0]);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = wcscmp(message, messages[middle].wide_english);
        if (!order) return messages[middle].wide_chinese;
        if (order < 0) high = middle;
        else low = middle + 1;
    }
    return message;
}

const char *tr_parameter(const char *name) {
    if (!chinese || !name) return name;
    const char *colon = strchr(name, ':');
    if (!colon) return tr(name);
    const char *label = tr(colon + 1);
    if (label == colon + 1) return name;
    static char labels[8][512];
    static unsigned next;
    char *result = labels[next++ % 8];
    snprintf(result, sizeof(labels[0]), "%.*s:%s", (int)(colon - name), name, label);
    return result;
}
