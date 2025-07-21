#include <curspriv.h>
#include <limits.h>

/*

### Description

   baudrate() is supposed to return the output speed of the terminal. In
   PDCurses, it simply returns INT_MAX.

   has_ic and has_il() return TRUE,  indicating that the terminal has the
   capability to insert and delete characters and lines,  respectively.

   erasechar() and killchar() return ^H and ^U, respectively -- the
   ERASE and KILL characters. In other curses implementations, these may
   vary by terminal type. erasewchar() and killwchar() are the wide-
   character versions; they take a pointer to a location in which to
   store the character, and return OK or ERR.

   longname() returns a pointer to a static area containing a verbose
   description of the current terminal. The maximum length of the string
   is 128 characters. It is defined only after the call to initscr() or
   newterm().

   termname() returns a pointer to a static area containing a short
   description of the current terminal (14 characters).

   termattrs() returns a logical OR of all video attributes supported by
   the terminal.

   wordchar() is a PDCurses extension of the concept behind the
   functions erasechar() and killchar(), returning the "delete word"
   character, ^W.

 */

int baudrate(void)
{
    return INT_MAX;
}

char erasechar(void)
{
    return _ECHAR;      /* character delete char (^H) */
}

bool has_ic(void)
{
    return TRUE;
}

bool has_il(void)
{
    return TRUE;
}

char killchar(void)
{
    return _DLCHAR;     /* line delete char (^U) */
}

char *longname(void)
{
    sprintf(ttytype, "pdcurses|PDCursesMod for %s", PDC_sysname());
    return ttytype + 9; /* skip "pdcurses|" */
}

chtype termattrs(void)
{
    return SP ? (chtype)SP->termattrs : (chtype)0;
}

attr_t term_attrs(void)
{
    return SP ? SP->termattrs : (attr_t)0;
}

char *termname(void)
{
    static char _termname[14] = "pdcurses";

    return _termname;
}

char wordchar(void)
{
    return _DWCHAR;         /* word delete char */
}

int erasewchar(wchar_t *ch)
{
    assert( ch);
    if (!ch)
        return ERR;

    *ch = (wchar_t)_ECHAR;
    return OK;
}

int killwchar(wchar_t *ch)
{
    assert( ch);
    if (!ch)
        return ERR;

    *ch = (wchar_t)_DLCHAR;

    return OK;
}
