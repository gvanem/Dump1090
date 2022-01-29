#ifndef _TRACE_WIN32H
#define _TRACE_WIN32H

#include <windows.h>

#define TRACE_COLOR_START  (FOREGROUND_INTENSITY | 3)  /* bright cyan */
#define TRACE_COLOR_ARGS   (FOREGROUND_INTENSITY | 7)  /* bright white */
#define TRACE_COLOR_OK     (FOREGROUND_INTENSITY | 2)  /* bright green */
#define TRACE_COLOR_ERR    (FOREGROUND_INTENSITY | 4)  /* bright red */

/*
 * Trace printer controlled by env-var "set RTLSDR_TRACE=level":
 */
#define TRACE(level, fmt, ...)                                    \
        do {                                                      \
          if (trace_level() >= level) {                           \
            _trace_file = __FILE__;                               \
            _trace_line = __LINE__;                               \
            trace_printf (TRACE_COLOR_START, NULL);               \
            trace_printf (TRACE_COLOR_ARGS, fmt, ## __VA_ARGS__); \
          }                                                       \
        } while (0)

#define TRACE_WINUSB(func, win_err) \
        trace_winusb(func, win_err, __FILE__, __LINE__)

extern const char *_trace_file;
extern unsigned    _trace_line;

int         trace_level (void);
void        trace_winusb (const char *func, DWORD win_err, const char *file, unsigned line);
void        trace_printf (unsigned short col, const char *fmt, ...);
const char *trace_strerror (DWORD err);

#endif
