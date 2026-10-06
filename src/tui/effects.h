#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>
bool effect_ui_open(uint32_t id);
bool effect_ui_key(wint_t key);
bool effect_ui_poll(void);
bool effect_ui_active(void);
bool effect_ui_quit(void);
void effect_ui_cancel(void);
void effect_ui_cleanup(void);
