/**
 * \file    interactive.c
 * \ingroup Misc
 * \brief   Function for interactive mode.
 *
 * Using either Windows-Console or Curses functions.
 */
#include <io.h>
#include <conio.h>

#include "interactive.h"
#include "aircraft.h"
#include "airports.h"
#include "sdrplay.h"
#include "smartlist.h"
#include "net_io.h"
#include "misc.h"

#undef MOUSE_MOVED
#include <curses.h>

typedef enum colours {
        COLOUR_DEFAULT = 0,
        COLOUR_WHITE,
        COLOUR_GREEN,
        COLOUR_RED,
        COLOUR_YELLOW,
        COLOUR_REVERSE,
        COLOUR_MAX
      } colours;

typedef struct colour_mapping {
        int    pair;    /* Not used in 'wincon_*()' functions */
        chtype attrib;
      } colour_mapping;

typedef struct API_funcs {
        int  (*init) (void);
        void (*exit) (void);
        void (*set_colour) (enum colours colour);
        int  (*clr_scr) (void);
        int  (*clr_eol) (void);
        int  (*gotoxy) (int y, int x);
        int  (*refresh) (int y, int x);
        int  (*print_line) (int y, int x, const char *str);
        void (*print_header) (void);
      } API_funcs;

static int  wincon_init (void);
static void wincon_exit (void);
static void wincon_set_colour (enum colours colour);
static int  wincon_gotoxy (int y, int x);
static int  wincon_clreol (void);
static int  wincon_clrscr (void);
static int  wincon_refresh (int y, int x);
static int  wincon_print_line (int y, int x, const char *str);
static void wincon_print_header (void);

static CONSOLE_SCREEN_BUFFER_INFO  console_info;
static HANDLE                      console_hnd = INVALID_HANDLE_VALUE;
static DWORD                       console_mode = 0;
static colour_mapping              colour_map [COLOUR_MAX];

static int  curses_init (void);
static void curses_exit (void);
static void curses_set_colour (enum colours colour);
static void curses_print_header (void);
static int  curses_refresh (int y, int x);

/* Or use CreatePseudoConsole() instead?
 * https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session
 */
static WINDOW *stats_win  = NULL;
static WINDOW *flight_win = NULL;

static API_funcs curses_api = {
       .init         = curses_init,
       .exit         = curses_exit,
       .set_colour   = curses_set_colour,
       .clr_scr      = clear,
       .clr_eol      = clrtoeol,
       .gotoxy       = move,
       .refresh      = curses_refresh,
       .print_line   = mvaddstr,
       .print_header = curses_print_header
      };

static API_funcs wincon_api = {
       .init         = wincon_init,
       .exit         = wincon_exit,
       .set_colour   = wincon_set_colour,
       .clr_scr      = wincon_clrscr,
       .clr_eol      = wincon_clreol,
       .gotoxy       = wincon_gotoxy,
       .refresh      = wincon_refresh,
       .print_line   = wincon_print_line,
       .print_header = wincon_print_header,
     };

/* Show the "DEP  DST" columns if we have a good `Modes.airport_db` file.
 */
static bool show_dep_dst = false;

/* Use this header and `snprintf()` format for both `show_dep_dst == false` and `show_dep_dst == true`.
 */
#define HEADER    "ICAO   Callsign  Reg-num  Cntry  %sAltitude  Speed   Lat      Long    Hdg   Dist  Msg  Seen %c"
//                                                  |__ == "DEP  DST  " -- if 'show_dep_dst == true'

#define LINE_FMT  "%06X %-9.9s %-8s %-5.5s  %s%-5s   %-5s %-7s %-8s %4.4s  %5.5s %5u %3llu s "
//                 |    |      |    |       | |      |    |    |    |      |     |   |__ ms_diff / 1000
//                 |    |      |    |       | |      |    |    |    |      |     |__ a->messages
//                 |    |      |    |       | |      |    |    |    |      |__ distance_buf
//                 |    |      |    |       | |      |    |    |    |__ heading_buf
//                 |    |      |    |       | |      |    |    |__ lon_buf
//                 |    |      |    |       | |      |    |__ lat_buf
//                 |    |      |    |       | |      |__ speed_buf
//                 |    |      |    |       | |__ alt_buf
//                 |    |      |    |       |__ dep_dst_buf
//                 |    |      |    |___ cc_short
//                 |    |      |____ reg_num
//                 |    |__ call_sign
//                 |__ a->addr

