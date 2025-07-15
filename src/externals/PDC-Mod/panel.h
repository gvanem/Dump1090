/* Public Domain Curses */

#pragma once

#include <curses.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct panel PANEL;

int         bottom_panel (PANEL *pan);
int         del_panel (PANEL *pan);
int         hide_panel (PANEL *pan);
int         move_panel (PANEL *pan, int starty, int startx);
PANEL      *new_panel (WINDOW *win);
PANEL      *panel_above (const PANEL *pan);
PANEL      *panel_below (const PANEL *pan);
PANEL      *ground_panel (SCREEN *sp);
PANEL      *ceiling_panel (SCREEN *sp);
int         panel_hidden (const PANEL *pan);
const void *panel_userptr (const PANEL *pan);
WINDOW     *panel_window (const PANEL *pan);
int         replace_panel (PANEL *pan, WINDOW *win);
int         set_panel_userptr (PANEL *pan, const void *uptr);
int         show_panel (PANEL *pan);
int         top_panel (PANEL *pan);
void        update_panels (void);

#if defined(__cplusplus)
}
#endif
