#pragma once

#ifndef UNICODE
#define UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#undef MOUSE_MOVED
#include <curspriv.h>
#include <stdlib.h>

typedef struct {
        short r, g, b;
        bool mapped;
      } PDCCOLOR;

extern PDCCOLOR pdc_color [PDC_MAXCOL];

extern HANDLE pdc_con_out, pdc_con_in;
extern DWORD  pdc_quick_edit;
extern DWORD  pdc_last_blink;
extern short  pdc_curstoreal[16], pdc_curstoansi[16];
extern short  pdc_oldf, pdc_oldb, pdc_oldu;
extern bool   pdc_conemu, pdc_wt, pdc_ansi;

void PDC_blink_text (void);
