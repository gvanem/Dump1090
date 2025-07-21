#include <curspriv.h>

/*

### Description

   erase() and werase() copy blanks (i.e. the background chtype) to
   every cell of the window.

   clear() and wclear() are similar to erase() and werase(), but they
   also call clearok() to ensure that the the window is cleared on the
   next wrefresh().

   clrtobot() and wclrtobot() clear the window from the current cursor
   position to the end of the window.

   clrtoeol() and wclrtoeol() clear the window from the current cursor
   position to the end of the current line.

### Return Value

   All functions return OK on success and ERR on error.

*/

int wclrtoeol(WINDOW *win)
{
    int x, y, minx;
    chtype blank, *ptr;

    assert( win);
    if (!win)
        return ERR;

    y = win->_cury;
    x = win->_curx;

    /* wrs (4/10/93) account for window background */

    blank = win->_bkgd;

    for (minx = x, ptr = &win->_y[y][x]; minx < win->_maxx; minx++, ptr++)
        *ptr = blank;

    PDC_mark_cells_as_changed( win, y, x, win->_maxx - 1);

    PDC_sync(win);
    return OK;
}

int clrtoeol(void)
{
    return wclrtoeol(stdscr);
}

int wclrtobot(WINDOW *win)
{
    int savey, savex;

    assert( win);
    if (!win)
        return ERR;

    savey = win->_cury;
    savex = win->_curx;

    /* should this involve scrolling region somehow ? */

    if (win->_cury + 1 < win->_maxy)
    {
        win->_curx = 0;
        win->_cury++;
        for (; win->_maxy > win->_cury; win->_cury++)
            wclrtoeol(win);
        win->_cury = savey;
        win->_curx = savex;
    }
    wclrtoeol(win);

    PDC_sync(win);
    return OK;
}

int clrtobot(void)
{
    return wclrtobot(stdscr);
}

int werase(WINDOW *win)
{
    if (wmove(win, 0, 0) == ERR)
        return ERR;

    return wclrtobot(win);
}

int erase(void)
{
    return werase(stdscr);
}

int wclear(WINDOW *win)
{
    assert( win);
    if (!win)
        return ERR;

    win->_clear = TRUE;
    return werase(win);
}

int clear(void)
{
    return wclear(stdscr);
}
