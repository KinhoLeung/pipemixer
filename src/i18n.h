#pragma once

#include <stdbool.h>
#include <wchar.h>

enum ui_language { UI_LANGUAGE_AUTO, UI_LANGUAGE_EN, UI_LANGUAGE_ZH_CN };

bool i18n_parse_language(const char *name, enum ui_language *language);
void i18n_init(enum ui_language language);
bool i18n_toggle_language(void);
bool i18n_is_chinese(void);
const char *tr(const char *message);
const wchar_t *trw(const wchar_t *message);
/* Display only: identifiers passed to PipeWire and automation stay unchanged. */
const char *tr_parameter(const char *name);
