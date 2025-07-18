/* PDCursesMod */

#include <curspriv.h>

/*man-start**************************************************************

delch
-----

### Synopsis

    int delch(void);
    int wdelch(WINDOW *win);
    int mvdelch(int y, int x);
    int mvwdelch(WINDOW *win, int y, int x);

### Description

   The character under the cursor in the window is deleted. All
   characters to the right on the same line are moved to the left one
   position and the last character on the line is filled with a blank.
   The cursor position does not change (after moving to y, x if
   coordinates are specified).

### Return Value

   All functions return OK on success and ERR on error.

### Portability
   Function              | X/Open | ncurses | NetBSD
   :---------------------|:------:|:-------:|:------:
   delch                 |    Y   |    Y    |   Y
   wdelch                |    Y   |    Y    |   Y
   mvdelch               |    Y   |    Y    |   Y
   mvwdelch              |    Y   |    Y    |   Y

**man-end****************************************************************/

int wdelch(WINDOW *win)
{
    int y, x, maxx;
    chtype *temp1;

    assert( win);
    if (!win)
        return ERR;

    y = win->_cury;
    x = win->_curx;
    maxx = win->_maxx - 1;
    temp1 = &win->_y[y][x];

    memmove(temp1, temp1 + 1, (maxx - x) * sizeof(chtype));

    /* wrs (4/10/93) account for window background */

    win->_y[y][maxx] = win->_bkgd;

    PDC_mark_cells_as_changed( win, y, x, maxx);

    PDC_sync(win);

    return OK;
}

int delch(void)
{
    return wdelch(stdscr);
}

int mvdelch(int y, int x)
{
    if (move(y, x) == ERR)
        return ERR;

    return wdelch(stdscr);
}

int mvwdelch(WINDOW *win, int y, int x)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wdelch(win);
}
