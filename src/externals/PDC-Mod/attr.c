#include <curspriv.h>

/*

### Description

   These functions manipulate the current attributes and/or colors of
   the named window. These attributes can be any combination of
   A_STANDOUT, A_REVERSE, A_BOLD, A_DIM, A_BLINK, A_UNDERLINE. These
   constants are defined in <curses.h> and can be combined with the
   bitwise-OR operator (|).

   The current attributes of a window are applied to all chtypes that
   are written into the window with waddch(). Attributes are a property
   of the chtype, and move with the character through any scrolling or
   insert/delete operations.

   wattrset() sets the current attributes of the given window to attrs.
   attrset() is the stdscr version.

   wattroff() turns off the named attributes without affecting any other
   attributes; wattron() turns them on.

   wcolor_set() sets the window color to the value of color_pair.  If
   opts is non-NULL,  it is treated as a pointer to an integer containing
   the desired color pair,  and color_pair is ignored (this is an ncurses
   extension).

   standout() is the same as attron(A_STANDOUT). standend() is the same
   as attrset(A_NORMAL); that is, it turns off all attributes.

   The attr_* and wattr_* functions are intended for use with the WA_*
   attributes. In PDCurses, these are the same as A_*, and there is no
   difference in behavior from the chtype-based functions.  If opts is
   non-NULL,  it is used as a pointer to an integer and the color pair
   is stored in it (this is an ncurses and PDCursesMod extension).

   wattr_get() retrieves the attributes and color pair for the specified
   window.

   wchgat() sets the color pair and attributes for the next n cells on
   the current line of a given window, without changing the existing
   text, or alterting the window's attributes. An n of -1 extends the
   change to the edge of the window. The changes take effect
   immediately.  If opts is non-NULL,  it is treated as a pointer to
   an integer containing the desired color pair,  and color_pair is
   ignored (this is an ncurses extension).

   wunderscore() turns on the A_UNDERLINE attribute; wunderend() turns
   it off. underscore() and underend() are the stdscr versions.

### Return Value

   All functions return OK on success and ERR on error.

 */

int wattroff(WINDOW *win, chtype attrs)
{
    assert( win);
    if (!win)
        return ERR;

    win->_attrs &= (~attrs & A_ATTRIBUTES);
    return OK;
}

int attroff(chtype attrs)
{
    return wattroff(stdscr, attrs);
}

int wattron(WINDOW *win, chtype attrs)
{
    chtype newcolr, oldcolr, newattr, oldattr;

    assert( win);
    if (!win)
        return ERR;

    if ((win->_attrs & A_COLOR) && (attrs & A_COLOR))
    {
        oldcolr = win->_attrs & A_COLOR;
        oldattr = win->_attrs ^ oldcolr;
        newcolr = attrs & A_COLOR;
        newattr = (attrs & A_ATTRIBUTES) ^ newcolr;
        newattr |= oldattr;
        win->_attrs = newattr | newcolr;
    }
    else
        win->_attrs |= (attrs & A_ATTRIBUTES);

    return OK;
}

int attron(chtype attrs)
{
    return wattron(stdscr, attrs);
}

int wattrset(WINDOW *win, chtype attrs)
{
    assert( win);
    if (!win)
        return ERR;

    win->_attrs = attrs & A_ATTRIBUTES;

    return OK;
}

int attrset(chtype attrs)
{
    return wattrset(stdscr, attrs);
}

int standend(void)
{
    return wattrset(stdscr, A_NORMAL);
}

int standout(void)
{
    return wattrset(stdscr, A_STANDOUT);
}

int wstandend(WINDOW *win)
{
    return wattrset(win, A_NORMAL);
}

int wstandout(WINDOW *win)
{
    return wattrset(win, A_STANDOUT);
}

chtype getattrs(const WINDOW *win)
{
    assert( win);
    return win ? win->_attrs : 0;
}

