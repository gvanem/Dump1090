/**
 * \file    interactive.c
 * \ingroup Misc
 *
 * Function for interactive mode.
 * Using either Windows-Console or Curses functions.
 */
#include <io.h>
#include <conio.h>

#include "interactive.h"
#include "aircraft.h"
#include "sdrplay.h"
#include "misc.h"

#if defined(USE_CURSES)
  #undef MOUSE_MOVED
  #include <curses.h>

  #define PRINTF(row, fmt, ...)   mvprintw (row, 0, fmt, __VA_ARGS__)

  typedef struct pair_attr {
          int pair;
          int bold;
        } pair_attr;

  static pair_attr colour_map [15];

#else
  #define PRINTF(row, fmt, ...)   printf (fmt "\n", __VA_ARGS__)

  static CONSOLE_SCREEN_BUFFER_INFO  console_info;
  static HANDLE                      console_hnd = INVALID_HANDLE_VALUE;
  static DWORD                       console_mode = 0;

  #define COLOR_GREEN   10  /* bright green;  FOREGROUND_INTENSITY + 2 */
  #define COLOR_RED     12  /* bright red;    FOREGROUND_INTENSITY + 4 */
  #define COLOR_YELLOW  14  /* bright yellow; FOREGROUND_INTENSITY + 6 */
  #define COLOR_WHITE   15  /* bright white;  FOREGROUND_INTENSITY + 7 */

  static int alternate_colours = 1;

  static void gotoxy (int x, int y)
  {
    COORD coord;

    if (console_hnd == INVALID_HANDLE_VALUE)
       return;

    coord.X = x - 1 + console_info.srWindow.Left;
    coord.Y = y - 1 + console_info.srWindow.Top;
    SetConsoleCursorPosition (console_hnd, coord);
  }

  static void clrscr (void)
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
  }

  /*
   * Fill the current line with spaces and put the cursor back at position 0.
   */
  void interactive_clreol (void)
  {
    if (console_hnd != INVALID_HANDLE_VALUE)
    {
      WORD   width = console_info.srWindow.Right - console_info.srWindow.Left + 1;
      char *filler = alloca (width+1);

      memset (filler, ' ', width-3);
      filler [width-2] = '\r';
      filler [width-1] = '\0';
      fputs (filler, stdout);
    }
  }

  static void set_colour (int colour)
  {
    WORD attr;

    if (console_hnd == INVALID_HANDLE_VALUE)
       return;

    attr = console_info.wAttributes;
    if (colour > 0)
    {
      attr &= ~7;
      attr |= colour;
    }
    SetConsoleTextAttribute (console_hnd, attr);
  }
#endif  /* USE_CURSES */

/**
 * Set this aircraft's estimated distance to our home position.
 *
 * Assuming a constant good last heading and speed, calculate the
 * new position from that using the elapsed time.
 */
static void set_est_home_distance (aircraft *a, uint64_t now)
{
  double      heading, distance, gc_distance, cart_distance;
  double      delta_X, delta_Y;
  cartesian_t cpos;

  if (!Modes.home_pos_ok || a->speed == 0 || !a->heading_is_valid)
     return;

  if (!VALID_POS(a->EST_position) || a->EST_seen_last < a->seen_last)
     return;

  spherical_to_cartesian (&cpos, a->EST_position);

  /* Ensure heading is in range '[-Phi .. +Phi]'
   */
  if (a->heading >= 180)
       heading = a->heading - 360;
  else heading = a->heading;

  heading = (TWO_PI * heading) / 360;  /* In radians */

  /* knots (1852 m/s) to distance (in meters) traveled in dT msec:
   */
  distance = 0.001852 * (double)a->speed * (now - a->EST_seen_last);
  a->EST_seen_last = now;

  delta_X = distance * sin (heading);
  delta_Y = distance * cos (heading);
  cpos.c_x += delta_X;
  cpos.c_y += delta_Y;

  cartesian_to_spherical (&a->EST_position, cpos);

  gc_distance     = great_circle_dist (a->EST_position, Modes.home_pos);
  cart_distance   = cartesian_distance (&cpos, &Modes.home_pos_cart);
  a->EST_distance = closest_to (a->EST_distance, gc_distance, cart_distance);

#if 0
  LOG_FILEONLY ("addr %04X: heading: %+7.1lf, delta_X: %+8.3lf, delta_Y: %+8.3lf, gc_distance: %6.1lf, cart_distance: %6.1lf\n",
                a->addr, 360.0*heading/TWO_PI, delta_X, delta_Y, gc_distance/1000, cart_distance/1000);
#endif
}

