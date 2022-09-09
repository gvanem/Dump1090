/**\file    misc.c
 * \ingroup Misc
 *
 */
#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0602)
  #undef  _WIN32_WINNT
  #define _WIN32_WINNT 0x0602  /* _WIN32_WINNT_WIN8 */
#endif

#include <stdint.h>
#include <sys/utime.h>
#include "misc.h"

/**
 * Log a message to the `Modes.log` file.
 */
void modeS_log (const char *buf)
{
  SYSTEMTIME now;
  char tbuf [30];

  if (!Modes.log)
     return;

  GetLocalTime (&now);
  snprintf (tbuf, sizeof(tbuf), "%02u:%02u:%02u.%03u",
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

  if (*buf == '\n')
     buf++;
  fprintf (Modes.log, "%s: %s", tbuf, buf);
}

/**
 * Print a character `c` to `Modes.log` or `stdout`.
 * Used only if `(Modes.debug & DEBUG_MONGOOSE)" is enabled by `--debug m`.
 */
void modeS_logc (char c, void *param)
{
  fputc (c, Modes.log ? Modes.log : stdout);
  MODES_NOTUSED (param);
}

/**
 * Print to `f` and optionally to `Modes.log`.
 */
void modeS_flogf (FILE *f, const char *fmt, ...)
{
  char buf [1000];
  va_list args;

  va_start (args, fmt);
  vsnprintf (buf, sizeof(buf), fmt, args);
  va_end (args);

  if (f && f != Modes.log)
  {
    fputs (buf, f);
    fflush (f);
  }
  if (Modes.log)
     modeS_log (buf);
}

/**
 * Convert standard suffixes (k, M, G) to an `uint32_t`
 *
 * \param in Hertz   a string to be parsed
 * \retval   the frequency as a `double`
 * \note Taken from Osmo-SDR's `convenience.c` and modified.
 */
uint32_t ato_hertz (const char *Hertz)
{
  char     tmp [20], *end, last_ch;
  int      len;
  double   multiplier = 1.0;
  uint32_t ret;

  strncpy (tmp, Hertz, sizeof(tmp));
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
 * Return TRUE if string `s1` starts with `s2`.
 *
 * Ignore casing of both strings.
 * And drop leading blanks in `s1` first.
 */
bool str_startswith (const char *s1, const char *s2)
{
  size_t s1_len, s2_len;

  s1_len = strlen (s1);
  s2_len = strlen (s2);

  if (s2_len > s1_len)
     return (FALSE);

  if (!_strnicmp (s1, s2, s2_len))
     return (TRUE);
  return (FALSE);
}

/**
 * Return TRUE if string `s1` ends with `s2`.
 */
bool str_endswith (const char *s1, const char *s2)
{
  const char *s1_end, *s2_end;

  if (strlen(s2) > strlen(s1))
     return (FALSE);

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
  static char dir [MG_PATH_MAX];

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
  dir[dirlen] = '\0';
  return (dir);
}

/**
 * Return a filename on Unix form:
 * All `\\` characters replaces with `/`.
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

#if MG_ENABLE_FILE
/*
 * Internals of 'externals/mongoose.c':
 */
typedef struct dirent {
        char d_name [MAX_PATH];
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
 * Touch a file to current time.
 */
int touch_file (const char *file)
{
  return _utime (file, NULL);
}

/**
 * Touch all files in a directory to current time.
 * Works reqursively if `recurse == true`.
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
    char  full_name [MAX_PATH];
    DWORD attrs;
    bool  is_dir;

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

/**
 * Use 64-bit tick-time for Mongoose?
 */
#if MG_ENABLE_CUSTOM_MILLIS
uint64_t mg_millis (void)
{
  return MSEC_TIME();
}
#endif


/**
 * Parse and split a `host[:port]` string into a host and port.
 * Set default port if the `:port` is missing.
 */
void set_host_port (const char *host_port, net_service *serv, uint16_t def_port)
{
  mg_str  str;
  mg_addr addr;
  char    buf [100];
  int     is_ip6 = -1;

  str = mg_url_host (host_port);
  memset (&addr, '\0', sizeof(addr));
  addr.port = mg_url_port (host_port);
  if (addr.port == 0)
     addr.port = def_port;

  if (mg_aton(str, &addr))
  {
    is_ip6 = addr.is_ip6;
    mg_ntoa (&addr, buf, sizeof(buf));
  }
  else
  {
    strncpy (buf, str.ptr, min(str.len, sizeof(buf)));
    buf [str.len] = '\0';
  }

  if (is_ip6 == -1 && strstr(host_port, "::"))
     printf ("Illegal address: '%s'. Try '[::ffff:a.b.c.d]:port' instead.\n", host_port);

  serv->host   = strdup (buf);
  serv->port   = addr.port;
  serv->is_ip6 = (is_ip6 == 1);
  DEBUG (DEBUG_NET, "is_ip6: %d, host: %s, port: %u.\n", is_ip6, serv->host, serv->port);
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
     flags |= FLAG_ALLARGS;
  else
  if (posixly_correct || *options == '+')
     flags &= ~FLAG_PERMUTE;

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
