/**
 * \file    interactive.c
 * \ingroup Misc
 * \brief   Function for interactive mode.
 *
 * Using either Windows-Console or Curses functions.
 *
 * \note For VT-sequences, refer:
 *  https://gist.github.com/fnky/458719343aabd01cfb17a3a4f7296797
 */
#include <io.h>
#include <conio.h>
#include <locale.h>

#include "interactive.h"
#include "aircraft.h"
#include "airports.h"
#include "smartlist.h"
#include "net_io.h"
#include "misc.h"
#include "externals/AirSpy/airspy.h"
#include "externals/SDRplay/sdrplay.h"

#undef MOUSE_MOVED
#include <curses.h>

extern SCREEN *SP;       /* in '$(CURSES_ROOT)/initscr.c' */
extern bool    pdc_wt;   /* in '$(CURSES_ROOT)/pdcscrn.c' */

typedef enum colours {
        COLOUR_DEFAULT = 0,
        COLOUR_WHITE,
        COLOUR_GREEN,
        COLOUR_RED,
        COLOUR_YELLOW,
        COLOUR_HEADER1,
        COLOUR_HEADER2,
        COLOUR_MAX
      } colours;

typedef struct colour_mapping {
        int    pair;    /* Not used in 'wincon_*()' functions */
        chtype attrib;
      } colour_mapping;

/**
 * \note
 * Using `_Printf_format_string_` or `ATTR_PRINTF()` on
 * function-pointers have no effect.
 */
typedef void (*print_xy_format)  (int x, int y, const char *fmt, ...);
typedef void (*print_xy_wformat) (int x, int y, const wchar_t *fmt, ...);

typedef struct API_funcs {
        bool             (*init) (void);
        void             (*exit) (void);
        void             (*set_colour) (enum colours colour);
        void             (*set_cursor) (bool enable);
        void             (*clr_scr) (void);
        void             (*clr_eol) (void);
        void             (*gotoxy) (int x, int y);
        wint_t           (*getch) (void);
        int              (*get_curx) (void);
        int              (*get_cury) (void);
        void             (*print_header) (void);
        void             (*hilight_header) (int x, int field);
        void             (*refresh) (int x, int y);
        print_xy_format  print_format;
        print_xy_wformat print_wformat;
      } API_funcs;

static bool   wincon_init (void);
static void   wincon_exit (void);
static void   wincon_set_colour (enum colours colour);
static void   wincon_set_cursor (bool enable);
static void   wincon_gotoxy (int x, int y);
static wint_t wincon_getch (void);
static int    wincon_get_curx (void);
static int    wincon_get_cury (void);
static void   wincon_clreol (void);
static void   wincon_clrscr (void);
static void   wincon_refresh (int x, int y);
static void   wincon_print_header (void);
static void   wincon_print_format (int x, int y, const char *fmt, ...);
static void   wincon_print_wformat (int x, int y, const wchar_t *fmt, ...);
static void   wincon_hilight_header (int x, int field);

static CONSOLE_SCREEN_BUFFER_INFO  con_info_out;
static CONSOLE_SCREEN_BUFFER_INFO  con_info_in;
static colour_mapping              colour_map [COLOUR_MAX];

static HANDLE  con_in  = INVALID_HANDLE_VALUE;
static HANDLE  con_out = INVALID_HANDLE_VALUE;
static DWORD   con_in_mode;
static DWORD   con_out_mode;
static bool    vt_out_supported, vt_in_supported, test_mode;
static int     x_scale, y_scale;
static HWND    con_wnd;
static char    spinner[] = "|/-\\";

/*
 * List of API function for the TUI (text user interface).
 */
static API_funcs *api = NULL;

/*
 * Or use CreatePseudoConsole() instead?
 *   https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session
 */
static WINDOW *stats_win  = NULL;
static WINDOW *flight_win = NULL;

static bool   curses_init (void);
static void   curses_exit (void);
static void   curses_set_colour (enum colours colour);
static void   curses_set_cursor (bool enable);
static void   curses_gotoxy (int x, int y);
static wint_t curses_getch (void);
static int    curses_get_curx (void);
static int    curses_get_cury (void);
static void   curses_refresh (int x, int y);
static void   curses_print_header (void);
static void   curses_print_format (int x, int y, const char *fmt, ...);
static void   curses_print_wformat (int x, int y, const wchar_t *fmt, ...);
static void   curses_hilight_header (int x, int field);
static bool   interactive_test_loop (void);
static bool   mouse_pos (HWND wnd, POINT *pos);
static bool   mouse_lclick (void);

static API_funcs curses_api = {
      .init           = curses_init,
      .exit           = curses_exit,
      .set_colour     = curses_set_colour,
      .set_cursor     = curses_set_cursor,
      .clr_scr        = (void (*)(void)) clear,
      .clr_eol        = (void (*)(void)) clrtoeol,
      .gotoxy         = curses_gotoxy,
      .getch          = curses_getch,
      .get_curx       = curses_get_curx,
      .get_cury       = curses_get_cury,
      .print_header   = curses_print_header,
      .print_format   = curses_print_format,
      .print_wformat  = curses_print_wformat,
      .hilight_header = curses_hilight_header,
      .refresh        = curses_refresh
     };

static API_funcs wincon_api = {
      .init           = wincon_init,
      .exit           = wincon_exit,
      .set_colour     = wincon_set_colour,
      .set_cursor     = wincon_set_cursor,
      .clr_scr        = wincon_clrscr,
      .clr_eol        = wincon_clreol,
      .gotoxy         = wincon_gotoxy,
      .getch          = wincon_getch,
      .get_curx       = wincon_get_curx,
      .get_cury       = wincon_get_cury,
      .print_header   = wincon_print_header,
      .print_format   = wincon_print_format,
      .print_wformat  = wincon_print_wformat,
      .hilight_header = wincon_hilight_header,
      .refresh        = wincon_refresh
    };

/*
 * Show the "DEP  DST" columns if we have a good `Modes.airport_db` file.
 */
static bool show_dep_dst = false;

/*
 * Trace for 'test_mode'
 */
#undef  TRACE
#define TRACE(fmt, ...) do {                                  \
                          if (test_mode)                      \
                             fprintf (stderr, "%s(%u): " fmt, \
                                      __FILE__, __LINE__,     \
                                      __VA_ARGS__);           \
                        } while (0)

/*
 * Use this header and `snprintf()` format for both `show_dep_dst == [false | true]`.
 */
#define HEADER    "ICAO   Callsign  Reg-num  Cntry  %sAltitude  Speed   Lat      Long    Hdg   Dist   Msg Seen %c"
                                                 /* |__ == "DEP  DST  " -- if 'show_dep_dst == true' */

/*
 * Use this format for `show_dep_dst == false`.
 */
