#include <curspriv.h>

/*

### Description

   The printw() functions add a formatted string to the window at the
   current or specified cursor position. The format strings are the same
   as used in the standard C library's printf(). (printw() can be used
   as a drop-in replacement for printf().)

   The duplication between vwprintw() and vw_printw() is for historic
   reasons. In PDCurses, they're the same.

### Return Value

   All functions return the number of characters printed, or ERR on
   error.

 */

/*
 * _vsnprintf() and earlier vsnprintf() return -1 if the output doesn't
 * fit in the buffer.  When that happens,  we try again with a
 * larger buffer, doubling its size until it fits.  C99-compliant
 * vsnprintf() returns the number of bytes actually needed (minus the
 * trailing zero).
 */
int vwprintw(WINDOW *win, const char *fmt, va_list varglist)
{
    char printbuf[513];
    int len, rval;
    char *buf = printbuf;
    va_list varglist_copy;
    size_t buffsize = sizeof( printbuf) - 1;

    va_copy( varglist_copy, varglist);
    len = _vsnprintf( buf, buffsize, fmt, varglist_copy);
    while( len < 0 || len > (int)buffsize)
    {
        if( -1 == len)       /* Microsoft,  glibc 2.0 & earlier */
            buffsize <<= 1;
        else                 /* glibc 2.0.6 & later (C99 behavior) */
            buffsize = len + 1;

        if( buf != printbuf)
            free( buf);
        buf = malloc( buffsize + 1);
        va_copy( varglist_copy, varglist);
        len = _vsnprintf( buf, buffsize, fmt, varglist_copy);
    }
    buf[len] = '\0';
    rval = (waddstr(win, buf) == ERR) ? ERR : len;
    if( buf != printbuf)
        free( buf);
    return rval;
}

int printw(const char *fmt, ...)
{
    va_list args;
    int retval;

    va_start(args, fmt);
    retval = vwprintw(stdscr, fmt, args);
    va_end(args);

    return retval;
}

int wprintw(WINDOW *win, const char *fmt, ...)
{
    va_list args;
    int retval;

    va_start(args, fmt);
    retval = vwprintw(win, fmt, args);
    va_end(args);

    return retval;
}

int mvprintw(int y, int x, const char *fmt, ...)
{
    va_list args;
    int retval;

    if (move(y, x) == ERR)
        return ERR;

    va_start(args, fmt);
    retval = vwprintw(stdscr, fmt, args);
    va_end(args);

    return retval;
}

int mvwprintw(WINDOW *win, int y, int x, const char *fmt, ...)
{
    va_list args;
    int retval;

    if (wmove(win, y, x) == ERR)
        return ERR;

    va_start(args, fmt);
    retval = vwprintw(win, fmt, args);
    va_end(args);

    return retval;
}

int vw_printw(WINDOW *win, const char *fmt, va_list varglist)
{
    return vwprintw(win, fmt, varglist);
}