#define DEP_DST_COLUMNS "DEP  DEST  "

/*
 * List of API function for the TUI (text user interface).
 */
static API_funcs *api = NULL;

/**
 * Do some `assert()` checks first and if Curses interface was
 * selected, set `api = &curses_api`.
 * Otherwise set `api = &wincon_api`.
 */
bool interactive_init (void)
{
  assert (api == NULL);

  if (Modes.tui_interface == TUI_CURSES)
       api = &curses_api;
  else api = &wincon_api;

  if (airports_rc() > 0)
     show_dep_dst = true;

  return ((*api->init)() == 0);
}

void interactive_exit (void)
{
  if (api)
    (*api->exit)();
  api = NULL;
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
    overload_count = 4;     /* let it show for 4 period (1 sec) */
  }
#if 0
  else if (bad_CRC == last_bad_CRC && good_CRC == last_good_CRC)
  {
    overload = GAIN_TOO_LOW;
    overload_count = 4;     /* let it show for 4 period (1 sec) */
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
    mvwprintw (stats_win, 20, 0, "HTTP GET:   %llu", Modes.stat.HTTP_get_requests);
    mvwprintw (stats_win, 21, 0, "HTTP bytes: %llu/%llu",
               Modes.stat.bytes_sent[MODES_NET_SERVICE_HTTP4] + Modes.stat.bytes_sent[MODES_NET_SERVICE_HTTP6],
               Modes.stat.bytes_recv[MODES_NET_SERVICE_HTTP4] + Modes.stat.bytes_recv[MODES_NET_SERVICE_HTTP6]);
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
  return (gain_idx);
}

/**
 * Poll for '+/-' keypresses and adjust the RTLSDR / SDRplay gain accordingly.
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
 * \param in a    the aircraft to show.
 * \param in now  the currect tick-timer in milli-seconds.
 */