#define LINE_FMT1  "%06X %-9.9s %-8s %-5.5s     %-5s   %-5s %-7s %-8s %4.4s  %5.5s %5u %3llu s "
//                  |    |      |    |          |      |    |    |    |      |     |   |__ ms_diff / 1000
//                  |    |      |    |          |      |    |    |    |      |     |__ a->messages
//                  |    |      |    |          |      |    |    |    |      |__ distance_buf
//                  |    |      |    |          |      |    |    |    |__ heading_buf
//                  |    |      |    |          |      |    |    |__ lon_buf
//                  |    |      |    |          |      |    |__ lat_buf
//                  |    |      |    |          |      |__ speed_buf
//                  |    |      |    |          |__ alt_buf
//                  |    |      |    |___ country
//                  |    |      |____ reg_num
//                  |    |__ call_sign
//                  |__ a->addr

/*
 * Use these formats for `show_dep_dst == true`.
 */
#define LINE_FMT2_1  "%06X %-9.9s %-8s %-5.5s  %-20s"
//                    |    |      |    |       |
//                    |    |      |    |       |
//                    |    |      |    |       |
//                    |    |      |    |       |
//                    |    |      |    |       |
//                    |    |      |    |       |
//                    |    |      |    |       |
//                    |    |      |    |       |
//                    |    |      |    |       |___ dep_dst_buf
//                    |    |      |    |___ country
//                    |    |      |____ reg_num
//                    |    |__ call_sign
//                    |__ a->addr

#define LINE_FMT2_2  "%-5s   %-5s %-7s %-8s %4.4s  %5.5s %5u %3llu s "
//                    |      |    |    |    |      |     |   |__ ms_diff / 1000
//                    |      |    |    |    |      |     |__ a->messages
//                    |      |    |    |    |      |__ distance_buf
//                    |      |    |    |    |__ heading_buf
//                    |      |    |    |__ lon_buf
//                    |      |    |__ lat_buf
//                    |      |__ speed_buf
//                    |__ alt_buf

#define DEP_DST_COLUMNS  "DEP  DEST  "

#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif

static const char *headers[] = {
                  "ICAO   ",  "Callsign  ", "Reg-num  ", "Cntry  ",
                  "DEP  DST   ", "Altitude  ", "Speed ",
                  "  Lat    ", "  Long   ", " Hdg  ", " Dist   ",
                  "Msg ", "Seen "
                };

#if 0
/**
 * \todo
 * define the column header and width dynamically at runtime.
 * Pseudo-code:
 */
typedef struct table {
     // TBD ...
      } table;

typedef struct table_row {
        const char *header;
        const char *format;
        int         max_len;
        int         flags;
      } table_row;

static void show_dep (const table *t, const aircraft *a);
static void show_dst (const table *t, const aircraft *a);

static const table_row headers1[] = {
                  { "ICAO",     "%06X",    6 },
                  { "Callsign", "%s",      0 },
                  { "Reg-num",  "%s",      0 },
                  { "Cntry",    "%s",      0 },
                  { "DEP",      (char*)show_dep, 0, ROW_FUNC },
                  { "DST",      (char*)show_dst, 0, ROW_FUNC },
                  { "Altitude", "%s",      0 },
                  { "Speed",    "%4u",     4 },
                  { "Lat",      "%+7.3lf", 7 },
                  { "Long",     "%+6.3lf", 6 },
                  { "Hdg",      "%d",      0 },
                  { "Dist",     "%.1lf",   0 },
                  { "Msg",      "%5u",     5 },
                  { "Seen",     "%2d",     2 },
                };

static table *g_table;

// Add this ..
static bool common_init (void)
{
  g_table = table_create();

  if (show_dep_dst)
       table_header (g_table, headers1);
  else table_header (g_table, headers2);
}

static void common_exit (void)
{
  table_delete (g_table);
  g_table = NULL;
}

static void show_one_aircraft (aircraft *a, int row, const POINT *mouse, uint64_t now)
{
  int column;

  for (column = 0; column < table_width(g_table); x++)
  {
    if (table_hit_test(column, row, mouse->x, mouse->y))
         table_hilight (g_table, column, row, a);
    else table_show (g_table, column, row, a);
  }
}
#endif

static void curses_hilight_header (int x, int field)
{
  curses_set_colour (COLOUR_HEADER2);
  mvaddstr (0, x, headers[field]);
  curses_set_colour (0);
}

/**
 * Print a `header' field in highlighted background or underline colour
 * when mouse is hovering on a header field or clicked.
 */
static void wincon_hilight_header (int x, int field)
{
  const char *bright = "\x1B[1;37;4;40m";
  char        buf [50];
  int         len;

  wincon_gotoxy (x, 0);
  len = snprintf (buf, sizeof(buf), "%s%s\x1B[m", bright, headers[field]);
  WriteConsoleA (con_out, buf, len, NULL, NULL);
}

static a_sort_t field_to_sort [DIM(headers)] = {
       INTERACTIVE_SORT_ICAO,
       INTERACTIVE_SORT_CALLSIGN,
       INTERACTIVE_SORT_REGNUM,
       INTERACTIVE_SORT_COUNTRY,
       INTERACTIVE_SORT_DEP_DEST,   /* not implemented */
       INTERACTIVE_SORT_ALTITUDE,
       INTERACTIVE_SORT_SPEED,
       INTERACTIVE_SORT_DISTANCE,
       INTERACTIVE_SORT_MESSAGES,
       INTERACTIVE_SORT_SEEN
     };

static bool mouse_header_check (const POINT *pos, bool clicked)
{
  int field, x, s;
  static int old_x = -1;

  if (pos->x == -1 || pos->y != 0) /* Mouse not at HEADER */
     return (false);

  if (pos->x == old_x && !clicked)
     return (false);

  old_x = pos->x;

  for (field = x = 0; field < DIM(headers); field++)
  {
    if (pos->x >= x && pos->x <= (LONG) strlen (headers[field]) + x)
    {
      (*api->hilight_header) (x, field);

      if (clicked)
      {
        s = field_to_sort [field];
        if (Modes.a_sort == s)     /* toggle ascending/descending sort */
           s = -s;
        aircraft_sort (s);
        LOG_FILEONLY ("field: %d (%s), clicked: 1, Modes.a_sort: %s\n",
                      field, headers[field], aircraft_sort_name(Modes.a_sort));
      }
      else
      {
        LOG_FILEONLY ("field: %d (%s), clicked: 0\n", field, headers[field]);
      }
      return (true);
    }
    x += strlen (headers[field]);
  }
  return (false);
}

/**
 * Common initialiser for both WinCon and Curses.
 */
