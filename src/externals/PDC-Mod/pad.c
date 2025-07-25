#include <curspriv.h>

/*

### Description

   A pad is a special kind of window, which is not restricted by the
   screen size, and is not necessarily associated with a particular part
   of the screen. You can use a pad when you need a large window, and
   only a part of the window will be on the screen at one time. Pads are
   not refreshed automatically (e.g., from scrolling or echoing of
   input). You can't call wrefresh() with a pad as an argument; use
   prefresh() or pnoutrefresh() instead. Note that these routines
   require additional parameters to specify the part of the pad to be
   displayed, and the location to use on the screen.

   newpad() creates a new pad data structure.

   subpad() creates a new sub-pad within a pad, at position (begy,
   begx), with dimensions of nlines lines and ncols columns. This
   position is relative to the pad, and not to the screen as with
   subwin. Changes to either the parent pad or sub-pad will affect both.
   When using sub-pads, you may need to call touchwin() before calling
   prefresh().

   pnoutrefresh() copies the specified pad to the virtual screen.

   prefresh() calls pnoutrefresh(), followed by doupdate().

   These routines are analogous to wnoutrefresh() and wrefresh(). (py,
   px) specifies the upper left corner of the part of the pad to be
   displayed; (sy1, sx1) and (sy2, sx2) describe the screen rectangle
   that will contain the selected part of the pad.

   pechochar() is functionally equivalent to addch() followed by a call
   to prefresh(), with the last-used coordinates and dimensions.
   pecho_wchar() is the wide-character version.

   is_pad() reports whether the specified window is a pad.

### Return Value

   All functions except is_pad() return OK on success and ERR on error.

*/

/* save values for pechochar() */

WINDOW *newpad(int nlines, int ncols)
{
    WINDOW *win;

    assert( nlines > 0 && ncols > 0);
    win = PDC_makenew(nlines, ncols, 0, 0);
    if (win)
        win = PDC_makelines(win);

    if (!win)
        return (WINDOW *)NULL;

    werase(win);

    win->_flags = _PAD;

    /* save default values in case pechochar() is the first call to
       prefresh(). */

    win->_pminrow = 0;
    win->_pmincol = 0;
    win->_sminrow = 0;
    win->_smincol = 0;
    win->_smaxrow = min(LINES, nlines) - 1;
    win->_smaxcol = min(COLS, ncols) - 1;
    PDC_add_window_to_list( win);

    return win;
}

WINDOW *subpad(WINDOW *orig, int nlines, int ncols, int begy, int begx)
{
    WINDOW *win;
    int i;

    assert( orig && (orig->_flags & _PAD));
    if (!orig || !(orig->_flags & _PAD))
        return (WINDOW *)NULL;

    /* make sure window fits inside the original one */

    if (begy < 0 || begx < 0 ||
        (begy + nlines) > orig->_maxy ||
        (begx + ncols)  > orig->_maxx)
        return (WINDOW *)NULL;

    if (!nlines)
        nlines = orig->_maxy - begy;

    if (!ncols)
        ncols = orig->_maxx - begx;

    assert( nlines > 0 && ncols > 0);
    win = PDC_makenew(nlines, ncols, begy, begx);
    if (!win)
        return (WINDOW *)NULL;

    /* initialize window variables */

    win->_attrs = orig->_attrs;
    win->_leaveit = orig->_leaveit;
    win->_scroll = orig->_scroll;
    win->_nodelay = orig->_nodelay;
    win->_use_keypad = orig->_use_keypad;
    win->_parent = orig;

    for (i = 0; i < nlines; i++)
        win->_y[i] = orig->_y[begy + i] + begx;

    win->_flags = _SUBPAD;

    /* save default values in case pechochar() is the first call
       to prefresh(). */

    win->_pminrow = 0;
    win->_pmincol = 0;
    win->_sminrow = 0;
    win->_smincol = 0;
    win->_smaxrow = min(LINES, nlines) - 1;
    win->_smaxcol = min(COLS, ncols) - 1;
    PDC_add_window_to_list( win);

    return win;
}

int prefresh(WINDOW *win, int py, int px, int sy1, int sx1, int sy2, int sx2)
{
    if (pnoutrefresh(win, py, px, sy1, sx1, sy2, sx2) == ERR)
        return ERR;

    doupdate();
    return OK;
}

int pnoutrefresh(WINDOW *w, int py, int px, int sy1, int sx1, int sy2, int sx2)
{
    int num_cols;
    int sline;
    int pline;

    assert( w);

    if (py < 0)
        py = 0;
    if (px < 0)
        px = 0;
    if (sy1 < 0)
        sy1 = 0;
    if (sx1 < 0)
        sx1 = 0;

    if ((!w || !(w->_flags & (_PAD|_SUBPAD)) ||
        (sy2 >= LINES) || (sx2 >= COLS)) ||
        (sy2 < sy1) || (sx2 < sx1))
        return ERR;

    sline = sy1;
    pline = py;

    num_cols = min((sx2 - sx1 + 1), (w->_maxx - px));

    while (sline <= sy2)
    {
        if (pline < w->_maxy)
        {
            memcpy(curscr->_y[sline] + sx1, w->_y[pline] + px,
                   num_cols * sizeof(chtype));

            PDC_mark_cells_as_changed( curscr, sline, sx1, sx2);
            PDC_set_changed_cells_range( w, pline, _NO_CHANGE, _NO_CHANGE);
        }

        sline++;
        pline++;
    }

    if (w->_clear)
    {
        w->_clear = FALSE;
        curscr->_clear = TRUE;
    }

    /* position the cursor to the pad's current position if possible --
       is the pad current position going to end up displayed? if not,
       then don't move the cursor; if so, move it to the correct place */

    if (!w->_leaveit && w->_cury >= py && w->_curx >= px &&
         w->_cury <= py + (sy2 - sy1) && w->_curx <= px + (sx2 - sx1))
    {
        curscr->_cury = (w->_cury - py) + sy1;
        curscr->_curx = (w->_curx - px) + sx1;
    }

    w->_pminrow = py;
    w->_pmincol = px;
    w->_sminrow = sy1;
    w->_smincol = sx1;
    w->_smaxrow = sy2;
    w->_smaxcol = sx2;
    return OK;
}

int PDC_pnoutrefresh_with_stored_params( WINDOW *pad)
{
    return prefresh(pad, pad->_pminrow, pad->_pmincol, pad->_sminrow,
                    pad->_smincol, pad->_smaxrow, pad->_smaxcol);
}

int pechochar(WINDOW *pad, chtype ch)
{
    int rval;

    if (waddch(pad, ch) == ERR)
        return ERR;

    rval = PDC_pnoutrefresh_with_stored_params( pad);
    if( rval == OK)
        doupdate( );
    return( rval);
}

int pecho_wchar(WINDOW *pad, const cchar_t *wch)
{
    int rval;

    assert( wch);
    if (!wch || (waddch(pad, *wch) == ERR))
        return ERR;

    rval = PDC_pnoutrefresh_with_stored_params( pad);
    if( rval == OK)
        doupdate( );
    return( rval);
}

bool is_pad(const WINDOW *pad)
{
    assert( pad);
    if (!pad)
        return FALSE;

    return (pad->_flags & _PAD) ? TRUE : FALSE;
}
