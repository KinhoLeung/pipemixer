#include "tui/menu.h"
#include "xmalloc.h"
#include "config.h"
#include "log.h"

void tui_menu_resize(struct tui_menu *const menu, int term_width, int term_height) {
    menu->x = 1;
    menu->y = 2;
    menu->w = term_width - 2;
    menu->h = term_height - 4;
    if (menu->win == NULL) {
        menu->win = newwin(menu->h, menu->w, menu->y, menu->x);
    } else {
        wresize(menu->win, menu->h, menu->w);
    }
}

struct tui_menu *tui_menu_create(unsigned int n_items) {
    struct tui_menu *menu = xzalloc(sizeof(*menu) + (sizeof(*menu->items) * n_items));
    menu->n_items = n_items;

    return menu;
}

void tui_menu_free(struct tui_menu *menu) {
    for (unsigned int i = 0; i < menu->n_items; i++) {
        wstring_free(&menu->items[i].wstr);
    }
    wstring_free(&menu->header);
    free(menu);
}

bool tui_menu_change_focus(struct tui_menu *const menu, int direction) {
    bool change = false;
    if (direction < 0) {
        if (menu->selected > 0) {
            menu->selected -= 1;
            change = true;
        } else if (config.wraparound) {
            menu->selected = menu->n_items - 1;
            change = true;
        }
    } else {
        if (menu->selected < menu->n_items - 1) {
            menu->selected += 1;
            change = true;
        } else if (config.wraparound) {
            menu->selected = 0;
            change = true;
        }
    }

    return change;
}

unsigned tui_menu_first_visible(const struct tui_menu *const menu) {
    const unsigned visible = menu->h > 2 ? (unsigned)(menu->h - 2) : 0;
    return visible && menu->selected >= visible ? menu->selected - visible + 1 : 0;
}

void tui_menu_draw(const struct tui_menu *const menu) {
    TRACE("tui_draw_menu: %dx%d at %dx%d", menu->w, menu->h, menu->x, menu->y);
    WINDOW *win = menu->win;

    werase(win);

    /* box */
    wmove(win, 0, 0);
    waddwstr(win, config.borders.tl);
    for (int x = 1; x < menu->w - 1; x++) {
        waddwstr(win, config.borders.ts);
    }
    waddwstr(win, config.borders.tr);

    wmove(win, menu->h - 1, 0);
    waddwstr(win, config.borders.bl);
    for (int x = 1; x < menu->w - 1; x++) {
        waddwstr(win, config.borders.bs);
    }
    waddwstr(win, config.borders.br);

    for (int y = 1; y < menu->h - 1; y++) {
        wmove(win, y, 0);
        waddwstr(win, config.borders.ls);
        wmove(win, y, menu->w - 1);
        waddwstr(win, config.borders.rs);
    }

    mvwaddnwstr(win, 0, 1, menu->header.data, menu->w - 2);
    const unsigned int visible = menu->h > 2 ? (unsigned int)(menu->h - 2) : 0;
    const unsigned int first = tui_menu_first_visible(menu);
    for (unsigned int row = 0; row < visible && first + row < menu->n_items; row++) {
        unsigned int i = first + row;
        if (i == menu->selected) {
            wattron(win, A_BOLD);
        }

        mvwaddnwstr(win, 1 + row, 1, menu->items[i].wstr.data, menu->w - 2);

        wattroff(win, A_BOLD);
    }

    wnoutrefresh(win);
}

