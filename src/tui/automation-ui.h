#pragma once
#include <stdbool.h>
#include <wchar.h>
void automation_ui_cancel(void);
void automation_ui_cleanup(void);
bool automation_ui_quit(void);
bool automation_ui_poll(void);
bool automation_ui_key(wint_t key);
