#pragma once
#include <windows.h>

/*
 * Trace printer controlled by env-var "set RTLSDR_TRACE=level":
 */
#define RTL_TRACE(level, fmt, ...)                                   \
        do {                                                         \
          if (trace_level() >= level)                                \
             trace_printf (__FILE__, __LINE__, fmt, ## __VA_ARGS__); \
        } while (0)

#define RTL_TRACE_WINUSB(func, win_err) \
        trace_winusb (__FILE__, __LINE__, func, win_err)

int         trace_level (void);
void        trace_winusb (const char *fname, unsigned line, const char *func, DWORD win_err);
void        trace_printf (const char *fname, unsigned line, const char *fmt, ...);
HANDLE      trace_file (BOOL use_stderr);
const char *trace_strerror (DWORD err);
