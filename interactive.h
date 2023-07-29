/**\file    interactive.h
 * \ingroup Misc
 *
 * Function for `--interactive` mode.
 * Using either Windows-Console or Curses functions.
 */
#ifndef _INTERACTIVE_H
#define _INTERACTIVE_H

#include <stdint.h>
#include "aircraft.h"

#define TUI_WINCON 1
#define TUI_CURSES 2

bool      interactive_init (void);
void      interactive_exit (void);
void      interactive_update_gain (void);
void      interactive_title_stats (void);
void      interactive_other_stats (void);
void      interactive_clreol (void);
void      interactive_show_data (uint64_t now);
aircraft *interactive_receive_data (const modeS_message *mm, uint64_t now);

#endif
