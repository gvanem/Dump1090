#include <curspriv.h>

/*

### Description

   The insstr() functions insert a character string into a window at the
   current cursor position, by repeatedly calling winsch(). When
   PDCurses is built with wide-character support enabled, the narrow-
   character functions treat the string as a multibyte string in the
   current locale, and convert it first. All characters to the right of
   the cursor are moved to the right, with the possibility of the
   rightmost characters on the line being lost. The cursor position
   does not change (after moving to y, x, if specified). The routines
   with n as the last argument insert at most n characters; if n is
   negative, then the entire string is inserted.

### Return Value

   All functions return OK on success and ERR on error.

 */

#define MAX_WSTR 80

int winsnstr(WINDOW *win, const char *str, int n)
{
    wchar_t wstr[MAX_WSTR], *p = wstr;
    int i = 0;
    int len;

    assert( win);
    assert( str);
    if (!win || !str)
        return ERR;

    len = (int)strlen(str);

    if (n < 0 || n > len)
        n = len;

    while( p < wstr + MAX_WSTR && str[i])
    {
        int retval = PDC_mbtowc(p, str + i, n - i);

        if (retval <= 0)
            break;
        p++;
        i += retval;
    }
    if( p == wstr + MAX_WSTR)        /* not enough room in wstr;  break */
        if( ERR == winsnstr( win, str + i, n - i))    /* str into parts */
            return ERR;

    while (p > wstr)
        if (winsch(win, *--p) == ERR)
            return ERR;

    return OK;
}

int insstr(const char *str)
{
    return winsnstr(stdscr, str, -1);
}

int winsstr(WINDOW *win, const char *str)
{
    return winsnstr(win, str, -1);
}

int mvinsstr(int y, int x, const char *str)
{
    if (move(y, x) == ERR)
        return ERR;

    return winsnstr(stdscr, str, -1);
}

int mvwinsstr(WINDOW *win, int y, int x, const char *str)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return winsnstr(win, str, -1);
}

int insnstr(const char *str, int n)
{
    return winsnstr(stdscr, str, n);
}

int mvinsnstr(int y, int x, const char *str, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return winsnstr(stdscr, str, n);
}

int mvwinsnstr(WINDOW *win, int y, int x, const char *str, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return winsnstr(win, str, n);
}

int wins_nwstr(WINDOW *win, const wchar_t *wstr, int n)
{
    const wchar_t *p;
    int len;

    assert( win);
    assert( wstr);
    if (!win || !wstr)
        return ERR;

    for (len = 0, p = wstr; *p; p++)
        len++;

    if (n < 0 || n > len)
        n = len;

    while (n)
        if (winsch(win, wstr[--n]) == ERR)
            return ERR;

    return OK;
}

int ins_wstr(const wchar_t *wstr)
{
    return wins_nwstr(stdscr, wstr, -1);
}

int wins_wstr(WINDOW *win, const wchar_t *wstr)
{
    return wins_nwstr(win, wstr, -1);
}

int mvins_wstr(int y, int x, const wchar_t *wstr)
{
    if (move(y, x) == ERR)
        return ERR;

    return wins_nwstr(stdscr, wstr, -1);
}

int mvwins_wstr(WINDOW *win, int y, int x, const wchar_t *wstr)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wins_nwstr(win, wstr, -1);
}

int ins_nwstr(const wchar_t *wstr, int n)
{
    return wins_nwstr(stdscr, wstr, n);
}

int mvins_nwstr(int y, int x, const wchar_t *wstr, int n)
{
    if (move(y, x) == ERR)
        return ERR;

    return wins_nwstr(stdscr, wstr, n);
}

int mvwins_nwstr(WINDOW *win, int y, int x, const wchar_t *wstr, int n)
{
    if (wmove(win, y, x) == ERR)
        return ERR;

    return wins_nwstr(win, wstr, n);
}
