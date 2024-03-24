/**\file    misc.c
 * \ingroup Misc
 * \brief   Various support functions.
 */
#include <stdint.h>
#include <sys/utime.h>
#include <inttypes.h>
#include <winsock2.h>
#include <windows.h>
#include <wininet.h>

#undef MOUSE_MOVED
#include <curses.h>

#if defined(USE_ASAN)
#include <sanitizer/asan_interface.h>
#endif

#if defined(USE_UBSAN)
#include <sanitizer/ubsan_interface.h>
#endif

#include "aircraft.h"
#include "sqlite3.h"
#include "trace.h"
#include "misc.h"
#include "rtl-sdr/version.h"

#define TSIZE (int)(sizeof("HH:MM:SS.MMM: ") - 1)

static bool modeS_log_reinit (const SYSTEMTIME *st);
static bool modeS_log_ignore (const char *msg);

/**
 * Log a message to the `Modes.log` file with a timestamp.
 * But no timestamp if `buf` starts with a `!`.
 */
void modeS_log (const char *buf)
{
  const char *time = NULL;
  char  day_change [20] = "";

  if (!Modes.log)
     return;

  if (*buf == '!')
     buf++;
  else
  {
    static WORD day = (WORD)-1;
    SYSTEMTIME  now;

    if (modeS_log_ignore(buf))
       return;

    assert (Modes.start_SYSTEMTIME.wYear);

    GetLocalTime (&now);
    time = modeS_SYSTEMTIME_to_str (&now, false);

    if (day == (WORD)-1)
       day = Modes.start_SYSTEMTIME.wDay;

    if (Modes.logfile_daily && now.wDay != day)
    {
      day = now.wDay;
      if (!modeS_log_reinit(&now))
         return;
    }
    else if (now.wDay != day)      /* show the date once per day */
    {
      snprintf (day_change, sizeof(day_change), "%02u/%02u/%02u:\n", now.wYear, now.wMonth, now.wDay);
      day = now.wDay;
    }
  }

  if (*buf == '\n')
     buf++;

  if (time)
       fprintf (Modes.log, "%s%s: %s", day_change, time, buf);
  else fprintf (Modes.log, "%*.*s%s", TSIZE, TSIZE, "", buf);
}

/**
 * A small log-buffer to catch errors from `externals/mongoose.c`.
 *
 * e.g. a `bind()` error is impossible to catch in the network event-handler.
 * Use this to look for "bind: 10048" == `WSAEADDRINUSE` etc.
 */
static char _err_buf [200];
static int  _err_idx = 0;

void modeS_err_set (bool on)
{
  if (on)
       _err_idx = 0;
  else _err_idx = -1;
}

char *modeS_err_get (void)
{
  return (_err_buf);
}

/**
 * Print a character `c` to `Modes.log` or `stdout`.
 * Used only if `(Modes.debug & DEBUG_MONGOOSE)` is enabled by `--debug m`.
 */
void modeS_logc (char c, void *param)
{
  /* Since everything gets written in text-mode, we do not
   * need an extra '\r'. We want plain '\n' line-endings.
   */
  if (c != '\r')
  {
    if (param)
         fputc (c, param);      /* to 'stderr' */
    else fputc (c, Modes.log ? Modes.log : stdout);

    if (_err_idx >= 0 && _err_idx < (int)sizeof(_err_buf)-2)
    {
      _err_buf [_err_idx++] = c;
      _err_buf [_err_idx] = '\0';
    }
  }
}

/**
 * Print to `f` and optionally to `Modes.log`.
 */
void modeS_flogf (FILE *f, _Printf_format_string_ const char *fmt, ...)
{
  char    buf [1000];
  char   *p = buf;
  va_list args;

  va_start (args, fmt);
  vsnprintf (buf, sizeof(buf), fmt, args);
  va_end (args);

  if (f && f != Modes.log) /* to `stdout` or `stderr` */
  {
    if (*p == '!')
       p++;
    fputs (p, f);
    fflush (f);
  }
  if (Modes.log)
     modeS_log (buf);
}

/**
 * Disable, then enable Mongoose logging based on the `Modes.debug` bits.
 */
void modeS_log_set (void)
{
  mg_log_set (0);   /* By default, disable all logging from Mongoose */

  if (Modes.debug & DEBUG_MONGOOSE)
  {
    mg_log_set_fn (modeS_logc, NULL);
    mg_log_set (MG_LL_DEBUG);
  }
  else if (Modes.debug & DEBUG_MONGOOSE2)
  {
    mg_log_set_fn (modeS_logc, NULL);
    mg_log_set (MG_LL_VERBOSE);
  }
}

/**
 * Open the initial or a new .log-file based on `Modes.logfile_daily`.
 *
 * If `st == NULL`, open `Modes.logfile_current` for the 1st time.
 * Otherwise set the `Modes.logfile_current` filename with a
 * `"YYYY-MM-DD.log"` suffix.
 */
static bool modeS_log_reinit (const SYSTEMTIME *st)
{
  static const char *dot = NULL;
  bool   initial = (st == NULL);

  if (initial)
  {
    SYSTEMTIME now;

    GetLocalTime (&now);
    st = &now;
    if (Modes.logfile_daily)
    {
      dot = strrchr (Modes.logfile_initial, '.');
      if (!dot || stricmp(dot, ".log"))
      {
        LOG_STDERR ("Unexpected log-file name \"%s\"\n"
                    "Disabling daily logfiles.\n", Modes.logfile_initial);
        Modes.logfile_daily = false;
      }
    }
  }

  /* Force a name-change from 'x.log' to 'x-YYYY-MM-DD.log'?
   */
  if (Modes.logfile_daily)
       snprintf (Modes.logfile_current, sizeof(Modes.logfile_current),
                 "%.*s-%04d-%02d-%02d.log",
                 (int) (dot - Modes.logfile_initial), Modes.logfile_initial,
                 st->wYear, st->wMonth, st->wDay);

  else strcpy_s (Modes.logfile_current, sizeof(Modes.logfile_current),
                 Modes.logfile_initial);

  if (Modes.log)
     fclose (Modes.log);

  Modes.log = fopen_excl (Modes.logfile_current, "at");
  return (Modes.log != NULL);
}

/**
 * Open the initial .log-file based on `Modes.logfile_daily`.
 */
bool modeS_log_init (void)
{
  bool rc = modeS_log_reinit (NULL);

  if (!rc)
     LOG_STDERR ("Failed to create/append to \"%s\" (%s). Continuing anyway.\n",
                 Modes.logfile_current, strerror(errno));
  return (rc);
}

/**
 * Close the current .log-file and free the `Modes.logfile_ignore` list.
 */
void modeS_log_exit (void)
{
  log_ignore *i_next;
  log_ignore *i = Modes.logfile_ignore;

  if (Modes.log)
  {
    if (!Modes.home_pos_ok)
       LOG_FILEONLY ("A valid home-position was not used.\n");
    fclose (Modes.log);
    Modes.log = NULL;
  }

  for (i = Modes.logfile_ignore; i; i = i_next)
  {
    i_next = i->next;
    LIST_DELETE (log_ignore, &Modes.logfile_ignore, i);
    free (i);
  }
}

/**
 * Add a `*msg` to the `Modes.logfile_ignore` list. <br>
 * Also remove quotes, trailing `#` and leading / trailing spaces.
 */
bool modeS_log_add_ignore (const char *msg)
{
  log_ignore *ignore = *msg ? calloc (sizeof(*ignore), 1) : NULL;
  size_t      i;

  if (ignore)
  {
    char *p = ignore->msg + 0;

    for (i = 0; i < sizeof(ignore->msg) - 1; i++, msg++)
        if (*msg != '"')
           *p++ = *msg;

    *p = '\0';
    p = strrchr (ignore->msg, '#');
    if (p)
       *p = '\0';

    str_trim (ignore->msg);
    LIST_ADD_TAIL (log_ignore, &Modes.logfile_ignore, ignore);
  }

  for (i = 0, ignore = Modes.logfile_ignore; ignore; ignore = ignore->next, i++)
      DEBUG (DEBUG_GENERAL, "%zd: '%s'\n", i, ignore->msg);
  return (true);
}

/**
 * Check for a message `msg` to be ignored.
 */
static bool modeS_log_ignore (const char *msg)
{
  const log_ignore *ignore;

  for (ignore = Modes.logfile_ignore; ignore; ignore = ignore->next)
      if (str_startswith(msg, ignore->msg))
         return (true);
  return (false);
}

/**
 * Format a `SYSTEMTIME` stucture into string. <br>
 * Optionally show Year/Month/Day-of-Mounth first.
 */
static char *modeS_strftime (const SYSTEMTIME *st, bool show_YMD)
{
  static char tbuf [50];
  size_t left = sizeof(tbuf);
  char  *ptr = tbuf;

  if (show_YMD)
  {
    int len = snprintf (ptr, left, "%02u/%02u/%02u, ",
                        st->wYear, st->wMonth, st->wDay);
    ptr  += len;
    left -= len;
  }
  snprintf (ptr, left, "%02u:%02u:%02u.%03u",
            st->wHour, st->wMinute, st->wSecond, st->wMilliseconds);
  return (tbuf);
}

char *modeS_SYSTEMTIME_to_str (const SYSTEMTIME *st, bool show_YMD)
{
  return modeS_strftime (st, show_YMD);
}

char *modeS_FILETIME_to_str (const FILETIME *ft, bool show_YMD)
{
  SYSTEMTIME st;

  FileTimeToSystemTime (ft, &st);
  return modeS_strftime (&st, show_YMD);
}

