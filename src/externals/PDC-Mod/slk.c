#include <curspriv.h>

/*

### Description

   These functions manipulate a window that contain Soft Label Keys
   (SLK). To use the SLK functions, a call to slk_init() must be made
   BEFORE initscr() or newterm(). slk_init() removes 1 or 2 lines from
   the useable screen, depending on the format selected.

   The line(s) removed from the screen are used as a separate window, in
   which SLKs are displayed.  Mouse clicks on the SLKs are returned as
   KEY_F() (function key) presses;  for example,  clicking on the leftmost
   SLK will cause KEY_F(1) to be added to the key queue.

   slk_init() requires a single parameter which describes the format of
   the SLKs as follows:

   0       3-2-3 format
   1       4-4 format
   2       4-4-4 format (ncurses extension)
   3       4-4-4 format with index line (ncurses extension)
   2 lines used
   55      5-5 format (pdcurses format)

   In PDCursesMod,  one can alternatively set fmt as a series of hex
   digits specifying the format.  For example,  0x414 would result
   in 4-1-4 format; 0x21b3 would result in 2-1-11-3 format;  and
   so on.  Also,  negating fmt results in the index line being added.

   Also,  in PDCursesMod,  one can call slk_init() at any time
   _after_ initscr(),  to reset the label format.  If you do this,
   you'll need to reset the label text and call slk_refresh().  However,
   you can't toggle the index line or turn SLK on or off after initscr()
   has been called.  Doing so would add/remove a line or two from the
   useable screen,  which would be difficult to handle correctly.

   slk_refresh(), slk_noutrefresh() and slk_touch() are analogous to
   refresh(), noutrefresh() and touch().

   slk_color() is analogous to color_set(),  and is similarly limited
   to 16-bit color pairs.  extended_slk_color() allows the ability to
   access color pairs beyond 64K.

### Return Value

   All functions return OK on success and ERR on error.

 */

static int label_length = 0;
static int labels = 0;
static int label_fmt = 0;
static int label_line = 0;
static bool hidden = FALSE;

#define MAX_LABEL_LENGTH 32

static struct SLK {
    chtype label[MAX_LABEL_LENGTH];
    int len;
    int format;
    int start_col;
} *slk = (struct SLK *)NULL;

/* See comments above on this function.   */

int slk_init(int fmt)
{
    int i;

    switch (fmt)
    {
    case 0:  /* 3 - 2 - 3 */
        label_fmt = 0x323;
        break;

    case 1:   /* 4 - 4 */
        label_fmt = 0x44;
        break;

    case 2:   /* 4 4 4 */
        label_fmt = 0x444;
        break;

    case 3:   /* 4 4 4  with index */
        label_fmt = -0x444;
        break;

    case 55:  /* 5 - 5 */
        label_fmt = 0x55;
        break;

    default:
        label_fmt = fmt;
        break;
    }

    labels = 0;
    for( i = abs( label_fmt); i; i /= 16)
       labels += i % 16;

    if( slk)
        free( slk);
    slk = (struct SLK *)calloc(labels, sizeof(struct SLK));

    if (!slk)
        labels = 0;
    if( SP)
        {
        if( SP->slk_winptr)
            wclear( SP->slk_winptr);
        PDC_slk_initialize( );
        }

    return slk ? OK : ERR;
}

/* draw a single button */

static void _drawone(int num)
{
    int i, col, slen;

    if (hidden)
        return;

    slen = slk[num].len;

    switch (slk[num].format)
    {
    case 0:  /* LEFT */
        col = 0;
        break;

    case 1:  /* CENTER */
        col = (label_length - slen) / 2;
        if (col + slen > label_length)
            --col;
        break;

    default:  /* RIGHT */
        col = label_length - slen;
    }

    if( col < 0)  /* Ensure start of label is visible */
        col = 0;
    wmove(SP->slk_winptr, label_line, slk[num].start_col);

    for (i = 0; i < label_length; ++i)
        waddch(SP->slk_winptr, (i >= col && i < (col + slen)) ?
               slk[num].label[i - col] : ' ');
}

