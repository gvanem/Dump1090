#include <curspriv.h>

/*

### Description

   newwin() creates a new window with the given number of lines, nlines
   and columns, ncols. The upper left corner of the window is at line
   begy, column begx. If nlines is zero, it defaults to LINES - begy;
   ncols to COLS - begx. Create a new full-screen window by calling
   newwin(0, 0, 0, 0).

   delwin() deletes the named window, freeing all associated memory.
   Subwindows must be deleted before the main window can be deleted.

   mvwin() moves the window so that the upper left-hand corner is at
   position (y,x). If the move would cause the window to be off the
   screen, it is an error and the window is not moved. Moving subwindows
   is allowed.

   subwin() creates a new subwindow within a window. The dimensions of
   the subwindow are nlines lines and ncols columns. The subwindow is at
   position (begy, begx) on the screen. This position is relative to the
   screen, and not to the window orig. Changes made to either window
   will affect both. When using this routine, you will often need to
   call touchwin() before calling wrefresh().

   derwin() is the same as subwin(), except that begy and begx are
   relative to the origin of the window orig rather than the screen.
   There is no difference between subwindows and derived windows.

   mvderwin() moves a derived window (or subwindow) inside its parent
   window. The screen-relative parameters of the window are not changed.
   This routine is used to display different parts of the parent window
   at the same physical position on the screen.

   dupwin() creates an exact duplicate of the window win.

   wgetparent() returns the parent WINDOW pointer for subwindows, or NULL
   for windows having no parent.

   wsyncup() causes a touchwin() of all of the window's parents.

   If syncok() is called with a second argument of TRUE, this causes a
   wsyncup() to be called every time the window is changed.

   is_subwin() reports whether the specified window is a subwindow,
   created by subwin() or derwin().

   is_syncok() reports whether the specified window is in syncok mode.

   wcursyncup() causes the current cursor position of all of a window's
   ancestors to reflect the current cursor position of the current
   window.

   wsyncdown() causes a touchwin() of the current window if any of its
   parent's windows have been touched.

   resize_window() allows the user to resize an existing window. It
   returns the pointer to the new window, or NULL on failure.

   wresize() is an ncurses-compatible wrapper for resize_window(). Note
   that, unlike ncurses, it will NOT process any subwindows of the
   window. (However, you still can call it _on_ subwindows.) It returns
   OK or ERR.

### Return Value

   newwin(), subwin(), derwin() and dupwin() return a pointer to the new
   window, or NULL on failure. delwin(), mvwin(), mvderwin() and
   syncok() return OK or ERR. wsyncup(), wcursyncup() and wsyncdown()
   return nothing.

### Errors

   It is an error to call resize_window() before calling initscr().
   Also, an error will be generated if we fail to create a newly sized
   replacement window for curscr, or stdscr. This could happen when
   increasing the window size. NOTE: If this happens, the previously
   successfully allocated windows are left alone; i.e., the resize is
   NOT cancelled for those windows.

*/

/*library-internals-begin************************************************

   PDC_makenew() allocates all data for a new WINDOW * except the actual
   lines themselves. If it's unable to allocate memory for the window
   structure, it will free all allocated memory and return a NULL
   pointer.

   PDC_makelines() allocates the memory for the lines.

   PDC_sync() handles wrefresh() and wsyncup() calls when a window is
   changed.

**library-internals-end**************************************************/

WINDOW *PDC_makenew(int nlines, int ncols, int begy, int begx)
{
    WINDOW *win;

    assert( nlines > 0 && ncols > 0);
    /* allocate the window structure itself */

    win = (WINDOW *)calloc(1, sizeof(WINDOW));
    assert( win);
    if (!win)
        return win;

    /* allocate the line pointer array */

    win->_y = (chtype **)malloc(nlines * sizeof(chtype *));
    assert( win->_y);

    /* allocate the minchng and maxchng arrays */

    win->_firstch = (int *)malloc(nlines * sizeof(int) * 2);
    assert( win->_firstch);
    if (!win->_firstch || !win->_y)
    {
        delwin( win);
        return (NULL);
    }

    win->_lastch = win->_firstch + nlines;

    /* initialize window variables */

    win->_maxy = nlines;  /* real max screen size */
    win->_maxx = ncols;   /* real max screen size */
    win->_begy = begy;
    win->_begx = begx;
    win->_bkgd = ' ';     /* wrs 4/10/93 -- initialize background to blank */
    win->_clear = (bool) ((nlines == LINES) && (ncols == COLS));
    win->_bmarg = nlines - 1;
    win->_parx = win->_pary = -1;

    /* init to say window all changed */

    touchwin(win);

    return win;
}