char *modeS_FILETIME_to_loc_str (const FILETIME *ft, bool show_YMD)
{
  static TIME_ZONE_INFORMATION tz_info;
  static LONG  timezone = 0;   /* minutes */
  static bool  done = false;
  ULONGLONG    ul = *(ULONGLONG*) ft;

  if (!done)
  {
    FILETIME ft2;
    DWORD    rc = GetTimeZoneInformation (&tz_info);
    LONG     dst_adjust = 0;

    if (rc == TIME_ZONE_ID_UNKNOWN || rc == TIME_ZONE_ID_STANDARD || rc == TIME_ZONE_ID_DAYLIGHT)
    {
      timezone   = tz_info.Bias + tz_info.StandardBias;
      dst_adjust = tz_info.StandardBias - tz_info.DaylightBias;
    }
    done = true;

    get_FILETIME_now (&ft2);
    DEBUG (DEBUG_GENERAL, "rc: %ld, timezone: %ld min, dst_adjust: %ld min, now: %s\n",
           rc, timezone, dst_adjust, modeS_FILETIME_to_loc_str(&ft2, true));
  }

  /* From minutes to 100 nsec units
   */
  ul -= 600000000ULL * (ULONGLONG) timezone;
  return modeS_FILETIME_to_str ((const FILETIME*)&ul, show_YMD);
}

/**
 * Convert a "frequency string" with standard suffixes (k, M, G)
 * to a `uint32_t` value.
 *
 * \param in  Hertz   a string to be parsed
 * \retval    the frequency as a `uint32_t` (max ~4.3 GHz)
 * \note      Taken from Osmo-SDR's `convenience.c` and modified.
 */
uint32_t ato_hertz (const char *Hertz)
{
  char     tmp [20], *end, last_ch;
  int      len;
  double   multiplier = 1.0;
  uint32_t ret;

  strcpy_s (tmp, sizeof(tmp), Hertz);
  len = strlen (tmp);
  last_ch = tmp [len-1];
  tmp [len-1] = '\0';

  switch (last_ch)
  {
    case 'g':
    case 'G':
          multiplier = 1E9;
          break;
    case 'm':
    case 'M':
          multiplier = 1E6;
          break;
    case 'k':
    case 'K':
          multiplier = 1E3;
          break;
  }
  ret = (uint32_t) strtof (tmp, &end);
  if (end == tmp || *end != '\0')
     return (0);
  return (uint32_t) (multiplier * ret);
}

/**
 * Turn an hex digit into its 4 bit decimal value.
 * Returns -1 if the digit is not in the 0-F range.
 */
int hex_digit_val (int c)
{
  c = tolower (c);
  if (c >= '0' && c <= '9')
     return (c - '0');
  if (c >= 'a' && c <= 'f')
     return (c - 'a' + 10);
  return (-1);
}

/*
 * Check for `[\x80 ... \x9f]` escaped values.
 *
 * Based on `mg_json_unescape()`.
 */
static bool unescape (const char *from, size_t from_len, char *to, size_t to_len)
{
  static const char hex_chars[] = "0123456789abcdef";
  size_t            from_idx, to_idx;

  for (from_idx = 0, to_idx = 0; from_idx < from_len && to_idx < to_len; from_idx++)
  {
    if (from [from_idx] == '\\' && from [from_idx+1] == 'x' && from_idx + 3 < from_len)
    {
      int b2 = tolower (from [from_idx+2]);
      int b3 = tolower (from [from_idx+3]);
      int val;

      if (b2 < '8' || b2 > '9' || b3 < '0' || b3 > 'f')   /* Give up */
         return (false);

      val = (b2 - '0') << 4;
      if (b3 <= '9')
           val += (b3 - '0');
      else val += (b3 - 'a') + 10;

      ((BYTE*)to) [to_idx++] = val;
      from_idx += 3;
    }
    else
    {
      to [to_idx++] = from [from_idx];
    }
  }

  if (to_idx >= to_len)
     return (false);

  if (to_len > 0)
     to [to_idx] = '\0';
  return (true);
}

/**
 * Decode any `\\x80 ... \\x9f` sequence in `value`.
 */
const char *unescape_hex (const char *value)
{
  static char buf [100];

  if (unescape(value, strlen(value), buf, sizeof(buf)))
     return (buf);
  return (value);
}

/**
 * Return true if string `s1` starts with `s2`.
 *
 * Ignore casing of both strings.
 */
bool str_startswith (const char *s1, const char *s2)
{
  size_t s1_len, s2_len;

  s1_len = strlen (s1);
  s2_len = strlen (s2);

  if (s2_len > s1_len)
     return (false);

  if (!strnicmp (s1, s2, s2_len))
     return (true);
  return (false);
}

/**
 * Return true if string `s1` ends with `s2`.
 */
bool str_endswith (const char *s1, const char *s2)
{
  const char *s1_end, *s2_end;

  if (strlen(s2) > strlen(s1))
     return (false);

  s1_end = strchr (s1, '\0') - 1;
  s2_end = strchr (s2, '\0') - 1;

  while (s2_end >= s2)
  {
    if (*s1_end != *s2_end)
       break;
    s1_end--;
    s2_end--;
  }
  return (s2_end == s2 - 1);
}

/**
 * Trim leading blanks (space/tab) from a string.
 */
char *str_ltrim (char *s)
{
  assert (s != NULL);

  while (s[0] && s[1] && isspace ((int)s[0]))
       s++;
  return (s);
}

/**
 * Trim trailing blanks (space/tab) from a string.
 */
char *str_rtrim (char *s)
{
  size_t n;

  assert (s != NULL);
  n = strlen (s);
  if (n == 0)
     return (s);

  n--;
  while (n)
  {
    int ch = (int)s [n];
    if (!isspace(ch))
       break;
    s [n--] = '\0';
  }

  return (s);
}

/**
 * Trim leading and trailing blanks (space/tab) from a string.
 */
char *str_trim (char *s)
{
  return str_rtrim (str_ltrim(s));
}

/**
 * Create a joined string from an array of strings.
 *
 * \param[in] array  the array of strings to join and return as a single string.
 * \param[in] sep    the separator between the `array` elements; after the first up-to the 2nd last
 *
 * \retval NULL  if `array` is empty
 * \retval !NULL a `malloc()`-ed string of the concatinated result.
 */
char *str_join (char *const *array, const char *sep)
{
  char  *p,  *ret = NULL;
  int    i, num;
  size_t sz = 0;

  if (!array || !array[0])
     return (NULL);

  /* Get the needed size for `ret`
   */
  for (i = num = 0; array[i]; i++, num++)
      sz += strlen (array[i]) + strlen (sep);

  sz++;
  sz -= strlen (sep);      /* No `sep` after last `array[]` */
  p = ret = malloc (sz);
  if (!p)
     return (NULL);

  for (i = 0; i < num; i++)
  {
    strcpy (p, array[i]);
    p = strchr (p, '\0');
    if (i < num - 1)
       strcpy (p, sep);
    p = strchr (p, '\0');
  }
  return (ret);
}

/**
 * A `strtok_r()` similar function.
 *
 * \param[in,out] ptr  on first call, the string to break apart looking for `sep` strings.
 * \param[in]     sep  the separator string to look for.
 * \param[in]     end  the pointer to the end. Ignored on 1st call.
 */
char *str_tokenize (char *ptr, const char *sep, char **end)
{
  if (!ptr)
     ptr = *end;

  while (*ptr && strchr(sep, *ptr))
    ++ptr;

  if (*ptr)
  {
    char *start = ptr;

    *end = start + 1;

    /* scan through the string to find where it ends, it ends on a
     * null byte or a character that exists in the separator string.
     */
    while (**end && !strchr(sep, **end))
      ++*end;

    if (**end)
    {
      **end = '\0';  /* zero terminate it! */
      ++*end;        /* advance the last pointer to beyond the null byte */
    }
    return (start);  /* return the position where the string starts */
  }
  return (NULL);
}

/**
 * From `man strsep`:
 *   Locate the first occurrence of any character in the string `delim`
 *   and replace it with a '\0'. The location of the next character after
 *   the `delim` character (or NULL, if the end of the string was reached)
 *   is stored in `*stringp`. The original value of `*stringp` is returned.
 */
char *str_sep (char **stringp, const char *delim)
{
  char       *s, *tok;
  const char *span;
  int         c, sc;

  s = *stringp;
  if (!s)
     return (NULL);

  for (tok = s; ;)
  {
    c = *s++;
    span = delim;
    do
    {
      sc = *span++;
      if (sc == c)
      {
        if (c == 0)
             s = NULL;
        else s [-1] = 0;
        *stringp = s;
        return (tok);
      }
    } while (sc != 0);
  }
}

/**
 * Strip drive-letter, directory and suffix from a filename.
 */
char *basename (const char *fname)
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
      if (IS_SLASH(*fname))
         base = fname + 1;
      fname++;
    }
  }
  return (char*) base;
}

/**
 * Return the directory part of a filename.
 * A static buffer is returned so make a copy of this ASAP.
 */
char *dirname (const char *fname)
{
  const char *p = fname;
  const char *slash = NULL;
  size_t      dirlen;
  static mg_file_path dir;

  if (!fname)
     return (NULL);

  if (fname[0] && fname[1] == ':')
  {
    slash = fname + 1;
    p += 2;
  }

  /* Find the rightmost slash.
   */
  while (*p)
  {
    if (IS_SLASH(*p))
       slash = p;
    p++;
  }

  if (slash == NULL)
  {
    fname = ".";
    dirlen = 1;
  }
  else
  {
    /* Remove any trailing slashes.
     */
    while (slash > fname && (IS_SLASH(slash[-1])))
        slash--;

    /* How long is the directory we will return?
     */
    dirlen = slash - fname + (slash == fname || slash[-1] == ':');
    if (*slash == ':' && dirlen == 1)
       dirlen += 2;
  }
  strncpy (dir, fname, dirlen);

  if (slash && *slash == ':' && dirlen == 3)
     dir[2] = '.';      /* for "x:foo" return "x:." */

  if (IS_SLASH(dir[dirlen-1]))
     dirlen--;

  dir [dirlen] = '\0';
  return (dir);
}

/**
 * Return a filename on Unix form:
 * All `\\` characters replaced with `/`.
 */
char *slashify (char *fname)
{
  char *p = fname;

  while (*p)
  {
    if (*p == '\\')
      *p = '/';
    p++;
  }
  return (fname);
}

/**
 * Add or initialize a test `which` to the test-list at `*spec`.
 */
bool test_add (char **spec, const char *which)
{
  char *s;

  assert (spec);
  assert (which);

  if (!*spec)
       s = mg_mprintf ("%s", which);
  else s = mg_mprintf ("%s,%s", *spec, which);

  free (*spec);
  *spec = s;
  return (s ? true : false);
}

