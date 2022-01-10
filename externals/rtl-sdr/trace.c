/*
 * Generic debug and trace macro. Print to 'stdout' if 'trace_level'
 * is sufficiently high. And do it in colours.
 */
#include <stdio.h>
#include <string.h>

#include "trace.h"

#if (TRACE_COLOR_START == TRACE_COLOR_ARGS) || \
    (TRACE_COLOR_START == TRACE_COLOR_OK)   || \
    (TRACE_COLOR_START == TRACE_COLOR_ERR)
  #error "All colours must be unique."
#endif

static CONSOLE_SCREEN_BUFFER_INFO console_info;
static HANDLE                     stdout_hnd;
static CRITICAL_SECTION           cs;
static int                        scope;

const char *_trace_file = NULL;
int         _trace_line  = -1;

#if defined(USE_TRACE) && (USE_TRACE == 1)  /* Rest of file */

static int trace_init (void)
{
  const char *env = getenv ("RTLSDR_TRACE");
  int   rc = 0;

  if (env)
  {
    InitializeCriticalSection (&cs);
    stdout_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
    GetConsoleScreenBufferInfo (stdout_hnd, &console_info);
    rc = atoi (env);
  }
  return (rc);
}

int trace_level (void)
{
  static int level = -1;

  if (level == -1)
     level = trace_init();
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

  EnterCriticalSection (&cs);
  set_color (col);

  if (col == TRACE_COLOR_START)
  {
    printf ("%*s%s(%u): ", 2*scope, "", _trace_file ? _trace_file : "<unknown file>" , _trace_line);
    LeaveCriticalSection (&cs);
    return;
  }

  va_start (args, fmt);
  vprintf (fmt, args);
  va_end (args);
  fflush (stdout);
  set_color (0);
  LeaveCriticalSection (&cs);
}

void trace_winusb (const char *func, DWORD win_err, const char *file, unsigned line)
{
  int level = trace_level();

  if (level <= 0)
     return;

  scope++;

  _trace_line = line;
  _trace_file = file;

  if (level >= 1 && win_err > 0)
  {
    trace_printf (TRACE_COLOR_START, NULL);
    trace_printf (TRACE_COLOR_ERR, "%s() failed with GetLastError() %lu.\n", func, win_err);
  }
  else if (level >= 2 && win_err == 0)
  {
    trace_printf (TRACE_COLOR_START, NULL);
    trace_printf (TRACE_COLOR_OK, "%s(), OK.\n", func);
  }
  scope--;
}
#endif /* USE_TRACE == 1 */
