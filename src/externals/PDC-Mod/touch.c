#include <curspriv.h>

/*

### Description

   touchwin() and touchline() throw away all information about which
   parts of the window have been touched, pretending that the entire
   window has been drawn on. This is sometimes necessary when using
   overlapping windows, since a change to one window will affect the
   other window, but the records of which lines have been changed in the
   other window will not reflect the change.

   untouchwin() marks all lines in the window as unchanged since the
   last call to wrefresh().

   wtouchln() makes n lines in the window, starting at line y, look as
   if they have (changed == 1) or have not (changed == 0) been changed
   since the last call to wrefresh().

   is_linetouched() returns TRUE if the specified line in the specified
   window has been changed since the last call to wrefresh().

   is_wintouched() returns TRUE if the specified window has been changed
   since the last call to wrefresh().

   touchoverlap(win1, win2) marks the portion of win2 which overlaps
   with win1 as modified.

### Return Value

   All functions return OK on success and ERR on error except
   is_wintouched() and is_linetouched().

 */

void PDC_set_changed_cells_range(WINDOW *win, int y, int start, int end)
{
    assert( win);
    assert( y >= 0 && y < win->_maxy);
    assert( start >= 0 || start == _NO_CHANGE);
    assert( start <= end);
    assert( end < win->_maxx);
    win->_firstch[y] = start;
    win->_lastch[y] = end;
}

void PDC_mark_line_as_changed(WINDOW *win, int y)
{
    assert( win);
    assert( y >= 0 && y < win->_maxy);
    win->_firstch[y] = 0;
    win->_lastch[y] = win->_maxx - 1;
}

void PDC_mark_cells_as_changed(WINDOW *win, int y, int start, int end)
{
    assert( win);
    assert( y >= 0 && y < win->_maxy);
    assert( start >= 0 || start == _NO_CHANGE);
    assert( start <= end);
    assert( end < win->_maxx);
    if( win->_firstch[y] == _NO_CHANGE)
    {
        win->_firstch[y] = start;
        win->_lastch[y] = end;
    }
    else
    {
        if( win->_firstch[y] > start)
            win->_firstch[y] = start;
        if( win->_lastch[y] < end)
            win->_lastch[y] = end;
    }
}

bool PDC_touched_range(const WINDOW *win, int y, int *firstch, int *lastch)
{
    assert( win);
    assert( y >= 0 && y < win->_maxy);
    if( win->_firstch[y] == _NO_CHANGE)
    {
        *firstch = *lastch = 0;
        return( FALSE);
    }
    else
    {
        *firstch = win->_firstch[y];
        *lastch = win->_lastch[y];
        return( TRUE);
    }
}

void PDC_mark_cell_as_changed(WINDOW *win, int y, int x)
{
    PDC_mark_cells_as_changed( win, y, x, x);
}

int touchwin(WINDOW *win)
{
    int i;

    assert( win);
    if (!win)
        return ERR;

    for (i = 0; i < win->_maxy; i++)
        PDC_mark_line_as_changed( win, i);

    return OK;
}

int touchline(WINDOW *win, int start, int count)
{
    int i;

    assert( win && count > 0 && start >= 0 && start + count <= win->_maxy);
    if (!win || count <= 0 || start < 0 || start + count > win->_maxy)
        return ERR;

    for (i = start; i < start + count; i++)
        PDC_mark_line_as_changed( win, i);

    return OK;
}

int untouchwin(WINDOW *win)
{
    int i;

    assert( win);
    if (!win)
        return ERR;

    for (i = 0; i < win->_maxy; i++)
        PDC_set_changed_cells_range( win, i, _NO_CHANGE, _NO_CHANGE);

    return OK;
}

int wtouchln(WINDOW *win, int y, int n, int changed)
{
    int i;

    assert( win && n >= 0 && y + n <= win->_maxy);
    if (!win || n < 0 || y + n > win->_maxy)
        return ERR;

    for (i = y; i < y + n; i++)
    {
        if (changed)
            PDC_mark_line_as_changed( win, i);
        else
            PDC_set_changed_cells_range( win, i, _NO_CHANGE, _NO_CHANGE);
    }

    return OK;
}

bool is_linetouched(WINDOW *win, int line)
{
    assert( win && line < win->_maxy && line >= 0);
    if (!win || line >= win->_maxy || line < 0)
        return FALSE;

    return (win->_firstch[line] != _NO_CHANGE) ? TRUE : FALSE;
}

bool is_wintouched(WINDOW *win)
{
    int i;

    assert( win);
    if (win)
        for (i = 0; i < win->_maxy; i++)
            if (win->_firstch[i] != _NO_CHANGE)
                return TRUE;

    return FALSE;
}

int touchoverlap(const WINDOW *win1, WINDOW *win2)
{
    int y, endy, endx, starty, startx;

    assert( win1);
    assert( win2);
    if (!win1 || !win2)
        return ERR;

    starty = max(win1->_begy, win2->_begy);
    startx = max(win1->_begx, win2->_begx);
    endy = min(win1->_maxy + win1->_begy, win2->_maxy + win2->_begy);
    endx = min(win1->_maxx + win1->_begx, win2->_maxx + win2->_begx);

    if (starty >= endy || startx >= endx)
        return OK;       /* there is no overlap */

    starty -= win2->_begy;
    startx -= win2->_begx;
    endy -= win2->_begy;
    endx -= win2->_begx;
    endx -= 1;

    for (y = starty; y < endy; y++)
        PDC_mark_cells_as_changed( win2, y, startx, endx);

    return OK;
}
