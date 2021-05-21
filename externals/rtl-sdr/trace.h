#ifndef _TRACE_WIN32
#define _TRACE_WIN32

#include <windows.h>

#define TRACE_COLOR_START  (FOREGROUND_INTENSITY | 3)  /* bright cyan */
#define TRACE_COLOR_ARGS   (FOREGROUND_INTENSITY | 7)  /* bright white */
#define TRACE_COLOR_OK     (FOREGROUND_INTENSITY | 2)  /* bright green */
#define TRACE_COLOR_ERR    (FOREGROUND_INTENSITY | 4)  /* bright red */

#define TRACE(level, fmt, ...)                                    \
        do {                                                      \
          if (trace_level() >= level) {                           \
            _trace_file = __FILE__;                               \
            _trace_line = __LINE__;                               \
            trace_printf (TRACE_COLOR_START, NULL);               \
            trace_printf (TRACE_COLOR_ARGS, fmt, ## __VA_ARGS__); \
          }                                                       \
        } while (0)

#define TRACE_LIBUSB(r) trace_libusb (r, __FUNCTION__, __FILE__, __LINE__)

extern const char *_trace_file;
extern unsigned    _trace_line;
extern unsigned    _trace_scope;

int  trace_level (void);
int  trace_libusb (int r, const char *func, const char *file, unsigned line);
void trace_printf (unsigned short col, const char *fmt, ...);

#endif
