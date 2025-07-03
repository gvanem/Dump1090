/**\file    interactive.h
 * \ingroup Misc
 * \brief   Function for `--interactive` mode
 *          Using either Windows-Console or Curses functions.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define TUI_WINCON 1
#define TUI_CURSES 2

bool interactive_init (void);
void interactive_exit (void);
void interactive_update_gain (void);
void interactive_title_stats (void);
void interactive_other_stats (void);
void interactive_raw_SBS_stats (void);
void interactive_clreol (void);
void interactive_show_data (uint64_t now);
