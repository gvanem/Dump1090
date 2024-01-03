/*
 * Generic debug and trace macro. Print to 'stdout' if 'trace_level'
 * is sufficiently high. And do it in colours.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "trace.h"
#include "version.h"

static int                        show_version = 0;
static int                        show_winusb = 0;
static CONSOLE_SCREEN_BUFFER_INFO console_info;
static HANDLE                     stdout_hnd;
static CRITICAL_SECTION           cs;

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
    if (rc > 0)
       show_version = 1;
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
  static char buf [512+20];
  char   err_buf [512], *p;

  if (err == ERROR_SUCCESS)
     strcpy (err_buf, "No error.");
  else
     if (!FormatMessageA (FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,
                          LANG_NEUTRAL, err_buf, sizeof(err_buf)-1, NULL))
        strcpy (err_buf, "Unknown error.");

  snprintf (buf, sizeof(buf), "%lu: %s", (u_long)err, err_buf);
  p = strchr (buf, '\0');
  if (p[-2] == '\r')
     p[-2] = '\0';
  return (buf);
}

void trace_printf (const char *file, unsigned line, const char *fmt, ...)
{
  va_list args;

  EnterCriticalSection (&cs);

  set_color (FOREGROUND_INTENSITY | 3);  /* bright cyan */
  printf ("%s(%u): ", file , line);
  if (show_version)
  {
    printf ("Version \"%d.%d.%d.%d\". Compiled \"%s\".\n", RTLSDR_MAJOR, RTLSDR_MINOR, RTLSDR_MICRO, RTLSDR_NANO, __DATE__);
    show_version = 0;
  }

  va_start (args, fmt);

  if (*fmt == '!')
  {
    set_color (FOREGROUND_INTENSITY | 4);  /* bright red */
    fmt++;
  }
  else if (*fmt == '|')
  {
    set_color (FOREGROUND_INTENSITY | 2);  /* bright green */
    fmt++;
  }
  else
    set_color (FOREGROUND_INTENSITY | 7);  /* bright white */

  vprintf (fmt, args);
  va_end (args);
  set_color (0);
  LeaveCriticalSection (&cs);
}

void trace_winusb (const char *file, unsigned line, const char *func, DWORD win_err)
{
  int level = trace_level();

  if (level <= 0)
     return;

  if (level >= 1 && win_err != ERROR_SUCCESS)
     trace_printf (file, line, "!%s() failed with %s\n", func, trace_strerror(win_err));

  else if ((level >= 2 || show_winusb) && win_err == ERROR_SUCCESS)
     trace_printf (file, line, "|%s(), OK.\n", func);
}