/**
 * Check if a test `which` is in the test-list at `spec`.
 */
bool test_contains (const char *spec, const char *which)
{
  char *p, *end, spec2 [100];

  assert (which);

  if (!spec)             /* no test-spec disables all */
     return (false);

  if (!strcmp(spec, "*"))
     return (true);      /* a '*' test-spec enables all */

  strncpy (spec2, spec, sizeof(spec2));
  for (p = str_tokenize(spec2, ",", &end); p; p = str_tokenize(NULL, ",", &end))
  {
    if (!stricmp(which, p))
       return (true);
  }
  return (false);
}

/**
 * Touch a file to current time.
 */
int touch_file (const char *file)
{
  return _utime (file, NULL);
}

/**
 * Open an existing file (or create) in share-mode but deny other
 * processes to write to the file.
 */
FILE *fopen_excl (const char *file, const char *mode)
{
  int fd, open_flags, share_flags;

  switch (*mode)
  {
    case 'r':
          open_flags  = _O_RDONLY;
          share_flags = S_IREAD;
          break;
    case 'w':
          open_flags  = _O_WRONLY;
          share_flags = S_IWRITE;
          break;
    case 'a':
          open_flags  = _O_CREAT | _O_WRONLY | _O_APPEND;
          share_flags = S_IWRITE;
          break;
    default:
          return (NULL);
  }

  if (mode[1] == '+')
     open_flags |= _O_CREAT | _O_TRUNC;

  if (mode[strlen(mode)-1] == 'b')
     open_flags |= O_BINARY;

  fd = _sopen (file, open_flags | _O_SEQUENTIAL, SH_DENYWR, share_flags);
  if (fd <= -1)
     return (NULL);
  return _fdopen (fd, mode);
}

#if MG_ENABLE_FILE
/*
 * Internals of 'externals/mongoose.c':
 */
typedef struct dirent {
        mg_file_path d_name;
      } dirent;

typedef struct win32_dir {
        HANDLE           handle;
        WIN32_FIND_DATAW info;
        dirent           result;
      } DIR;

extern DIR    *opendir (const char *name);
extern int     closedir (DIR *d);
extern dirent *readdir (DIR *d);

/**
 * Touch all files in a directory to current time.
 * Works on all sub-directories if `recurse == true`.
 */
int touch_dir (const char *directory, bool recurse)
{
  dirent *d;
  DIR    *dir = opendir (directory);
  int     rc = 0;

  if (!dir)
  {
    DEBUG (DEBUG_GENERAL, "GetLastError(): %lu\n", GetLastError());
    return (0);
  }

  while ((d = readdir(dir)) != NULL)
  {
    mg_file_path full_name;
    DWORD        attrs;
    bool         is_dir;

    if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
       continue;

    snprintf (full_name, sizeof(full_name), "%s\\%s", directory, d->d_name);
    attrs = GetFileAttributesA (full_name);
    is_dir = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));

    if (!is_dir && !recurse)
       continue;

    if (is_dir)
         rc += touch_dir (full_name, true);
    else rc += touch_file (full_name);
  }
  closedir (dir);
  return (rc);
}
#endif /* MG_ENABLE_FILE */

/**
 * \def DELTA_EPOCH_IN_USEC
 *
 * Number of micro-seconds between the beginning of the Windows epoch
 * (Jan. 1, 1601) and the Unix epoch (Jan. 1, 1970).
 */
#define DELTA_EPOCH_IN_USEC  11644473600000000Ui64

static uint64_t FILETIME_to_unix_epoch (const FILETIME *ft)
{
  uint64_t res = (uint64_t) ft->dwHighDateTime << 32;

  res += ft->dwLowDateTime;
  res /= 10;                   /* from 100 nano-sec periods to usec */
  res -= DELTA_EPOCH_IN_USEC;  /* from Win epoch to Unix epoch */
  return (res);
}

int _gettimeofday (struct timeval *tv, void *timezone)
{
  FILETIME ft;
  uint64_t tim;

  GetSystemTimePreciseAsFileTime (&ft);
  tim = FILETIME_to_unix_epoch (&ft);
  tv->tv_sec  = (long) (tim / 1000000L);
  tv->tv_usec = (long) (tim % 1000000L);
  (void) timezone;
  return (0);
}

int get_timespec_UTC (struct timespec *ts)
{
  return timespec_get (ts, TIME_UTC);
}

/**
 * Returns a `FILETIME *ft`.
 *
 * From: https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsystemtimepreciseasfiletime
 *   retrieves the current system date and time with the highest possible
 *   level of precision (<1us). The retrieved information is in
 *   Coordinated Universal Time (UTC) format.
 */
void get_FILETIME_now (FILETIME *ft)
{
  GetSystemTimePreciseAsFileTime (ft);
}

/**
 * Return micro-second time-stamp as a double.
 *
 * \note QueryPerformanceFrequency() is not related to the RDTSC instruction
 *       since it works poorly when power "management technologies" does it's
 *       tricks.
 * \ref  https://learn.microsoft.com/en-us/windows/win32/dxtecharts/game-timing-and-multicore-processors
 *       https://yakvi.github.io/handmade-hero-notes/html/day10.html
 */
double get_usec_now (void)
{
  static uint64_t frequency = 0ULL;
  LARGE_INTEGER   ticks;
  double          usec;

  if (frequency == 0ULL)
  {
    QueryPerformanceFrequency ((LARGE_INTEGER*)&frequency);
    DEBUG (DEBUG_GENERAL, "QueryPerformanceFrequency(): %.3f MHz\n", (double)frequency / 1E6);
  }
  QueryPerformanceCounter (&ticks);
  usec = 1E6 * ((double)ticks.QuadPart / (double)frequency);
  return (usec);
}

/**
 * Initialize the above timing-values.
 */
void init_timings (void)
{
  get_usec_now();
  get_FILETIME_now (&Modes.start_FILETIME);
  modeS_FILETIME_to_loc_str (&Modes.start_FILETIME, true);
  GetLocalTime (&Modes.start_SYSTEMTIME);
}

/**
 * Use 64-bit tick-time for Mongoose?
 */
#if MG_ENABLE_CUSTOM_MILLIS
uint64_t mg_millis (void)
{
  return MSEC_TIME();
}
#endif

#if defined(_DEBUG)
/**
 * Check for memory-leaks in `_DEBUG` mode.
 */
static _CrtMemState start_state;

void crtdbug_exit (void)
{
  _CrtMemState end_state, diff_state;

  _CrtMemCheckpoint (&end_state);
  if (!_CrtMemDifference(&diff_state, &start_state, &end_state))
     LOG_STDERR ("No mem-leaks detected.\n");
  else
  {
    _CrtCheckMemory();
    _CrtSetDbgFlag (0);
    _CrtDumpMemoryLeaks();
  }
}

void crtdbug_init (void)
{
  _HFILE file  = _CRTDBG_FILE_STDERR;
  int    mode  = _CRTDBG_MODE_FILE;
  int    flags = _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF | _CRTDBG_DELAY_FREE_MEM_DF;

  _CrtSetReportFile (_CRT_ASSERT, file);
  _CrtSetReportMode (_CRT_ASSERT, mode);
  _CrtSetReportFile (_CRT_ERROR, file);
  _CrtSetReportMode (_CRT_ERROR, mode);
  _CrtSetReportFile (_CRT_WARN, file);
  _CrtSetReportMode (_CRT_WARN, mode);
  _CrtSetDbgFlag (flags | _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG));
  _CrtMemCheckpoint (&start_state);
}

#elif defined(USE_MIMALLOC)
/**
 * Setting for the `mimalloc` code
 */
void mimalloc_init (void)
{
  mi_option_enable (mi_option_show_errors);

  if (Modes.debug & DEBUG_GENERAL)
  {
    mi_option_enable (mi_option_show_stats);
    mi_option_enable (mi_option_destroy_on_exit);
    mi_option_enable (mi_option_verbose);
  }
}

void mimalloc_exit (void)
{
}

static void mi_cdecl stats_cb (const char *msg, void *arg)
{
  FILE *f = arg;

  modeS_flogf (f, "!%s", msg);
}

void mimalloc_stats (void)
{
  if (Modes.log && mi_option_is_enabled(mi_option_show_stats))
  {
    modeS_flogf (Modes.log, "MIMALLOC stats:\n");
    mi_stats_print_out (stats_cb, Modes.log);
  }
}

static const char *mimalloc_version (void)
{
  static char buf [30];
  int    ver = mi_version();

  snprintf (buf, sizeof(buf), "mimalloc ver: %d.%d\n", ver / 100, ver % 100);
  return (buf);
}
#endif  /* _DEBUG */

/* A dummy for `show_version_info()`
 */
#if !defined(USE_MIMALLOC)
#define mimalloc_version() ""
#endif

/**
 * Return err-number and string for 'err'.
 */
const char *win_strerror (DWORD err)
{
  static  char buf [512+20];
  char    err_buf [512], *p;
  HRESULT hr = 0;

  if (HRESULT_SEVERITY(err))
     hr = err;

  if (err == ERROR_SUCCESS)
     strcpy (err_buf, "No error");
  else if (err == ERROR_BAD_EXE_FORMAT)
     strcpy (err_buf, "Bad EXE format");
  else if (!FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,
                           LANG_NEUTRAL, err_buf, sizeof(err_buf)-1, NULL))
     strcpy (err_buf, "Unknown error");

  if (hr != 0)
       snprintf (buf, sizeof(buf), "0x%08lX: %s", (u_long)hr, err_buf);
  else snprintf (buf, sizeof(buf), "%lu: %s", (u_long)err, err_buf);

  p = strrchr (buf, '\r');
  if (p)
     *p = '\0';

  p = strrchr (buf, '.');
  if (p && p[1] == '\0')
     *p = '\0';
  return (buf);
}

/**
 * Return a string describing an error-code from RTLSDR.
 *
 * `rtlsdr_last_error()` always returns a positive value for WinUSB errors.
 *
 * While RTLSDR errors returned from all `rtlsdr_x()` functions are negative.
 * And rather sparse:
 *   \li -1 if device handle is invalid
 *   \li -2 if EEPROM size is exceeded (depends on rtlsdr_x() function)
 *   \li -3 if no EEPROM was found     (depends on rtlsdr_x() function)
 */
