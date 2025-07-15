/* PDCurses */

#include <curspriv.h>

/*man-start**************************************************************

scanw
-----

### Synopsis

    int scanw(const char *fmt, ...);
    int wscanw(WINDOW *win, const char *fmt, ...);
    int mvscanw(int y, int x, const char *fmt, ...);
    int mvwscanw(WINDOW *win, int y, int x, const char *fmt, ...);
    int vwscanw(WINDOW *win, const char *fmt, va_list varglist);
    int vw_scanw(WINDOW *win, const char *fmt, va_list varglist);

### Description

   These routines correspond to the standard C library's scanf() family.
   Each gets a string from the window via wgetnstr(), and uses the
   resulting line as input for the scan.

   The duplication between vwscanw() and vw_scanw() is for historic
   reasons. In PDCurses, they're the same.

### Return Value

   On successful completion, these functions return the number of items
   successfully matched. Otherwise they return ERR.

### Portability
   Function              | X/Open | ncurses | NetBSD
   :---------------------|:------:|:-------:|:------:
   scanw                 |    Y   |    Y    |   Y
   wscanw                |    Y   |    Y    |   Y
   mvscanw               |    Y   |    Y    |   Y
   mvwscanw              |    Y   |    Y    |   Y
   vwscanw               |    Y   |    Y    |   Y
   vw_scanw              |    Y   |    Y    |   Y

**man-end****************************************************************/

int vwscanw(WINDOW *win, const char *fmt, va_list varglist)
{
    char scanbuf[256];

    if (wgetnstr(win, scanbuf, 255) == ERR)
        return ERR;

    return vsscanf(scanbuf, fmt, varglist);
}

int scanw(const char *fmt, ...)
{
    va_list args;
    int retval;

    va_start(args, fmt);
    retval = vwscanw(stdscr, fmt, args);
    va_end(args);

    return retval;
}

int wscanw(WINDOW *win, const char *fmt, ...)
{
    va_list args;
    int retval;

    va_start(args, fmt);
    retval = vwscanw(win, fmt, args);
    va_end(args);

    return retval;
}

int mvscanw(int y, int x, const char *fmt, ...)
{
    va_list args;
    int retval;

    if (move(y, x) == ERR)
        return ERR;

    va_start(args, fmt);
    retval = vwscanw(stdscr, fmt, args);
    va_end(args);

    return retval;
}

int mvwscanw(WINDOW *win, int y, int x, const char *fmt, ...)
{
    va_list args;
    int retval;

    if (wmove(win, y, x) == ERR)
        return ERR;

    va_start(args, fmt);
    retval = vwscanw(win, fmt, args);
    va_end(args);

    return retval;
}

int vw_scanw(WINDOW *win, const char *fmt, va_list varglist)
{
    return vwscanw(win, fmt, varglist);
}

