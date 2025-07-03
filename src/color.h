/**\file    color.h
 * \ingroup Misc
 */
#pragma once

#include <stdarg.h>
#include "misc.h"

/**
 * \brief
 * Print to console using embedded colour-codes inside the string-format.
 *
 * These are the default colour that `C_init_colour_map()` is initialised with.
 * Can be used like:
 * \code
 *   C_printf ("  " C_BR_RED "<not found>" C_DEFAULT);
 * \endcode
 */
#define C_BR_CYAN     "~1"   /**< bright cyan */
#define C_BR_GREEN    "~2"   /**< bright green */
#define C_BR_YELLOW   "~3"   /**< bright yellow */
#define C_BR_MEGENTA  "~4"   /**< bright magenta */
#define C_BR_RED      "~5"   /**< bright red */
#define C_BR_WHITE    "~6"   /**< bright white */
#define C_DK_CYAN     "~7"   /**< dark cyan */
#define C_BG_RED      "~8"   /**< white on red background */
#define C_BG_BLACK    "~9"   /**< white on black background (not yet) */
#define C_DEFAULT     "~0"   /**< restore default colour */

extern int C_printf (_Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(1, 2);

extern int C_vprintf (const char *fmt, va_list args);
extern int C_puts    (const char *str);
extern int C_putc    (int ch);