const char *get_rtlsdr_error (void)
{
  uint32_t err = rtlsdr_last_error();

  if (err == 0)
     return ("No error.");
  return trace_strerror (err);
}

/**
 * Return a random integer in range `[a..b]`. \n
 * Ref: http://stackoverflow.com/questions/2509679/how-to-generate-a-random-number-from-within-a-range
 */
uint32_t random_range (uint32_t min, uint32_t max)
{
  static bool done = false;
  double scaled;

  if (!done)
  {
    srand (time(NULL));
    done = true;
  }
  scaled = (double) rand() / RAND_MAX;
  return (uint32_t) ((max - min + 1) * scaled) + min;
}

int32_t random_range2 (int32_t min, int32_t max)
{
  double scaled = (double) rand() / RAND_MAX;
  return (int32_t) ((max - min + 1) * scaled) + min;
}

/**
 * Return nicely formatted string `"xx,xxx,xxx"`
 * with thousand separators (left adjusted).
 *
 * Use 8 buffers in round-robin.
 */
const char *qword_str (uint64_t val)
{
  static char buf [8][30];
  static int  idx = 0;
  char   tmp[30];
  char  *rc = buf [idx++];

  if (val < 1000ULL)
  {
    sprintf (rc, "%lu", (u_long)val);
  }
  else if (val < 1000000ULL)       /* < 1E6 */
  {
    sprintf (rc, "%lu,%03lu", (u_long)(val/1000UL), (u_long)(val % 1000UL));
  }
  else if (val < 1000000000ULL)    /* < 1E9 */
  {
    sprintf (tmp, "%9" PRIu64, val);
    sprintf (rc, "%.3s,%.3s,%.3s", tmp, tmp+3, tmp+6);
  }
  else if (val < 1000000000000ULL) /* < 1E12 */
  {
    sprintf (tmp, "%12" PRIu64, val);
    sprintf (rc, "%.3s,%.3s,%.3s,%.3s", tmp, tmp+3, tmp+6, tmp+9);
  }
  else                                      /* >= 1E12 */
  {
    sprintf (tmp, "%15" PRIu64, val);
    sprintf (rc, "%.3s,%.3s,%.3s,%.3s,%.3s", tmp, tmp+3, tmp+6, tmp+9, tmp+12);
  }
  idx &= 7;
  return str_ltrim (rc);
}

const char *dword_str (DWORD val)
{
  return qword_str ((uint64_t)val);
}

void *memdup (const void *from, size_t size)
{
  void *ret = malloc (size);

  if (ret)
     return memcpy (ret, from, size);
  return (NULL);
}

/**
 * Print some details about the Sqlite3 package.
 */
static void print_sql_info (void)
{
  const char *opt;
  int   i, sz = 0;

  printf ("Sqlite3 ver:  %s. Build options: ", sqlite3_libversion());
  for (i = 0; (opt = sqlite3_compileoption_get(i)) != NULL; i++)
  {
    const char *opt_next = sqlite3_compileoption_get (i+1);

    sz += printf ("SQLITE_%s%s", opt, opt_next ? ", " : "\n");
    if (opt_next)
       sz += sizeof(", SQLITE_") + strlen (opt_next);
    if (sz >= 140)
    {
      fputs ("\n              ", stdout);
      sz = 0;
    }
  }
}

static void print_packed_web_info (void)
{
#if defined(USE_PACKED_DLL)
  /**
   * \todo
   * Iterate over resources in `web-page.dll` and print
   * all `mg_packed_spec_x()` descriptions.
   */
#endif
}

/**
 * Return the compiler info the program was built with.
 */
static const char *compiler_info (void)
{
  static char buf [50];

#if defined(__clang__)
  snprintf (buf, sizeof(buf), "clang-cl %d.%d.%d",
            __clang_major__, __clang_minor__, __clang_patchlevel__);

#elif defined(_MSC_FULL_VER)
  snprintf (buf, sizeof(buf), "Microsoft cl %d.%d.%d",
            _MSC_VER / 100, _MSC_VER % 100, _MSC_FULL_VER % 100000);

#else
  snprintf (buf, sizeof(buf), "Microsoft cl %d.%d",
            (_MSC_VER / 100), _MSC_VER % 100);
#endif
  return (buf);
}

#if defined(MG_ENABLE_SELECT)
  #define NETPOLLER  "select()"
#elif defined(MG_ENABLE_EPOLL)
  #define NETPOLLER  "epoll"
#elif defined(MG_ENABLE_POLL)
  #define NETPOLLER "WSAPoll()"
#else
  #error "Cannot define 'NETPOLLER'?!"
#endif

static const char *build_features (void)
{
  static char        buf [150];
  static const char *features[] = {
  #if defined(_DEBUG)
    "debug",
  #else
    "release",
  #endif
  #if defined(_WIN64)
    "x64",
  #else
    "x86",
  #endif
  #if defined(USE_ASAN)
    "ASAN",
  #endif
  #if defined(USE_UBSAN)
    "UBSAN",
  #endif
  #if defined(USE_MIMALLOC)
    "MIMALLOC",
  #endif
  #if defined(USE_GEN_ROUTES)
    "GEN_ROUTES",
  #endif
  #if defined(USE_PACKED_DLL)
    "Packed-Web",
  #endif
  #if defined(USE_READSB_DEMOD)
    "readsb-demod",
  #endif
  "NETPOLLER=" NETPOLLER ,
    NULL
  };
  const char *f;
  char  *p = buf;
  int    i;

  for (i = 0, f = features[0]; f; f = features[++i])
  {
    strcpy (p, f);
    p += strlen (f);
    *p++ = ',';
    *p++ = ' ';
  }
  p[-2] = '\0';
  return (buf);
}

#if defined(USE_ASAN) || defined(__DOXYGEN__)
/**
 * Override of the default ASAN options set by `%ASAN_OPTIONS%`.
 */
const char *__asan_default_options (void)
{
  static const char *asan_options;
  static bool done = false;

  printf ("%s() called\n", __func__);
  if (!done)
      asan_options = getenv ("ASAN_OPTIONS");
  done = true;
  if (asan_options)
     return (asan_options);
  return ("debug=1:check_initialization_order=1:debug=1:windows_hook_rtl_allocators=1:log_path=ASAN");
}
#endif

#if defined(USE_UBSAN) || defined(__DOXYGEN__)
/**
 * Override of the default UBSAN options set by `%UBSAN_OPTIONS%`.
 */
const char *__ubsan_default_options (void)
{
  static const char *ubsan_options;
  static bool done = false;

  printf ("%s() called\n", __func__);
  if (!done)
      ubsan_options = getenv ("UBSAN_OPTIONS");
  done = true;
  if (ubsan_options)
     return (ubsan_options);
  return ("print_stacktrace=1:log_path=UBSAN");
}
#endif

/**
 * Print a long string to screen.
 * Try to wrap nicely according to the screen-width.
 * Multiple spaces ("  ") are collapsed into one space.
 */
void puts_long_line (const char *start, size_t indent)
{
  static size_t width = 0;
  size_t        left;
  const char   *c = start;

  if (width == 0)
  {
    CONSOLE_SCREEN_BUFFER_INFO console_info;
    HANDLE  hnd = GetStdHandle (STD_OUTPUT_HANDLE);

    width = UINT_MAX;

    memset (&console_info, '\0', sizeof(console_info));
    if (hnd != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hnd, &console_info) &&
        GetFileType(hnd) == FILE_TYPE_CHAR)
    {
      width = console_info.srWindow.Right - console_info.srWindow.Left + 1;
    }
  }

  left = width - indent;

  while (*c)
  {
    if (*c == ' ')
    {
      /* Break a long line at a space.
       */
      const char *p = strchr (c+1, ' ');
      int   ch;

      if (!p)
         p = strchr (c+1, '\0');
      if (left < 2 || (left <= (size_t)(p - c)))
      {
        printf ("\n%*c", (int)indent, ' ');
        left  = width - indent;
        start = ++c;
        continue;
      }
      ch = (int) c[1];
      if (isspace(ch))  /* Drop excessive blanks */
      {
        c++;
        continue;
      }
    }
    putchar (*c++);
    left--;
  }
  putchar ('\n');
}

/**
 * Print the CFLAGS and LDFLAGS we were built with.
 *
 * On a `make depend` (`DOING_MAKE_DEPEND` is defined),
 * do not add the above generated files to the dependency output.
 *
 * When building with `msbuild Dump1090.vcxproj` or during `make docs`,
 * do not included these generated files.
 */
#if defined(__clang__)
  #define CFLAGS   "cflags_clang-cl.h"
  #define LDFLAGS  "ldflags_clang-cl.h"
#else
  #define CFLAGS   "cflags_cl.h"
  #define LDFLAGS  "ldflags_cl.h"
#endif

#if defined(DOING_MSBUILD) || defined(DOING_MAKE_DEPEND) || defined(__DOXYGEN__)
  #undef CFLAGS
  #undef LDFLAGS
#endif

static void print_CFLAGS (void)
{
#if defined(CFLAGS)
  #include CFLAGS
  fputs ("CFLAGS:  ", stdout);
  puts_long_line (cflags, sizeof("CFLAGS:  ") - 1);
#else
  fputs ("CFLAGS:  Unknown\n", stdout);
#endif
}

static void print_LDFLAGS (void)
{
#if defined(LDFLAGS)
  #include LDFLAGS
  fputs ("LDFLAGS: ", stdout);
  puts_long_line (ldflags, sizeof("LDFLAGS: ") - 1);
#else
  fputs ("LDFLAGS: Unknown\n", stdout);
#endif
}