WINDOW *PDC_makelines(WINDOW *win)
{
    int i, nlines, ncols;

    assert( win);
    if (!win)
        return (NULL);

    assert( win->_maxy > 0);
    assert( win->_maxx > 0);

    nlines = win->_maxy;
    ncols = win->_maxx;

    win->_y[0] = (chtype *)malloc(ncols * nlines * sizeof(chtype));
    assert( win->_y[0]);
    if (!win->_y[0])
    {
        /* if error, free all the data */

        delwin( win);
        return (NULL);
    }
    for (i = 1; i < nlines; i++)
        win->_y[i] = win->_y[i - 1] + ncols;

    return win;
}

void PDC_sync(WINDOW *win)
{
    if (win->_immed)
        wrefresh(win);
    if (win->_sync)
        wsyncup(win);
}

/*
  See the OpenBSD and FreeBSD reallocarray() extension.  This is similar,  but
  exists for all platforms,  and always frees the pointer and returns NULL for a
  zero-byte allocation.  realloc() does this on most platforms, but not FreeBSD,
  and it's not guaranteed in the C specifications.
 */
void *PDC_realloc_array(void *ptr, size_t nmemb, size_t size)
{
    if( !nmemb || !size)
    {
        free( ptr);
        ptr = NULL;
    }
    else
    {
        size_t nbytes = nmemb * size;

        assert( nbytes / size == nmemb);
        ptr = realloc( ptr, nbytes);
    }
    return( ptr);
}

#define is_power_of_two( X)   (!((X) & ((X) - 1)))

static void _resize_window_list( SCREEN *scr_ptr)
{
   if( is_power_of_two( scr_ptr->n_windows))
      scr_ptr->window_list =
                 (WINDOW **)PDC_realloc_array( scr_ptr->window_list,
                     scr_ptr->n_windows * 2, sizeof( WINDOW *));
}

void PDC_add_window_to_list( WINDOW *win)
{
   SP->n_windows++;
   _resize_window_list( SP);
   assert( SP->window_list);
   SP->window_list[SP->n_windows - 1] = win;
}

WINDOW *newwin(int nlines, int ncols, int begy, int begx)
{
    WINDOW *win;

    if (!nlines)
        nlines = LINES - begy;
    if (!ncols)
        ncols  = COLS  - begx;

    assert( nlines > 0 && ncols > 0);
    if( nlines <= 0 || ncols <= 0)
        return (NULL);

    assert( SP);
    if( !SP)
        return (NULL);
    if( !(SP->off_screen_windows & OFF_SCREEN_WINDOWS_TO_RIGHT_AND_BOTTOM))
    {
        if( begy + nlines > SP->lines || begx + ncols > SP->cols)
            return (NULL);
    }

    if( !(SP->off_screen_windows & OFF_SCREEN_WINDOWS_TO_LEFT_AND_TOP))
    {
    if( begy < 0 || begx < 0)
        return (NULL);
    }

    win = PDC_makenew(nlines, ncols, begy, begx);
    if (win)
        win = PDC_makelines(win);

    if (win)
    {
        werase(win);
        PDC_add_window_to_list( win);
    }

    return win;
}

int delwin(WINDOW *win)
{
    int i;

    assert( win);
    if (!win)
        return ERR;

            /* make sure win has no subwindows */
    for( i = 0; i < SP->n_windows; i++)
    {
        assert( SP->window_list[i]->_parent != win);
        if( SP->window_list[i]->_parent == win)
            return( ERR);
    }

    if( win->_firstch && win->_y && win->_y[0])
    {
        i = 0;     /* make sure win is in the window list */
        while( i < SP->n_windows && SP->window_list[i] != win)
            i++;
        assert( i < SP->n_windows);
        if( i == SP->n_windows)
            return( ERR);
        SP->n_windows--;        /* remove win from window list */
        SP->window_list[i] = SP->window_list[SP->n_windows];
        _resize_window_list( SP);
    }

    /* subwindows use parents' lines */

    if (!(win->_flags & (_SUBWIN|_SUBPAD)))
        if (win->_y[0])
           free(win->_y[0]);

    if( win->_firstch)
        free(win->_firstch);
    if( win->_y)
        free(win->_y);
    free(win);
    return OK;
}

