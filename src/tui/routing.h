#pragma once

void routing_language_changed(void);

#include <ncurses.h>
#include <stdint.h>

void routing_draw(int height, int width);
bool routing_key(wint_t key, bool special);
void routing_mouse(const MEVENT *event);
void routing_error(const char *message);
bool routing_poll(void);
void routing_cleanup(void);
bool routing_selected(uint32_t *output, uint32_t *input);
uint32_t routing_selected_node(bool input);
void routing_monitor_status(const char *status);