int wcolor_set(WINDOW *win, short color_pair, void *opts)
{
    const int integer_color_pair = (opts ? *(int *)opts : (int)color_pair);

    assert( win);
    if (!win)
        return ERR;

    win->_attrs = (win->_attrs & ~A_COLOR) | COLOR_PAIR(integer_color_pair);

    return OK;
}

int color_set(short color_pair, void *opts)
{
    return wcolor_set(stdscr, color_pair, opts);
}

int wattr_get(WINDOW *win, attr_t *attrs, short *color_pair, void *opts)
{
    assert( win);
    if (!win)
        return ERR;

    if (attrs)
        *attrs = win->_attrs & (A_ATTRIBUTES & ~A_COLOR);

    if (color_pair)
        *color_pair = (short)PAIR_NUMBER(win->_attrs);
    if( opts)
        *(int *)opts = (int)PAIR_NUMBER( win->_attrs);

    return OK;
}

int attr_get(attr_t *attrs, short *color_pair, void *opts)
{
    return wattr_get(stdscr, attrs, color_pair, opts);
}

int wattr_off(WINDOW *win, attr_t attrs, void *opts)
{
    INTENTIONALLY_UNUSED_PARAMETER( opts);
    assert( !opts);
    return wattroff(win, attrs);
}

int attr_off(attr_t attrs, void *opts)
{
    INTENTIONALLY_UNUSED_PARAMETER( opts);
    assert( !opts);
    return wattroff(stdscr, attrs);
}

int wattr_on(WINDOW *win, attr_t attrs, void *opts)
{
    if( opts)
        attrs = (attrs & ~A_COLOR) | COLOR_PAIR( *(int *)opts);
    return wattron(win, attrs);
}

int attr_on(attr_t attrs, void *opts)
{
    return wattr_on(stdscr, attrs, opts);
}

int wattr_set(WINDOW *win, attr_t attrs, short color_pair, void *opts)
{
    const int integer_color_pair = (opts ? *(int *)opts : (int)color_pair);

    assert( win);
    if (!win)
        return ERR;

    win->_attrs = (attrs & (A_ATTRIBUTES & ~A_COLOR)) | COLOR_PAIR(integer_color_pair);

    return OK;
}

int attr_set(attr_t attrs, short color_pair, void *opts)
{
    return wattr_set(stdscr, attrs, color_pair, opts);
}

int wchgat(WINDOW *win, int n, attr_t attr, short color, const void *opts)
{
    chtype *dest, newattr;
    int startpos, endpos;
    const int integer_color_pair = (opts ? *(int *)opts : (int)color);

    assert( win);
    if (!win)
        return ERR;

    newattr = (attr & A_ATTRIBUTES) | COLOR_PAIR(integer_color_pair);

    startpos = win->_curx;
    endpos = ((n < 0) ? win->_maxx : min(startpos + n, win->_maxx)) - 1;
    dest = win->_y[win->_cury];

    for (n = startpos; n <= endpos; n++)
        dest[n] = (dest[n] & A_CHARTEXT) | newattr;

    PDC_mark_cells_as_changed( win, win->_cury, startpos, endpos);

    PDC_sync(win);

    return OK;
}

int chgat(int n, attr_t attr, short color, const void *opts)
{
    return wchgat(stdscr, n, attr, color, opts);
}

int mvchgat(int y, int x, int n, attr_t attr, short color, const void *opts)
{
    if (move(y, x) == ERR)
        return ERR;

    return wchgat(stdscr, n, attr, color, opts);
}

int mvwchgat(WINDOW *win, int y, int x, int n, attr_t attr, short color,
             const void *opts)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wchgat(win, n, attr, color, opts);
}

int underend(void)
{
    return wattroff(stdscr, A_UNDERLINE);
}

int wunderend(WINDOW *win)
{
    return wattroff(win, A_UNDERLINE);
}

int underscore(void)
{
    return wattron(stdscr, A_UNDERLINE);
}

int wunderscore(WINDOW *win)
{
    return wattron(win, A_UNDERLINE);
}
