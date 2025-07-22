#include <curspriv.h>

/*

### Description

   cbreak() and nocbreak() toggle cbreak mode. In cbreak mode,
   characters typed by the user are made available immediately, and
   erase/kill character processing is not performed. In nocbreak mode,
   typed characters are buffered until a newline or carriage return.
   Interrupt and flow control characters are unaffected by this mode.
   PDCurses always starts in cbreak mode.

   echo() and noecho() control whether typed characters are echoed by
   the input routine. Initially, input characters are echoed. Subsequent
   calls to echo() and noecho() do not flush type-ahead.

   is_cbreak(), is_echo(), is_nl(), and is_raw() are ncurses extensions.
   They return the current state of the corresponding flags,  or -1 if
   the library is uninitialized.

   PDC_getcbreak() and PDC_getecho() are older versions of is_cbreak()
   and is_echo(),  but return TRUE if the flag is set and FALSE if it is
   not set or the library is uninitialized.  Use of these two functions
   is deprecated.

   halfdelay() is similar to cbreak(), but allows for a time limit to be
   specified, in tenths of a second. This causes getch() to block for
   that period before returning ERR if no key has been received. tenths
   must be between 1 and 255.

   keypad() controls whether getch() returns function/special keys as
   single key codes (e.g., the left arrow key as KEY_LEFT). Per X/Open,
   the default for keypad mode is OFF. You'll probably want it on. With
   keypad mode off, if a special key is pressed, getch() does nothing or
   returns ERR.

   nodelay() controls whether wgetch() is a non-blocking call. If the
   option is enabled, and no input is ready, wgetch() will return ERR.
   If disabled, wgetch() will hang until input is ready.

   nl() enables the translation of a carriage return into a newline on
   input. nonl() disables this. Initially, the translation does occur.

   raw() and noraw() toggle raw mode. Raw mode is similar to cbreak
   mode, in that characters typed are immediately passed through to the
   user program. The difference is that in raw mode, the INTR, QUIT,
   SUSP, and STOP characters are passed through without being
   interpreted, and without generating a signal.

   In PDCurses, the meta() function sets raw mode on or off.

   timeout() and wtimeout() set blocking or non-blocking reads for the
   specified window. If the delay is negative, a blocking read is used;
   if zero, then non-blocking reads are done -- if no input is waiting,
   ERR is returned immediately. If the delay is positive, the read
   blocks for the delay period; if the period expires, ERR is returned.
   The delay is given in milliseconds, but this is rounded down to 50ms
   (1/20th sec) intervals, with a minimum of one interval if a postive
   delay is given; i.e., 1-99 will wait 50ms, 100-149 will wait 100ms,
   etc.

   wgetdelay() returns the delay timeout as set in wtimeout().

   intrflush(), notimeout(), noqiflush(), qiflush() and typeahead() do
   nothing in PDCurses, but are included for compatibility with other
   curses implementations.

   crmode() and nocrmode() are archaic equivalents to cbreak() and
   nocbreak(), respectively.

   is_keypad() reports whether the specified window is in keypad mode.

   is_nodelay() reports whether the specified window is in nodelay mode.

### Return Value

   All functions that return integers return OK on success and ERR on
   error.  is_keypad() and is_nodelay() return TRUE or FALSE.

   is_notimeout() is provided for compatibility with other curses
   implementations.  It has no real meaning in PDCursesMod and will
   always return FALSE.

 */

int cbreak(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    SP->cbreak = TRUE;

    return OK;
}

int nocbreak(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    SP->cbreak = FALSE;
    SP->delaytenths = 0;

    return OK;
}

bool PDC_getcbreak(void)
{
    assert( SP);
    return( SP->cbreak);
}

int is_cbreak(void)
{
    return( SP ? SP->cbreak : -1);
}

int is_echo(void)
{
    return( SP ? SP->echo : -1);
}


int echo(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    SP->echo = TRUE;

    return OK;
}

int noecho(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    SP->echo = FALSE;

    return OK;
}

bool PDC_getecho(void)
{
    assert( SP);
    return( SP->echo);
}

int halfdelay(int tenths)
{
    assert( SP);
    if (!SP || tenths < 1 || tenths > 255)
        return ERR;

    SP->delaytenths = tenths;

    return OK;
}

int intrflush(WINDOW *win, bool bf)
{
    INTENTIONALLY_UNUSED_PARAMETER( win);
    INTENTIONALLY_UNUSED_PARAMETER( bf);
    return OK;
}

int keypad(WINDOW *win, bool bf)
{
    assert( win);
    if (!win)
        return ERR;

    win->_use_keypad = bf;

    return OK;
}

int meta(WINDOW *win, bool bf)
{
    INTENTIONALLY_UNUSED_PARAMETER( win);
    assert( SP);
    if (!SP)
        return ERR;

    SP->raw_inp = bf;

    return OK;
}

int nl(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    SP->autocr = TRUE;

    return OK;
}

int nonl(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    SP->autocr = FALSE;

    return OK;
}

int is_nl(void)
{
    return( SP ? SP->autocr : -1);
}

int nodelay(WINDOW *win, bool flag)
{
    assert( win);
    if (!win)
        return ERR;

    win->_nodelay = flag;

    return OK;
}

int notimeout(WINDOW *win, bool flag)
{
    INTENTIONALLY_UNUSED_PARAMETER( win);
    INTENTIONALLY_UNUSED_PARAMETER( flag);
    return OK;
}

int wgetdelay(const WINDOW *win)
{
    assert( win);
    if (!win)
        return 0;

    return win->_delayms;
}

int raw(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    PDC_set_keyboard_binary(TRUE);
    SP->raw_inp = TRUE;

    return OK;
}

int noraw(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    PDC_set_keyboard_binary(FALSE);
    SP->raw_inp = FALSE;

    return OK;
}

int is_raw(void)
{
    return( SP ? SP->raw_inp : -1);
}

void noqiflush(void)
{
}

void qiflush(void)
{
}

int typeahead(int fildes)
{
    INTENTIONALLY_UNUSED_PARAMETER( fildes);
    return OK;
}

void wtimeout(WINDOW *win, int delay)
{
    assert( win);
    if (!win)
        return;

    if (delay < 0)
    {
        /* This causes a blocking read on the window, so turn on delay
           mode */

        win->_nodelay = FALSE;
        win->_delayms = 0;
    }
    else if (!delay)
    {
        /* This causes a non-blocking read on the window, so turn off
           delay mode */

        win->_nodelay = TRUE;
        win->_delayms = 0;
    }
    else
    {
        /* This causes the read on the window to delay for the number of
           milliseconds. Also forces the window into non-blocking read
           mode */

     /* win->_nodelay = TRUE; */
        win->_delayms = delay;
    }
}

void timeout(int delay)
{
    wtimeout(stdscr, delay);
}

int crmode(void)
{
    return cbreak();
}

int nocrmode(void)
{
    return nocbreak();
}

bool is_keypad(const WINDOW *win)
{
    assert( win);
    if (!win)
        return FALSE;

    return win->_use_keypad;
}

bool is_nodelay(const WINDOW *win)
{
    assert( win);
    if (!win)
        return FALSE;

    return win->_nodelay;
}

bool is_notimeout(const WINDOW *win)
{
    assert( win);
    INTENTIONALLY_UNUSED_PARAMETER( win);
    return FALSE;
}
