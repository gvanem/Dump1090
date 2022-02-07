/*
 * Generic debug and trace macro. Print to 'stdout' if 'trace_level'
 * is sufficiently high. And do it in colours.
 */
#include <stdio.h>
#include <string.h>

#include "trace.h"
#include "version.h"

#if (TRACE_COLOR_START == TRACE_COLOR_ARGS) || \
    (TRACE_COLOR_START == TRACE_COLOR_OK)   || \
    (TRACE_COLOR_START == TRACE_COLOR_ERR)
  #error "All colours must be unique."
#endif

static int                        show_version = 1;
static int                        show_winusb = 0;
static CONSOLE_SCREEN_BUFFER_INFO console_info;
static HANDLE                     stdout_hnd;
static CRITICAL_SECTION           cs;

const char *_trace_file = NULL;
unsigned    _trace_line = 0;

static int trace_init (void)
{
  char *env = getenv ("RTLSDR_TRACE");
  char *winusb = env ? strstr(env, ",winusb") : NULL;
  int   rc = 0;

  InitializeCriticalSection (&cs);
  stdout_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
  GetConsoleScreenBufferInfo (stdout_hnd, &console_info);

  /* Supported syntax: "level[,winsub]"
   */
  if (env)
  {
    if (winusb >= env+1)
    {
      show_winusb = 1;
      *winusb = '\0';
    }
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
  fflush (stdout);

  if (col == 0)
       SetConsoleTextAttribute (stdout_hnd, console_info.wAttributes);
  else SetConsoleTextAttribute (stdout_hnd, (console_info.wAttributes & ~7) | col);
}

/**
 * Return err-number and string for 'err'.
 * Only use this with `GetLastError()`.
 */
const char *trace_strerror (DWORD err)
{
  static  char buf [512+20];
  char    err_buf [512], *p;

  if (err == ERROR_SUCCESS)
     strcpy (err_buf, "No error");
  else
  if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,
                      LANG_NEUTRAL, err_buf, sizeof(err_buf)-1, NULL))
     strcpy (err_buf, "Unknown error");

  snprintf (buf, sizeof(buf), "%lu: %s", (u_long)err, err_buf);
  p = strchr (buf, '\0');
  if (p[-2] == '\r')
     p[-2] = '\0';
  return (buf);
}

void trace_printf (unsigned short col, const char *fmt, ...)
{
  va_list args;

  EnterCriticalSection (&cs);
  set_color (col);

  if (col == TRACE_COLOR_START)
  {
    printf ("%s(%u): ", _trace_file ? _trace_file : "<unknown file>" , _trace_line);
    if (show_version)
    {
      printf ("Version %d.%d.%d. Compiled: \"%s\".\n", RTLSDR_MAJOR, RTLSDR_MINOR, RTLSDR_MICRO, __DATE__);
      show_version = 0;
    }
    LeaveCriticalSection (&cs);
    return;
  }

  va_start (args, fmt);
  vprintf (fmt, args);
  va_end (args);
  set_color (0);
  LeaveCriticalSection (&cs);
}

void trace_winusb (const char *func, DWORD win_err, const char *file, unsigned line)
{
  int level = trace_level();

  if (level <= 0)
     return;

  _trace_line = line;
  _trace_file = file;

  if (level >= 1 && win_err != ERROR_SUCCESS)
  {
    trace_printf (TRACE_COLOR_START, NULL);
    trace_printf (TRACE_COLOR_ERR, "%s() failed with %s.\n", func, trace_strerror(win_err));
  }
  else if ((level >= 2 || show_winusb) && win_err == ERROR_SUCCESS)
  {
    trace_printf (TRACE_COLOR_START, NULL);
    trace_printf (TRACE_COLOR_OK, "%s(), OK.\n", func);
  }
}