static const char *__DATE__str (void)
{
#if 0
  return (__DATE__);    /* e.g. "Mar  2 2024" */
#else
  /*
   * Convert `__DATE__ into `DD MMM YYYY`.
   * Based on:
   *   https://bytes.com/topic/c/answers/215378-convert-__date__-unsigned-int
   */
  #define YEAR() ((((__DATE__[7] - '0') * 10 + (__DATE__[8] - '0')) * 10 + \
                    (__DATE__[9] - '0')) * 10 + (__DATE__[10] - '0'))

  #define MONTH() ( __DATE__[2] == 'n' ? 0 \
                  : __DATE__[2] == 'b' ? 1 \
                  : __DATE__[2] == 'r' ? (__DATE__[0] == 'M' ? 2 : 3) \
                  : __DATE__[2] == 'y' ? 4 \
                  : __DATE__[2] == 'n' ? 5 \
                  : __DATE__[2] == 'l' ? 6 \
                  : __DATE__[2] == 'g' ? 7 \
                  : __DATE__[2] == 'p' ? 8 \
                  : __DATE__[2] == 't' ? 9 \
                  : __DATE__[2] == 'v' ? 10 : 11)

  #define DAY() ((__DATE__[4] == ' ' ? 0 : __DATE__[4] - '0') * 10 + \
                 (__DATE__[5] - '0'))

  static char buf [30];
  static char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

  snprintf (buf, sizeof(buf), "%d %.3s %04d",
            DAY(), months + 3*MONTH(), YEAR());
  return (buf);         /* e.g. "2 Mar 2024" */
#endif
}

/**
 * Print version information.
 */
void show_version_info (bool verbose)
{
  printf ("dump1090 ver: %s (%s, %s).\nBuilt on %s.\n",
          PROG_VERSION, compiler_info(), build_features(),
          __DATE__str());

  if (verbose)
  {
    printf ("RTL-SDR ver:  %d.%d.%d.%d from https://%s.\n",
            RTLSDR_MAJOR, RTLSDR_MINOR, RTLSDR_MICRO, RTLSDR_NANO, RTL_VER_ID);
    printf ("PDCurses ver: %s\n", PDC_VERDOT);
    printf ("Mongoose ver: %s\n", MG_VERSION);
    printf ("Miniz ver:    %s\n", mz_version());
    printf ("%s", mimalloc_version());
    print_packed_web_info();
    print_sql_info();
    print_CFLAGS();
    print_LDFLAGS();
  }
}

/**
 * \def DEF_FUNC
 * Handy macro to both define and declare the function-pointers
 * for `WinInet.dll`
 */
#define DEF_FUNC(ret, f, args)  typedef ret (WINAPI *func_##f) args; \
                                static func_##f p_##f = NULL

/**
 * Download a single file using the WinInet API.
 * Load `WinInet.dll` dynamically.
 */
DEF_FUNC (HINTERNET, InternetOpenA, (const char *user_agent,
                                     DWORD       access_type,
                                     const char *proxy_name,
                                     const char *proxy_bypass,
                                     DWORD       flags));

DEF_FUNC (HINTERNET, InternetOpenUrlA, (HINTERNET   hnd,
                                        const char *url,
                                        const char *headers,
                                        DWORD       headers_len,
                                        DWORD       flags,
                                        DWORD_PTR   context));

DEF_FUNC (BOOL, InternetReadFile, (HINTERNET hnd,
                                   void     *buffer,
                                   DWORD     num_bytes_to_read,
                                   DWORD    *num_bytes_read));

DEF_FUNC (BOOL, InternetGetLastResponseInfoA, (DWORD *err_code,
                                               char  *err_buff,
                                               DWORD *err_buff_len));

DEF_FUNC (BOOL, InternetCloseHandle, (HINTERNET handle));

DEF_FUNC (BOOL, HttpQueryInfoA, (HINTERNET handle,
                                 DWORD     info_level,
                                 void     *buf,
                                 DWORD    *buf_len,
                                 DWORD    *index));

/**
 * \def BUF_INCREMENT
 * Initial and incremental buffer size of `download_to_buf()`.
 * Also the size for `download_to_file()` buffer.
 */
#define BUF_INCREMENT (50*1024)

/**
 * Handles dynamic loading and unloading of DLLs and their functions.
 */
int load_dynamic_table (struct dyn_struct *tab, int tab_size)
{
  int i, required_missing = 0;

  for (i = 0; i < tab_size; tab++, i++)
  {
    const struct dyn_struct *prev = i > 0 ? (tab - 1) : NULL;
    HINSTANCE    mod_handle;
    FARPROC      func_addr;

    if (prev && !stricmp(tab->mod_name, prev->mod_name))
         mod_handle = prev->mod_handle;
    else mod_handle = LoadLibraryA (tab->mod_name);

    if (mod_handle)
    {
      func_addr = GetProcAddress (mod_handle, tab->func_name);
      *tab->func_addr = func_addr;
      if (!func_addr && !tab->optional)
         required_missing++;
    }
    tab->mod_handle = mod_handle;
  }
  return (i - required_missing);
}

int unload_dynamic_table (struct dyn_struct *tab, int tab_size)
{
  int i;

  for (i = 0; i < tab_size; tab++, i++)
  {
    if (tab->mod_handle)
       FreeLibrary (tab->mod_handle);
    tab->mod_handle = 0;
    *tab->func_addr = NULL;
  }
  return (i);
}

/**
 * Return error-string for `err` from `WinInet.dll`.
 *
 * Try to get a more detailed error-code and text from
 * the server response using `InternetGetLastResponseInfoA()`.
 */
const char *wininet_strerror (DWORD err)
{
  HMODULE mod = GetModuleHandleA ("wininet.dll");
  static char buf [512];

  Modes.wininet_last_error = NULL;

  if (mod && FormatMessageA (FORMAT_MESSAGE_FROM_HMODULE,
                             mod, err, MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),
                             buf, sizeof(buf), NULL))
  {
    static char err_buf [512];
    char   wininet_err_buf [200];
    char  *p;
    DWORD  wininet_err = 0;
    DWORD  wininet_err_len = sizeof(wininet_err_buf)-1;

    Modes.wininet_last_error = buf;

    p = strrchr (buf, '\r');
    if (p)
       *p = '\0';

    p = strrchr (buf, '.');
    if (p && p[1] == '\0')
       *p = '\0';

    p = err_buf;
    p += snprintf (err_buf, sizeof(err_buf), "%lu: %s", (u_long)err, buf);

    if ((*p_InternetGetLastResponseInfoA) (&wininet_err, wininet_err_buf, &wininet_err_len) &&
        wininet_err > INTERNET_ERROR_BASE && wininet_err <= INTERNET_ERROR_LAST)
    {
      snprintf (p, (size_t)(p-err_buf), " (%lu/%s)", (u_long)wininet_err, wininet_err_buf);
      p = strrchr (p, '.');
      if (p && p[1] == '\0')
         *p = '\0';
    }
    Modes.wininet_last_error = err_buf;
    return (err_buf);
  }
  return win_strerror (err);
}

/**
 * Setup the `h1` and `h2` handles for a WinInet transfer.
 */
static bool download_init (HINTERNET *h1, HINTERNET *h2, const char *url)
{
  DWORD url_flags;

  *h1 = (*p_InternetOpenA) ("dump1090", INTERNET_OPEN_TYPE_DIRECT,
                            NULL, NULL,
                            INTERNET_FLAG_NO_COOKIES);   /* no automatic cookie handling */
  if (*h1 == NULL)
  {
    wininet_strerror (GetLastError());
    DEBUG (DEBUG_NET, "InternetOpenA() failed: %s.\n", Modes.wininet_last_error);
    return (false);
  }

  url_flags = INTERNET_FLAG_RELOAD |
              INTERNET_FLAG_PRAGMA_NOCACHE |
              INTERNET_FLAG_NO_CACHE_WRITE |
              INTERNET_FLAG_NO_UI;

//url_flags |= INTERNET_FLAG_EXISTING_CONNECT;

  if (!strncmp(url, "https://", 8))
     url_flags |= INTERNET_FLAG_SECURE;

  *h2 = (*p_InternetOpenUrlA) (*h1, url, NULL, 0, url_flags, INTERNET_NO_CALLBACK);
  if (*h2 == NULL)
  {
    wininet_strerror (GetLastError());
    DEBUG (DEBUG_NET, "InternetOpenA() failed: %s.\n", Modes.wininet_last_error);
    return (false);
  }
  return (true);
}

/**
 * Load and use the *WinInet API* dynamically.
 */
#define ADD_VALUE(func)  { false, NULL, "wininet.dll", #func, (void**) &p_##func }
                        /* ^ no functions are optional */

static struct dyn_struct wininet_funcs[] = {
                         ADD_VALUE (InternetOpenA),
                         ADD_VALUE (InternetOpenUrlA),
                         ADD_VALUE (InternetGetLastResponseInfoA),
                         ADD_VALUE (InternetReadFile),
                         ADD_VALUE (InternetCloseHandle),
                         ADD_VALUE (HttpQueryInfoA)
                       };

typedef struct download_ctx {
        const char *url;
        const char *file;
        HINTERNET   h1;
        HINTERNET   h2;
        FILE       *f;
        BOOL        wininet_rc;          /* last 'InternetReadFile()' result */
        char        file_buf [BUF_INCREMENT];
        char       *dl_buf;
        size_t      dl_buf_sz;
        size_t      dl_buf_pos;
        DWORD       bytes_read;
        DWORD       bytes_read_total;
        uint32_t    written_to_file;
        bool        got_last_chunk;
      } download_ctx;

static int http_status = -1;

int download_status (void)
{
  return (http_status);
}

static bool download_exit (download_ctx *ctx, bool rc)
{
  if (ctx->f)
     fclose (ctx->f);

  if (ctx->h2)
  {
    char  buf [100] = "";
    DWORD len = sizeof(buf);

    if ((*p_HttpQueryInfoA) (ctx->h2, HTTP_QUERY_STATUS_CODE, buf, &len, NULL))
       http_status = atoi (buf);

    (*p_InternetCloseHandle) (ctx->h2);
  }

  if (ctx->h1)
    (*p_InternetCloseHandle) (ctx->h1);

  ctx->h1 = ctx->h2 = NULL;
  unload_dynamic_table (wininet_funcs, DIM(wininet_funcs));
  return (rc);
}

/**
 * The callback for downloading to a file.
 */
