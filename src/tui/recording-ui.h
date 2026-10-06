#pragma once
#include <stdbool.h>
#include <wchar.h>
void recording_ui_cancel(void);
void recording_ui_cleanup(void);
bool recording_ui_poll(void);
bool recording_ui_key(wint_t key);
bool recording_ui_quit(void);