int mvwin(WINDOW *win, int y, int x)
{
    assert( win);
    if( !win)
        return ERR;

    if( !(SP->off_screen_windows & OFF_SCREEN_WINDOWS_TO_LEFT_AND_TOP))
    {
        if( y < 0 || x < 0)
            return ERR;
    }

    if( !(SP->off_screen_windows & OFF_SCREEN_WINDOWS_TO_RIGHT_AND_BOTTOM))
    {
        if( y + win->_maxy > LINES || x + win->_maxx > COLS)
            return ERR;
    }

    win->_begy = y;
    win->_begx = x;
    touchwin(win);

    return OK;
}

WINDOW *subwin(WINDOW *orig, int nlines, int ncols, int begy, int begx)
{
    WINDOW *win;
    int i, j, k;

    /* make sure window fits inside the original one */

    assert( orig);
    if (!orig || (begy < orig->_begy) || (begx < orig->_begx) ||
        (begy + nlines) > (orig->_begy + orig->_maxy) ||
        (begx + ncols) > (orig->_begx + orig->_maxx))
        return (NULL);

    j = begy - orig->_begy;
    k = begx - orig->_begx;

    if (!nlines)
        nlines = orig->_maxy - j;
    if (!ncols)
        ncols  = orig->_maxx - k;

    assert( nlines > 0 && ncols > 0);
    if( nlines <= 0 || ncols <= 0)
        return (NULL);
    win = PDC_makenew(nlines, ncols, begy, begx);
    if (!win)
        return (NULL);

    /* initialize window variables */

    win->_attrs = orig->_attrs;
    win->_bkgd = orig->_bkgd;
    win->_leaveit = orig->_leaveit;
    win->_scroll = orig->_scroll;
    win->_nodelay = orig->_nodelay;
    win->_delayms = orig->_delayms;
    win->_use_keypad = orig->_use_keypad;
    win->_immed = orig->_immed;
    win->_sync = orig->_sync;
    win->_pary = j;
    win->_parx = k;
    win->_parent = orig;

    for (i = 0; i < nlines; i++, j++)
        win->_y[i] = orig->_y[j] + k;

    win->_flags |= _SUBWIN;
    PDC_add_window_to_list( win);

    return win;
}

WINDOW *derwin(WINDOW *orig, int nlines, int ncols, int begy, int begx)
{
    return subwin(orig, nlines, ncols, begy + orig->_begy, begx + orig->_begx);
}

int mvderwin(WINDOW *win, int pary, int parx)
{
    int i, j;
    WINDOW *mypar;

    assert( win && win->_parent);
    if (!win || !(win->_parent))
        return ERR;

    mypar = win->_parent;

    if (pary < 0 || parx < 0 || (pary + win->_maxy) > mypar->_maxy ||
                                (parx + win->_maxx) > mypar->_maxx)
        return ERR;

    j = pary;

    for (i = 0; i < win->_maxy; i++)
        win->_y[i] = (mypar->_y[j++]) + parx;

    win->_pary = pary;
    win->_parx = parx;

    return OK;
}

WINDOW *dupwin(WINDOW *win)
{
    WINDOW *new_win;
    chtype *ptr, *ptr1;
    int nlines, ncols, begy, begx, i;

    assert( win);
    if (!win)
        return (NULL);

    nlines = win->_maxy;
    ncols = win->_maxx;
    begy = win->_begy;
    begx = win->_begx;

    new_win = PDC_makenew(nlines, ncols, begy, begx);
    if (new_win)
        new_win = PDC_makelines(new_win);

    if (!new_win)
        return (NULL);

    /* copy the contents of win into new_win */

    for (i = 0; i < nlines; i++)
    {
        for (ptr = new_win->_y[i], ptr1 = win->_y[i];
             ptr < new_win->_y[i] + ncols; ptr++, ptr1++)
            *ptr = *ptr1;

        PDC_mark_line_as_changed( new_win, i);
    }

    new_win->_curx = win->_curx;
    new_win->_cury = win->_cury;
    new_win->_maxy = win->_maxy;
    new_win->_maxx = win->_maxx;
    new_win->_begy = win->_begy;
    new_win->_begx = win->_begx;
    new_win->_flags = win->_flags;
    new_win->_attrs = win->_attrs;
    new_win->_clear = win->_clear;
    new_win->_leaveit = win->_leaveit;
    new_win->_scroll = win->_scroll;
    new_win->_nodelay = win->_nodelay;
    new_win->_delayms = win->_delayms;
    new_win->_use_keypad = win->_use_keypad;
    new_win->_tmarg = win->_tmarg;
    new_win->_bmarg = win->_bmarg;
    new_win->_parx = win->_parx;
    new_win->_pary = win->_pary;
    new_win->_parent = win->_parent;
    new_win->_bkgd = win->_bkgd;
    new_win->_flags = win->_flags;
    PDC_add_window_to_list( new_win);

    return new_win;
}

