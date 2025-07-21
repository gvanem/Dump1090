#include <curspriv.h>

/*

### Description

   bkgdset() and wbkgdset() manipulate the background of a window. The
   background is a chtype consisting of any combination of attributes
   and a character; it is combined with each chtype added or inserted to
   the window by waddch() or winsch(). Only the attribute part is used
   to set the background of non-blank characters, while both character
   and attributes are used for blank positions.

   bkgd() and wbkgd() not only change the background, but apply it
   immediately to every cell in the window.

   wbkgrnd(), wbkgrndset() and wgetbkgrnd() are the "wide-character"
   versions of these functions, taking a pointer to a cchar_t instead of
   a chtype. However, in PDCurses, cchar_t and chtype are the same.

   The attributes that are defined with the attrset()/attron() set of
   functions take precedence over the background attributes if there is
   a conflict (e.g., different color pairs).

### Return Value

   bkgd() and wbkgd() return OK, unless the window is NULL, in which
   case they return ERR.

 */

int wbkgd(WINDOW *win, chtype ch)
{
    int x, y;
    chtype oldcolr, oldch, newcolr, newch, colr, attr;
    chtype oldattr = 0, newattr = 0;
    chtype *winptr;

    assert( win);
    if (!win)
        return ERR;

    if (win->_bkgd == ch)
        return OK;

    oldcolr = win->_bkgd & A_COLOR;
    if (oldcolr)
        oldattr = (win->_bkgd & A_ATTRIBUTES) ^ oldcolr;

    oldch = win->_bkgd & A_CHARTEXT;

    wbkgdset(win, ch);

    newcolr = win->_bkgd & A_COLOR;
    if (newcolr)
        newattr = (win->_bkgd & A_ATTRIBUTES) ^ newcolr;

    newch = win->_bkgd & A_CHARTEXT;

    /* what follows is what seems to occur in the System V
       implementation of this routine */

    for (y = 0; y < win->_maxy; y++)
    {
        for (x = 0; x < win->_maxx; x++)
        {
            winptr = win->_y[y] + x;

            ch = *winptr;

            /* determine the colors and attributes of the character read
               from the window */

            colr = ch & A_COLOR;
            attr = ch & (A_ATTRIBUTES ^ A_COLOR);

            /* if the color is the same as the old background color,
               then make it the new background color, otherwise leave it */

            if (colr == oldcolr)
                colr = newcolr;

            /* remove any attributes (non color) from the character that
               were part of the old background, then combine the
               remaining ones with the new background */

            attr ^= oldattr;
            attr |= newattr;

            /* change character if it is there because it was the old
               background character */

            ch &= A_CHARTEXT;
            if (ch == oldch)
                ch = newch;

            ch |= (attr | colr);

            *winptr = ch;

        }
    }

    touchwin(win);
    PDC_sync(win);
    return OK;
}

int bkgd(chtype ch)
{
    return wbkgd(stdscr, ch);
}

void wbkgdset(WINDOW *win, chtype ch)
{
    if (win)
    {
        if (!(ch & A_CHARTEXT))
            ch |= ' ';
        win->_bkgd = ch;
    }
}

void bkgdset(chtype ch)
{
    wbkgdset(stdscr, ch);
}

chtype getbkgd(WINDOW *win)
{
    assert( win);
    return win ? win->_bkgd : (chtype)ERR;
}

int wbkgrnd(WINDOW *win, const cchar_t *wch)
{
    assert( wch);
    return wch ? wbkgd(win, *wch) : ERR;
}

int bkgrnd(const cchar_t *wch)
{
    return wbkgrnd(stdscr, wch);
}

void wbkgrndset(WINDOW *win, const cchar_t *wch)
{
    if (wch)
        wbkgdset(win, *wch);
}

void bkgrndset(const cchar_t *wch)
{
    wbkgrndset(stdscr, wch);
}

int wgetbkgrnd(WINDOW *win, cchar_t *wch)
{
    assert( win);
    assert( wch);
    if (!win || !wch)
        return ERR;

    *wch = win->_bkgd;

    return OK;
}

int getbkgrnd(cchar_t *wch)
{
    return wgetbkgrnd(stdscr, wch);
}

