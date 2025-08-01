#include <curspriv.h>

/*

### Description

   move() and wmove() move the cursor associated with the window to the
   given location. This does not move the physical cursor of the
   terminal until refresh() is called. The position specified is
   relative to the upper left corner of the window, which is (0,0).

   mvcur() moves the physical cursor without updating any window cursor
   positions.

### Return Value

   All functions return OK on success and ERR on error.
 */

int move(int y, int x)
{
    assert( stdscr);
    if (!stdscr || x < 0 || y < 0 || x >= stdscr->_maxx || y >= stdscr->_maxy)
        return ERR;

    stdscr->_curx = x;
    stdscr->_cury = y;

    return OK;
}

int mvcur(int oldrow, int oldcol, int newrow, int newcol)
{
    assert( SP);
    INTENTIONALLY_UNUSED_PARAMETER( oldrow);
    INTENTIONALLY_UNUSED_PARAMETER( oldcol);
    if (!SP || newrow < 0 || newrow >= LINES || newcol < 0 || newcol >= COLS)
        return ERR;

    PDC_gotoyx(newrow, newcol);
    SP->cursrow = newrow;
    SP->curscol = newcol;

    return OK;
}

int wmove(WINDOW *win, int y, int x)
{
    assert( win);
    if (!win || x < 0 || y < 0 || x >= win->_maxx || y >= win->_maxy)
        return ERR;

    win->_curx = x;
    win->_cury = y;

    return OK;
}
