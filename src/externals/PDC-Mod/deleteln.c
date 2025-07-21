#include <curspriv.h>

/*

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

 */

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
