/*
 * This file is an amalgamation of the needed sources files
 * in PDCurses-MOD. By combining all the individual C code files into
 * this single file, the entire code can be compiled as a single
 * translation unit.
 */
#include <windows.h>

#define PDC_99 1
#undef  PDC_NCMOUSE
#undef  MOUSE_MOVED

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
#include "mouse.c"       /* From '$(PDC-MOD-ROOT)/common/mouse.c */
#include "mouse2.c"      /* From '$(PDC-MOD-ROOT)/pdcurses/mouse.c */
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

#pragma comment (lib, "winmm.lib")