/**
 * Return a string showing this aircraft's distance to our home position.
 *
 * If `Modes.metric == true`, return it in kilo-meters. <br>
 * Otherwise Nautical Miles.
 */
static const char *get_home_distance (const aircraft *a, const char **km_nm)
{
  static char buf [20];
  double divisor = Modes.metric ? 1000.0 : 1852.0;

  if (km_nm)
     *km_nm = Modes.metric ? "km" : "Nm";

  if (a->distance <= SMALL_VAL)
     return (NULL);

  snprintf (buf, sizeof(buf), "%.1lf", a->distance / divisor);
  return (buf);
}

/**
 * As for `get_home_distance()`, but return the estimated distance.
 */
static const char *get_est_home_distance (const aircraft *a, const char **km_nm)
{
  static char buf [20];
  double divisor = Modes.metric ? 1000.0 : 1852.0;

  if (km_nm)
     *km_nm = Modes.metric ? "km" : "Nm";

  if (a->EST_distance <= SMALL_VAL)
     return (NULL);

  snprintf (buf, sizeof(buf), "%.1lf", a->EST_distance / divisor);
  return (buf);
}

/*
 * Called every 250 msec (`MODES_INTERACTIVE_REFRESH_TIME`) while
 * in interactive mode to update the Console Windows Title.
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
  uint64_t        good_CRC = Modes.stat.good_CRC + Modes.stat.fixed;
  uint64_t        bad_CRC  = Modes.stat.bad_CRC  - Modes.stat.fixed;

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

  snprintf (buf, sizeof(buf), "Dev: %s. CRC: %llu / %llu / %llu. Gain: %s%s",
            Modes.selected_dev, good_CRC, Modes.stat.fixed, bad_CRC, gain, overload);

  last_good_CRC = good_CRC;
  last_bad_CRC  = bad_CRC;

  SetConsoleTitleA (buf);
}

static int gain_increase (int gain_idx)
{
  if (Modes.rtlsdr.device && gain_idx < Modes.rtlsdr.gain_count-1)
  {
    Modes.gain = Modes.rtlsdr.gains [++gain_idx];
    rtlsdr_set_tuner_gain (Modes.rtlsdr.device, Modes.gain);
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
#if 0
  else if (ch == 'g' || ch == 'G')
  {
    if (Modes.gain_auto)
    {
      gain_manual();
      LOG_FILEONLY ("Gain: AUTO -> manual.\n");
    }
    else
    {
      gain_auto();
      LOG_FILEONLY ("Gain: manual -> AUTO.\n");
    }
  }
#endif
}

#if defined(USE_CURSES)
bool interactive_init (void)
{
  initscr();
  Modes.interactive_rows = getmaxy (stdscr);
  start_color();
  use_default_colors();

  init_pair (1, COLOR_WHITE, COLOR_BLUE);
  init_pair (2, COLOR_GREEN, COLOR_BLUE);
  init_pair (3, COLOR_YELLOW, COLOR_GREEN);
  init_pair (4, COLOR_RED, COLOR_BLUE);

  colour_map [COLOR_BLACK].pair  = 0;
  colour_map [COLOR_BLACK].bold  = A_NORMAL;

  colour_map [COLOR_WHITE].pair  = 1;
  colour_map [COLOR_WHITE].bold  = A_BOLD;

  colour_map [COLOR_GREEN].pair  = 2;
  colour_map [COLOR_GREEN].bold  = A_BOLD;

  colour_map [COLOR_YELLOW].pair = 3;
  colour_map [COLOR_YELLOW].bold = A_NORMAL;

  colour_map [COLOR_RED].pair    = 4;
  colour_map [COLOR_RED].bold    = A_BOLD;

  noecho();
  mousemask (0, NULL);
  clear();
  refresh();
  return (true);
}

void interactive_exit (void)
{
  endwin();
}

void interactive_clreol (void)
{
  if (stdscr)
     wclrtoeol (stdscr);
}

static void set_colour (int colour)
{
  int pair, bold;

  assert (colour >= 0);
  assert (colour < DIM(colour_map));

  pair = colour_map [colour].pair;
  assert (pair < COLOR_PAIRS);

  bold = colour_map [colour].bold;
  assert (bold == A_NOPRMAL || bold == A_BOLD);
  attrset (COLOR_PAIR(pair) | bold);
}
#else

bool interactive_init (void)
{
  console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
  if (console_hnd == INVALID_HANDLE_VALUE)
     return (false);

  if (_isatty(1) == 0)
  {
    LOG_STDERR ("Do not redirect 'stdout' in interactive mode.\n"
                "Do '%s [options] 2> file` instead.\n", Modes.who_am_I);
    return (false);
  }

  GetConsoleScreenBufferInfo (console_hnd, &console_info);
  GetConsoleMode (console_hnd, &console_mode);
  if (console_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
     SetConsoleMode (console_hnd, console_mode | DISABLE_NEWLINE_AUTO_RETURN);

  DWORD new_mode = console_mode & ~(ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT);
  SetConsoleMode (console_hnd, new_mode);

  Modes.interactive_rows = console_info.srWindow.Bottom - console_info.srWindow.Top - 1;
  return (true);
}

void interactive_exit (void)
{
  gotoxy (1, Modes.interactive_rows);
  set_colour (0);
  if (console_hnd != INVALID_HANDLE_VALUE)
     SetConsoleMode (console_hnd, console_mode);
  console_hnd = INVALID_HANDLE_VALUE;
}
#endif /* USE_CURSES */

