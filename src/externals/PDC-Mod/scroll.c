#include <curspriv.h>

/*

### Description

   scroll() causes the window to scroll up one line. This involves
   moving the lines in the window data strcture.

   With a positive n, scrl() and wscrl() scroll the window up n lines
   (line i + n becomes i); otherwise they scroll the window down n
   lines.

   For these functions to work, scrolling must be enabled via
   scrollok(). Note also that scrolling is not allowed if the supplied
   window is a pad.

### Return Value

   All functions return OK on success and ERR on error.

 */

int PDC_wscrl(WINDOW *win, int top, int bottom, int n)
{
    int start, end, n_lines;
    chtype blank, *tptr, *endptr;

    /* Check if window scrolls. Valid for window AND pad */

    assert( win);
    if (!win || !n)
        return ERR;

    blank = win->_bkgd;
    start = top;
    end = bottom + 1;
    n_lines = end - start;

    if (n > 0)             /* scroll up */
    {
        if( n > n_lines)
            n = n_lines;
        memmove( win->_y[start], win->_y[start + n],
                     (n_lines - n) * win->_maxx * sizeof( chtype));
        tptr = win->_y[end - n];
    }
    else                  /* scroll down */
    {
        n = -n;
        if( n > n_lines)
            n = n_lines;
        memmove( win->_y[start + n], win->_y[start],
                     (n_lines - n) * win->_maxx * sizeof( chtype));
        tptr = win->_y[top];
    }

        /* make blank lines */

    endptr = tptr + n * win->_maxx;
    while( tptr < endptr)
        *tptr++ = blank;

    touchline(win, start, n_lines);
    return OK;
}

int wscrl(WINDOW *win, int n)
{
    int rval;

    assert( win);
    if (!win || !win->_scroll || !n)
        rval = ERR;
    else
        rval = PDC_wscrl( win, win->_tmarg, win->_bmarg, n);
    if( OK == rval)
        PDC_sync( win);
    return( rval);
}

int scrl(int n)
{
    return wscrl(stdscr, n);
}

int scroll(WINDOW *win)
{
    return wscrl(win, 1);
}
