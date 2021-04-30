/*
 * Generic debug and trace macro. Print to 'stdout' if 'trace_level'
 * is sufficiently high. And on Windows, do it in colours.
 */
#include <stdio.h>
#include <string.h>
#include <libusb.h>
#include "trace.h"

#if (TRACE_COLOR_START == TRACE_COLOR_ARGS) || \
    (TRACE_COLOR_START == TRACE_COLOR_OK)   || \
    (TRACE_COLOR_START == TRACE_COLOR_ERR)
  #error "All colours must be unique."
#endif

static CONSOLE_SCREEN_BUFFER_INFO console_info;
static HANDLE                     stdout_hnd;

const char *_trace_file = NULL;
unsigned    _trace_line  = 0;
unsigned    _trace_scope = 0;

int trace_level (void)
{
  static int level = -1;

  if (level == -1)
  {
    const char *env = getenv ("RTLSDR_DEBUG");

    if (env)
    {
      level = atoi (env);
      stdout_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
      GetConsoleScreenBufferInfo (stdout_hnd, &console_info);
    }
  }
  return (level);
}

static void set_color (unsigned short col)
{
  if (col == 0)
       SetConsoleTextAttribute (stdout_hnd, console_info.wAttributes);
  else SetConsoleTextAttribute (stdout_hnd, (console_info.wAttributes & ~7) | col);
}

void trace_printf (unsigned short col, const char *fmt, ...)
{
  va_list args;

  set_color (col);

  if (col == TRACE_COLOR_START)
  {
    printf ("%*s%s(%u): ", 2*_trace_scope, "", _trace_file, _trace_line);
    return;
  }
  va_start (args, fmt);
  vprintf (fmt, args);
  va_end (args);
  fflush (stdout);
  fflush (stderr);
  set_color (0);
}

int trace_libusb (int r, const char *func, const char *file, unsigned line)
{
  int level = trace_level();

  if (level < 1)
     return (r);

  _trace_scope++;
  _trace_line = line;
  _trace_file = file;

  if (level >= 1 && r < 0)
  {
    trace_printf (TRACE_COLOR_START, NULL);
    trace_printf (TRACE_COLOR_ERR, "%s() failed with %d/%s: %s\n",
                  func, r, libusb_error_name(r), libusb_strerror(r));
  }
  else if (level >= 2 && r > 0)
  {
    trace_printf (TRACE_COLOR_START, NULL);
    trace_printf (TRACE_COLOR_OK, "%s() ok; %d.\n", func, r);
  }
  _trace_scope--;
  return (r);
}
