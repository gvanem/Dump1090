#include <curspriv.h>

/*

### Description

   These routines call wgetch() repeatedly to build a string,
   interpreting erase and kill characters along the way, until a newline
   or carriage return is received. When PDCurses is built with wide-
   character support enabled, the narrow-character functions convert the
   wgetch()'d values into a multibyte string in the current locale
   before returning it. The resulting string is placed in the area
   pointed to by *str. The routines with n as the last argument read at
   most n characters.  Note that this does not include the terminating
   '\0' character;  be sure your buffer has room for that.

   Note that there's no way to know how long the buffer passed to
   wgetstr() is, so use wgetnstr() to avoid buffer overflows.

### Return Value

   These functions return ERR on failure or any other value on success.

*/

#define MAXLINE 255

int wgetnstr(WINDOW *win, char *str, int n)
{
    wchar_t wstr[MAXLINE + 1];
    wint_t wintstr[MAXLINE + 1];
    int i;

    if (n < 0 || n > MAXLINE)
        n = MAXLINE;

    if (wgetn_wstr(win, wintstr, n) == ERR)
        return ERR;
    for (i = 0; i < n; ++i) {
        wstr[i] = (wchar_t)wintstr[i];
    }

    return (int)PDC_wcstombs(str, wstr, n);
}

int getstr(char *str)
{
    return wgetnstr(stdscr, str, MAXLINE);
}

int wgetstr(WINDOW *win, char *str)
{
    return wgetnstr(win, str, MAXLINE);
}

int mvgetstr(int y, int x, char *str)
{
    if (move(y, x) == ERR)
        return ERR;

    return wgetnstr(stdscr, str, MAXLINE);
}

int mvwgetstr(WINDOW *win, int y, int x, char *str)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wgetnstr(win, str, MAXLINE);
}

int getnstr(char *str, int n)
{
    return wgetnstr(stdscr, str, n);
}

int mvgetnstr(int y, int x, char *str, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return wgetnstr(stdscr, str, n);
}

int mvwgetnstr(WINDOW *win, int y, int x, char *str, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wgetnstr(win, str, n);
}

static void _clear_preceding_char( WINDOW *win, const int ch)
{
    waddstr(win, "\b \b");
    if( PDC_wcwidth( (int32_t)ch) == 2 || ch < ' ')
       waddstr(win, "\b \b");    /* fullwidth & ctrl chars take two columns */
}

int wgetn_wstr(WINDOW *win, wint_t *wstr, int n)
{
    int i, num, x, chars;
    wint_t *p;
    bool stop, oldecho, oldcbreak, oldnodelay;

    assert( win);
    assert( wstr);
    assert( SP);
    if (!win || !wstr)
        return ERR;

    chars = 0;
    p = wstr;
    stop = FALSE;

    x = win->_curx;

    oldcbreak = SP->cbreak; /* remember states */
    oldecho = SP->echo;
    oldnodelay = win->_nodelay;

    SP->echo = FALSE;       /* we do echo ourselves */
    cbreak();               /* ensure each key is returned immediately */
    win->_nodelay = FALSE;  /* don't return -1 */

    wrefresh(win);

    while (!stop)
    {
        wint_t ch;

        wget_wch( win, &ch);

        switch (ch)
        {

        case '\t':
            ch = ' ';
            num = TABSIZE - (win->_curx - x) % TABSIZE;
            for (i = 0; i < num; i++)
            {
                if (chars < n)
                {
                    if (oldecho)
                        waddch(win, ch);
                    *p++ = (wint_t)ch;
                    ++chars;
                }
                else
                    beep();
            }
            break;

        case _ECHAR:        /* CTRL-H -- Delete character */
            if (p > wstr)
            {
                ch = *--p;
                if (oldecho)
                   _clear_preceding_char( win, ch);
                chars--;
            }
            break;

        case _DLCHAR:       /* CTRL-U -- Delete line */
            while (p > wstr)
            {
                ch = *--p;
                if (oldecho)
                   _clear_preceding_char( win, ch);
            }
            chars = 0;
            break;

        case _DWCHAR:       /* CTRL-W -- Delete word */

            while ((p > wstr) && (*(p - 1) == ' '))
            {
                --p;        /* remove space */
                if (oldecho)
                   _clear_preceding_char( win, *p);
                chars--;
            }
            while ((p > wstr) && (*(p - 1) != ' '))
            {
                ch = *--p;
                if (oldecho)
                   _clear_preceding_char( win, ch);
                chars--;
            }
            break;

        case '\n':
        case '\r':
            stop = TRUE;
            if (oldecho)
                waddch(win, '\n');
            break;

        default:
            if (chars < n)
            {
                if( ch < KEY_MIN || ch >= KEY_MAX)
                {
                    *p++ = ch;
                    if (oldecho)
                        waddch(win, ch);
                    chars++;
                }
            }
            else
                beep();

            break;

        }

        wrefresh(win);
    }

    *p = '\0';

    SP->echo = oldecho;     /* restore old settings */
    SP->cbreak = oldcbreak;
    win->_nodelay = oldnodelay;

    return OK;
}

int get_wstr(wint_t *wstr)
{
    return wgetn_wstr(stdscr, wstr, MAXLINE);
}

int wget_wstr(WINDOW *win, wint_t *wstr)
{
    return wgetn_wstr(win, wstr, MAXLINE);
}

int mvget_wstr(int y, int x, wint_t *wstr)
{
    if (move(y, x) == ERR)
        return ERR;

    return wgetn_wstr(stdscr, wstr, MAXLINE);
}

int mvwget_wstr(WINDOW *win, int y, int x, wint_t *wstr)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wgetn_wstr(win, wstr, MAXLINE);
}

int getn_wstr(wint_t *wstr, int n)
{
    return wgetn_wstr(stdscr, wstr, n);
}

int mvgetn_wstr(int y, int x, wint_t *wstr, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return wgetn_wstr(stdscr, wstr, n);
}

int mvwgetn_wstr(WINDOW *win, int y, int x, wint_t *wstr, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wgetn_wstr(win, wstr, n);
}
