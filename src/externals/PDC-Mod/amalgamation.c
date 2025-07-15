/*
 * This file is an amalgamation of the needed sources files
 * in PDCurses-MOD. By combining all the individual C code files into
 * this single file, the entire code can be compiled as a single
 * translation unit.
 */
#define PDC_99 1

/* Other possible defines:
 *   #define PDC_WIDE
 *   #define PDC_RGB
 *   #define PDC_FORCE_UTF8
 *   #define PDCDEBUG
 */
#undef PDC_NCMOUSE
#undef MOUSE_MOVED

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
#include "mouse2.c"
#include "move.c"
#include "outopts.c"
#include "overlay.c"
#include "pad.c"
#include "pdcdisp.c"
#include "pdcgetsc.c"
#include "pdckbd.c"
#include "pdcscrn.c"
#include "pdcsetsc.c"
#include "pdcutil.c"
#include "panel.c"
#include "printw.c"
#include "refresh.c"
#include "scroll.c"
#include "slk.c"
#include "termattr.c"
#include "touch.c"
#include "util.c"
#include "winclip.c"
#include "window.c"
#include "debug.c"

#pragma comment (lib, "winmm.lib")
