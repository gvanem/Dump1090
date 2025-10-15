/**\file    color.c
 * \ingroup Misc
 *
 * \brief
 * Print to console using embedded colour-codes inside the string-format.
 * E.g.
 *   \code{.c}
 *     C_printf ("~4Hello ~2world~0.\n");
 *   \endcode
 *
 * will print to stdout with `Hello` mapped to colour 4
 * and `world` mapped to colour 2.
 * See the `colour_map[]` array below.
 *
 * By default, the colour indices maps to these foreground colour:
 * + 0: the startup forground *and* background colour.
 * + 1: bright cyan foreground.
 * + 2: bright green foreground.
 * + 3: bright yellow foreground.
 * + 4: bright magenta foreground.
 * + 5: bright red foreground.
 * + 6: bright white foreground.
 * + 7: dark cyan foreground.
 * + 8: white on bright red background (not yet).
 */
#include "color.h"

#define VALID_CH(c)   ((c) >= -1 && (c) <= 255)

#define loBYTE(w)     (BYTE)(w)
#define hiBYTE(w)     (BYTE)((WORD)(w) >> 8)

#define FATAL(fmt, ...)                             \
        do {                                        \
          fprintf (stderr, "\nFATAL: %s(%u): " fmt, \
                   __FILE__, __LINE__,              \
                   ## __VA_ARGS__);                 \
          if (IsDebuggerPresent())                  \
               abort();                             \
          else ExitProcess (GetCurrentProcessId()); \
        } while (0)

#ifndef C_BUF_SIZE
#define C_BUF_SIZE 2048
#endif

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#ifndef DISABLE_NEWLINE_AUTO_RETURN
#define DISABLE_NEWLINE_AUTO_RETURN 0x0008
#endif

static bool  c_use_colours      = true;
static bool  c_use_ansi_colours = false;
static bool  c_raw_mode         = false;

static char  c_buf [C_BUF_SIZE];
static char *c_head = NULL, *c_tail = NULL;
static FILE *c_out = NULL;

static CRITICAL_SECTION c_crit;

static void C_flush (void);

/** The console-buffer information initialised by
 *  `GetConsoleScreenBufferInfo()` in `C_init()`.
 */
static CONSOLE_SCREEN_BUFFER_INFO console_info;


/** The handle for `STD_OUTPUT_HANDLE`.
 *  If `stdout` output is redirected, this will remain at `INVALID_HANDLE_VALUE`.
 */
static HANDLE c_handle = INVALID_HANDLE_VALUE;

/**
 * Array of colour indices to WinCon colour values (foreground and background combined).
 * Set in C_init().
 */
static WORD colour_map [10];

/**
 * Array of colour indices to ANSI-sequences (foreground and background combined).
 *
 * This map is set when the colour_map[] is set.
 */
static char colour_map_ansi [DIM(colour_map)] [20];

static const char *wincon_to_ansi (WORD col);

/**
 * Customize the `colour_map [1..N]`.
 * Must be a list terminated by 0.
 *
 * `colour_map[0]` can \b not be modified. It is reserved for the default
 * colour. I.e. the active colour in effect when program started.
 */
#if defined(__clang__)
  #pragma clang diagnostic push
  #pragma clang diagnostic ignored "-Wvarargs"
#endif

static int C_init_colour_map (unsigned short col, ...)
{
  uint8_t i;
  va_list args;

  va_start (args, col);

  colour_map[0] = console_info.wAttributes;
  i = 1;

  while (col && i < DIM(colour_map))
  {
    colour_map [i] = col;
    i++;
    col = (WORD) va_arg (args, int);
  }

  if (i == DIM(colour_map))
     FATAL ("'colour_map[]' has room for maximum %d values.\n", i);

  /* Set the rest to default colours in case not all elements was filled.
   */
  while (i < DIM(colour_map))
  {
    col = console_info.wAttributes;
    colour_map [i++] = col;
  }

  /**
   * Fill the ANSI-sequence array by looping over colour_map_ansi[].
   * \note the size of both colour_map_ansi[] and colour_map[] \b are equal.
   */
  for (i = 0; i < DIM(colour_map_ansi); i++)
  {
    const char *p = wincon_to_ansi (colour_map[i]);

    strncpy (colour_map_ansi[i], p, sizeof(colour_map_ansi[i]));
  }
  return (1);
}

#if defined(__clang__)
  #pragma clang diagnostic pop
#endif

/**
 * The global exit function.
 * Flushes the output buffer and deletes the critical section.
 */
static void C_exit (void)
{
  if (c_use_ansi_colours)
  {
#if 0
    c_raw_mode = true;
    C_puts (colour_map_ansi[0]);
    c_raw_mode = false;
#else
    fputs (colour_map_ansi[0], c_out);
#endif
  }
  else
  {
    TRACE ("console_info.wAttributes: 0x%04X\n", console_info.wAttributes);
    SetConsoleTextAttribute (c_handle, console_info.wAttributes);
  }

  if (c_out)
     C_flush();
  if (c_tail)
     DeleteCriticalSection (&c_crit);   /* C_init() was called */

  c_head = c_tail = NULL;
  c_out  = NULL;
  c_handle = INVALID_HANDLE_VALUE;
}

/**
 * Our local initialiser function. Called once to:
 *
 *  + Get the console-buffer information from Windows Console.
 *  + If the console is not redirected:
 *      1. get the screen height and width.
 *      2. setup the colour_map[] array and the
 *         colour_map_ansi[] array. Even if ANSI output is \b not wanted.
 *  + Set c_out to default `stdout` and setup buffer head and tail.
 *  + Initialise the critical-section structure `c_crit`.
 */
static void C_init (void)
{
  bool okay;

  if (c_head && c_out)  /* already done this */
     return;

  c_handle = GetStdHandle (STD_OUTPUT_HANDLE);
  okay = (c_handle != INVALID_HANDLE_VALUE &&
          GetConsoleScreenBufferInfo(c_handle, &console_info) &&
          GetFileType(c_handle) == FILE_TYPE_CHAR);

  if (okay)
  {
    WORD bg = console_info.wAttributes & ~7;

    C_init_colour_map ((bg + 3) | FOREGROUND_INTENSITY,    /* "~1" -> bright cyan */
                       (bg + 2) | FOREGROUND_INTENSITY,    /* "~2" -> bright green */
                       (bg + 6) | FOREGROUND_INTENSITY,    /* "~3" -> bright yellow */
                       (bg + 5) | FOREGROUND_INTENSITY,    /* "~4" -> bright magenta */
                       (bg + 4) | FOREGROUND_INTENSITY,    /* "~5" -> bright red */
                       (bg + 7) | FOREGROUND_INTENSITY,    /* "~6" -> bright white */
                       (bg + 3),                           /* "~7" -> dark cyan */
                       (16*4 + 7) | FOREGROUND_INTENSITY,  /* "~8" -> white on red background */
                       0);
  }
  else
    c_use_colours = false;

  if (c_use_colours)
  {
    DWORD mode = 0;
    BOOL  rc;

    if (GetConsoleMode (c_handle, &mode) && (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING))
    {
      c_use_ansi_colours = true;

      mode |= ENABLE_LVB_GRID_WORLDWIDE | ENABLE_PROCESSED_OUTPUT |
              ENABLE_WRAP_AT_EOL_OUTPUT | DISABLE_NEWLINE_AUTO_RETURN;

      rc = SetConsoleMode (c_handle, mode);
      TRACE ("Has VT-mode. Setting mode: 0x%08lX, rc: %d\n", mode, rc);

      SetConsoleOutputCP (CP_UTF8);
    }
  }

  c_out  = stdout;
  c_head = c_buf;
  c_tail = c_head + C_BUF_SIZE - 1;
  InitializeCriticalSection (&c_crit);

  atexit (C_exit);
}

/**
 * Set console foreground and optionally background color.
 * FG is in the low 4 bits.
 * BG is in the upper 4 bits of the BYTE.
 * If 'col == 0', set default console colour.
 */
static void C_set_col (WORD col)
{
  static WORD last_attr = (WORD)-1;
  WORD   attr;

  if (col == 0)     /* restore to default colour */
     attr = console_info.wAttributes;

  else
  {
    BYTE fg, bg;

    attr = col;
    fg   = loBYTE (attr);
    bg   = hiBYTE (attr);

    if (bg == (BYTE)-1)
    {
      attr = console_info.wAttributes & ~7;
      attr &= ~8;     /* Since 'wAttributes' could have been hi-intensity at startup. */
    }
    else
      attr = (WORD) (bg << 4);

    attr |= fg;
  }

  if (attr != last_attr)
     SetConsoleTextAttribute (c_handle, attr);
  last_attr = attr;
}

/**
 * Create an ANSI-sequence array.
 *
 * \param[in] col the Windows colour to map to a corresponding ANSI sequence.
 * \return    the ANSI sequence.
 */
static const char *wincon_to_ansi (WORD col)
{
  static char ret [20];  /* max: "\x1B[30;1;40;1m" == 12 */
  static BYTE wincon_to_SGR [8] = { 0, 4, 2, 6, 1, 5, 3, 7 };
  BYTE   fg, bg, SGR;
  bool   bold;
  char  *p   = ret;
  char  *end = ret + sizeof(ret);

  if (col == 0)
     return ("\x1B[0m");

  fg  = col & 7;
  SGR = wincon_to_SGR [fg & ~FOREGROUND_INTENSITY];
  bold = (col & FOREGROUND_INTENSITY);

  if (bold)
       p += snprintf (p, end - p, "\x1B[%d;1m", 30 + SGR);
  else p += snprintf (p, end - p, "\x1B[%dm", 30 + SGR);

  bold = (col & BACKGROUND_INTENSITY);
  bg   = ((BYTE)col & ~BACKGROUND_INTENSITY) >> 4;
  if (bg && bg != (console_info.wAttributes >> 4))
  {
    SGR = wincon_to_SGR [bg];
    if (bold)
         snprintf (p - 1, end - p + 1, ";%d;1m", 40 + SGR);
    else snprintf (p - 1, end - p + 1, ";%dm", 40 + SGR);
  }
  return (ret);
}

/**
 * Set console colour using an ANSI sequence.
 * The corresponding WinCon colour set in `colour_map[]` is used as a lookup-value.
 */
static void C_set_ansi (unsigned short col)
{
  uint8_t i;

  c_raw_mode = true;
  for (i = 0; i < DIM(colour_map); i++)
      if (col == colour_map[i])
      {
#if 0
        C_puts (colour_map_ansi[i]);
#else
        fputs (colour_map_ansi[i], c_out);
#endif
        break;
      }
  c_raw_mode = false;
}

/**
 * Change colour using ANSI or WinCon API.
 * Does nothing if console is redirected.
 */
static void C_set_colour (unsigned short col)
{
  if (c_use_ansi_colours)
     C_set_ansi (col);
  else if (c_use_colours)
     C_set_col (col);
}

/**
 * Write out our buffer.
 */
static void C_flush (void)
{
  size_t to_write = (unsigned int) (c_head - c_buf);

  if (to_write == 0)
     return;

  assert (c_out);

  EnterCriticalSection (&c_crit);

#if 0
  _write (_fileno(c_out), c_buf, (unsigned int)to_write);
#else
  fputs (c_buf, c_out);
#endif

  /* restart buffer
   */
  c_head = c_buf;
  LeaveCriticalSection (&c_crit);
}

/**
 * An printf() style console print function.
 */
int C_printf (_Printf_format_string_ const char *fmt, ...)
{
  int     len;
  va_list args;

  C_init();

  va_start (args, fmt);
  len = C_vprintf (fmt, args);
  va_end (args);
  return (len);
}

/**
 * A var-arg style console print function.
 */
int C_vprintf (const char *fmt, va_list args)
{
  int len1, len2;

  if (c_raw_mode)
  {
    C_flush();
    len1 = vfprintf (c_out, fmt, args);
    fflush (c_out);
  }
  else
  {
    char buf [2*C_BUF_SIZE];

    len2 = vsnprintf (buf, sizeof(buf)-1, fmt, args);
    len1 = C_puts (buf);
    if (len2 < len1)
       FATAL ("len1: %d, len2: %d. c_buf: '%.*s',\nbuf: '%s'\n",
              len1, len2, (int)(c_head - c_buf), c_buf, buf);
  }
  return (len1);
}

/**
 * Put a single character to output buffer (at `c_head`).
 * Interpret a "~n" sequence as output buffer gets filled.
 */
int C_putc (int ch)
{
  static  bool get_color = false;
  uint8_t i;
  int     rc = 0;

  C_init();

  assert (c_head);
  assert (c_tail);
  assert (c_head >= c_buf);
  assert (c_head <= c_tail);

  if (!c_raw_mode)
  {
    if (get_color)
    {
      WORD color;

      get_color = false;
      if (ch == '~')
         goto put_it;

      i = ch - '0';
      if (i >= 0 && i < DIM(colour_map))
         color = colour_map [i];
      else
         FATAL ("Illegal color index %d ('%c'/0x%02X) in c_buf: '%.*s'\n",
                i, ch, ch, (int)(c_head - c_buf), c_buf);

      C_flush();
      C_set_colour (color);
      return (0);
    }

    if (ch == '~')
    {
      get_color = true;   /* change state; get colour index in next char */
      return (0);
    }
  }

put_it:
  *c_head++ = (char) ch;
  rc++;

  if (ch == '\n' || c_head >= c_tail)
     C_flush();
  return (rc);
}

/**
 * Put a 0-terminated string to output buffer.
 */
int C_puts (const char *str)
{
  int ch, rc = 0;

  for (rc = 0; (ch = *str) != '\0'; str++)
      rc += C_putc (ch);
  return (rc);
}

#if defined(TEST)

global_data Modes;

void modeS_flogf (FILE *f, _Printf_format_string_ const char *fmt, ...)
{
  char    buf [1000];
  va_list args;

  va_start (args, fmt);
  vsnprintf (buf, sizeof(buf), fmt, args);
  va_end (args);
  fputs (buf, stdout);
  (void) f;
}

/*
 * Rewritten example from:
 *   https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
 */
#define ESC "\x1b"
#define CSI "\x1b["

static bool enable_VT_mode (HANDLE *hnd)
{
  DWORD mode = 0;

  // Set output mode to handle virtual terminal sequences
  *hnd = GetStdHandle (STD_OUTPUT_HANDLE);
  if (*hnd == INVALID_HANDLE_VALUE)
     return (false);

  if (!GetConsoleMode(*hnd, &mode))
     return (false);

  mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  if (!SetConsoleMode(*hnd, mode))
     return (false);
  return (true);
}

static void print_vertical_border (void)
{
  printf (ESC "(0");       // Enter Line drawing mode
  printf (CSI "104;93m");  // bright yellow on bright blue
  printf ("x");            // in line drawing mode, \x78 -> \u2502 "Vertical Bar"
  printf (CSI "0m");       // restore color
  printf (ESC "(B");       // exit line drawing mode
}

static void print_horizontal_border (const COORD *size, bool top)
{
  printf (ESC "(0");          // Enter Line drawing mode
  printf (CSI "104;93m");     // Make the border bright yellow on bright blue
  printf (top ? "l" : "m");   // print left corner

  for (int i = 1; i < size->X - 1; i++)
      printf ("q");           // in line drawing mode, \x71 -> \u2500 "HORIZONTAL SCAN LINE-5"

  printf (top ? "k" : "j");   // print right corner
  printf (CSI "0m");
  printf (ESC "(B");          // exit line drawing mode
}

static void print_status_line (const char *message, const COORD *size)
{
  printf (CSI "%d;1H", size->Y);
  printf (CSI "K");    // clear the line
  printf ("%s", message);
}

int main (void)
{
  HANDLE hnd;
  bool   ok = enable_VT_mode (&hnd);

  if (!ok)
  {
    printf ("Unable to enter VT processing mode. Quitting.\n");
    return (-1);
  }

  CONSOLE_SCREEN_BUFFER_INFO ScreenBufferInfo;
  int   line, num_lines, num_tab_stops;
  COORD size;

  memset (&size, '\0', sizeof(size));
  GetConsoleScreenBufferInfo (hnd, &ScreenBufferInfo);

  size.X = ScreenBufferInfo.srWindow.Right - ScreenBufferInfo.srWindow.Left + 1;
  size.Y = ScreenBufferInfo.srWindow.Bottom - ScreenBufferInfo.srWindow.Top + 1;

  // Enter the alternate buffer
  printf (CSI "?1049h");

  // Clear screen, tab stops, set, stop at columns 16, 32
  printf (CSI "1;1H");
  printf (CSI "2J");      // Clear screen

  num_tab_stops = 4;      // (0, 20, 40, width)
  printf (CSI "3g");      // clear all tab stops
  printf (CSI "1;20H");   // Move to column 20
  printf (ESC "H");       // set a tab stop

  printf (CSI "1;40H");   // Move to column 40
  printf (ESC "H");       // set a tab stop

  // Set scrolling margins to 3, h-2
  printf (CSI "3;%dr", size.Y - 2);

  num_lines = size.Y - 4;

  printf (CSI "1;1H");
  printf (CSI "102;30m");
  printf ("Windows 10 Anniversary Update - VT Example");
  printf (CSI "0m");

  // Print a top border - Yellow
  printf (CSI "2;1H");
  print_horizontal_border (&size, true);

  // Print a bottom border
  printf (CSI "%d;1H", size.Y - 1);
  print_horizontal_border (&size, false);

  // draw columns
  printf (CSI "3;1H");

  for (line = 0; line < num_lines * num_tab_stops; line++)
  {
    print_vertical_border();
    if (line + 1 != num_lines * num_tab_stops)  // don't advance to next line if this is the last line
       printf ("\t");                           // advance to next tab stop
  }

  print_status_line ("Press any key to see text printed between tab stops.", &size);
  _getwch();

  // Fill columns with output
  printf (CSI "3;1H");
  for (line = 0; line < num_lines; line++)
  {
    int tab = 0;
    for (tab = 0; tab < num_tab_stops - 1; tab++)
    {
      print_vertical_border();
      printf ("line=%d", line);
      printf ("\t");             // advance to next tab stop
    }
    print_vertical_border();     // print border at right side
    if (line + 1 != num_lines)
       printf ("\t");            // advance to next tab stop, (on the next line)
  }

  print_status_line ("Press any key to demonstrate scroll margins", &size);
  _getwch();

  printf (CSI "3;1H");
  for (line = 0; line < num_lines * 2; line++)
  {
    printf (CSI "K"); // clear the line

    for (int tab = 0; tab < num_tab_stops - 1; tab++)
    {
      print_vertical_border();
      printf ("line=%d", line);
      printf ("\t");           // advance to next tab stop
    }
    print_vertical_border();   // print border at right side
    if (line + 1 != num_lines * 2)
    {
      printf ("\n");           // Advance to next line. If we're at the bottom of the margins, the text will scroll.
      printf ("\r");           // return to first col in buffer
    }
  }

  print_status_line ("Press any key to exit", &size);
  _getwch();

  // Exit the alternate buffer
  printf (CSI "?1049l");
  return (0);
}
#endif