static bool download_to_file_cb (download_ctx *ctx)
{
  DWORD sz;

  if (ctx->got_last_chunk)
     puts ("");

  sz = (int) fwrite (ctx->dl_buf, 1, (size_t)ctx->bytes_read, ctx->f);
  if (sz != ctx->bytes_read)
     return (false);

  ctx->written_to_file += (uint32_t) sz;
  printf ("Got %u kB.\r", ctx->written_to_file / 1024);
  assert (ctx->dl_buf_pos == 0);
  return (true);
}

/**
 * The callback for downloading to a malloc()'ed buffer.
 */
static bool download_to_buf_cb (download_ctx *ctx)
{
  if (ctx->bytes_read_total + ctx->bytes_read >= ctx->dl_buf_sz)
  {
    char *more;

    DEBUG (DEBUG_NET, "Limit reached. dl_buf_sz: %zu\n", ctx->dl_buf_sz);
    ctx->dl_buf_sz += BUF_INCREMENT;
    more = realloc (ctx->dl_buf, ctx->dl_buf_sz);
    if (!more)
       return (false);

    ctx->dl_buf = more;
  }

  ctx->dl_buf_pos += ctx->bytes_read;
  assert (ctx->dl_buf_pos < ctx->dl_buf_sz);
  ctx->dl_buf [ctx->dl_buf_pos] = '\0';    /* 0 terminate */

  DEBUG (DEBUG_NET, "bytes_read_total: %lu, dl_buf_pos: %zu\n",
         ctx->bytes_read_total, ctx->dl_buf_pos);
  return (true);
}

static bool download_common (download_ctx *ctx,
                             const char   *url,
                             const char   *file,
                             bool        (*callback)(download_ctx *ctx))
{
  memset (ctx, '\0', sizeof(*ctx));
  ctx->url  = url;
  ctx->file = file;
  http_status = -1; /* unknown now */

  if (ctx->file)
  {
    ctx->dl_buf    = ctx->file_buf;
    ctx->dl_buf_sz = sizeof (ctx->file_buf);
    ctx->f = fopen (ctx->file, "w+b");
    if (!ctx->f)
    {
      DEBUG (DEBUG_NET, "Failed to create '%s'; %s.\n", ctx->file, strerror(errno));
      return download_exit (ctx, false);
    }
  }
  else
  {
    ctx->dl_buf_sz = BUF_INCREMENT;
    ctx->dl_buf    = malloc (ctx->dl_buf_sz);
    if (!ctx->dl_buf)
    {
      DEBUG (DEBUG_NET, "Failed to allocate %d kByte!.\n", BUF_INCREMENT/1024);
      return (false);
    }
  }

  if (load_dynamic_table(wininet_funcs, DIM(wininet_funcs)) != DIM(wininet_funcs))
  {
    DEBUG (DEBUG_NET, "Failed to load the needed 'WinInet.dll' functions.\n");
    return download_exit (ctx, false);
  }

  if (!download_init(&ctx->h1, &ctx->h2, ctx->url))
     return download_exit (ctx, false);

  while (1)
  {
    ctx->bytes_read = 0;
    ctx->wininet_rc = (*p_InternetReadFile) (
                         ctx->h2,
                         ctx->dl_buf    + ctx->dl_buf_pos,
                         ctx->dl_buf_sz - ctx->dl_buf_pos,
                         &ctx->bytes_read);

    if (!ctx->wininet_rc || ctx->bytes_read == 0)
    {
      DEBUG (DEBUG_NET, "got_last_chunk.\n");
      ctx->got_last_chunk = true;
    }
    if (!(*callback)(ctx) || ctx->got_last_chunk)
       break;

    ctx->bytes_read_total += ctx->bytes_read;
  }
  return download_exit (ctx, ctx->got_last_chunk);
}

/**
 * Download a file from url using the Windows *WinInet API*.
 *
 * \param[in] url   the URL to retrieve from.
 * \param[in] file  the file to write to.
 * \retval    The number of bytes written to `file`.
 */
uint32_t download_to_file (const char *url, const char *file)
{
  download_ctx ctx;

  if (!download_common(&ctx, url, file, download_to_file_cb))
     return (0);
  return (ctx.written_to_file);
}

/**
 * Download from an url using the Windows *WinInet API*.
 *
 * \param[in] url  the URL to retrieve from.
 * \retval    The allocated result. Caller must free this.
 */
char *download_to_buf (const char *url)
{
  download_ctx ctx;

  if (!download_common(&ctx, url, NULL, download_to_buf_cb))
     return (NULL);
  return (ctx.dl_buf);
}

/**
 * From SDRangel's 'sdrbase/util/azel.cpp':
 *
 * Convert geodetic latitude to geocentric latitude;
 * angle from centre of Earth between the point and equator.
 *
 * \ref https://en.wikipedia.org/wiki/Latitude#Geocentric_latitude
 *
 * \param in lat  The geodetic latitude in radians.
 */
static double geocentric_latitude (double lat)
{
  double e2 = 0.00669437999014;

  return atan ((1.0 - e2) * tan(lat));
}

/**
 * Try to figure out some issues with cartesian position going crazy.
 * Ignore the `z` axis (just print level above earth).
 */
static void assert_cart (const cartesian_t *cpos, double heading, unsigned line)
{
#ifdef _DEBUG
  if (fabs(cpos->c_x) > EARTH_RADIUS || fabs(cpos->c_y) > EARTH_RADIUS)
  {
    double x = cpos->c_x / 1E3;
    double y = cpos->c_y / 1E3;
    double z = (EARTH_RADIUS - cpos->c_z) / 1E3;

    fprintf (stderr, "assertion at 'misc.c(%u)': x=%.2f, y=%.2f, z=%.2f, heading=%.2f.\n",
             line, x, y, z, TWO_PI * heading / 360);
    abort();
  }
#else
  MODES_NOTUSED (cpos);
  MODES_NOTUSED (heading);
  MODES_NOTUSED (line);
#endif
}

/**
 * Convert spherical coordinate to cartesian.
 * Also calculates radius and a normal vector.
 *
 * \param in  pos   The position on the Geoid.
 * \param out cart  The position on Cartesian form.
 */
void spherical_to_cartesian (const pos_t *pos, cartesian_t *cart)
{
  double lat, lon, geo_lat;
  pos_t _pos = *pos;

  ASSERT_POS (_pos);
  lat  = TWO_PI * _pos.lat / 360.0;
  lon  = TWO_PI * _pos.lon / 360.0;
  geo_lat = geocentric_latitude (lat);

  cart->c_x = EARTH_RADIUS * cos (lon) * cos (geo_lat);
  cart->c_y = EARTH_RADIUS * sin (lon) * cos (geo_lat);
  cart->c_z = EARTH_RADIUS * sin (geo_lat);
  assert_cart (cart, 0.0, __LINE__);
}

/**
 * \ref https://keisan.casio.com/exec/system/1359533867
 */
bool cartesian_to_spherical (const cartesian_t *cart, pos_t *pos_out, double heading)
{
  pos_t pos;

  assert_cart (cart, heading, __LINE__);

  /* We do not need this; close to EARTH_RADIUS.
   *
   * double radius = sqrt (cart->c_x * cart->c_x + cart->c_y * cart->c_y + cart->c_z * cart->c_z);
   */
  pos.lon = 360.0 * atan2 (cart->c_y, cart->c_x) / TWO_PI;
  pos.lat = 360.0 * atan2 (hypot(cart->c_x, cart->c_y), cart->c_z) / TWO_PI;
  *pos_out = pos;
  return (VALID_POS(pos));
}

/**
 * Return the distance between 2 Cartesian points.
 */
double cartesian_distance (const cartesian_t *a, const cartesian_t *b)
{
  static double old_rc = 0.0;
  double delta_X, delta_Y, rc;

  assert_cart (a, 0.0, __LINE__);
  assert_cart (b, 0.0, __LINE__);

  delta_X = b->c_x - a->c_x;
  delta_Y = b->c_y - a->c_y;

  rc = hypot (delta_X, delta_Y);   /* sqrt (delta_X*delta_X, delta_Y*delta_Y) */

//assert (fabs(rc - old_rc) < 6000.0);  /* 6 km */
  old_rc = rc;
  (void) old_rc;
  return (rc);
}

/**
 * Return the closest of `val1` and `val2` to `val`.
 */
double closest_to (double val, double val1, double val2)
{
  double diff1 = fabs (val1 - val);
  double diff2 = fabs (val2 - val);

  return (diff2 > diff1 ? val1 : val2);
}

/**
 * Distance between 2 points on a spherical earth.
 * This has up to 0.5% error because the earth isn't actually spherical
 * (but we don't use it in situations where that matters)
 *
 * \ref https://en.wikipedia.org/wiki/Great-circle_distance
 */
double great_circle_dist (pos_t pos1, pos_t pos2)
{
  double lat1 = TWO_PI * pos1.lat / 360.0;  /* convert to radians */
  double lon1 = TWO_PI * pos1.lon / 360.0;
  double lat2 = TWO_PI * pos2.lat / 360.0;
  double lon2 = TWO_PI * pos2.lon / 360.0;
  double angle;

  /* Avoid a 'NaN'
   */
  if (fabs(lat1 - lat2) < SMALL_VAL && fabs(lon1 - lon2) < SMALL_VAL)
     return (0.0);

  angle = sin(lat1) * sin(lat2) + cos(lat1) * cos(lat2) * cos(fabs(lon1 - lon2));

  /* Radius of the Earth * 'arcosine of angular distance'.
   */
  return (EARTH_RADIUS * acos(angle));
}

/**
 * Set this aircraft's distance to our home position.
 *
 * The reference time-tick is the latest of `a->odd_CPR_time` and `a->even_CPR_time`.
 *
 * \todo move to aircraft.c
 */
static void set_home_distance (aircraft *a)
{
  double distance;

  if (!(VALID_POS(Modes.home_pos) && VALID_POS(a->position)))
     return;

  distance = great_circle_dist (a->position, Modes.home_pos);
  if (distance != 0.0)
     a->distance = distance;

  a->EST_position = a->position;

  if (a->even_CPR_time > 0 && a->odd_CPR_time > 0)
     a->EST_seen_last = (a->even_CPR_time > a->odd_CPR_time) ? a->even_CPR_time : a->odd_CPR_time;
}

/**
 * Helper function for decoding the **CPR** (*Compact Position Reporting*). <br>
 * Always positive MOD operation, used for CPR decoding.
 */
