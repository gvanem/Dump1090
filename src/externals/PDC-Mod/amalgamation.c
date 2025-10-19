/*
 * This file is an amalgamation of the needed sources files
 * in PDCurses-MOD.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h> /* struct timeval */
#undef MOUSE_MOVED

#ifdef _INC_CONIO
#error "<conio.h> must not be included"
#endif

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