static void show_one_aircraft (aircraft *a, int row, uint64_t now)
{
  int   altitude, speed;
  char  alt_buf [10]      = "  - ";
  char  lat_buf [10]      = "   - ";
  char  lon_buf [10]      = "    - ";
  char  speed_buf [8]     = " - ";
  char  heading_buf [8]   = " - ";
  char  distance_buf [10] = " - ";
  char  dep_buf [30]      = " -";
  char  dst_buf [30]      = " -";
  char  dep_dst_buf [20]  = "  ";
  char  line_buf [120];
  bool  restore_colour    = false;
  const char *reg_num     = "  -";
  const char *call_sign   = "  -";
  const char *km_nmiles   = NULL;
  const char *cc_short;
  int64_t     ms_diff;

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
    snprintf (alt_buf, sizeof(alt_buf), "%5d", altitude);
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

  /* If it's valid plane and not a helicopter, post a ADSB-LOL API request for the flight-info.
   * Or return already cached flight-info.
   */
  if (!a->is_helicopter && call_sign[0] != ' ')
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

  cc_short = aircraft_get_country (a->addr, true);
  if (!cc_short)
     cc_short = "--";

  if (show_dep_dst)
     snprintf (dep_dst_buf, sizeof(dep_dst_buf), "%-4.4s %-4.4s     ", dep_buf, dst_buf);

  snprintf (line_buf, sizeof(line_buf), LINE_FMT,
            a->addr, call_sign, reg_num, cc_short, dep_dst_buf, alt_buf, speed_buf, lat_buf, lon_buf, heading_buf,
            distance_buf, a->messages, ms_diff / 1000);

  (*api->print_line) (row, 0, line_buf);

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
  int        row = 1, count = 0;
  int        i, max;
  bool       clear_screen = (Modes.raw == 0 ? true : false);

  /* Unless `--raw` mode is active, clear the screen to remove old info.
   * But only if current number of aircrafts is less than last time.
   * This is to avoid an annoying blinking of the console.
   */
  if (clear_screen)
  {
    if (old_count == -1 || aircraft_numbers_valid() < old_count)
       (*api->clr_scr)();
    (*api->gotoxy) (0, 0);

    if (Modes.a_sort != INTERACTIVE_SORT_NONE)
    {
      int max1 = aircraft_sort();
      int max2 = smartlist_len (Modes.aircrafts);
      assert (max1 == max2);
    }
  }

  (*api->print_header)();

  max = smartlist_len (Modes.aircrafts);
  for (i = count = 0; i < max && count < Modes.interactive_rows && !Modes.exit; i++)
  {
    aircraft *a = smartlist_get (Modes.aircrafts, i);

    if (!aircraft_valid(a))    /* ignore these. "Mode A/C"? */
       continue;

    if (a->show != A_SHOW_NONE)
    {
      aircraft_set_est_home_distance (a, now);
      show_one_aircraft (a, row, now);
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

  (*api->refresh) (row, 0);

  old_count = count;
}

/**
 * "Windows Console" API functions
 */
static int wincon_init (void)
{
  console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
  if (console_hnd == INVALID_HANDLE_VALUE)
     return (-1);

  if (_isatty(1) == 0)
  {
    LOG_STDERR ("Do not redirect 'stdout' in interactive mode.\n"
                "Do '%s [options] 2> file` instead.\n", Modes.who_am_I);
    return (-1);
  }

  GetConsoleScreenBufferInfo (console_hnd, &console_info);
  GetConsoleMode (console_hnd, &console_mode);
  if (console_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
     SetConsoleMode (console_hnd, console_mode | DISABLE_NEWLINE_AUTO_RETURN);

  DWORD new_mode = console_mode & ~(ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT);
  SetConsoleMode (console_hnd, new_mode);

  Modes.interactive_rows  = console_info.srWindow.Bottom - console_info.srWindow.Top - 1;

  WORD bg = console_info.wAttributes & ~7;

  colour_map [COLOUR_DEFAULT].attrib = console_info.wAttributes;  /* default colour */
  colour_map [COLOUR_WHITE  ].attrib = (bg | 15);                 /* bright white */
  colour_map [COLOUR_GREEN  ].attrib = (bg | 10);                 /* bright green */
  colour_map [COLOUR_RED    ].attrib = (bg | 12);                 /* bright red */
  colour_map [COLOUR_YELLOW ].attrib = (bg | 14);                 /* bright yellow */
  colour_map [COLOUR_REVERSE].attrib = 9 + (7 << 4);              /* bright blue on white */
  return (0);
}

static void wincon_exit (void)
{
  wincon_gotoxy (Modes.interactive_rows-1, 0);
  wincon_set_colour (0);
  if (console_hnd != INVALID_HANDLE_VALUE)
     SetConsoleMode (console_hnd, console_mode);
  console_hnd = INVALID_HANDLE_VALUE;
}

static int wincon_gotoxy (int y, int x)
{
  COORD coord;

  if (console_hnd == INVALID_HANDLE_VALUE)
     return (-1);

  coord.X = x + console_info.srWindow.Left;
  coord.Y = y + console_info.srWindow.Top;
  SetConsoleCursorPosition (console_hnd, coord);
  return (0);
}

static int wincon_clrscr (void)
{
  WORD width = console_info.srWindow.Right - console_info.srWindow.Left + 1;
  WORD y = console_info.srWindow.Top;

  while (y <= console_info.srWindow.Bottom)
  {
    DWORD written;
    COORD coord = { console_info.srWindow.Left, y++ };

    FillConsoleOutputCharacter (console_hnd, ' ', width, coord, &written);
    FillConsoleOutputAttribute (console_hnd, console_info.wAttributes, width, coord, &written);
  }
  return (0);
}

/*
 * Fill the current line with spaces and put the cursor back at position 0.
 */
static int wincon_clreol (void)
{
#if 0
  if (console_hnd != INVALID_HANDLE_VALUE)
  {
    WORD   width = console_info.srWindow.Right - console_info.srWindow.Left + 1;
    char *filler = alloca (width+1);

    memset (filler, ' ', width-3);
    filler [width-2] = '\r';
    filler [width-1] = '\0';
    fputs (filler, stdout);
  }
#endif
  return (0);
}

static void wincon_set_colour (enum colours colour)
{
  assert (colour >= 0);
  assert (colour < COLOUR_MAX);
  if (console_hnd != INVALID_HANDLE_VALUE)
     SetConsoleTextAttribute (console_hnd, (WORD)colour_map [colour].attrib);
}

static int wincon_refresh (int y, int x)
{
  /* Nothing to do here */
  (void) x;
  (void) y;
  return (0);
}

static int wincon_print_line (int y, int x, const char *str)
{
  puts (str);
  (void) x;    /* Cursor already set */
  (void) y;
  return (0);
}

static int  spin_idx = 0;
static char spinner[] = "|/-\\";

static void wincon_print_header (void)
{
  wincon_set_colour (COLOUR_REVERSE);
  printf (HEADER, show_dep_dst ? DEP_DST_COLUMNS : "", spinner[spin_idx & 3]);
  wincon_set_colour (0);
  spin_idx++;
}

/**
 * PDCurses API functions
 */
static int curses_init (void)
{
  bool slk_ok = (slk_init(1) == OK);

  initscr();

  /* Ensure PCcurses do not clear the screen on exit
   */
  SP->_preserve = true;

  if (slk_ok)
  {
    slk_set (1, "Help", 0);
    slk_set (2, "Quit", 0);
    slk_attron (A_REVERSE);
  }

  Modes.interactive_rows = getmaxy (stdscr);
  if (Modes.interactive_rows == 0)
     return (-1);

  if (has_colors())
     start_color();

  use_default_colors();
  if (!can_change_color())
     return (-1);

  init_pair (COLOUR_WHITE,  COLOR_WHITE, COLOR_BLUE);
  init_pair (COLOUR_GREEN,  COLOR_GREEN, COLOR_BLUE);
  init_pair (COLOUR_RED,    COLOR_RED,   COLOR_BLUE);
  init_pair (COLOUR_YELLOW, COLOR_YELLOW, COLOR_GREEN);

  colour_map [COLOUR_DEFAULT].pair   = 0;
  colour_map [COLOUR_DEFAULT].attrib = A_NORMAL;

  colour_map [COLOUR_WHITE  ].pair   = 1;
  colour_map [COLOUR_WHITE  ].attrib = A_BOLD;

  colour_map [COLOUR_GREEN  ].pair   = 2;
  colour_map [COLOUR_GREEN  ].attrib = A_BOLD;

  colour_map [COLOUR_RED    ].pair   = 3;
  colour_map [COLOUR_RED    ].attrib = A_BOLD;

  colour_map [COLOUR_YELLOW ].pair   = 4;
  colour_map [COLOUR_YELLOW ].attrib = A_NORMAL;

  colour_map [COLOUR_REVERSE].pair   = 5;
  colour_map [COLOUR_REVERSE].attrib = A_REVERSE | COLOR_BLUE;

  noecho();
  curs_set (0);
  mousemask (0, NULL);
  clear();
  refresh();

#if 0   // \todo
  stats_win = subwin (stdscr, 4, COLS, 0, 0);
  wattron (stats_win, A_REVERSE);
  LOG_FILEONLY ("stats_win: %p, SP->lines: %u, SP->cols: %u\n", stats_win, SP->lines, SP->cols);

  flight_win = subwin (stdscr, 4, COLS, 0, 0);
  wattron (flight_win, A_REVERSE);
  LOG_FILEONLY ("flight_win: %p, SP->lines: %u, SP->cols: %u\n", flight_win, SP->lines, SP->cols);
#endif

  return (0);
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
  int pair, attrib, attr;

  assert (colour >= 0);
  assert (colour < COLOUR_MAX);

  pair = colour_map [colour].pair;
  assert (pair < COLOR_PAIRS);

  attrib = colour_map [colour].attrib;

  attr = attrib & A_ATTRIBUTES;
  assert (attr == A_NORMAL || attr == A_BOLD || attr == A_REVERSE);
  attrset (COLOR_PAIR(pair) | attrib);
}

static int curses_refresh (int y, int x)
{
  wrefresh (stats_win);
  move (y, x);
//clrtobot();
  refresh();
  return (0);
}

static void curses_print_header (void)
{
  curses_set_colour (COLOUR_REVERSE);
  mvprintw (0, 0, HEADER, show_dep_dst ? DEP_DST_COLUMNS : "", spinner[spin_idx & 3]);
  curses_set_colour (0);
  spin_idx++;
}