/* redraw each button */

static void _redraw(void)
{
    int i;

    if( !hidden)
    {
        for (i = 0; i < labels; ++i)
            _drawone(i);
        if (label_fmt < 0)
        {
            const chtype save_attr = SP->slk_winptr->_attrs;

            wattrset(SP->slk_winptr, A_NORMAL);
            wmove(SP->slk_winptr, 0, 0);
            whline(SP->slk_winptr, 0, COLS);

            for (i = 0; i < labels; i++)
                mvwprintw(SP->slk_winptr, 0, slk[i].start_col, "F%d", i + 1);

            SP->slk_winptr->_attrs = save_attr;
        }
    }
}

/* slk_set() Used to set a slk label to a string.

   labnum  = 1 - 8 (or 10) (number of the label)
   label   = string (8 or 7 bytes total), or NULL
   justify = 0 : left, 1 : center, 2 : right
 */
int slk_set(int labnum, const char *label, int justify)
{
    wchar_t wlabel[MAX_LABEL_LENGTH];

    PDC_mbstowcs(wlabel, label, MAX_LABEL_LENGTH - 1);
    return slk_wset(labnum, wlabel, justify);
}

int slk_refresh(void)
{
    return (slk_noutrefresh() == ERR) ? ERR : doupdate();
}

int slk_noutrefresh(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    return wnoutrefresh(SP->slk_winptr);
}

char *slk_label(int labnum)
{
    static char temp[MAX_LABEL_LENGTH + 1];
    wchar_t *wtemp = slk_wlabel(labnum);

    PDC_wcstombs(temp, wtemp, MAX_LABEL_LENGTH);
    return temp;
}

int slk_clear(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    hidden = TRUE;
    werase(SP->slk_winptr);
    return wrefresh(SP->slk_winptr);
}

int slk_restore(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    hidden = FALSE;
    _redraw();
    return wrefresh(SP->slk_winptr);
}

int slk_touch(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    return touchwin(SP->slk_winptr);
}

int slk_attron(const chtype attrs)
{
    int rc;

    assert( SP);
    if (!SP)
        return ERR;

    rc = wattron(SP->slk_winptr, attrs);
    _redraw();

    return rc;
}

int slk_attr_on(const attr_t attrs, void *opts)
{
    INTENTIONALLY_UNUSED_PARAMETER( opts);
    return slk_attron(attrs);
}

int slk_attroff(const chtype attrs)
{
    int rc;

    assert( SP);
    if (!SP)
        return ERR;

    rc = wattroff(SP->slk_winptr, attrs);
    _redraw();

    return rc;
}

int slk_attr_off(const attr_t attrs, void *opts)
{
    INTENTIONALLY_UNUSED_PARAMETER( opts);
    return slk_attroff(attrs);
}

int slk_attrset(const chtype attrs)
{
    int rc;

    assert( SP);
    if (!SP)
        return ERR;

    rc = wattrset(SP->slk_winptr, attrs);
    _redraw();

    return rc;
}

attr_t slk_attr( void)
{
    assert( SP);
    assert( SP->slk_winptr);
    if (!SP || !SP->slk_winptr)
        return A_REVERSE;           /* default attribute for SLK */

    return( SP->slk_winptr->_attrs & (A_ATTRIBUTES & ~A_COLOR));
}

int extended_slk_color( int pair)
{
    int rc;

    assert( SP);
    if (!SP)
        return ERR;

    rc = wcolor_set(SP->slk_winptr, 0, (void *)&pair);
    _redraw();

    return rc;
}

int slk_color(short color_pair)
{
    int integer_color_pair = (int)color_pair;

    assert( SP);
    if (!SP)
        return ERR;
    return( extended_slk_color( integer_color_pair));
}

