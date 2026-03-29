/*
 * Generic debug and trace macro. Print to 'stdout' if 'trace_level'
 * is sufficiently high. And do it in colours.
 */
#include <stdio.h>
#include <string.h>

#include "trace.h"
#include "version.h"

static BOOL                       show_version = FALSE;
static BOOL                       show_winusb = FALSE;
static CONSOLE_SCREEN_BUFFER_INFO console_info;
static HANDLE                     console_hnd = INVALID_HANDLE_VALUE;
static FILE                      *console_file = NULL;
static CRITICAL_SECTION           console_cs;
static void set_color (unsigned short col);

static void trace_exit (void)
{
  set_color (0);
  console_hnd = INVALID_HANDLE_VALUE;
  console_file = NULL;
  DeleteCriticalSection (&console_cs);
}

static int trace_init (void)
{
  char *env = getenv ("RTLSDR_TRACE");
  char *winusb = env ? strstr (env, ",winusb") : NULL;
  int   rc = 0;

  InitializeCriticalSection (&console_cs);
  console_hnd = trace_file (FALSE);
  atexit (trace_exit);

  /* Supported syntax: "level[,winsub]"
   */
  if (env)
  {
    if (winusb >= env + 1)
    {
      show_winusb = TRUE;
      *winusb = '\0';
    }
    rc = atoi (env);
    if (rc > 0)
       show_version = TRUE;
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

HANDLE trace_file (BOOL use_stderr)
{
  console_file = (use_stderr ? stderr : stdout);
  console_hnd = GetStdHandle (use_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
  GetConsoleScreenBufferInfo (console_hnd, &console_info);
  return (console_hnd);
}

static void set_color (unsigned short col)
{
  fflush (console_file);

  if (col == 0)
       SetConsoleTextAttribute (console_hnd, console_info.wAttributes);
  else SetConsoleTextAttribute (console_hnd, (console_info.wAttributes & ~7) | col);
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

/**
 * Strip drive-letter, directory and suffix from a filename.
 */
static const char *basename (const char *fname)
{
  const char *base = fname;

  if (fname && *fname)
  {
    if (fname[0] && fname[1] == ':')
    {
      fname += 2;
      base = fname;
    }
    while (*fname)
    {
      if (*fname == '\\' || *fname == '/')
         base = fname + 1;
      fname++;
    }
  }
  return (base);
}

void trace_printf (const char *fname, unsigned line, const char *fmt, ...)
{
  va_list args;

  EnterCriticalSection (&console_cs);

  set_color (FOREGROUND_INTENSITY | 3);  /* bright cyan */
  fprintf (console_file, "%s(%u): ", basename(fname), line);

  if (show_version)  /* show the version-info on 1st call only */
  {
    fprintf (console_file, "Version \"%d.%d.%d.%d\". Compiled \"%s\". ",
             RTLSDR_MAJOR, RTLSDR_MINOR, RTLSDR_MICRO, RTLSDR_NANO, __DATE__);
    show_version = FALSE;
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

  vfprintf (console_file, fmt, args);
  va_end (args);
  set_color (0);
  LeaveCriticalSection (&console_cs);
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
