#include <curspriv.h>

/*

### Description

   def_prog_mode() and def_shell_mode() save the current terminal modes
   as the "program" (in curses) or "shell" (not in curses) state for use
   by the reset_prog_mode() and reset_shell_mode() functions. This is
   done automatically by initscr().

   reset_prog_mode() and reset_shell_mode() restore the terminal to
   "program" (in curses) or "shell" (not in curses) state. These are
   done automatically by endwin() and doupdate() after an endwin(), so
   they would normally not be called before these functions.

   savetty() and resetty() save and restore the state of the terminal
   modes. savetty() saves the current state in a buffer, and resetty()
   restores the state to what it was at the last call to savetty().

   curs_set() alters the appearance of the cursor. A visibility of 0
   makes it disappear; 1 makes it appear "normal" (usually an underline)
   and 2 makes it "highly visible" (usually a block).

   ripoffline() reduces the size of stdscr by one line. If the "line"
   parameter is positive, the line is removed from the top of the
   screen; if negative, from the bottom. Up to 5 lines can be ripped off
   stdscr by calling ripoffline() repeatedly. The function argument,
   init, is called from within initscr() or newterm(), so ripoffline()
   must be called before either of these functions. The init function
   receives a pointer to a one-line WINDOW, and the width of the window.
   Calling ripoffline() with a NULL init function pointer is an error.

   napms() suspends the program for the specified number of
   milliseconds. draino() is an archaic equivalent. Note that since
   napms() attempts to give up a time slice and yield control back to
   the OS, all times are approximate. (In DOS, the delay is actually
   rounded to the nearest 'tick' (~55 milliseconds),  with a minimum of
   one interval; i.e., 1-82 will wait ~55ms, 83-137 will wait ~110ms,
   etc.)  0 returns immediately.

   resetterm(), fixterm() and saveterm() are archaic equivalents for
   reset_shell_mode(), reset_prog_mode() and def_prog_mode(),
   respectively.

### Return Value

   All functions return OK on success and ERR on error, except
   curs_set(), which returns the previous visibility.
 */

static struct cttyset {
    bool been_set;
    SCREEN saved;
  } ctty[3];

enum { PDC_SH_TTY,
       PDC_PR_TTY,
       PDC_SAVE_TTY
     };

static void _save_mode(int i)
{
    ctty[i].been_set = TRUE;

    memcpy(&(ctty[i].saved), SP, sizeof(SCREEN));

    PDC_save_screen_mode(i);
}

static int _restore_mode(int i)
{
    if (ctty[i].been_set == TRUE)
    {
        WINDOW **window_list = SP->window_list;
        const int n_windows = SP->n_windows;
        struct _pdc_pair *pairs = SP->pairs;
        const int pairs_allocated = SP->pairs_allocated;
        hash_idx_t *pair_hash_tbl = SP->pair_hash_tbl;
        const int pair_hash_tbl_size = SP->pair_hash_tbl_size;
        const int pair_hash_tbl_used = SP->pair_hash_tbl_used;

        memcpy(SP, &(ctty[i].saved), sizeof(SCREEN));
        SP->window_list = window_list;
        SP->n_windows = n_windows;
        SP->pairs = pairs;
        SP->pairs_allocated = pairs_allocated;
        SP->pair_hash_tbl = pair_hash_tbl;
        SP->pair_hash_tbl_size = pair_hash_tbl_size;
        SP->pair_hash_tbl_used = pair_hash_tbl_used;

        if (ctty[i].saved.raw_out)
            raw();

        PDC_restore_screen_mode(i);

        if ((LINES != ctty[i].saved.lines) ||
            (COLS != ctty[i].saved.cols))
            resize_term(ctty[i].saved.lines, ctty[i].saved.cols);

        PDC_curs_set(ctty[i].saved.visibility);

        PDC_gotoyx(ctty[i].saved.cursrow, ctty[i].saved.curscol);
    }

    return ctty[i].been_set ? OK : ERR;
}

int def_prog_mode(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    _save_mode(PDC_PR_TTY);

    return OK;
}

int def_shell_mode(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    _save_mode(PDC_SH_TTY);

    return OK;
}

int reset_prog_mode(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    _restore_mode(PDC_PR_TTY);
    PDC_reset_prog_mode();

    return OK;
}

int reset_shell_mode(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    _restore_mode(PDC_SH_TTY);
    PDC_reset_shell_mode();

    return OK;
}

int resetty(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    return _restore_mode(PDC_SAVE_TTY);
}

int savetty(void)
{
    assert( SP);
    if (!SP)
        return ERR;

    _save_mode(PDC_SAVE_TTY);

    return OK;
}

int curs_set(int visibility)
{
    int ret_vis;

    assert( visibility >= 0);
    assert( !(visibility & ~0xf0f));
    if ((visibility < 0) || (visibility & ~0xf0f))
        return ERR;

    ret_vis = PDC_curs_set(visibility);

    /* If the cursor is changing from invisible to visible, update
       its position
     */
    if (visibility && !ret_vis)
        PDC_gotoyx(SP->cursrow, SP->curscol);

    return ret_vis;
}

/*
 TODO : must initscr() be called for napms to work?  Certainly not
 on some platforms,  but is it true for all?
 */
int napms(int ms)
{
    assert( SP);
    if (!SP)
        return ERR;

    if (SP->dirty)
    {
        int curs_state = SP->visibility;
        bool leave_state = is_leaveok(curscr);

        SP->dirty = FALSE;

        leaveok(curscr, TRUE);

        wrefresh(curscr);

        leaveok(curscr, leave_state);
        curs_set(curs_state);
    }

    if( ms > 0)
        PDC_napms(ms);

    return OK;
}

int ripoffline(int line, int (*init)(WINDOW *, int))
{
    static RIPPEDOFFLINE *linesripped = NULL;
    static int linesrippedoff = 0;

    if( !init && SP)
    {                 /* copy ripped-off line data into the SCREEN struct */
        SP->linesripped = linesripped;
        SP->linesrippedoff = linesrippedoff;
        linesripped = NULL;
        return OK;
    }
    assert( line && init);
    if (linesrippedoff < MAX_RIPPEDOFFLINES && line && init)
    {
        if( !linesripped)
            linesripped = calloc( MAX_RIPPEDOFFLINES, sizeof( RIPPEDOFFLINE));
        linesripped[(int)linesrippedoff].line = line;
        linesripped[(int)linesrippedoff++].init = init;

        return OK;
    }

    return ERR;
}

int draino(int ms)
{
    return napms(ms);
}

int resetterm(void)
{
    return reset_shell_mode();
}

int fixterm(void)
{
    return reset_prog_mode();
}

int saveterm(void)
{
    return def_prog_mode();
}