static bool common_init (void)
{
  uint32_t num;

  if (airports_num(&num) && num > 0)
     show_dep_dst = true;

  if (_isatty(STDOUT_FILENO) == 0)
  {
    if (!test_mode)
    {
      LOG_STDERR ("Do not redirect 'stdout' in interactive mode.\n"
                  "Do '%s [options] 2> file` instead.\n", Modes.who_am_I);
      return (false);
    }
  }

  /* Do `con_out'
   */
  con_out = GetStdHandle (STD_OUTPUT_HANDLE);
  if (con_out != INVALID_HANDLE_VALUE)
  {
    CONSOLE_FONT_INFO fi;
    COORD             coord;
    DWORD             con_out_new_mode;

    GetConsoleScreenBufferInfo (con_out, &con_info_out);
    GetConsoleMode (con_out, &con_out_mode);

    GetCurrentConsoleFont (con_out, FALSE, &fi);
    coord = GetConsoleFontSize (con_out, fi.nFont);
    x_scale = coord.X;
    y_scale = coord.Y;

    con_wnd = GetConsoleWindow();

    con_out_new_mode = con_out_mode & ~(ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE);
    con_out_new_mode |= (ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                         DISABLE_NEWLINE_AUTO_RETURN);

    if (SetConsoleMode(con_out, con_out_new_mode))
         vt_out_supported = (con_out_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    else TRACE ("SetConsoleMode(con_out) failed; %s\n", win_strerror(GetLastError()));
  }

  /* Do `con_in'
   */
  con_in = GetStdHandle (STD_INPUT_HANDLE);
  if (con_in != INVALID_HANDLE_VALUE)
  {
    DWORD con_in_new_mode;

    GetConsoleScreenBufferInfo (con_in, &con_info_in);

    GetConsoleMode (con_in, &con_in_mode);

    con_in_new_mode = con_in_mode & ~(ENABLE_ECHO_INPUT      |
                                      ENABLE_LINE_INPUT      |
                                      ENABLE_PROCESSED_INPUT |
                                   // ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                      0);

    /* If Quick Edit is enabled, it will mess with receiving mouse events.
     * Hence disable it.
     */
    con_in_new_mode &= ~ENABLE_QUICK_EDIT_MODE;

    con_in_new_mode |= (
                     // ENABLE_MOUSE_INPUT     |
                     // ENABLE_WINDOW_INPUT    |
                        ENABLE_PROCESSED_INPUT |
                        ENABLE_AUTO_POSITION   |
                        ENABLE_EXTENDED_FLAGS  |
                        ENABLE_VIRTUAL_TERMINAL_INPUT
                       );

    if (SetConsoleMode(con_in, con_in_new_mode))
         vt_in_supported = (con_in_new_mode & ENABLE_VIRTUAL_TERMINAL_INPUT);
    else TRACE ("SetConsoleMode(con_in) failed; %s\n", win_strerror(GetLastError()));
  }
  return (true);
}

static void common_exit (void)
{
  BOOL rc;

  if (con_out != INVALID_HANDLE_VALUE)
  {
    rc = SetConsoleMode (con_out, con_out_mode);
    if (!rc)
       TRACE ("SetConsoleMode(con_out) failed; %s\n", win_strerror(GetLastError()));
  }

  if (con_in != INVALID_HANDLE_VALUE)
  {
    rc = SetConsoleMode (con_in, con_in_mode | ENABLE_EXTENDED_FLAGS);
    if (!rc)
       TRACE ("SetConsoleMode(con_in) failed; %s\n", win_strerror(GetLastError()));
  }
  con_out = con_in = INVALID_HANDLE_VALUE;
}

/**
 * Check the UTF-8 output of 2 airports.
 *
 * "KEF" should translate to "Reykjavik" with an acute 'i'; U+00ED, as hex "C3 AD"
 * https://www.compart.com/en/unicode/U+00ED
 *
 * "ENFL" should translate to "Floro" with stoke across the 'o'; U+00F8, as hex "C3 B8"
 * https://www.compart.com/en/unicode/U+00F8
 */
static void test_utf8 (void)
{
  const char    *KEFa =  "Reykjav\xC3\xADk";  /* UTF-8 encoding */
  const wchar_t *KEFw = L"Reykjav\xEDk";      /* UTF-16 encoding */
  const char    *FLOa =  "Flor\xC3\xB8";      /* UTF-8 encoding */
  const wchar_t *FLOw = L"Flor\xF8";          /* UTF-16 encoding */
  int   y;

  setlocale (LC_ALL, ".utf8");

  y =  (*api->get_cury)();

  (*api->print_format) (0, y++, "Using TUI=%s, locale: %s\n",
                        Modes.tui_interface == TUI_CURSES ? "Curses" : "WinCon",
                        setlocale(LC_ALL, NULL));

  (*api->print_format) (0, y++, "KEFa: '%s'\n"
                                "FLOa: '%s'\n", KEFa, FLOa);
  y++;

  (*api->print_wformat) (0, y++, L"KEFw: '%s'\n"
                                  "FLOw: '%s'\n", KEFw, FLOw);
  y += 2;

  (*api->print_wformat) (0, y++, L"Wide:  %-20.20s: %S", KEFw, hex_dump(KEFw, 2*wcslen(KEFw)));
  (*api->print_wformat) (0, y++, L"Wide:  %-20.20s: %S", FLOw, hex_dump(FLOw, 2*wcslen(FLOw)));

  y++;
  (*api->print_format) (0, y++, "ASCII: %-20.20s: %s", KEFa, hex_dump(KEFa, strlen(KEFa)));
  (*api->print_format) (0, y++, "ASCII: %-20.20s: %s", FLOa, hex_dump(FLOa, strlen(FLOa)));
  (*api->gotoxy) (0, y+1);
}

/**
 * Send a message to the console-window to show our icon.
 * Thus showing it in the System-Menu and Task-bar.
 * But it's not showing up in the "Alt-Tab" window !?
 */
static void set_console_icon (HWND wnd)
{
  HANDLE icon;

  if (!Modes.console_icon)
     return;

  icon = LoadImage (GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APPICON), IMAGE_ICON, 0, 0, 0);
  if (icon && IsWindow(wnd))
     SendMessage (wnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
}

/**
 * Call `common_init()` and if Curses interface was
 * selected, set `api = &curses_api`.
 * Otherwise set `api = &wincon_api`.
 *
 * And call the TUI-specific init.
 */
bool interactive_init (void)
{
  bool rc1, rc2;

  test_mode = test_contains (Modes.tests, "console");

  assert (api == NULL);

  if (Modes.tui_interface == TUI_CURSES)
       api = &curses_api;
  else api = &wincon_api;

  rc1 = common_init();
  rc2 = (*api->init)();

  set_console_icon (con_wnd);

  (*api->set_cursor) (false);

  if (test_mode && rc2)
     test_utf8();

  if (!test_mode)
     (*api->clr_scr)();
  else
  {
    (*api->print_format) (0, (*api->get_cury)(),
             "rc1: %d, rc2: %d, TUI: %s, con_in_mode: 0x%08lX, con_out_mode: 0x%08lX\n"
             "con_info_out: Bottom: %4u, Top: %4u, Right: %4u, Left: %4u\n"
             "con_info_in:  Bottom: %4u, Top: %4u, Right: %4u, Left: %4u\n"
             "con_wnd: 0x%p, vt_out_supported: %d, vt_in_supported: %d\n\n",
             rc1, rc2, Modes.tui_interface == TUI_CURSES ? "Curses" : "WinCon",
             con_in_mode, con_out_mode,
             con_info_out.srWindow.Bottom, con_info_out.srWindow.Top,
             con_info_out.srWindow.Right,  con_info_out.srWindow.Left,
             con_info_in.srWindow.Bottom,  con_info_in.srWindow.Top,
             con_info_in.srWindow.Right,   con_info_in.srWindow.Left,
             con_wnd, vt_out_supported, vt_in_supported);

    return interactive_test_loop();
  }

  return (rc1 && rc2);
}

void interactive_exit (void)
{
  if (api)
  {
    (*api->set_cursor) (true);
    (*api->exit)();
  }
  api = NULL;
  common_exit();
}

void interactive_clreol (void)
{
  if (api)
    (*api->clr_eol)();
}

/**
 * Return a string showing this aircraft's distance to our home position.
 *
 * If `Modes.metric == true`, return it in kilo-meters. <br>
 * Otherwise Nautical Miles.
 */
static void get_home_distance (aircraft *a, const char **km_nmiles)
{
  double divisor = Modes.metric ? 1000.0 : 1852.0;

  if (km_nmiles)
     *km_nmiles = Modes.metric ? "km" : "Nm";

  if (a->distance > BIG_VAL)
       snprintf (a->distance_buf, sizeof(a->distance_buf), "%.0lf", a->distance / divisor);
  else if (a->distance > SMALL_VAL)
       snprintf (a->distance_buf, sizeof(a->distance_buf), "%.1lf", a->distance / divisor);
  else a->distance_buf[0] = '\0';
}

/**
 * As for `get_home_distance()`, but return the estimated distance.
 */
static void get_est_home_distance (aircraft *a, const char **km_nmiles)
{
  double divisor = Modes.metric ? 1000.0 : 1852.0;

  if (km_nmiles)
     *km_nmiles = Modes.metric ? "km" : "Nm";

  if (a->distance_EST > BIG_VAL)
       snprintf (a->distance_buf_EST, sizeof(a->distance_buf_EST), "%.0lf", a->distance_EST / divisor);
  else if (a->distance_EST > SMALL_VAL)
       snprintf (a->distance_buf_EST, sizeof(a->distance_buf_EST), "%.1lf", a->distance_EST / divisor);
  else a->distance_buf_EST[0] = '\0';
}

/*
 * Called every 250 msec (`MODES_INTERACTIVE_REFRESH_TIME`) while
 * in interactive mode to update the Console Windows Title.
 *
 * Called from `background_tasks()` in the main thread.
 */
void interactive_title_stats (void)
{
  #define GAIN_TOO_LOW   " (too low?)"
  #define GAIN_TOO_HIGH  " (too high?)"
  #define GAIN_ERASE     "            "

  char            buf [100];
  char            gain [10];
  static uint64_t last_good_CRC, last_bad_CRC;
  static int      overload_count = 0;
  static char    *overload = GAIN_ERASE;
  uint64_t        good_CRC = Modes.stat.CRC_good + Modes.stat.CRC_fixed;
  uint64_t        bad_CRC  = Modes.stat.CRC_bad;

  if (Modes.gain_auto)
       strcpy (gain, "Auto");
  else snprintf (gain, sizeof(gain), "%.1f dB", (double)Modes.gain / 10.0);

  if (overload_count > 0)
  {
    if (--overload_count == 0)
       overload = GAIN_ERASE;
  }
  else if (bad_CRC - last_bad_CRC > 2*(good_CRC - last_good_CRC))
  {
    overload = GAIN_TOO_HIGH;
    overload_count = 4;     /* let it show for 4 periods (1 sec) */
  }
#if 0
  else if (bad_CRC == last_bad_CRC && good_CRC == last_good_CRC)
  {
    overload = GAIN_TOO_LOW;
    overload_count = 4;     /* let it show for 4 periods (1 sec) */
  }
#endif

  snprintf (buf, sizeof(buf), "Dev: %s. CRC: %llu / %llu. Gain: %s%s",
            Modes.selected_dev, good_CRC, bad_CRC, gain, overload);

  last_good_CRC = good_CRC;
  last_bad_CRC  = bad_CRC;

  SetConsoleTitleA (buf);
}

/*
 * Called from `background_tasks()` in the main thread.
 * This function does nothing when `Modes.tui_interface != TUI_CURSES`.
 */
void interactive_other_stats (void)
{
  if (stats_win)
  {
    /**
     * \todo
     * Fill the `stats_win' with some handy accumulated statistics.
     * Like number of unique planes, CSV/SQL-lookups and cache hits,
     * Number of network clients, bytes etc.
     */
    uint64_t sum1, sum2;

    sum1 = Modes.stat.HTTP_stat [0].HTTP_get_requests + Modes.stat.HTTP_stat [1].HTTP_get_requests;
    mvwprintw (stats_win, 20, 0, "HTTP GET:   %llu", sum1);

    sum1 = Modes.stat.bytes_sent[MODES_NET_SERVICE_HTTP4] + Modes.stat.bytes_sent[MODES_NET_SERVICE_HTTP6];
    sum2 = Modes.stat.bytes_recv[MODES_NET_SERVICE_HTTP4] + Modes.stat.bytes_recv[MODES_NET_SERVICE_HTTP6];
    mvwprintw (stats_win, 21, 0, "HTTP bytes: %llu/%llu", sum1, sum2);
  }

  /* Refresh the sub-window for flight-information.
   */
  if (flight_win)
  {
    /** \todo */
  }
}

void interactive_raw_SBS_stats (void)
{
  /** \todo */
}

static int gain_increase (int gain_idx)
{
  if (Modes.rtlsdr.device && gain_idx < Modes.rtlsdr.gain_count-1)
  {
    Modes.gain = Modes.rtlsdr.gains [++gain_idx];
    rtlsdr_set_tuner_gain (Modes.rtlsdr.device, Modes.gain);
    LOG_FILEONLY ("Increasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  else if (Modes.rtl_tcp_in && gain_idx < Modes.rtltcp.gain_count-1)
  {
    Modes.gain = Modes.rtltcp.gains [++gain_idx];
    rtl_tcp_set_gain (Modes.rtl_tcp_in, Modes.gain);
    LOG_FILEONLY ("Increasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  else if (Modes.sdrplay.device && gain_idx < Modes.sdrplay.gain_count-1)
  {
    Modes.gain = Modes.sdrplay.gains [++gain_idx];
    sdrplay_set_gain (Modes.sdrplay.device, Modes.gain);
    LOG_FILEONLY ("Increasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  else if (Modes.airspy.device && gain_idx < Modes.airspy.gain_count-1)
  {
    Modes.gain = Modes.airspy.gains [++gain_idx];
    airspy_set_gain (Modes.airspy.device, Modes.gain);
    LOG_FILEONLY ("Increasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  return (gain_idx);
}

static int gain_decrease (int gain_idx)
{
  if (Modes.rtlsdr.device && gain_idx > 0)
  {
    Modes.gain = Modes.rtlsdr.gains [--gain_idx];
    rtlsdr_set_tuner_gain (Modes.rtlsdr.device, Modes.gain);
    LOG_FILEONLY ("Decreasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  else if (Modes.rtl_tcp_in && gain_idx > 0)
  {
    Modes.gain = Modes.rtltcp.gains [--gain_idx];
    rtl_tcp_set_gain (Modes.rtl_tcp_in, Modes.gain);
    LOG_FILEONLY ("Decreasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  else if (Modes.sdrplay.device && gain_idx > 0)
  {
    Modes.gain = Modes.sdrplay.gains [--gain_idx];
    sdrplay_set_gain (Modes.sdrplay.device, Modes.gain);
    LOG_FILEONLY ("Decreasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  else if (Modes.airspy.device && gain_idx > 0)
  {
    Modes.gain = Modes.airspy.gains [--gain_idx];
    airspy_set_gain (Modes.airspy.device, Modes.gain);
    LOG_FILEONLY ("Decreasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  }
  return (gain_idx);
}

/**
 * Poll for '+/-' keypresses and adjust the RTLSDR / SDRplay / AirSpy gain accordingly.
 * But within the min/max gain settings for the device.
 */
void interactive_update_gain (void)
{
  static int gain_idx = -1;
  int    i, ch;

  if (gain_idx == -1)
  {
    for (i = 0; i < Modes.rtlsdr.gain_count; i++)
        if (Modes.gain == Modes.rtlsdr.gains[i])
        {
          gain_idx = i;
          break;
        }
    if (Modes.sdrplay.device)
       gain_idx = Modes.sdrplay.gain_count / 2;
    else if (Modes.airspy.device)
       gain_idx = Modes.airspy.gain_count / 2;
  }

  if (!_kbhit())
     return;

  ch = _getch();

  /* If we have auto-gain enabled, switch to manual gain
   * on a '-' or '+' keypress. Start with the middle gain-value.
   */
  if (Modes.gain_auto && (ch == '-' || ch == '+'))
  {
    LOG_FILEONLY ("Gain: AUTO -> manual.\n");
    Modes.gain_auto = false;

    if (Modes.rtlsdr.device)
    {
      rtlsdr_set_tuner_gain_mode (Modes.rtlsdr.device, 1);
      gain_idx = Modes.rtlsdr.gain_count / 2;
    }
    else if (Modes.rtl_tcp_in)
    {
      rtl_tcp_set_gain_mode (Modes.rtl_tcp_in, 1);
      gain_idx = Modes.rtltcp.gain_count / 2;
    }
    else if (Modes.sdrplay.device)
    {
      sdrplay_set_gain (Modes.sdrplay.device, 0);
      gain_idx = Modes.sdrplay.gain_count / 2;
    }
    else if (Modes.airspy.device)
    {
      airspy_set_gain (Modes.airspy.device, 0);
      gain_idx = Modes.airspy.gain_count / 2;
    }
  }

  if (ch == '+')
     gain_idx = gain_increase (gain_idx);
  else if (ch == '-')
     gain_idx = gain_decrease (gain_idx);
  else if (toupper(ch) == 'G' || toupper(ch) == 'A')   /* toggle gain-mode; Manual <-> Auto */
  {
    if (!Modes.rtlsdr.gains)
       return;

    if (Modes.gain_auto)
    {
      Modes.gain_auto = false;
      Modes.gain = Modes.rtlsdr.gains [gain_idx];
      rtlsdr_set_tuner_gain_mode (Modes.rtlsdr.device, 1);
      rtlsdr_set_tuner_gain (Modes.rtlsdr.device, Modes.gain);
      LOG_FILEONLY ("Gain: Auto -> Manual.\n");
    }
    else
    {
      Modes.gain_auto = true;
      rtlsdr_set_tuner_gain_mode (Modes.rtlsdr.device, 0);
      LOG_FILEONLY ("Gain: Manual -> Auto.\n");
    }
  }
}

/**
 * Show information for a single valid aircraft.
 *
 * If `a->show == A_SHOW_FIRST_TIME`, print in GREEN colour.
 * If `a->show == A_SHOW_LAST_TIME`, print in RED colour.
 *
 * \param in a        the aircraft to show.
 * \param in row      the row to print the line at; `[1 .. Modes.interactive_rows]'.
 * \param in mouse    the X/Y position of the mouse.
 * \param in now      the currect tick-timer in milli-seconds.
 */
static void show_one_aircraft (aircraft *a, int row, const POINT *mouse, uint64_t now)
{
  int         altitude, speed;
  char        alt_buf [10]      = "  - ";
  char        lat_buf [10]      = "   - ";
  char        lon_buf [10]      = "    - ";
  char        speed_buf [8]     = " - ";
  char        heading_buf [8]   = " - ";
  char        distance_buf [10] = " - ";
  char        dep_buf [30]      = " -";
  char        dst_buf [30]      = " -";
  char        dep_dst_buf [30]  = "";
  bool        restore_colour    = false;
  const char *reg_num   = "  -";
  const char *call_sign = "  -";
  const char *km_nmiles = NULL;
  const char *country   = NULL;
  int64_t     ms_diff;
  int         min_x1, min_x2;  /* mouse hit-test */
  int         max_x1, max_x2 = strlen ("ICAO   Callsign  Reg-num  Cntry  DEP  DEST");

  /* Convert units to metric if option `--metric` was used.
   */
  if (Modes.metric)
  {
    altitude = (int) round (a->altitude / 3.2828);
    speed    = (int) round (a->speed * 1.852);
  }
  else
  {
    altitude = a->altitude;
    speed    = (int) round (a->speed);
  }

  if ((a->AC_flags & MODES_ACFLAGS_AOG_VALID) && (a->AC_flags & MODES_ACFLAGS_AOG))
  {
    strcpy (alt_buf, " Grnd");
  }
  else if (a->AC_flags & MODES_ACFLAGS_ALTITUDE_VALID)
  {
    if (altitude < 0)
         strcpy (alt_buf, " Grnd");
    else snprintf (alt_buf, sizeof(alt_buf), "%5d", altitude);
  }

  if (a->position.lat != 0.0)
     snprintf (lat_buf, sizeof(lat_buf), "% +7.03f", a->position.lat);

  if (a->position.lon != 0.0)
     snprintf (lon_buf, sizeof(lon_buf), "% +8.03f", a->position.lon);

  if (speed)
     snprintf (speed_buf, sizeof(speed_buf), "%4d", speed);

  if (a->AC_flags & MODES_ACFLAGS_HEADING_VALID)
     snprintf (heading_buf, sizeof(heading_buf), "%3d", (int)round(a->heading));

  if (Modes.home_pos_ok && a->distance_ok)
  {
    get_home_distance (a, &km_nmiles);
    get_est_home_distance (a, &km_nmiles);
    strcpy_s (distance_buf, sizeof(distance_buf), a->distance_buf);
  }

  if (a->SQL)
  {
    if (a->SQL->reg_num[0])
       reg_num = a->SQL->reg_num;
  }
  else if (a->CSV)
  {
    if (a->CSV->reg_num[0])
       reg_num = a->CSV->reg_num;
  }

  if (a->call_sign[0])
     call_sign = a->call_sign;

  /* If it's valid plane and not a helicopter, post a ADSB-LOL API
   * request for the flight-info. Or return already cached flight-info.
   */
  if (show_dep_dst && !a->is_helicopter && call_sign[0] != ' ')
  {
    const char *departure, *destination;

    airports_API_get_flight_info (call_sign, a->addr, &departure, &destination);

    /* Both are known or both are NULL.
     */
    if (departure && destination)
    {
      strcpy_s (dep_buf, sizeof(dep_buf), departure);
      strcpy_s (dst_buf, sizeof(dst_buf), destination);
    }
    else
    {
      strcpy (dep_buf, " ?");
      strcpy (dst_buf, " ?");
    }

    if (mouse->y == row)    /* mouse-Y at correct line */
    {
      min_x1 = strlen ("ICAO   Callsign  Reg-num  Cntry  ");
      min_x2 = strlen ("ICAO   Callsign  Reg-num  Cntry  DEP ");
      max_x1 = min_x2;

      if (departure && mouse->x >= min_x1 && mouse->x <= max_x1)
      {
        /* show full name for DEP only
         */
        snprintf (dep_dst_buf, sizeof(dep_dst_buf), "%S",
                  u8_format(airport_find_location(dep_buf), 0));
      }
      else if (destination && mouse->x >= min_x2 && mouse->x <= max_x2)
      {
        /* show short name for DEP and full name for DEST
         */
        snprintf (dep_dst_buf, sizeof(dep_dst_buf), "%-4.4s %S",
                  dep_buf, u8_format(airport_find_location(dst_buf), 0));
        alt_buf[0] = '\0';    /* do not print Altitude now */
      }
    }
  }

  if (a->show == A_SHOW_FIRST_TIME)
  {
    (*api->set_colour) (COLOUR_GREEN);
    restore_colour = true;

    if (!(Modes.debug & DEBUG_PLANE))
       airports_API_flight_log_entering (a);
  }
  else if (a->show == A_SHOW_NORMAL)
  {
    if (!a->is_helicopter && !a->done_flight_info)
    {
      if (Modes.debug & DEBUG_PLANE)
           a->done_flight_info = true;
      else a->done_flight_info = airports_API_flight_log_resolved (a);
    }

    if (a->is_helicopter)
       ;        /**< \todo print reg_num in dark RED colour */
  }
  else if (a->show == A_SHOW_LAST_TIME)
  {
    (*api->set_colour) (COLOUR_RED);
    restore_colour = true;

    if (!(Modes.debug & DEBUG_PLANE))
       airports_API_flight_log_leaving (a);
  }

  ms_diff = (now - a->seen_last);
  if (ms_diff < 0LL)  /* clock wrapped */
     ms_diff = 0L;

#if 0
  /*
   * if mouse at the "Cntry" column, show the long country name.
   */
  if (mouse->y == row)
  {
    min_x1 = strlen ("ICAO   Callsign  Reg-num  ");
    max_x1 = strlen ("ICAO   Callsign  Reg-num  Cntry ");
    if (mouse->x >= min_x1 && mouse->x <= max_x1)
       country = aircraft_get_country (a->addr, false);
  }
#endif

  if (!country)
  {
    country = aircraft_get_country (a->addr, true);
    if (!country)
       country = "--";
  }

  if (show_dep_dst)
  {
    if (!dep_dst_buf[0])
       snprintf (dep_dst_buf, sizeof(dep_dst_buf), "%-4.4s %-4.4s", dep_buf, dst_buf);

    (*api->print_format) (0, row, LINE_FMT2_1,
                          a->addr, call_sign, reg_num, country, dep_dst_buf);

    (*api->print_format) (max_x2 + 5, row, LINE_FMT2_2,
                          alt_buf, speed_buf, lat_buf, lon_buf, heading_buf,
                          distance_buf, a->messages, ms_diff / 1000);
  }
  else
  {
    (*api->print_format) (0, row, LINE_FMT1,
                          a->addr, call_sign, reg_num, country,
                          alt_buf, speed_buf, lat_buf, lon_buf, heading_buf,
                          distance_buf, a->messages, ms_diff / 1000);
  }

  if (restore_colour)
     (*api->set_colour) (0);
}

/**
 * Show the currently captured aircraft information on screen.
 *
 * \param in now  the currect tick-timer in milli-seconds
 */
void interactive_show_data (uint64_t now)
{
  static int old_count = -1;
  int        row = 1;     /* HEADER at line 0, 1st aircraft at line 1 */
  int        count = 0;
  int        i, max;
  POINT      pos = { -1, -1 };
  bool       clear_screen = (Modes.raw == 0 ? true : false);
  bool       rc = false;

  /* Unless `--raw` mode is active, clear the screen to remove old info.
   * But only if current number of aircrafts is less than last time.
   * This is to avoid an annoying blinking of the console.
   */
  if (clear_screen)
  {
    if (old_count == -1 || aircraft_numbers_valid() < old_count)
    {
      (*api->clr_scr)();
      (*api->set_cursor) (false);   /* Need to hide the cursor again! */
    }
    (*api->gotoxy) (0, 0);

    aircraft_sort (Modes.a_sort);
  }

  mouse_pos (con_wnd, &pos);
//rc = mouse_header_check (&pos, mouse_lclick());
  if (!rc)
     (*api->print_header)();

  max = smartlist_len (Modes.aircrafts);
  for (i = count = 0; i < max && row < Modes.interactive_rows && !Modes.exit; i++)
  {
    aircraft *a = smartlist_get (Modes.aircrafts, i);

    if (!aircraft_valid(a))    /* Ignore these. "Mode A/C"? */
       continue;

    if (a->show != A_SHOW_NONE)
    {
      aircraft_set_est_home_distance (a, now);
      show_one_aircraft (a, row, &pos, now);
      row++;
    }

    /* Simple state-machine for the plane's show-state
     */
    if (a->show == A_SHOW_FIRST_TIME)
       a->show = A_SHOW_NORMAL;
    else if (a->show == A_SHOW_LAST_TIME)
       a->show = A_SHOW_NONE;      /* don't show again before deleting it */

    count++;
  }

  (*api->refresh) (0, row);

  old_count = count;
}

/**
 * "Windows Console" API functions
 */
static bool wincon_init (void)
{
  WORD bg = con_info_out.wAttributes & ~7;

  Modes.interactive_rows = con_info_out.srWindow.Bottom - con_info_out.srWindow.Top - 1;

  colour_map [COLOUR_DEFAULT].attrib = con_info_out.wAttributes;  /* default colour */
  colour_map [COLOUR_WHITE  ].attrib = (bg | 15);                 /* bright white */
  colour_map [COLOUR_GREEN  ].attrib = (bg | 10);                 /* bright green */
  colour_map [COLOUR_RED    ].attrib = (bg | 12);                 /* bright red */
  colour_map [COLOUR_YELLOW ].attrib = (bg | 14);                 /* bright yellow */
  colour_map [COLOUR_HEADER1].attrib = 9 + (7 << 4);              /* bright blue on white */
  return (true);
}

static void wincon_exit (void)
{
  if (!test_mode)
     wincon_gotoxy (0, Modes.interactive_rows-1);
  wincon_set_colour (0);
}

static wint_t wincon_getch (void)
{
  if (!_kbhit())
     return (0);
  return (wint_t) _getch();
}

static void wincon_gotoxy (int x, int y)
{
  COORD coord;
  BOOL  rc;

  if (vt_out_supported)
  {
    fprintf (stdout, "\x1B[%d;%dH", y + 1, x + 1);
    return;
  }

  coord.X = x + con_info_out.srWindow.Left;
  coord.Y = y + con_info_out.srWindow.Top;
  rc = SetConsoleCursorPosition (con_out, coord);
  if (!rc)
     TRACE ("SetConsoleCursorPosition (%d,%d) failed; %s\n",
            x, y, win_strerror(GetLastError()));
}

/**
 * Return the cursor X-position relative display window
 */
static int wincon_get_curx (void)
{
  GetConsoleScreenBufferInfo (con_out, &con_info_out);
  return (con_info_out.dwCursorPosition.X - con_info_out.srWindow.Left);
}

/**
 * Return the cursor Y-position relative display window
 */
static int wincon_get_cury (void)
{
  GetConsoleScreenBufferInfo (con_out, &con_info_out);
  return (con_info_out.dwCursorPosition.Y - con_info_out.srWindow.Top);
}

static void wincon_clrscr (void)
{
  WORD width = con_info_out.srWindow.Right - con_info_out.srWindow.Left + 1;
  WORD y     = con_info_out.srWindow.Top;

  while (y <= con_info_out.srWindow.Bottom)
  {
    DWORD written;
    COORD coord = { con_info_out.srWindow.Left, y++ };

    FillConsoleOutputCharacter (con_out, ' ', width, coord, &written);
    FillConsoleOutputAttribute (con_out, con_info_out.wAttributes, width, coord, &written);
  }
}

/*
 * Fill the current line with spaces and put the cursor back at position 0.
 */
static void wincon_clreol (void)
{
#if 0
  if (con_out != INVALID_HANDLE_VALUE)
  {
    WORD   width = con_info_out.srWindow.Right - con_info_out.srWindow.Left + 1;
    char *filler = alloca (width + 1);

    memset (filler, ' ', width - 3);
    filler [width-2] = '\r';
    filler [width-1] = '\0';
    fputs (filler, stdout);
  }
#endif
}

static void wincon_set_colour (enum colours colour)
{
  assert (colour >= 0);
  assert (colour < COLOUR_MAX);
  if (con_out != INVALID_HANDLE_VALUE)
     SetConsoleTextAttribute (con_out, (WORD)colour_map [colour].attrib);
}

static void wincon_set_cursor (bool enable)
{
  CONSOLE_CURSOR_INFO ci;
  static int orig_size = -1;

  if (test_mode || con_out == INVALID_HANDLE_VALUE)
     return;

  GetConsoleCursorInfo (con_out, &ci);
  if (orig_size == -1)
     orig_size = ci.dwSize;

  if (enable)
  {
    ci.bVisible = TRUE;
    ci.dwSize   = orig_size;
  }
  else
  {
    ci.bVisible = FALSE;
    ci.dwSize   = 1;       /* smallest value allowed, but ignored */
  }
  SetConsoleCursorInfo (con_out, &ci);
}

static void wincon_refresh (int x, int y)
{
  /* Nothing to do here */
  (void) x;
  (void) y;
}

static void wincon_print_format (int x, int y, const char *fmt, ...)
{
  va_list args;
  char    buf [1000];

  va_start (args, fmt);
  vsnprintf (buf, sizeof(buf), fmt, args);
  va_end (args);
  wincon_gotoxy (x, y);
  fputs (buf, stdout);
}

static void wincon_print_wformat (int x, int y, const wchar_t *fmt, ...)
{
  va_list args;
  wchar_t buf [1000];

  va_start (args, fmt);
  _vsnwprintf (buf, DIM(buf), fmt, args);
  va_end (args);
  wincon_gotoxy (x, y);
  fputws (buf, stdout);
}

static void wincon_print_header (void)
{
  static int spin_idx = 0;

  wincon_set_colour (COLOUR_HEADER1);
  fprintf (stdout, HEADER, show_dep_dst ? DEP_DST_COLUMNS : "", spinner[spin_idx & 3]);
  wincon_set_colour (0);
  spin_idx++;
}

/**
 * PDCurses API functions
 */
static bool curses_init (void)
{
  bool  slk_ok = false;
  short pair, fg, bg;

  _putenv ("WT_SESSION=1");

  if (!test_mode)
     slk_ok = (slk_init(1) == OK);

  initscr();

  if (!test_mode && slk_ok)
  {
    slk_set (1, "Help", 0);
    slk_set (2, "Quit", 0);
    slk_attron (A_REVERSE | A_BOLD);
  }

  Modes.interactive_rows = getmaxy (stdscr);
  if (Modes.interactive_rows == 0)
  {
    LOG_STDERR ("getmaxy (stdscr) return 0!\n");
    return (false);
  }

  if (has_colors())
     start_color();

  use_default_colors();
  if (!can_change_color())
  {
    LOG_STDERR ("can_change_color() failed!\n");
    return (false);
  }

  init_pair (COLOUR_WHITE,  COLOR_WHITE, COLOR_BLUE);
  init_pair (COLOUR_GREEN,  COLOR_GREEN, COLOR_BLUE);
  init_pair (COLOUR_RED,    COLOR_RED,   COLOR_BLUE);
  init_pair (COLOUR_YELLOW, COLOR_YELLOW, COLOR_GREEN);

  colour_map [COLOUR_DEFAULT].pair   = COLOUR_DEFAULT;
  colour_map [COLOUR_DEFAULT].attrib = A_NORMAL;

  colour_map [COLOUR_WHITE  ].pair   = COLOUR_WHITE;
  colour_map [COLOUR_WHITE  ].attrib = A_BOLD;

  colour_map [COLOUR_GREEN  ].pair   = COLOUR_GREEN;
  colour_map [COLOUR_GREEN  ].attrib = A_BOLD;

  colour_map [COLOUR_RED    ].pair   = COLOUR_RED;
  colour_map [COLOUR_RED    ].attrib = A_BOLD;

  colour_map [COLOUR_YELLOW ].pair   = COLOUR_YELLOW;
  colour_map [COLOUR_YELLOW ].attrib = A_NORMAL;

  colour_map [COLOUR_HEADER1].pair   = COLOUR_HEADER1;

  if (vt_out_supported)
  {
    init_pair (COLOUR_HEADER1, 15, 27); /* Needs VT-support */
    colour_map [COLOUR_HEADER1].attrib = A_BOLD | A_UNDERLINE;
  }
  else
  {
    init_pair (COLOUR_HEADER1, COLOR_BLACK, COLOR_WHITE);
  }

  /* For curses_hilight_header()
   */
  init_pair (COLOUR_HEADER2, 15, 254);
  colour_map [COLOUR_HEADER2].attrib = A_UNDERLINE;

  LOG_FILEONLY ("pair_content():\n");
  for (pair = COLOUR_WHITE; pair < COLOUR_MAX; pair++)
  {
    fg = bg = 0;
    pair_content (pair, &fg, &bg);
    LOG_FILEONLY ("!  pair[%d] -> fg: %2d, bg: %3d\n", pair, fg, bg);
  }

  noecho();
  mousemask (0, NULL);

//refresh();

#if 0   // \todo
  stats_win = subwin (stdscr, 4, COLS, 0, 0);
  wattron (stats_win, A_REVERSE);
  LOG_FILEONLY ("stats_win: %p, SP->lines: %u, SP->cols: %u\n", stats_win, SP->lines, SP->cols);

  flight_win = subwin (stdscr, 4, COLS, 0, 0);
  wattron (flight_win, A_REVERSE);
  LOG_FILEONLY ("flight_win: %p, SP->lines: %u, SP->cols: %u\n", flight_win, SP->lines, SP->cols);
#endif

  return (true);
}

static void curses_exit (void)
{
  if (stats_win)
     delwin (stats_win);

  if (flight_win)
     delwin (flight_win);

  endwin();
  delscreen (SP);  /* PDCurses does not free this memory */
}

static void curses_set_colour (enum colours colour)
{
  chtype pair;
  attr_t attrib;

  assert (colour >= 0);
  assert (colour < COLOUR_MAX);

  pair = colour_map [colour].pair;

  /* A variable (potentially == `INT_MAX`) in PDCurses-MOD.
   */
  assert (pair < (chtype)COLOR_PAIRS);

  attrib = colour_map [colour].attrib;
  attrset (COLOR_PAIR(pair) | attrib);
}

static void curses_set_cursor (bool enable)
{
  if (!test_mode)
     curs_set (enable ? 1 : 0);
}

static void curses_refresh (int x, int y)
{
  if (stats_win)
     wrefresh (stats_win);
  move (y, x);
  refresh();
}

static void curses_gotoxy (int x, int y)
{
  move (y, x);
}

static wint_t curses_getch (void)
{
  wint_t c;

  get_wch (&c);
  if (c == KEY_MOUSE)
     c = 0;
  return (wint_t) c;
}

static int curses_get_curx (void)
{
  return getcurx (stdscr);
}

static int curses_get_cury (void)
{
  return getcury (stdscr);
}

static void curses_print_format (int x, int y, const char *fmt, ...)
{
  va_list args;

  move (y, x);
  va_start (args, fmt);
  vw_printw (stdscr, fmt, args);
  va_end (args);
}

static void curses_print_wformat (int x, int y, const wchar_t *fmt, ...)
{
  va_list args;
  wchar_t buf [1000];

  va_start (args, fmt);
  _vsnwprintf (buf, DIM(buf), fmt, args);
  mvaddwstr (y, x, buf);
  va_end (args);
}

static void curses_print_header (void)
{
  static int spin_idx = 0;

  curses_set_colour (COLOUR_HEADER1);
  mvprintw (0, 0, HEADER, show_dep_dst ? DEP_DST_COLUMNS : "", spinner[spin_idx & 3]);
  curses_set_colour (0);
  spin_idx++;
}

static bool mouse_pos (HWND wnd, POINT *pos)
{
  pos->x = pos->y = -1;

  if (!wnd)
     return (false);

  if (!GetCursorPos(pos))
  {
    TRACE ("GetCursorPos() failed; %s\n", win_strerror(GetLastError()));
    return (false);
  }

  if (!ScreenToClient(wnd, pos))
  {
    TRACE ("ScreenToClient() failed; %s\n", win_strerror(GetLastError()));
    return (false);
  }

 if (x_scale > 0)
    pos->x /= x_scale;
 if (y_scale > 0)
    pos->y /= y_scale;

 if (test_mode && Modes.tui_interface == TUI_CURSES)
    LOG_FILEONLY ("pos->x: %ld, pos->y: %ld\n", pos->x, pos->y);
  return (true);
}

static bool mouse_lclick (void)
{
  return (GetAsyncKeyState(VK_LBUTTON) & 0x8001);
}

static bool interactive_test_loop (void)
{
  int y = (*api->get_cury)();

  if (Modes.tui_interface == TUI_CURSES)
     nodelay (stdscr, TRUE);   /* use non-blocking `(*api->getch)()` */

  while (!Modes.exit)
  {
    POINT  pos;
    wint_t ch;
    bool   clicked;

    mouse_pos (con_wnd, &pos);

    if (pos.x > -1 && pos.y > -1)
    {
      clicked = mouse_lclick();
      (*api->print_format) (0, y, "pos.x: %ld, pos.y: %ld, %s\r",
                            pos.x, pos.y, clicked ? "clicked" : "       ");
      if (!mouse_header_check (&pos, clicked))
      {
        (*api->gotoxy) (0, 0);
        (*api->print_header)();
      }
    }

    ch = (*api->getch)();
    if (ch)
    {
      (*api->print_format) (0, y+1, "ch: %d / '%c'", ch, ch);
      if (ch == 'q')
         Modes.exit = true;
    }
    Sleep (200);
  }
  return (false);
}