static int CPR_mod_func (int a, int b)
{
  int res = a % b;

  if (res < 0)
     res += b;
  return (res);
}

/**
 * Helper function for decoding the **CPR** (*Compact Position Reporting*).
 *
 * Calculates **NL** *(lat)*; *Number of Longitude* zone. <br>
 * Given the latitude, this function returns the number of longitude zones between 1 and 59.
 *
 * The NL function uses the precomputed table from 1090-WP-9-14. <br>
 * Refer [The-1090MHz-riddle](./The-1090MHz-riddle.pdf), page 45 for the exact equation.
 */
static int CPR_NL_func (double lat)
{
  if (lat < 0)
     lat = -lat;   /* Table is symmetric about the equator. */

  if (lat > 60.0)
     goto L60;

  if (lat > 44.2)
     goto L42;

  if (lat > 30.0)
     goto L30;

  if (lat < 10.47047130) return (59);
  if (lat < 14.82817437) return (58);
  if (lat < 18.18626357) return (57);
  if (lat < 21.02939493) return (56);
  if (lat < 23.54504487) return (55);
  if (lat < 25.82924707) return (54);
  if (lat < 27.93898710) return (53);
  if (lat < 29.91135686) return (52);

L30:
  if (lat < 31.77209708) return (51);
  if (lat < 33.53993436) return (50);
  if (lat < 35.22899598) return (49);
  if (lat < 36.85025108) return (48);
  if (lat < 38.41241892) return (47);
  if (lat < 39.92256684) return (46);
  if (lat < 41.38651832) return (45);
  if (lat < 42.80914012) return (44);
  if (lat < 44.19454951) return (43);

L42:
  if (lat < 45.54626723) return (42);
  if (lat < 46.86733252) return (41);
  if (lat < 48.16039128) return (40);
  if (lat < 49.42776439) return (39);
  if (lat < 50.67150166) return (38);
  if (lat < 51.89342469) return (37);
  if (lat < 53.09516153) return (36);
  if (lat < 54.27817472) return (35);
  if (lat < 55.44378444) return (34);
  if (lat < 56.59318756) return (33);
  if (lat < 57.72747354) return (32);
  if (lat < 58.84763776) return (31);
  if (lat < 59.95459277) return (30);

L60:
  if (lat < 61.04917774) return (29);
  if (lat < 62.13216659) return (28);
  if (lat < 63.20427479) return (27);
  if (lat < 64.26616523) return (26);
  if (lat < 65.31845310) return (25);
  if (lat < 66.36171008) return (24);
  if (lat < 67.39646774) return (23);
  if (lat < 68.42322022) return (22);
  if (lat < 69.44242631) return (21);
  if (lat < 70.45451075) return (20);
  if (lat < 71.45986473) return (19);
  if (lat < 72.45884545) return (18);
  if (lat < 73.45177442) return (17);
  if (lat < 74.43893416) return (16);
  if (lat < 75.42056257) return (15);
  if (lat < 76.39684391) return (14);
  if (lat < 77.36789461) return (13);
  if (lat < 78.33374083) return (12);
  if (lat < 79.29428225) return (11);
  if (lat < 80.24923213) return (10);
  if (lat < 81.19801349) return (9);
  if (lat < 82.13956981) return (8);
  if (lat < 83.07199445) return (7);
  if (lat < 83.99173563) return (6);
  if (lat < 84.89166191) return (5);
  if (lat < 85.75541621) return (4);
  if (lat < 86.53536998) return (3);
  if (lat < 87.00000000) return (2);
  return (1);
}

static int CPR_N_func (double lat, int is_odd)
{
  int nl = CPR_NL_func (lat) - is_odd;

  if (nl < 1)
     nl = 1;
  return (nl);
}

static double CPR_Dlong_func (double lat, int is_odd)
{
  return (360.0 / CPR_N_func (lat, is_odd));
}

/**
 * Decode the **CPR** (*Compact Position Reporting*).
 *
 * This algorithm comes from: <br>
 * http://www.lll.lu/~edward/edward/adsb/DecodingADSBposition.html.
 *
 * A few remarks:
 *
 * \li 131072 is 2^17 since CPR latitude and longitude are encoded in 17 bits.
 * \li We assume that we always received the odd packet as last packet for
 *     simplicity. This may provide a position that is less fresh of a few seconds.
 */
void decode_CPR (struct aircraft *a)
{
  const double air_dlat0 = 360.0 / 60;
  const double air_dlat1 = 360.0 / 59;
  double       lat0      = a->even_CPR_lat;
  double       lat1      = a->odd_CPR_lat;
  double       lon0      = a->even_CPR_lon;
  double       lon1      = a->odd_CPR_lon;

  /* Compute the Latitude Index "j"
   */
  int    j = (int) floor (((59*lat0 - 60*lat1) / 131072) + 0.5);
  double rlat0 = air_dlat0 * (CPR_mod_func(j, 60) + lat0 / 131072);
  double rlat1 = air_dlat1 * (CPR_mod_func(j, 59) + lat1 / 131072);

  if (rlat0 >= 270)
     rlat0 -= 360;

  if (rlat1 >= 270)
     rlat1 -= 360;

  /* Check that both are in the same latitude zone, or return.
   */
  if (CPR_NL_func(rlat0) != CPR_NL_func(rlat1))
     return;

  /* Compute ni and the longitude index m
   */
  if (a->odd_CPR_time > a->even_CPR_time)
  {
    /* Use odd packet */
    int ni = CPR_N_func (rlat1, 1);
    int m  = (int) floor ((((lon0 * (CPR_NL_func(rlat1) - 1)) -
                          (lon1 * CPR_NL_func(rlat1))) / 131072.0) + 0.5);
    a->position.lon = CPR_Dlong_func (rlat1, 1) * (CPR_mod_func (m, ni) + lon1 / 131072);
    a->position.lat = rlat1;
  }
  else
  {
    /* Use even packet */
    int ni = CPR_N_func (rlat0, 0);
    int m = (int) floor ((((lon0 * (CPR_NL_func(rlat0) - 1)) -
                         (lon1 * CPR_NL_func(rlat0))) / 131072) + 0.5);
    a->position.lon = CPR_Dlong_func (rlat0, 0) * (CPR_mod_func(m, ni) + lon0 / 131072);
    a->position.lat = rlat0;
  }

  /* Normalize to -180 .. +180
   */
#if 0
  if (a->position.lon > 180)
     a->position.lon -= 360;
#else
  a->position.lon -= floor ((a->position.lon + 180) / 360) * 360;
#endif

  set_home_distance (a);
}

/*
 * Copyright (c) 2002 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */
#define PRINT_ERROR ((opterr) && (*options != ':'))

/**
 * \def FLAG_PERMUTE   permute non-options to the end of argv
 * \def FLAG_ALLARGS   treat non-options as args to option "-1"
 * \def FLAG_LONGONLY  operate as `getopt_long_only()`.
 */
#define FLAG_PERMUTE    0x01
#define FLAG_ALLARGS    0x02
#define FLAG_LONGONLY   0x04

/** Return values
 *
 * \def BADCH
 *  If getopt() encounters an option character that was not in optstring, then `?` is returned.
 *
 * \def BADARG
 *  If getopt() encounters an option with a missing argument, then the return value depends on
 *  the first character in `optstring`: if it is `:`, then `:` is returned; otherwise `?` is returned.
 */
#define BADCH       (int)'?'
#define BADARG      ((*options == ':') ? (int)':' : (int)'?')
#define INORDER     (int)1

#define EMSG        ""

#define NO_PREFIX   (-1)
#define D_PREFIX    0
#define DD_PREFIX   1
#define W_PREFIX    2

char *optarg;
int   optind, opterr = 1, optopt;

static const char *place = EMSG; /**< option letter processing */

static int nonopt_start = -1;    /**< first non option argument (for permute) */
static int nonopt_end   = -1;    /**< first option after non options (for permute) */
static int dash_prefix  = NO_PREFIX;

/* Error messages
 */
static const char recargchar[]   = "option requires an argument -- %c";
static const char illoptchar[]   = "illegal option -- %c"; /* From P1003.2 */

static const char gnuoptchar[]   = "invalid option -- %c";
static const char recargstring[] = "option `%s%s' requires an argument";
static const char ambig[]        = "option `%s%.*s' is ambiguous";
static const char noarg[]        = "option `%s%.*s' doesn't allow an argument";
static const char illoptstring[] = "unrecognized option `%s%s'";

/**
 * Compute the greatest common divisor of a and b.
 */
static int gcd (int a, int b)
{
  int c;

  c = a % b;
  while (c != 0)
  {
    a = b;
    b = c;
    c = a % b;
  }
  return (b);
}

/**
 * Exchange the block from nonopt_start to nonopt_end with the block
 * from nonopt_end to opt_end (keeping the same order of arguments
 * in each block).
 */
static void permute_args (int panonopt_start, int panonopt_end,
                          int opt_end, char * const *nargv)
{
  int   cstart, cyclelen, i, j, ncycle, nnonopts, nopts, pos;
  char *swap;

  /* compute lengths of blocks and number and size of cycles
   */
  nnonopts = panonopt_end - panonopt_start;
  nopts    = opt_end - panonopt_end;
  ncycle   = gcd (nnonopts, nopts);
  cyclelen = (opt_end - panonopt_start) / ncycle;

  for (i = 0; i < ncycle; i++)
  {
    cstart = panonopt_end + i;
    pos = cstart;
    for (j = 0; j < cyclelen; j++)
    {
      if (pos >= panonopt_end)
           pos -= nnonopts;
      else pos += nopts;

      swap = nargv[pos];
      ((char**)nargv) [pos] = nargv[cstart];
      ((char**)nargv) [cstart] = swap;
    }
  }
}

/**
 * Print a warning to stderr.
 */
static void warnx (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  fprintf (stderr, "%s: ", Modes.who_am_I);
  vfprintf (stderr, fmt, ap);
  fprintf (stderr, "\n");
  va_end (ap);
}

/**
 * Parse long options in `argc` / `argv` argument vector.
 *
 * \retval -1     if `short_too` is set and the option does not match `long_options`.
 * \retval BADCH  if no match found.
 * \retval BADARG if option is missing required argument.
 * \retval 0      if option and possibly an argument was found.
 */