WINDOW *wgetparent(const WINDOW *win)
{
    assert( win);
    if (!win)
        return (NULL);

    return win->_parent;
}

WINDOW *resize_window(WINDOW *win, int nlines, int ncols)
{
    WINDOW *new_win;
    int save_cury, save_curx, new_begy, new_begx;

    assert( SP);
    assert( win);
    assert( nlines >= 0 && ncols >= 0);
    if (!win || !SP || nlines < 0 || ncols < 0)
        return (NULL);

    if (win->_flags & _SUBPAD)
    {
        new_win = subpad(win->_parent, nlines, ncols, win->_begy, win->_begx);
        if (!new_win)
            return (NULL);
    }
    else if (win->_flags & _SUBWIN)
    {
        new_win = subwin(win->_parent, nlines, ncols, win->_begy, win->_begx);
        if (!new_win)
            return (NULL);
    }
    else
    {
        if (win == SP->slk_winptr)
        {
            new_begy = SP->lines - SP->slklines;
            new_begx = 0;
        }
        else
        {
            new_begy = win->_begy;
            new_begx = win->_begx;
        }

        new_win = PDC_makenew(nlines, ncols, new_begy, new_begx);
        if (!new_win)
            return (NULL);
    }
    save_curx = min(win->_curx, (new_win->_maxx - 1));
    save_cury = min(win->_cury, (new_win->_maxy - 1));

    if (!(win->_flags & (_SUBPAD|_SUBWIN)))
    {
        new_win = PDC_makelines(new_win);
        if (!new_win)
            return (NULL);

        new_win->_bkgd = win->_bkgd;
        werase(new_win);

        copywin(win, new_win, 0, 0, 0, 0, min(win->_maxy, new_win->_maxy) - 1,
                min(win->_maxx, new_win->_maxx) - 1, FALSE);

        if (win->_y[0])
            free(win->_y[0]);
    }

    new_win->_flags = win->_flags;
    new_win->_attrs = win->_attrs;
    new_win->_clear = win->_clear;
    new_win->_leaveit = win->_leaveit;
    new_win->_scroll = win->_scroll;
    new_win->_nodelay = win->_nodelay;
    new_win->_delayms = win->_delayms;
    new_win->_use_keypad = win->_use_keypad;
    new_win->_tmarg = (win->_tmarg > new_win->_maxy - 1) ? 0 : win->_tmarg;
    new_win->_bmarg = (win->_bmarg == win->_maxy - 1) ?
                  new_win->_maxy - 1 : min(win->_bmarg, (new_win->_maxy - 1));
    new_win->_parent = win->_parent;
    new_win->_immed = win->_immed;
    new_win->_sync = win->_sync;
    new_win->_bkgd = win->_bkgd;

    new_win->_curx = save_curx;
    new_win->_cury = save_cury;
    free(win->_firstch);
    free(win->_y);

    *win = *new_win;
    free(new_win);

    return win;
}

int wresize(WINDOW *win, int nlines, int ncols)
{
    return (resize_window(win, nlines, ncols) ? OK : ERR);
}

void wsyncup(WINDOW *win)
{
    WINDOW *tmp;

    for (tmp = win; tmp; tmp = tmp->_parent)
        touchwin(tmp);
}

int syncok(WINDOW *win, bool bf)
{
    assert( win);
    if (!win)
        return ERR;

    win->_sync = bf;

    return OK;
}

bool is_subwin(const WINDOW *win)
{
    assert( win);
    if (!win)
        return FALSE;

    return ((win->_flags & _SUBWIN) ? TRUE : FALSE);
}

bool is_syncok(const WINDOW *win)
{
    assert( win);
    if (!win)
        return FALSE;

    return win->_sync;
}

void wcursyncup(WINDOW *win)
{
    WINDOW *tmp;

    for (tmp = win; tmp && tmp->_parent; tmp = tmp->_parent)
        wmove(tmp->_parent, tmp->_pary + tmp->_cury, tmp->_parx + tmp->_curx);
}

void wsyncdown(WINDOW *win)
{
    WINDOW *tmp;

    for (tmp = win; tmp; tmp = tmp->_parent)
    {
        if (is_wintouched(tmp))
        {
            touchwin(win);
            break;
        }
    }
}
