#include <curspriv.h>

/* get the cursor size/shape */

int PDC_get_cursor_mode(void)
{
    CONSOLE_CURSOR_INFO ci;

    GetConsoleCursorInfo(pdc_con_out, &ci);
    return ci.dwSize;
}

/* return number of screen rows */

int PDC_get_rows(void)
{
    CONSOLE_SCREEN_BUFFER_INFO scr;

    GetConsoleScreenBufferInfo(pdc_con_out, &scr);
    return scr.srWindow.Bottom - scr.srWindow.Top + 1;
}

/* return width of screen/viewport */

int PDC_get_columns(void)
{
    CONSOLE_SCREEN_BUFFER_INFO scr;

    GetConsoleScreenBufferInfo(pdc_con_out, &scr);
    return scr.srWindow.Right - scr.srWindow.Left + 1;
}
