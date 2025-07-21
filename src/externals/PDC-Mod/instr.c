#include <curspriv.h>

/*

### Description

   These functions take characters (or wide characters) from the current
   or specified position in the window, and return them as a string in
   str (or wstr). Attributes are ignored. The functions with n as the
   last argument return a string at most n characters long.

### Return Value

   Upon successful completion, innstr(), mvinnstr(), mvwinnstr() and
   winnstr() return the number of characters actually read into the
   string; instr(), mvinstr(), mvwinstr() and winstr() return OK.
   Otherwise, all these functions return ERR.
 */

int winnstr(WINDOW *win, char *str, int n)
{
    wchar_t wstr[513];

    assert( win);
    assert( str);
    if (n < 0 || n > 512)
        n = 512;

    if (winnwstr(win, wstr, n) == ERR)
        return ERR;

    return (int)PDC_wcstombs(str, wstr, n);
}

int instr(char *str)
{
    return (ERR == winnstr(stdscr, str, stdscr->_maxx)) ? ERR : OK;
}

int winstr(WINDOW *win, char *str)
{
    return (ERR == winnstr(win, str, win->_maxx)) ? ERR : OK;
}

int mvinstr(int y, int x, char *str)
{
    if (move(y, x) == ERR)
        return ERR;

    return (ERR == winnstr(stdscr, str, stdscr->_maxx)) ? ERR : OK;
}

int mvwinstr(WINDOW *win, int y, int x, char *str)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return (ERR == winnstr(win, str, win->_maxx)) ? ERR : OK;
}

int innstr(char *str, int n)
{
    return winnstr(stdscr, str, n);
}

int mvinnstr(int y, int x, char *str, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return winnstr(stdscr, str, n);
}

int mvwinnstr(WINDOW *win, int y, int x, char *str, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return winnstr(win, str, n);
}

int winnwstr(WINDOW *win, wchar_t *wstr, int n)
{
    chtype *src;
    int i;

    assert( win);
    assert( wstr);
    if (!win || !wstr)
        return ERR;

    if (n < 0 || (win->_curx + n) > win->_maxx)
        n = win->_maxx - win->_curx;

    src = win->_y[win->_cury] + win->_curx;

    for (i = 0; i < n; i++)
        wstr[i] = (wchar_t)src[i] & A_CHARTEXT;

    wstr[i] = L'\0';

    return i;
}

int inwstr(wchar_t *wstr)
{
    return (ERR == winnwstr(stdscr, wstr, stdscr->_maxx)) ? ERR : OK;
}

int winwstr(WINDOW *win, wchar_t *wstr)
{
    return (ERR == winnwstr(win, wstr, win->_maxx)) ? ERR : OK;
}

int mvinwstr(int y, int x, wchar_t *wstr)
{
    if (move(y, x) == ERR)
        return ERR;

    return (ERR == winnwstr(stdscr, wstr, stdscr->_maxx)) ? ERR : OK;
}

int mvwinwstr(WINDOW *win, int y, int x, wchar_t *wstr)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return (ERR == winnwstr(win, wstr, win->_maxx)) ? ERR : OK;
}

int innwstr(wchar_t *wstr, int n)
{
    return winnwstr(stdscr, wstr, n);
}

int mvinnwstr(int y, int x, wchar_t *wstr, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return winnwstr(stdscr, wstr, n);
}

int mvwinnwstr(WINDOW *win, int y, int x, wchar_t *wstr, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return winnwstr(win, wstr, n);
}
