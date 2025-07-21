#include <curspriv.h>

/*

### Description

   border(), wborder(), and box() draw a border around the edge of the
   window. If any argument is zero, an appropriate default is used:

    ls    left side of border             ACS_VLINE
    rs    right side of border            ACS_VLINE
    ts    top side of border              ACS_HLINE
    bs    bottom side of border           ACS_HLINE
    tl    top left corner of border       ACS_ULCORNER
    tr    top right corner of border      ACS_URCORNER
    bl    bottom left corner of border    ACS_LLCORNER
    br    bottom right corner of border   ACS_LRCORNER

   hline() and whline() draw a horizontal line, using ch, starting from
   the current cursor position. The cursor position does not change. The
   line is at most n characters long, or as many as will fit in the
   window.

   vline() and wvline() draw a vertical line, using ch, starting from
   the current cursor position. The cursor position does not change. The
   line is at most n characters long, or as many as will fit in the
   window.

   The *_set functions are the "wide-character" versions, taking
   pointers to cchar_t instead of chtype. Note that in PDCurses, chtype
   and cchar_t are the same.

### Return Value

   These functions return OK on success and ERR on error.

*/

/* _attr_passthru() -- Takes a single chtype 'ch' and checks if the
   current attribute of window 'win', as set by wattrset(), and/or the
   current background of win, as set by wbkgd(), should by combined with
   it. Attributes set explicitly in ch take precedence.
 */
static chtype _attr_passthru(WINDOW *win, chtype ch)
{
    chtype attr;

    /* If the incoming character doesn't have its own attribute, then
       use the current attributes for the window. If the incoming
       character has attributes, but not a color component, OR the
       attributes to the current attributes for the window. If the
       incoming character has a color component, use only the attributes
       from the incoming character.
     */
    attr = ch & A_ATTRIBUTES;
    if (!(attr & A_COLOR))
        attr |= win->_attrs;

    /* wrs (4/10/93) -- Apply the same sort of logic for the window
       background, in that it only takes precedence if other color
       attributes are not there. */

    if (!(attr & A_COLOR))
        attr |= win->_bkgd & A_ATTRIBUTES;
    else
        attr |= win->_bkgd & (A_ATTRIBUTES ^ A_COLOR);

    ch = (ch & A_CHARTEXT) | attr;
    return ch;
}

int wborder(WINDOW *win, chtype ls, chtype rs, chtype ts, chtype bs,
            chtype tl, chtype tr, chtype bl, chtype br)
{
    int i, ymax, xmax;

    assert( win);
    if (!win)
        return ERR;

    ymax = win->_maxy - 1;
    xmax = win->_maxx - 1;

    ls = _attr_passthru(win, ls ? ls : ACS_VLINE);
    rs = _attr_passthru(win, rs ? rs : ACS_VLINE);
    ts = _attr_passthru(win, ts ? ts : ACS_HLINE);
    bs = _attr_passthru(win, bs ? bs : ACS_HLINE);
    tl = _attr_passthru(win, tl ? tl : ACS_ULCORNER);
    tr = _attr_passthru(win, tr ? tr : ACS_URCORNER);
    bl = _attr_passthru(win, bl ? bl : ACS_LLCORNER);
    br = _attr_passthru(win, br ? br : ACS_LRCORNER);

    for (i = 1; i < xmax; i++)
    {
        win->_y[0][i] = ts;
        win->_y[ymax][i] = bs;
    }

    for (i = 1; i < ymax; i++)
    {
        win->_y[i][0] = ls;
        win->_y[i][xmax] = rs;
    }

    win->_y[0][0] = tl;
    win->_y[0][xmax] = tr;
    win->_y[ymax][0] = bl;
    win->_y[ymax][xmax] = br;

    for (i = 1; i < ymax; i++)
    {
        PDC_mark_cell_as_changed( win, i, 0);
        PDC_mark_cell_as_changed( win, i, xmax);
    }
    PDC_set_changed_cells_range( win, 0, 0, xmax);
    PDC_set_changed_cells_range( win, ymax, 0, xmax);

    PDC_sync(win);

    return OK;
}

