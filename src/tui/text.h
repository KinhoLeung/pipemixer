#pragma once

#include <ncurses.h>

int tui_text_width(const char *text);
int tui_wide_width(const wchar_t *text);
int tui_write_wide(WINDOW *win, int y, int x, const wchar_t *text, int columns);
int tui_write_text(WINDOW *win, int y, int x, const char *text, int columns);
