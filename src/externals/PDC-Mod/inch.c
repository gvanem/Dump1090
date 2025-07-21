#include <curspriv.h>

/*

### Description

   The inch() functions retrieve the character and attribute from the
   current or specified window position, in the form of a chtype. If a
   NULL window is specified, (chtype)ERR is returned.

   The in_wch() functions are the wide-character versions; instead of
   returning a chtype, they store a cchar_t at the address specified by
   wcval, and return OK or ERR. (No value is stored when ERR is
   returned.) Note that in PDCurses, chtype and cchar_t are the same.

 */

chtype winch(WINDOW *win)
{
    assert( win);
    if (!win)
        return (chtype)ERR;

    return win->_y[win->_cury][win->_curx];
}

chtype inch(void)
{
    return winch(stdscr);
}

chtype mvinch(int y, int x)
{
    if (move(y, x) == ERR)
        return (chtype)ERR;

    return stdscr->_y[stdscr->_cury][stdscr->_curx];
}

chtype mvwinch(WINDOW *win, int y, int x)
{
    if (wmove(win, y, x) == ERR)
        return (chtype)ERR;

    return win->_y[win->_cury][win->_curx];
}

int win_wch(WINDOW *win, cchar_t *wcval)
{
    assert( win);
    assert( wcval);
    if (!win || !wcval)
        return ERR;

    *wcval = win->_y[win->_cury][win->_curx];

    return OK;
}

int in_wch(cchar_t *wcval)
{
    return win_wch(stdscr, wcval);
}

int mvin_wch(int y, int x, cchar_t *wcval)
{
    assert( wcval);
    if (!wcval || (move(y, x) == ERR))
        return ERR;

    *wcval = stdscr->_y[stdscr->_cury][stdscr->_curx];

    return OK;
}

int mvwin_wch(WINDOW *win, int y, int x, cchar_t *wcval)
{
    assert( wcval);
    if (!wcval || (wmove(win, y, x) == ERR))
        return ERR;

    *wcval = win->_y[win->_cury][win->_curx];
    return OK;
}