static int parse_long_options (char *const *nargv, const char *options,
                               const struct option *long_options,
                               int *idx, int short_too, int flags)
{
  const char *current_argv, *has_equal;
  const char *current_dash;
  size_t      current_argv_len;
  int         i, match, exact_match, second_partial_match;

  current_argv = place;
  switch (dash_prefix)
  {
    case D_PREFIX:
         current_dash = "-";
         break;
    case DD_PREFIX:
         current_dash = "--";
         break;
    case W_PREFIX:
         current_dash = "-W ";
         break;
    default:
         current_dash = "";
         break;
  }

  match = -1;
  exact_match = 0;
  second_partial_match = 0;

  optind++;

  has_equal = strchr (current_argv, '=');
  if (!has_equal)
      has_equal = strchr (current_argv, ':');

  if (has_equal)
  {
    /* argument found (--option=arg)
     */
    current_argv_len = has_equal - current_argv;
    has_equal++;
  }
  else
    current_argv_len = strlen (current_argv);

  for (i = 0; long_options[i].name; i++)
  {
    /* find matching long option
     */
    if (strncmp(current_argv, long_options[i].name, current_argv_len))
       continue;

    if (strlen(long_options[i].name) == current_argv_len)
    {
      /* exact match */
      match = i;
      exact_match = 1;
      break;
    }

    /* If this is a known short option, don't allow
     * a partial match of a single character.
     */
    if (short_too && current_argv_len == 1)
       continue;

    if (match == -1)        /* first partial match */
        match = i;
    else if ((flags & FLAG_LONGONLY) ||
             long_options[i].has_arg != long_options[match].has_arg ||
             long_options[i].flag != long_options[match].flag ||
             long_options[i].val != long_options[match].val)
        second_partial_match = 1;
  }

  if (!exact_match && second_partial_match)
  {
    /* ambiguous abbreviation */
    if (PRINT_ERROR)
       warnx (ambig, current_dash, (int)current_argv_len, current_argv);
    optopt = 0;
    return (BADCH);
  }

  if (match != -1)       /* option found */
  {
    if (long_options[match].has_arg == no_argument && has_equal)
    {
      if (PRINT_ERROR)
         warnx (noarg, current_dash, (int)current_argv_len, current_argv);

      if (long_options[match].flag == NULL)
           optopt = long_options[match].val;
      else optopt = 0;
      return (BADCH);
    }

    if (long_options[match].has_arg == required_argument ||
        long_options[match].has_arg == optional_argument)
    {
      if (has_equal)
         optarg = (char*) has_equal;
      else if (long_options[match].has_arg == required_argument)
      {
        /* optional argument doesn't use next nargv
         */
        optarg = nargv[optind++];
      }
    }

    if ((long_options[match].has_arg == required_argument) && !optarg)
    {
      /* Missing argument; leading ':' indicates no error should be generated.
       */
      if (PRINT_ERROR)
         warnx (recargstring, current_dash, current_argv);

      if (long_options[match].flag == NULL)
           optopt = long_options[match].val;
      else optopt = 0;
      --optind;
      return (BADARG);
    }
  }
  else        /* unknown option */
  {
    if (short_too)
    {
      --optind;
      return (-1);
    }
    if (PRINT_ERROR)
       warnx (illoptstring, current_dash, current_argv);
    optopt = 0;
    return (BADCH);
  }

  if (idx)
     *idx = match;

  if (long_options[match].flag)
  {
    *long_options[match].flag = long_options[match].val;
    return (0);
  }
  return (long_options[match].val);
}

/**
 * Parse `argc` / `argv` argument vector.
 * Called by user level routines.
 */
static int getopt_internal (int nargc, char * const *nargv,
                            const char *options,
                            const struct option *long_options,
                            int *idx, int flags)
{
  char *oli;                /* option letter list index */
  int   optchar, short_too;
  int   posixly_correct;    /* no static, can be changed on the fly */

  if (options == NULL)
     return (-1);

  /* Disable GNU extensions if POSIXLY_CORRECT is set or options
   * string begins with a '+'.
   */
  posixly_correct = (getenv("POSIXLY_CORRECT") != NULL);

  if (*options == '-')
  {
    flags |= FLAG_ALLARGS;
  }
  else if (posixly_correct || *options == '+')
  {
    flags &= ~FLAG_PERMUTE;
  }

  if (*options == '+' || *options == '-')
     options++;

  /* Some GNU programs (like cvs) set optind to 0 instead of
   * using optreset.  Work around this braindamage.
   */
  if (optind == 0)
     optind = 1;

  optarg = NULL;

start:
  if (!*place)              /* update scanning pointer */
  {
    if (optind >= nargc)    /* end of argument vector */
    {
      place = EMSG;
      if (nonopt_end != -1)
      {
        /* do permutation, if we have to
         */
        permute_args (nonopt_start, nonopt_end, optind, nargv);
        optind -= nonopt_end - nonopt_start;
      }
      else if (nonopt_start != -1)
      {
        /* If we skipped non-options, set optind to the first of them.
         */
        optind = nonopt_start;
      }
      nonopt_start = nonopt_end = -1;
      return (-1);
    }

    if (*(place = nargv[optind]) != '-' || place[1] == '\0')
    {
      place = EMSG;       /* found non-option */
      if (flags & FLAG_ALLARGS)
      {
        /* GNU extension:
         * return non-option as argument to option 1
         */
        optarg = nargv[optind++];
        return (INORDER);
      }

      if (!(flags & FLAG_PERMUTE))
      {
        /* If no permutation wanted, stop parsing at first non-option.
         */
        return (-1);
      }

      /* do permutation
       */
      if (nonopt_start == -1)
          nonopt_start = optind;
      else if (nonopt_end != -1)
      {
        permute_args (nonopt_start, nonopt_end, optind, nargv);
        nonopt_start = optind - (nonopt_end - nonopt_start);
        nonopt_end = -1;
      }
      optind++;

      /* process next argument
       */
      goto start;
    }

    if (nonopt_start != -1 && nonopt_end == -1)
       nonopt_end = optind;

    /* If we have "-" do nothing, if "--" we are done.
     */
    if (place[1] != '\0' && *++place == '-' && place[1] == '\0')
    {
      optind++;
      place = EMSG;

      /* We found an option (--), so if we skipped non-options, we have to permute.
       */
      if (nonopt_end != -1)
      {
        permute_args (nonopt_start, nonopt_end, optind, nargv);
        optind -= nonopt_end - nonopt_start;
      }
      nonopt_start = nonopt_end = -1;
      return (-1);
    }
  }

  /* Check long options if:
   *  1) we were passed some
   *  2) the arg is not just "-"
   *  3) either the arg starts with -- we are getopt_long_only()
   */
  if (long_options && place != nargv[optind] && (*place == '-' || (flags & FLAG_LONGONLY)))
  {
    short_too = 0;
    dash_prefix = D_PREFIX;
    if (*place == '-')
    {
      place++;     /* --foo long option */
      dash_prefix = DD_PREFIX;
    }
    else if (*place != ':' && strchr(options, *place) != NULL)
      short_too = 1;      /* could be short option too */

    optchar = parse_long_options (nargv, options, long_options, idx, short_too, flags);
    if (optchar != -1)
    {
      place = EMSG;
      return (optchar);
    }
  }

  optchar = (int)*place++;
  if (optchar == (int)':' || (optchar == (int)'-' && *place != '\0') || (oli = strchr(options, optchar)) == NULL)
  {
    /* If the user specified "-" and '-' isn't listed in
     * options, return -1 (non-option) as per POSIX.
     * Otherwise, it is an unknown option character (or ':').
     */
    if (optchar == (int)'-' && *place == '\0')
       return (-1);

    if (!*place)
       ++optind;

    if (PRINT_ERROR)
       warnx (posixly_correct ? illoptchar : gnuoptchar, optchar);
    optopt = optchar;
    return (BADCH);
  }

  if (long_options && optchar == 'W' && oli[1] == ';')
  {
    /* -W long-option
     */
    if (*place)         /* no space */
       ;                /* NOTHING */
    else if (++optind >= nargc)    /* no arg */
    {
      place = EMSG;
      if (PRINT_ERROR)
         warnx (recargchar, optchar);
      optopt = optchar;
      return (BADARG);
    }
    else               /* white space */
      place = nargv [optind];

    dash_prefix = W_PREFIX;
    optchar = parse_long_options (nargv, options, long_options, idx, 0, flags);
    place = EMSG;
    return (optchar);
  }

  if (*++oli != ':')     /* doesn't take argument */
  {
    if (!*place)
        ++optind;
  }
  else                  /* takes (optional) argument */
  {
    optarg = NULL;
    if (*place)         /* no white space */
        optarg = (char*) place;
    else if (oli[1] != ':')    /* arg not optional */
    {
      if (++optind >= nargc)   /* no arg */
      {
        place = EMSG;
        if (PRINT_ERROR)
           warnx (recargchar, optchar);
        optopt = optchar;
        return (BADARG);
      }
      else
        optarg = nargv[optind];
    }
    place = EMSG;
    ++optind;
  }

  /* dump back option letter
   */
  return (optchar);
}

/**
 * Parse `argc` / `argv` argument vector.
 */
int getopt (int nargc, char * const *nargv, const char *options)
{
  /** We don't pass FLAG_PERMUTE to getopt_internal() since
   *  the BSD getopt(3) (unlike GNU) has never done this.
   */
  return getopt_internal (nargc, nargv, options, NULL, NULL, 0);
}

/**
 * Parse `argc` / `argv` argument vector.
 */
int getopt_long (int nargc, char * const *nargv, const char *options,
                 const struct option *long_options, int *idx)
{
  return getopt_internal (nargc, nargv, options, long_options, idx, FLAG_PERMUTE);
}

/**
 * Parse `argc` / `argv` argument vector.
 */
int getopt_long_only (int nargc, char * const *nargv, const char *options,
                      const struct option *long_options, int *idx)
{
  return getopt_internal (nargc, nargv, options, long_options, idx,
                          FLAG_PERMUTE|FLAG_LONGONLY);
}
