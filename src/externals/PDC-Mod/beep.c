#include <curspriv.h>

/*

### Description

   beep() sounds the audible bell on the terminal, if possible; if not,
   it calls flash().

   flash() "flashes" the screen, by inverting the foreground and
   background of every cell, pausing, and then restoring the original
   attributes.

### Return Value

   These functions return ERR if called before initscr(), otherwise OK.

*/

int beep(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    if (SP->audible)
         PDC_beep();
    else flash();
    return OK;
}

int flash(void)
{
    int z, y, x;

    assert( curscr);
    if (!curscr)
        return ERR;

    /* Reverse each cell; wait; restore the screen */

    for (z = 0; z < 2; z++)
    {
        for (y = 0; y < LINES; y++)
            for (x = 0; x < COLS; x++)
                curscr->_y[y][x] ^= A_REVERSE;

        wrefresh(curscr);
        if (!z)
            napms(50);
    }
    return OK;
}
