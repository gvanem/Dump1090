#include <curspriv.h>

/*

### Description

   These routines read a chtype or cchar_t string from the window,
   starting at the current or specified position, and ending at the
   right margin, or after n elements, whichever is less.

### Return Value

   All functions return the number of elements read, or ERR on error.

 */

int winchnstr(WINDOW *win, chtype *ch, int n)
{
    chtype *src;
    int i;

    assert( win);
    assert( ch);
    if (!win || !ch || n < 0)
        return ERR;

    if ((win->_curx + n) > win->_maxx)
        n = win->_maxx - win->_curx;

    src = win->_y[win->_cury] + win->_curx;

    for (i = 0; i < n; i++)
        *ch++ = *src++;

    *ch = (chtype)0;

    return OK;
}

int inchstr(chtype *ch)
{
    return winchnstr(stdscr, ch, stdscr->_maxx - stdscr->_curx);
}

int winchstr(WINDOW *win, chtype *ch)
{
    return winchnstr(win, ch, win->_maxx - win->_curx);
}

int mvinchstr(int y, int x, chtype *ch)
{
    if (move(y, x) == ERR)
        return ERR;

    return winchnstr(stdscr, ch, stdscr->_maxx - stdscr->_curx);
}

int mvwinchstr(WINDOW *win, int y, int x, chtype *ch)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return winchnstr(win, ch, win->_maxx - win->_curx);
}

int inchnstr(chtype *ch, int n)
{
    return winchnstr(stdscr, ch, n);
}

int mvinchnstr(int y, int x, chtype *ch, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return winchnstr(stdscr, ch, n);
}

int mvwinchnstr(WINDOW *win, int y, int x, chtype *ch, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return winchnstr(win, ch, n);
}

int win_wchnstr(WINDOW *win, cchar_t *wch, int n)
{
    return winchnstr(win, wch, n);
}

int in_wchstr(cchar_t *wch)
{
    return win_wchnstr(stdscr, wch, stdscr->_maxx - stdscr->_curx);
}

int win_wchstr(WINDOW *win, cchar_t *wch)
{
    return win_wchnstr(win, wch, win->_maxx - win->_curx);
}

int mvin_wchstr(int y, int x, cchar_t *wch)
{
    if (move(y, x) == ERR)
        return ERR;

    return win_wchnstr(stdscr, wch, stdscr->_maxx - stdscr->_curx);
}

int mvwin_wchstr(WINDOW *win, int y, int x, cchar_t *wch)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return win_wchnstr(win, wch, win->_maxx - win->_curx);
}

int in_wchnstr(cchar_t *wch, int n)
{
    return win_wchnstr(stdscr, wch, n);
}

int mvin_wchnstr(int y, int x, cchar_t *wch, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return win_wchnstr(stdscr, wch, n);
}

int mvwin_wchnstr(WINDOW *win, int y, int x, cchar_t *wch, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return win_wchnstr(win, wch, n);
}

