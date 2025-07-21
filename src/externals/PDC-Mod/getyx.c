#include <curspriv.h>

/*

### Description

   The getyx() macro (defined in curses.h -- the prototypes here are
   merely illustrative) puts the current cursor position of the
   specified window into y and x. getbegyx() and getmaxyx() return the
   starting coordinates and size of the specified window, respectively.
   getparyx() returns the starting coordinates of the parent's window,
   if the specified window is a subwindow; otherwise it sets y and x to
   -1. These are all macros.

   getsyx() gets the coordinates of the virtual screen cursor, and
   stores them in y and x. If leaveok() is TRUE, it returns -1, -1. If
   lines have been removed with ripoffline(), then getsyx() includes
   these lines in its count; so, the returned y and x values should only
   be used with setsyx().

   setsyx() sets the virtual screen cursor to the y, x coordinates. If
   either y or x is -1, leaveok() is set TRUE, else it's set FALSE.

   getsyx() and setsyx() are meant to be used by a library routine that
   manipulates curses windows without altering the position of the
   cursor. Note that getsyx() is defined only as a macro.

   getbegy(), getbegx(), getcurx(), getcury(), getmaxy(), getmaxx(),
   getpary(), and getparx() return the appropriate coordinate or size
   values, or ERR in the case of a NULL window.

 */

int getbegy(const WINDOW *win)
{
    assert( win);
    return win ? win->_begy : ERR;
}

int getbegx(const WINDOW *win)
{
    assert( win);
    return win ? win->_begx : ERR;
}

int getcury(const WINDOW *win)
{
    assert( win);
    return win ? win->_cury : ERR;
}

int getcurx(const WINDOW *win)
{
    assert( win);
    return win ? win->_curx : ERR;
}

int getpary(const WINDOW *win)
{
    assert( win);
    return win ? win->_pary : ERR;
}

int getparx(const WINDOW *win)
{
    assert( win);
    return win ? win->_parx : ERR;
}

int getmaxy(const WINDOW *win)
{
    assert( win);
    return win ? win->_maxy : ERR;
}

int getmaxx(const WINDOW *win)
{
    assert( win);
    return win ? win->_maxx : ERR;
}

void setsyx(int y, int x)
{
    if (curscr)
    {
        curscr->_leaveit = y == -1 || x == -1;

        if (!curscr->_leaveit)
            wmove(curscr, y, x);
    }
}