int slk_attr_set(const attr_t attrs, short color_pair, void *opts)
{
    const int integer_color_pair = (opts ? *(int *)opts : (int)color_pair);

    return slk_attrset(attrs | COLOR_PAIR(integer_color_pair));
}

static void _slk_calc(void)
{
    int i, j, idx, remaining_space;
    int n_groups = 0, group_size[10];

    label_length = COLS / labels;
    if (label_length > MAX_LABEL_LENGTH)
        label_length = MAX_LABEL_LENGTH;
    remaining_space = COLS - label_length * labels + 1;
    for( i = abs( label_fmt); i; i /= 16)
        group_size[n_groups++] = i % 16;
               /* We really want at least two spaces between groups: */
    while( label_length > 1 && remaining_space < n_groups - 1)
    {
        label_length--;
        remaining_space += labels;
    }

    for( i = idx = 0; i < n_groups; i++)
        for( j = 0; j < group_size[i]; j++, idx++)
            slk[idx].start_col = label_length * idx
                     + (i ? (i * remaining_space) / (n_groups - 1) : 0);

    if( label_length)
       --label_length;

    /* make sure labels are all in window */

    _redraw();
}

void PDC_slk_initialize(void)
{
    if (slk)
    {
        assert( SP);
        if (label_fmt < 0)
        {
            SP->slklines = 2;
            label_line = 1;
        }
        else
            SP->slklines = 1;

        if (!SP->slk_winptr)
        {
            SP->slk_winptr = newwin(SP->slklines, COLS,
                                    LINES - SP->slklines, 0);
            if (!SP->slk_winptr)
                return;

            wattrset(SP->slk_winptr, A_REVERSE);
        }

        _slk_calc();

        touchwin(SP->slk_winptr);
    }
}

void PDC_slk_free(void)
{
    if (slk)
    {
        if (SP->slk_winptr)
        {
            delwin(SP->slk_winptr);
            SP->slk_winptr = (WINDOW *)NULL;
        }

        free(slk);
        slk = (struct SLK *)NULL;

        label_length = 0;
        labels = 0;
        label_fmt = 0;
        label_line = 0;
        hidden = FALSE;
    }
}

int PDC_mouse_in_slk(int y, int x)
{
    int i;

    /* If the line on which the mouse was clicked is NOT the last line
       of the screen, or the SLKs are hidden,  we are not interested in it.
     */
    assert( SP);
    if (!slk || hidden || !SP->slk_winptr
                        || (y != SP->slk_winptr->_begy + label_line))
        return 0;

    for (i = 0; i < labels; i++)
        if (x >= slk[i].start_col && x < (slk[i].start_col + label_length))
            return i + 1;

    return 0;
}

int slk_wset(int labnum, const wchar_t *label, int justify)
{
    if (labnum < 1 || labnum > labels || justify < 0 || justify > 2)
        return ERR;

    labnum--;

    if (!label || !(*label))
    {
        /* Clear the label */

        *slk[labnum].label = 0;
        slk[labnum].format = 0;
        slk[labnum].len = 0;
    }
    else
    {
        int i;

        /* Skip leading spaces */

        while( *label == L' ')
            label++;

        /* Copy it */

        for (i = 0; label[i] && i < MAX_LABEL_LENGTH - 1; i++)
            slk[labnum].label[i] = label[i];

        /* Drop trailing spaces */

        while( i && label[i - 1] == L' ')
            i--;

        slk[labnum].label[i] = 0;
        slk[labnum].format = justify;
        slk[labnum].len = i;
    }

    _drawone(labnum);

    return OK;
}

wchar_t *slk_wlabel(int labnum)
{
    static wchar_t temp[MAX_LABEL_LENGTH + 1];
    chtype *p;
    int i;

    if (labnum < 1 || labnum > labels)
        return (wchar_t *)0;

    for (i = 0, p = slk[labnum - 1].label; *p; i++)
        temp[i] = (wchar_t)*p++;

    temp[i] = '\0';
    return temp;
}

