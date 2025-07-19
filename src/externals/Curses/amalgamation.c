/*
 * This file is an amalgamation of the needed sources files
 * in PDCurses. By combining all the individual C code files into
 * this single file, the entire code can be compiled as a single
 * translation unit.
 *
 * Extra warning control for PDCurses:
 */
#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wunused-parameter"

#elif defined(_MSC_VER)
  #pragma warning (disable: 4100)  /* 'opts': unreferenced formal parameter */
  #pragma warning (disable: 4245)  /* 'return': conversion from 'int' to 'unsigned long', signed/unsigned mismatch */
  #pragma warning (disable: 4459)  /* declaration of 'first_col' hides global declaration */
#endif

#define PDC_99 1

/* Other possible PDCurses defines:
 *   #define PDC_WIDE
 *   #define PDC_RGB
 *   #define PDC_FORCE_UTF8
 *   #define PDCDEBUG
 */

#undef PDC_NCMOUSE

#include "addch.c"
#include "addstr.c"
#include "attr.c"
#include "border.c"
#include "bkgd.c"
#include "clear.c"
#include "color.c"
#include "getch.c"
#include "getyx.c"
#include "initscr.c"
#include "inopts.c"
#include "kernel.c"
#include "mouse.c"
#include "move.c"
#include "outopts.c"
#include "overlay.c"
#include "pad.c"
#include "pdcclip.c"
#include "pdcdisp.c"
#include "pdcgetsc.c"
#include "pdckbd.c"
#include "pdcscrn.c"
#include "pdcsetsc.c"
#include "pdcutil.c"
#include "printw.c"
#include "refresh.c"
#include "scroll.c"
#include "slk.c"
#include "touch.c"
#include "util.c"
#include "window.c"