int border(chtype ls, chtype rs, chtype ts, chtype bs, chtype tl,
           chtype tr, chtype bl, chtype br)
{
    return wborder(stdscr, ls, rs, ts, bs, tl, tr, bl, br);
}

int box(WINDOW *win, chtype verch, chtype horch)
{
    return wborder(win, verch, verch, horch, horch, 0, 0, 0, 0);
}

int whline(WINDOW *win, chtype ch, int n)
{
    chtype *dest;
    int startpos, endpos;

    assert( win);
    if (!win || n < 1)
        return ERR;

    startpos = win->_curx;
    endpos = min(startpos + n, win->_maxx) - 1;
    dest = win->_y[win->_cury];
    ch = _attr_passthru(win, ch ? ch : ACS_HLINE);

    for (n = startpos; n <= endpos; n++)
        dest[n] = ch;

    n = win->_cury;

    PDC_mark_cells_as_changed( win, n, startpos, endpos);

    PDC_sync(win);

    return OK;
}

int hline(chtype ch, int n)
{
    return whline(stdscr, ch, n);
}

int mvhline(int y, int x, chtype ch, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return whline(stdscr, ch, n);
}

int mvwhline(WINDOW *win, int y, int x, chtype ch, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return whline(win, ch, n);
}

int wvline(WINDOW *win, chtype ch, int n)
{
    int endpos, x;

    assert( win);
    if (!win || n < 1)
        return ERR;

    endpos = min(win->_cury + n, win->_maxy);
    x = win->_curx;

    ch = _attr_passthru(win, ch ? ch : ACS_VLINE);

    for (n = win->_cury; n < endpos; n++)
    {
        win->_y[n][x] = ch;
        PDC_mark_cell_as_changed( win, n, x);
    }

    PDC_sync(win);

    return OK;
}

int vline(chtype ch, int n)
{
    return wvline(stdscr, ch, n);
}

int mvvline(int y, int x, chtype ch, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return wvline(stdscr, ch, n);
}

int mvwvline(WINDOW *win, int y, int x, chtype ch, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wvline(win, ch, n);
}

int wborder_set (WINDOW *win, const cchar_t *ls, const cchar_t *rs,
                 const cchar_t *ts, const cchar_t *bs, const cchar_t *tl,
                 const cchar_t *tr, const cchar_t *bl, const cchar_t *br)
{
    return wborder(win, ls ? *ls : 0, rs ? *rs : 0, ts ? *ts : 0,
                        bs ? *bs : 0, tl ? *tl : 0, tr ? *tr : 0,
                        bl ? *bl : 0, br ? *br : 0);
}

int border_set (const cchar_t *ls, const cchar_t *rs, const cchar_t *ts,
                const cchar_t *bs, const cchar_t *tl, const cchar_t *tr,
                const cchar_t *bl, const cchar_t *br)
{
    return wborder_set(stdscr, ls, rs, ts, bs, tl, tr, bl, br);
}

int box_set(WINDOW *win, const cchar_t *verch, const cchar_t *horch)
{
    return wborder_set(win, verch, verch, horch, horch,
                       (const cchar_t *)NULL, (const cchar_t *)NULL,
                       (const cchar_t *)NULL, (const cchar_t *)NULL);
}

int whline_set(WINDOW *win, const cchar_t *wch, int n)
{
    assert( wch);
    return wch ? whline(win, *wch, n) : ERR;
}

int hline_set(const cchar_t *wch, int n)
{
    return whline_set(stdscr, wch, n);
}

int mvhline_set(int y, int x, const cchar_t *wch, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return whline_set(stdscr, wch, n);
}

int mvwhline_set(WINDOW *win, int y, int x, const cchar_t *wch, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return whline_set(win, wch, n);
}

int wvline_set(WINDOW *win, const cchar_t *wch, int n)
{
    assert( wch);
    return wch ? wvline(win, *wch, n) : ERR;
}

int vline_set(const cchar_t *wch, int n)
{
    return wvline_set(stdscr, wch, n);
}

int mvvline_set(int y, int x, const cchar_t *wch, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return wvline_set(stdscr, wch, n);
}

int mvwvline_set(WINDOW *win, int y, int x, const cchar_t *wch, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wvline_set(win, wch, n);
}
