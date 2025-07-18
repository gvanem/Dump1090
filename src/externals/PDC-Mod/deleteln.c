/* PDCursesMod */

#include <curspriv.h>

/*man-start**************************************************************

deleteln
--------

### Synopsis

    int deleteln(void);
    int wdeleteln(WINDOW *win);
    int insdelln(int n);
    int winsdelln(WINDOW *win, int n);
    int insertln(void);
    int winsertln(WINDOW *win);

    int mvdeleteln(int y, int x);
    int mvwdeleteln(WINDOW *win, int y, int x);
    int mvinsertln(int y, int x);
    int mvwinsertln(WINDOW *win, int y, int x);

### Description

   With the deleteln() and wdeleteln() functions, the line under the
   cursor in the window is deleted. All lines below the current line are
   moved up one line. The bottom line of the window is cleared. The
   cursor position does not change.

   With the insertln() and winsertn() functions, a blank line is
   inserted above the current line and the bottom line is lost.

   mvdeleteln(), mvwdeleteln(), mvinsertln() and mvwinsertln() allow
   moving the cursor and inserting/deleting in one call.

### Return Value

   All functions return OK on success and ERR on error.

### Portability
   Function              | X/Open | ncurses | NetBSD
   :---------------------|:------:|:-------:|:------:
   deleteln              |    Y   |    Y    |   Y
   wdeleteln             |    Y   |    Y    |   Y
   mvdeleteln            |    -   |    -    |   -
   mvwdeleteln           |    -   |    -    |   -
   insdelln              |    Y   |    Y    |   Y
   winsdelln             |    Y   |    Y    |   Y
   insertln              |    Y   |    Y    |   Y
   winsertln             |    Y   |    Y    |   Y
   mvinsertln            |    -   |    -    |   -
   mvwinsertln           |    -   |    -    |   -

**man-end****************************************************************/

int wdeleteln(WINDOW *win)
{
    assert( win);
    if (!win)
        return ERR;

    return( PDC_wscrl( win, win->_cury, win->_maxy - 1, 1));
}

int deleteln(void)
{
    return wdeleteln(stdscr);
}

int mvdeleteln(int y, int x)
{
    if (move(y, x) == ERR)
        return ERR;

    return wdeleteln(stdscr);
}

int mvwdeleteln(WINDOW *win, int y, int x)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wdeleteln(win);
}

int winsdelln(WINDOW *win, int n)
{
    assert( win);
    if (!win)
        return ERR;
    return( PDC_wscrl( win, win->_cury, win->_maxy - 1, -n));
}

int insdelln(int n)
{
    return winsdelln(stdscr, n);
}

int winsertln(WINDOW *win)
{
    assert( win);
    if (!win)
        return ERR;

    return( PDC_wscrl( win, win->_cury, win->_maxy - 1, -1));
}

int insertln(void)
{
    return winsertln(stdscr);
}

int mvinsertln(int y, int x)
{
    if (move(y, x) == ERR)
        return ERR;

    return winsertln(stdscr);
}

int mvwinsertln(WINDOW *win, int y, int x)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return winsertln(win);
}
