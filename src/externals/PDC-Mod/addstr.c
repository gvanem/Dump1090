#include <curspriv.h>

/*

### Description

   These routines write all the characters of the null-terminated string
   str or wide-character string wstr to the given window. The
   functionality is similar to calling waddch() once for each character
   in the string; except that, when PDCurses is built with wide-
   character support enabled, the narrow-character functions treat the
   string as a multibyte string in the current locale, and convert it.
   The routines with n as the last argument write at most n characters;
   if n is negative, then the entire string will be added.

### Return Value

   All functions return OK or ERR.
 */

int waddnstr(WINDOW *win, const char *str, int n)
{
    int i = 0;

    assert( win);
    assert( str);
    if (!win || !str)
        return ERR;

    while( (i < n || n < 0) && str[i])
    {
        wchar_t wch;
        int retval = PDC_mbtowc(&wch, str + i, n >= 0 ? n - i : 6);

        if (retval <= 0)
            return OK;

        i += retval;
        if (waddch(win, wch) == ERR)
            return ERR;
    }

    return OK;
}

int addstr(const char *str)
{
    return waddnstr(stdscr, str, -1);
}

int addnstr(const char *str, int n)
{
    return waddnstr(stdscr, str, n);
}

int waddstr(WINDOW *win, const char *str)
{
    return waddnstr(win, str, -1);
}

int mvaddstr(int y, int x, const char *str)
{
    if (move(y, x) == ERR)
        return ERR;

    return waddnstr(stdscr, str, -1);
}

int mvaddnstr(int y, int x, const char *str, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return waddnstr(stdscr, str, n);
}

int mvwaddstr(WINDOW *win, int y, int x, const char *str)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return waddnstr(win, str, -1);
}

int mvwaddnstr(WINDOW *win, int y, int x, const char *str, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return waddnstr(win, str, n);
}

int waddnwstr(WINDOW *win, const wchar_t *wstr, int n)
{
    int i = 0;

    assert( win);
    assert( wstr);
    if (!win || !wstr)
        return ERR;

    while( (i < n || n < 0) && wstr[i])
    {
        chtype wch = wstr[i++];

        if (waddch(win, wch) == ERR)
            return ERR;
    }

    return OK;
}

int addwstr(const wchar_t *wstr)
{
    return waddnwstr(stdscr, wstr, -1);
}

int addnwstr(const wchar_t *wstr, int n)
{
    return waddnwstr(stdscr, wstr, n);
}

int waddwstr(WINDOW *win, const wchar_t *wstr)
{
    return waddnwstr(win, wstr, -1);
}

int mvaddwstr(int y, int x, const wchar_t *wstr)
{
    if (move(y, x) == ERR)
        return ERR;

    return waddnwstr(stdscr, wstr, -1);
}

int mvaddnwstr(int y, int x, const wchar_t *wstr, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return waddnwstr(stdscr, wstr, n);
}

int mvwaddwstr(WINDOW *win, int y, int x, const wchar_t *wstr)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return waddnwstr(win, wstr, -1);
}

int mvwaddnwstr(WINDOW *win, int y, int x, const wchar_t *wstr, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return waddnwstr(win, wstr, n);
}
