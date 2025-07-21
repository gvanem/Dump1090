#include <curspriv.h>

/*

### Description

   These routines write a chtype or cchar_t string directly into the
   window structure, starting at the current or specified position. The
   four routines with n as the last argument copy at most n elements,
   but no more than will fit on the line. If n == -1 then the whole
   string is copied, up to the maximum number that will fit on the line.

   The cursor position is not advanced. These routines do not check for
   newline or other special characters, nor does any line wrapping
   occur.

### Return Value

   All functions return OK or ERR.

*/

int waddchnstr(WINDOW *win, const chtype *ch, int n)
{
    int y, x;
    chtype *ptr;

    assert( win);
    assert( ch);
    if (!win || !ch || !n || n < -1)
        return ERR;

    x = win->_curx;
    y = win->_cury;
    ptr = &(win->_y[y][x]);

    if (n == -1 || n > win->_maxx - x)
        n = win->_maxx - x;

    for (; n && *ch; n--, x++, ptr++, ch++)
    {
        if (*ptr != *ch)
        {
            PDC_mark_cell_as_changed( win, y, x);
            *ptr = *ch;
        }
    }

    return OK;
}

int addchstr(const chtype *ch)
{
    return waddchnstr(stdscr, ch, -1);
}

int addchnstr(const chtype *ch, int n)
{
    return waddchnstr(stdscr, ch, n);
}

int waddchstr(WINDOW *win, const chtype *ch)
{
    return waddchnstr(win, ch, -1);
}

int mvaddchstr(int y, int x, const chtype *ch)
{
    if (move(y, x) == ERR)
        return ERR;

    return waddchnstr(stdscr, ch, -1);
}

int mvaddchnstr(int y, int x, const chtype *ch, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return waddchnstr(stdscr, ch, n);
}

int mvwaddchstr(WINDOW *win, int y, int x, const chtype *ch)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return waddchnstr(win, ch, -1);
}

int mvwaddchnstr(WINDOW *win, int y, int x, const chtype *ch, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return waddchnstr(win, ch, n);
}

int wadd_wchnstr(WINDOW *win, const cchar_t *wch, int n)
{
    return waddchnstr(win, wch, n);
}

int add_wchstr(const cchar_t *wch)
{
    return wadd_wchnstr(stdscr, wch, -1);
}

int add_wchnstr(const cchar_t *wch, int n)
{
    return wadd_wchnstr(stdscr, wch, n);
}

int wadd_wchstr(WINDOW *win, const cchar_t *wch)
{
    return wadd_wchnstr(win, wch, -1);
}

int mvadd_wchstr(int y, int x, const cchar_t *wch)
{
    if (move(y, x) == ERR)
        return ERR;

    return wadd_wchnstr(stdscr, wch, -1);
}

int mvadd_wchnstr(int y, int x, const cchar_t *wch, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return wadd_wchnstr(stdscr, wch, n);
}

int mvwadd_wchstr(WINDOW *win, int y, int x, const cchar_t *wch)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wadd_wchnstr(win, wch, -1);
}

int mvwadd_wchnstr(WINDOW *win, int y, int x, const cchar_t *wch, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wadd_wchnstr(win, wch, n);
}
