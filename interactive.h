/**\file    interactive.h
 * \ingroup Misc
 *
 * Function for `--interactive` mode.
 * Using either Windows-Console or Curses functions.
 */
#ifndef _INTERACTIVE_H
#define _INTERACTIVE_H

#undef MOUSE_MOVED
#include <curses.h>
#include <stdint.h>

#include "aircraft.h"

#define MODES_INTERACTIVE_REFRESH_TIME  250   /* Milliseconds */
#define MODES_INTERACTIVE_TTL         60000   /* TTL (msec) before being removed */

bool      interactive_init (void);
void      interactive_exit (void);
void      interactive_update_gain (void);
void      interactive_title_stats (void);
void      interactive_clreol (void);
void      interactive_show_data (uint64_t now);
aircraft *interactive_receive_data (const modeS_message *mm, uint64_t now);

#endif