/**
 * Show information for a single aircraft.
 *
 * If `a->show == A_SHOW_FIRST_TIME`, print in GREEN colour.
 * If `a->show == A_SHOW_LAST_TIME`, print in RED colour.
 *
 * \param in a    the aircraft to show.
 * \param in now  the currect tick-timer in milli-seconds.
 */
static bool interactive_show_aircraft (const aircraft *a, int row, uint64_t now)
{
  int   altitude           = a->altitude;
  int   speed              = a->speed;
  char  alt_buf [10]       = "  - ";
  char  lat_buf [10]       = "   - ";
  char  lon_buf [10]       = "    - ";
  char  speed_buf [8]      = " - ";
  char  heading_buf [8]    = " - ";
  char  distance_buf [10]  = " - ";
  char  RSSI_buf [7]       = " - ";
  bool  restore_colour     = false;
  const char *reg_num      = "";
  const char *call_sign    = "";
  const char *flight       = "";
  const char *distance     = NULL;
  const char *est_distance = NULL;
  const char *km_nm = NULL;
  const char *cc_short;
  double  sig_avg = 0;
  int64_t ms_diff;

  /* Convert units to metric if --metric was specified.
   */
  if (Modes.metric)
  {
    altitude = (int) round ((double)altitude / 3.2828);
    speed    = (int) round ((double)speed * 1.852);
  }

  /* Get the average RSSI from last 4 messages.
   */
  for (uint8_t i = 0; i < DIM(a->sig_levels); i++)
      sig_avg += a->sig_levels[i];
  sig_avg /= DIM(a->sig_levels);

  if (sig_avg > 1E-5)
     snprintf (RSSI_buf, sizeof(RSSI_buf), "% +4.1lf", 10 * log10(sig_avg));

  if (altitude)
     snprintf (alt_buf, sizeof(alt_buf), "%5d", altitude);

  if (a->position.lat != 0.0)
     snprintf (lat_buf, sizeof(lat_buf), "% +7.03f", a->position.lat);

  if (a->position.lon != 0.0)
     snprintf (lon_buf, sizeof(lon_buf), "% +8.03f", a->position.lon);

  if (speed)
     snprintf (speed_buf, sizeof(speed_buf), "%4d", speed);

  if (a->heading_is_valid)
     snprintf (heading_buf, sizeof(heading_buf), "%3d", a->heading);

  if (Modes.home_pos_ok)
  {
    distance     = get_home_distance (a, &km_nm);
    est_distance = get_est_home_distance (a, &km_nm);
    if (est_distance)
       snprintf (distance_buf, sizeof(distance_buf), "%s", est_distance);
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

  if (!a->flight[0] && call_sign[0])
       flight = call_sign;
  else flight = a->flight;

  if (a->show == A_SHOW_FIRST_TIME)
  {
    set_colour (COLOR_GREEN);
    restore_colour = true;
    LOG_FILEONLY ("plane '%06X' entering.\n", a->addr);
  }
  else if (a->show == A_SHOW_LAST_TIME)
  {
    char alt_buf2 [10] = "-";

    if (altitude >= 1)
       _itoa (altitude, alt_buf2, 10);

    set_colour (COLOR_RED);
    restore_colour = true;

    LOG_FILEONLY ("plane '%06X' leaving. Active for %.1lf sec. Altitude: %s m, Distance: %s/%s %s.\n",
                  a->addr, (double)(now - a->seen_first) / 1000.0,
                  alt_buf2,
                  distance     ? distance     : "-",
                  est_distance ? est_distance : "-", km_nm);
  }

  ms_diff = (now - a->seen_last);
  if (ms_diff < 0LL)  /* clock wrapped */
     ms_diff = 0L;

  cc_short = aircraft_get_country (a->addr, true);
  if (!cc_short)
     cc_short = "--";

  PRINTF (row, "%06X %-9.9s %-8s %-6s %-5s     %-5s %-7s %-8s   %-5s %6s  %5s %5u  %2llu sec ",
          a->addr, flight, reg_num, cc_short, alt_buf, speed_buf, lat_buf, lon_buf, heading_buf,
          distance_buf, RSSI_buf, a->messages, ms_diff / 1000);

  if (restore_colour)
     set_colour (0);

  (void) row;
  return (!restore_colour);
}

/**
 * \todo
 * Check if the console has scrolled (due to a QuickEdit action).
 * Do it by 'ReadConsoleOutputCharacter()' and look for "ICAO  "
 * at lines > 1.
 */
static bool console_messed_up (void)
{
#if 0
  if (console_hnd != INVALID_HANDLE_VALUE)
  {
    char  buf [sizeof("ICAO  ")];
    COORD coord = { 0, 20 };
    DWORD read  = 0;

    ReadConsoleOutputCharacter (console_hnd, buf, sizeof(buf), coord, &read);
    LOG_FILEONLY ("buf: '%.*s'\n", (int)read, buf);
  }
#endif
  return (false);
}

/**
 * Show the currently captured aircraft information on screen.
 * \param in now  the currect tick-timer in mill-seconds
 */
void interactive_show_data (uint64_t now)
{
  #define HEADER  "ICAO   Callsign  Reg-num  Cntry  Altitude  Speed   Lat      Long    Hdg     Dist   RSSI   Msg  Seen %c"

  static int spin_idx = 0;
  static int old_count = -1;
  int        row, count = 0;
  char       spinner[] = "|/-\\";
  aircraft  *a = Modes.aircrafts;

#if defined(USE_CURSES)
  static bool done_header = false;

  set_colour (COLOR_WHITE);
  mvprintw (0, 0, HEADER, spinner[spin_idx & 3]);
  set_colour (0);

  if (!done_header)
  {
    mvhline (1, 0, ACS_HLINE, strlen(HEADER)-1);
    done_header = true;
  }
#else
  /*
   * Unless debug or raw-mode is active, clear the screen to remove old info.
   * But only if current number of aircrafts is less than last time. This is to
   * avoid an annoying blinking of the console.
   */
  if (Modes.debug == 0)
  {
    if (old_count == -1 || aircraft_numbers() < old_count || console_messed_up())
       clrscr();
    gotoxy (1, 1);
  }

  set_colour (COLOR_WHITE);
  printf (HEADER "\n", spinner[spin_idx & 3]);
  set_colour (0);
  puts ("-----------------------------------------------------------------------------------------------------");
#endif

  spin_idx++;
  row = 2;

  while (a && count < Modes.interactive_rows && !Modes.exit)
  {
    bool colour_changed = false;

    if (a->show != A_SHOW_NONE)
    {
      set_est_home_distance (a, now);
      colour_changed = interactive_show_aircraft (a, row, now);
      row++;
    }

    /* Simple state-machine for the plane's show-state
     */
    if (a->show == A_SHOW_FIRST_TIME)
       a->show = A_SHOW_NORMAL;
    else if (a->show == A_SHOW_LAST_TIME)
       a->show = A_SHOW_NONE;      /* don't show again before deleting it */

    a = a->next;
    count++;

#if 0
    if (colour_changed || alternate_colours)
    {
      set_colour (COLOR_YELLOW);
      alternate_colours ^= 1;
    }
#else
    (void) colour_changed;
#endif
  }

#if defined(USE_CURSES)
  move (row, 0);
  clrtobot();
  refresh();
#else
  (void) row;
#endif

  old_count = count;
}

/**
 * Receive new messages and populate the interactive mode with more info.
 */
aircraft *interactive_receive_data (const modeS_message *mm, uint64_t now)
{
  aircraft *a;
  char     *p;
  uint32_t  addr;

  if (!mm->CRC_ok)
     return (NULL);

  /* Loookup our aircraft or create a new one.
   */
  addr = aircraft_get_addr (mm->AA[0], mm->AA[1], mm->AA[2]);
  a = aircraft_find_or_create (addr, now);
  if (!a)
     return (NULL);

  a->seen_last = now;
  a->messages++;

  /* Ensure number of elements is 2^n.
   */
  assert ((DIM(a->sig_levels) & -(int)DIM(a->sig_levels)) == DIM(a->sig_levels));

  a->sig_levels [a->sig_idx++] = mm->sig_level;
  a->sig_idx &= DIM(a->sig_levels) - 1;

  if (mm->msg_type == 5 || mm->msg_type == 21)
  {
    if (mm->identity)
         a->identity = mm->identity;   /* Set thee Squawk code. */
    else a->identity = 0;
  }

  if (mm->msg_type == 0 || mm->msg_type == 4 || mm->msg_type == 20)
  {
    a->altitude = mm->altitude;
  }
  else if (mm->msg_type == 17)
  {
    if (mm->ME_type >= 1 && mm->ME_type <= 4)
    {
      memcpy (a->flight, mm->flight, sizeof(a->flight));
      p = a->flight + sizeof(a->flight)-1;
      while (*p == ' ')
        *p-- = '\0';  /* Remove trailing spaces */

    }
    else if ((mm->ME_type >= 9  && mm->ME_type <= 18) || /* Airborne Position (Baro Altitude) */
             (mm->ME_type >= 20 && mm->ME_type <= 22))   /* Airborne Position (GNSS Height) */
    {
      a->altitude = mm->altitude;
      if (mm->odd_flag)
      {
        a->odd_CPR_lat  = mm->raw_latitude;
        a->odd_CPR_lon  = mm->raw_longitude;
        a->odd_CPR_time = now;
      }
      else
      {
        a->even_CPR_lat  = mm->raw_latitude;
        a->even_CPR_lon  = mm->raw_longitude;
        a->even_CPR_time = now;
      }

      /* If the two reports are less than 10 minutes apart, compute the position.
       * This used to be '10 sec', but I used some code from:
       *   https://github.com/Mictronics/readsb/blob/master/track.c
       *
       * which says:
       *   A wrong relative position decode would require the aircraft to
       *   travel 360-100=260 NM in the 10 minutes of position validity.
       *   This is impossible for planes slower than 1560 knots (Mach 2.3) over the ground.
       */
      int64_t t_diff = (int64_t) (a->even_CPR_time - a->odd_CPR_time);

      if (llabs(t_diff) <= 60*10*1000)
           decode_CPR (a);
   /* else LOG_FILEONLY ("t_diff for '%04X' too large: %lld sec.\n", a->addr, t_diff/1000); */
    }
    else if (mm->ME_type == 19)
    {
      if (mm->ME_subtype == 1 || mm->ME_subtype == 2)
      {
        a->speed   = mm->velocity;
        a->heading = mm->heading;
        a->heading_is_valid = mm->heading_is_valid;
      }
    }
  }
  return (a);
}

