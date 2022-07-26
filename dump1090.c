/**
 * \file    dump1090.c
 * \ingroup Main
 * \brief   Dump1090, a Mode-S messages decoder for RTLSDR devices.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include <malloc.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <limits.h>
#include <assert.h>
#include <sys/stat.h>
#include <io.h>
#include <conio.h>
#include <process.h>

#include "misc.h"
#include "trace.h"
#include "sdrplay.h"

/**
 * \addtogroup Main      Main decoder
 * \addtogroup Misc      Support functions
 * \addtogroup Mongoose  Web server
 *
 * \mainpage Dump1090
 *
 * # Introduction
 *
 * A simple ADS-B (**Automatic Dependent Surveillance - Broadcast**) receiver, decoder and web-server. <br>
 * It requires a *RTLSDR* USB-stick and a USB-driver installed using the *Automatic Driver Installer*
 * [**Zadig**](https://zadig.akeo.ie/).
 *
 * The code for Osmocom's [**librtlsdr**](https://osmocom.org/projects/rtl-sdr/wiki) is built into this program.
 * Hence no dependency on *RTLSDR.DLL*.
 *
 * This *Mode S* decoder is based on the Dump1090 by *Salvatore Sanfilippo*.
 *
 * ### Basic block-diagram:
 * \image html dump1090-blocks.png
 *
 * ### Example Web-client page:
 * \image html dump1090-web.png
 *
 * ### More here later ...
 *
 * Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * ```
 *  *  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  *  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ```
 */

#define MODES_DEFAULT_RATE         2000000
#define MODES_DEFAULT_FREQ         1090000000
#define MODES_ASYNC_BUF_NUMBER     12
#define MODES_DATA_LEN             (16*16384)   /* 256k */

#define MODES_PREAMBLE_US             8         /* microseconds */
#define MODES_LONG_MSG_BITS         112
#define MODES_SHORT_MSG_BITS         56
#define MODES_FULL_LEN             (MODES_PREAMBLE_US + MODES_LONG_MSG_BITS)
#define MODES_LONG_MSG_BYTES       (MODES_LONG_MSG_BITS / 8)
#define MODES_SHORT_MSG_BYTES      (MODES_SHORT_MSG_BITS / 8)
#define MODES_MAX_SBS_SIZE          256

#define MODES_ICAO_CACHE_LEN       1024   /* Power of two required. */
#define MODES_ICAO_CACHE_TTL         60   /* Time to live of cached addresses (sec). */
#define MODES_UNIT_FEET               0
#define MODES_UNIT_METERS             1

/**
 * When debug is set to DEBUG_NOPREAMBLE, the first sample must be
 * at least greater than a given level for us to dump the signal.
 */
#define DEBUG_NOPREAMBLE_LEVEL           25

#define MODES_INTERACTIVE_REFRESH_TIME  250   /* Milliseconds */
#define MODES_INTERACTIVE_ROWS           15   /* Rows on screen */
#define MODES_INTERACTIVE_TTL         60000   /* TTL (msec) before being removed */
#define MODES_CONNECT_TIMEOUT          5000   /* msec timeout for an active connect */

#define MG_NET_POLL_TIME             (MODES_INTERACTIVE_REFRESH_TIME / 2)

#define MODES_CONTENT_TYPE_CSS    "text/css;charset=utf-8"
#define MODES_CONTENT_TYPE_HTML   "text/html;charset=utf-8"
#define MODES_CONTENT_TYPE_JSON   "application/json"
#define MODES_CONTENT_TYPE_JS     "application/javascript;charset=utf-8"
#define MODES_CONTENT_TYPE_PNG    "image/png"

global_data Modes;

/**
 * \typedef struct modeS_message
 * The structure we use to store information about a decoded message.
 */
typedef struct modeS_message {
        uint8_t  msg [MODES_LONG_MSG_BYTES]; /**< Binary message. */
        int      msg_bits;                   /**< Number of bits in message */
        int      msg_type;                   /**< Downlink format # */
        bool     CRC_ok;                     /**< True if CRC was valid */
        uint32_t CRC;                        /**< Message CRC */
        double   sig_level;                  /**< RSSI, in the range [0..1], as a fraction of full-scale power */
        int      error_bit;                  /**< Bit corrected. -1 if no bit corrected. */
        int      AA1, AA2, AA3;              /**< ICAO Address bytes 1, 2 and 3 */
        bool     phase_corrected;            /**< True if phase correction was applied. */

        /** DF11
         */
        int ca;                              /**< Responder capabilities. */

        /** DF 17
         */
        int  ME_type;                        /**< Extended squitter message type. */
        int  ME_subtype;                     /**< Extended squitter message subtype. */
        int  heading;                        /**< Horizontal angle of flight. */
        bool heading_is_valid;
        int  aircraft_type;
        int  odd_flag;                       /**< 1 = Odd, 0 = Even CPR message. */
        int  UTC_flag;                       /**< UTC synchronized? */
        int  raw_latitude;                   /**< Non decoded latitude */
        int  raw_longitude;                  /**< Non decoded longitude */
        char flight [9];                     /**< 8 chars flight number. */
        int  EW_dir;                         /**< 0 = East, 1 = West. */
        int  EW_velocity;                    /**< E/W velocity. */
        int  NS_dir;                         /**< 0 = North, 1 = South. */
        int  NS_velocity;                    /**< N/S velocity. */
        int  vert_rate_source;               /**< Vertical rate source. */
        int  vert_rate_sign;                 /**< Vertical rate sign. */
        int  vert_rate;                      /**< Vertical rate. */
        int  velocity;                       /**< Computed from EW and NS velocity. */

        /** DF4, DF5, DF20, DF21
         */
        int flight_status;                   /**< Flight status for DF4, 5, 20 and 21 */
        int DR_status;                       /**< Request extraction of downlink request. */
        int UM_status;                       /**< Request extraction of downlink request. */
        int identity;                        /**< 13 bits identity (Squawk). */

        /** Fields used by multiple message types.
         */
        int altitude, unit;
      } modeS_message;

aircraft *interactive_receive_data (const modeS_message *mm, uint64_t now);
void      modeS_send_raw_output (const modeS_message *mm);
void      modeS_send_SBS_output (const modeS_message *mm, const aircraft *a);
void      modeS_user_message (const modeS_message *mm);
void      set_est_home_distance (aircraft *a, uint64_t now);
void      spherical_to_cartesian (cartesian_t *cart, pos_t point);
bool      ICAO_is_military (uint32_t addr);

int       fix_single_bit_errors (uint8_t *msg, int bits);
int       fix_two_bits_errors (uint8_t *msg, int bits);
int       detect_modeS (uint16_t *m, uint32_t mlen);
void      decode_hex_message (mg_iobuf *msg, int loop_cnt);
void      decode_SBS_message (mg_iobuf *msg, int loop_cnt);
int       modeS_message_len_by_type (int type);
uint16_t *compute_magnitude_vector (const uint8_t *data);
void      background_tasks (void);
void      modeS_exit (void);
void      sigint_handler (int sig);

u_short        handler_port (intptr_t service);
const char    *handler_descr (intptr_t service);
mg_connection *handler_conn (intptr_t service);
void           connection_read (connection *conn, msg_handler handler, bool is_server);
void           connection_send (intptr_t service, const void *msg, size_t len);
connection    *connection_get_addr (const mg_addr *addr, intptr_t service, bool is_server);
void           connection_free (connection *this_conn, intptr_t service);
unsigned       connection_free_all (void);

static CONSOLE_SCREEN_BUFFER_INFO console_info;
static HANDLE console_hnd = INVALID_HANDLE_VALUE;
static DWORD  console_mode = 0;
static bool   dev_selection_done = false;

#define COLOUR_GREEN  10  /* bright green; FOREGROUND_INTENSITY + 2 */
#define COLOUR_RED    12  /* bright red;   FOREGROUND_INTENSITY + 4 */
#define COLOUR_WHITE  15  /* bright white; FOREGROUND_INTENSITY + 7 */

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

void setcolor (int color)
{
  WORD attr;

  if (console_hnd == INVALID_HANDLE_VALUE)
     return;

  attr = console_info.wAttributes;
  if (color > 0)
  {
    attr &= ~7;
    attr |= color;
  }
  SetConsoleTextAttribute (console_hnd, attr);
}

void console_title_stats (void)
{
  char            buf [100];
  char            gain [10];
  static uint64_t last_good_CRC, last_bad_CRC;
  static int      ovl_count = 0;
  static char    *overload = "            ";
  uint64_t        good_CRC = Modes.stat.good_CRC + Modes.stat.fixed;
  uint64_t        bad_CRC  = Modes.stat.bad_CRC  - Modes.stat.fixed;

  if (Modes.gain_auto)
       strcpy (gain, "auto");
  else snprintf (gain, sizeof(gain), "%.1f", (double)Modes.gain / 10.0);

  if (bad_CRC - last_bad_CRC > 2*(good_CRC - last_good_CRC))
  {
     overload = " (too high?)";
     ovl_count = 3;   /* let it show for 3 refreshes */
  }
  else if (ovl_count > 0 && --ovl_count == 0)
    overload = "            ";

  snprintf (buf, sizeof(buf), "Dev: %s. CRC: %llu / %llu / %llu. Gain: %s dB%s",
            Modes.selected_dev ? Modes.selected_dev : "?",
            good_CRC, Modes.stat.fixed, bad_CRC, gain, overload);

  last_good_CRC = good_CRC;
  last_bad_CRC  = bad_CRC;
  SetConsoleTitle (buf);
}

static int gain_increase (int gain_idx)
{
  if (Modes.rtlsdr.device && gain_idx < Modes.rtlsdr.gain_count-1)
  {
    Modes.gain = Modes.rtlsdr.gains [++gain_idx];
    rtlsdr_set_tuner_gain (Modes.rtlsdr.device, Modes.gain);
  }
  else if (Modes.sdrplay.device && gain_idx < Modes.sdrplay.gain_count-1)
  {
    Modes.gain = Modes.sdrplay.gains [++gain_idx];
    sdrplay_set_gain (Modes.sdrplay.device, Modes.gain);
  }
  LOG_FILEONLY ("Increasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  return (gain_idx);
}

static int gain_decrease (int gain_idx)
{
  if (Modes.rtlsdr.device && gain_idx > 0)
  {
    Modes.gain = Modes.rtlsdr.gains [--gain_idx];
    rtlsdr_set_tuner_gain (Modes.rtlsdr.device, Modes.gain);
  }
  else if (Modes.sdrplay.device && gain_idx > 0)
  {
    Modes.gain = Modes.sdrplay.gains [--gain_idx];
    sdrplay_set_gain (Modes.sdrplay.device, Modes.gain);
  }
  LOG_FILEONLY ("Decreasing gain to %.1f dB.\n", (double)Modes.gain / 10.0);
  return (gain_idx);
}

/**
 * Poll for '+/-' keypresses and adjust the RTLSDR / SDRplay gain accordingly.
 * But within the min/max gain settings for the device.
 */
void console_update_gain (void)
{
  static int gain_idx = -1;
  int i, ch;

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

int console_init (void)
{
  console_hnd = GetStdHandle (STD_OUTPUT_HANDLE);
  if (console_hnd == INVALID_HANDLE_VALUE)
     return (1);

  GetConsoleScreenBufferInfo (console_hnd, &console_info);
  GetConsoleMode (console_hnd, &console_mode);
  if (console_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)
     SetConsoleMode (console_hnd, console_mode | DISABLE_NEWLINE_AUTO_RETURN);

  if (Modes.interactive_rows == 0)  /* Option `--interactive-rows` not used */
     Modes.interactive_rows = console_info.srWindow.Bottom - console_info.srWindow.Top - 1;
  return (0);
}

void console_exit (void)
{
  gotoxy (1, Modes.interactive_rows);
  setcolor (0);
  if (console_hnd != INVALID_HANDLE_VALUE)
     SetConsoleMode (console_hnd, console_mode);
  console_hnd = INVALID_HANDLE_VALUE;
}

#if defined(_DEBUG)
static _CrtMemState last_state;

void crtdbug_exit (void)
{
  _CrtMemState new_state, diff_state;

  _CrtMemCheckpoint (&new_state);
  if (!_CrtMemDifference(&diff_state, &last_state, &new_state))
  {
    LOG_STDERR ("No mem-leaks detected.\n");
    return;
  }
  LOG_STDERR ("Mem-leak report:\n");
  _CrtCheckMemory();
  _CrtSetDbgFlag (0);
  _CrtDumpMemoryLeaks();
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
  _CrtMemCheckpoint (&last_state);
}
#endif  /* _DEBUG */

/**
 * Return a string describing an error-code from RTLSDR
 *
 * This can be from `librtlsdr` itself or from `WinUsb`.
 */
const char *get_rtlsdr_error (int err)
{
  if (err >= 0)
     return ("No error");
  if (err == -ENOMEM)
     return strerror (-err);

#if 0  // \todo
  return rtlsdr_error_name (err);
#else
  static char buf [100];
  snprintf (buf, sizeof(buf), "WinUsb-error %d", err);
  return (buf);
#endif
}

/**
 * Set the RTLSDR gain verbosively.
 */
void verbose_gain_set (rtlsdr_dev_t *dev, int gain)
{
  int r = rtlsdr_set_tuner_gain_mode (dev, 1);

  if (r < 0)
  {
    LOG_STDERR ("WARNING: Failed to enable manual gain.\n");
    return;
  }
  r = rtlsdr_set_tuner_gain (dev, gain);
  if (r)
       LOG_STDERR ("WARNING: Failed to set tuner gain.\n");
  else LOG_STDERR ("Tuner gain set to %.0f dB.\n", gain/10.0);
}

/**
 * Set the RTLSDR gain verbosively to AUTO.
 */
void verbose_gain_auto (rtlsdr_dev_t *dev)
{
  int r = rtlsdr_set_tuner_gain_mode (dev, 0);

  if (r)
       LOG_STDERR ("WARNING: Failed to enable automatic gain.\n");
  else LOG_STDERR ("Tuner gain set to automatic.\n");
}

/**
 * Set the RTLSDR gain verbosively to the nearest available
 * gain value given in `*target_gain`.
 */
void nearest_gain (rtlsdr_dev_t *dev, uint16_t *target_gain)
{
  int    gain_in;
  int    i, err1, err2, nearest;
  int    r = rtlsdr_set_tuner_gain_mode (dev, 1);
  char   gbuf [200], *p = gbuf;
  size_t left = sizeof(gbuf);

  if (r)
  {
    LOG_STDERR ("WARNING: Failed to enable manual gain.\n");
    return;
  }

  Modes.rtlsdr.gain_count = rtlsdr_get_tuner_gains (dev, NULL);
  if (Modes.rtlsdr.gain_count <= 0)
     return;

  Modes.rtlsdr.gains = malloc (sizeof(int) * Modes.rtlsdr.gain_count);
  Modes.rtlsdr.gain_count = rtlsdr_get_tuner_gains (dev, Modes.rtlsdr.gains);
  nearest = Modes.rtlsdr.gains[0];
  if (!target_gain)
     return;

  gain_in = *target_gain;

  for (i = 0; i < Modes.rtlsdr.gain_count; i++)
  {
    err1 = abs (gain_in - nearest);
    err2 = abs (gain_in - Modes.rtlsdr.gains[i]);

    p += snprintf (p, left, "%.1f, ", Modes.rtlsdr.gains[i] / 10.0);
    left = sizeof(gbuf) - (p - gbuf) - 1;
    if (err2 < err1)
       nearest = Modes.rtlsdr.gains[i];
  }
  p[-2] = '\0';
  LOG_STDERR ("Supported gains: %s.\n", gbuf);
  *target_gain = (uint16_t) nearest;
}

/**
 * Enable RTLSDR direct sampling mode (not used yet).
 */
void verbose_direct_sampling (rtlsdr_dev_t *dev, int on)
{
  int r = rtlsdr_set_direct_sampling (dev, on);

  if (r)
  {
    LOG_STDERR ("WARNING: Failed to set direct sampling mode.\n");
    return;
  }
  if (on == 0)
     LOG_STDERR ("Direct sampling mode disabled.\n");
  else if (on == 1)
     LOG_STDERR ("Enabled direct sampling mode, input 1/I.\n");
  else if (on == 2)
     LOG_STDERR ("Enabled direct sampling mode, input 2/Q.\n");
}

/**
 * Set RTLSDR PPM error-correction.
 */
void verbose_ppm_set (rtlsdr_dev_t *dev, int ppm_error)
{
  double tuner_freq = 0.0;
  int    r;

  r = rtlsdr_set_freq_correction (dev, ppm_error);
  if (r < 0)
     LOG_STDERR ("WARNING: Failed to set PPM correction.\n");
  else
  {
    rtlsdr_get_xtal_freq (dev, NULL, &tuner_freq);
    LOG_STDERR ("Tuner correction set to %d PPM; %.3lf MHz.\n", ppm_error, tuner_freq / 1E6);
  }
}

/**
 * Set RTLSDR automatic gain control.
 */
void verbose_agc_set (rtlsdr_dev_t *dev, int agc)
{
  int r = rtlsdr_set_agc_mode (dev, agc);

  if (r < 0)
       LOG_STDERR ("WARNING: Failed to set AGC.\n");
  else LOG_STDERR ("AGC %s okay.\n", agc ? "enabled" : "disabled");
}

/**
 * Set RTLSDR Bias-T
 */
void verbose_bias_tee (rtlsdr_dev_t *dev, int bias_t)
{
  int r = rtlsdr_set_bias_tee (dev, bias_t);

  if (bias_t)
  {
    if (r)
         LOG_STDERR ("Failed to activate Bias-T.\n");
    else LOG_STDERR ("Activated Bias-T on GPIO PIN 0.\n");
  }
}

/**
 * Add an aircraft record to `Modes.aircraft_list`.
 */
int aircraft_CSV_add_entry (const aircraft_CSV *rec)
{
  static aircraft_CSV *copy = NULL;
  static aircraft_CSV *dest = NULL;
  static aircraft_CSV *hi_end;

  /* Not a valid ICAO address. Parse error?
   */
  if (rec->addr == 0 || rec->addr > 0xFFFFFF)
     return (1);

  if (!copy)
  {
    copy = dest = malloc (ONE_MEGABYTE);  /* initial buffer */
    hi_end = copy + (ONE_MEGABYTE / sizeof(*rec));
  }
  else if (dest == hi_end - 1)
  {
    size_t new_num = 10000 + Modes.aircraft_num_CSV;

    copy   = realloc (Modes.aircraft_list, sizeof(*rec) * new_num);
    dest   = copy + Modes.aircraft_num_CSV;
    hi_end = copy + new_num;
  }

  if (!copy)
     return (0);

  Modes.aircraft_list = copy;
  assert (dest < hi_end);
  memcpy (dest, rec, sizeof(*rec));
  Modes.aircraft_num_CSV++;
  dest = copy + Modes.aircraft_num_CSV;
  return (1);
}

/**
 * The compare function for `qsort()` and `bsearch()`.
 */
int aircraft_CSV_compare_on_addr (const void *_a, const void *_b)
{
  const aircraft_CSV *a = (const aircraft_CSV*) _a;
  const aircraft_CSV *b = (const aircraft_CSV*) _b;

  if (a->addr < b->addr)
     return (-1);
  if (a->addr > b->addr)
     return (1);
  return (0);
}

/**
 * Do a binary search for an aircraft in `Modes.aircraft_list`.
 */
const aircraft_CSV *aircraft_CSV_lookup_entry (uint32_t addr)
{
  aircraft_CSV key = { addr, "" };

  if (!Modes.aircraft_list)
     return (NULL);
  return bsearch (&key, Modes.aircraft_list, Modes.aircraft_num_CSV,
                  sizeof(*Modes.aircraft_list), aircraft_CSV_compare_on_addr);
}

/**
 * If `Modes.debug != 0`, do a simple test on the `Modes.aircraft_list`.
 */
void aircraft_CSV_test (void)
{
  const aircraft_CSV *a_CSV;
  const char  *reg_num, *manufact;
  unsigned     i, num_ok;
  uint32_t     addr;
  static const aircraft_CSV a_tests[] = {
               { 0xAA3487, "N757F",   "Raytheon Aircraft Company" },
               { 0x800737, "VT-ANQ",  "Boeing" },
               { 0xAB34DE, "N821DA",  "Beech"  },
               { 0x800737, "VT-ANQ",  "Boeing" },
               { 0xA713D5, "N555UW",  "Piper" },
               { 0x3532C1, "T.23-01", "AIRBUS" },  // Spanish Air Force
             };
#if 0
  for (i = 0; i < min(100, Modes.aircraft_num_CSV); i++)
  {
    a_CSV = Modes.aircraft_list + i;
    LOG_STDOUT ("  addr: %06X, reg-num: '%s'\n", a_CSV->addr, a_CSV->reg_num);
  }
#endif

  LOG_STDOUT ("5 random records from \"%s\":\n", Modes.aircraft_db);
  for (i = num_ok = 0; i < DIM(a_tests); i++)
  {
    addr = a_tests[i].addr;
    a_CSV = aircraft_CSV_lookup_entry (addr);
    reg_num = manufact = "?";
    if (a_CSV && a_CSV->reg_num[0])
    {
      reg_num = a_CSV->reg_num;
      num_ok++;
    }
    if (a_CSV && a_CSV->manufact[0])
       manufact = a_CSV->manufact;
    LOG_STDOUT ("  addr: %06X, reg-num: '%-7s', manufact: '%s' %s\n",
                addr, reg_num, manufact, ICAO_is_military(addr) ? ", Military" : "");
  }
  LOG_STDOUT ("%3u OKAY\n", num_ok);
  LOG_STDOUT ("%3u FAIL\n", i - num_ok);
}

/**
 * The CSV callback for adding a record to `Modes.aircraft_list`.
 *
 * \param[in]  ctx   the CSV context structure.
 * \param[in]  value the value for this CSV field in record `ctx->rec_num`.
 *
 * Match the fields 0, 1, 3 and 10 for a record like this:
 * ```
 * "icao24","registration","manufacturericao","manufacturername","model","typecode","serialnumber","linenumber",
 * "icaoaircrafttype","operator","operatorcallsign","operatoricao","operatoriata","owner","testreg","registered",
 * "reguntil","status","built","firstflightdate","seatconfiguration","engines","modes","adsb","acars","notes",
 * "categoryDescription"
 * ```
 *
 * 27 fields!
 */
int aircraft_CSV_parse (struct CSV_context *ctx, const char *value)
{
  static aircraft_CSV rec = { 0, "" };
  int    rc = 1;

  if (ctx->field_num == 0)        /* "icao24" field */
  {
    if (strlen(value) == 6)
       rec.addr = mg_unhexn (value, 6);
  }
  else if (ctx->field_num == 1)   /* "registration" field */
  {
    strncpy (rec.reg_num, value, sizeof(rec.reg_num));
  }
  else if (ctx->field_num == 3)   /* "manufacturername" field */
  {
    strncpy (rec.manufact, value, sizeof(rec.manufact));
  }
  else if (ctx->field_num == 10)  /* "operatorcallsign" field */
  {
    strncpy (rec.call_sign, value, sizeof(rec.call_sign));
  }
  else if (ctx->field_num == ctx->num_fields - 1)  /* we got the last field */
  {
    rc = aircraft_CSV_add_entry (&rec);
    memset (&rec, '\0', sizeof(rec));    /* ready for a new record. */
  }
  return (rc);
}

/**
 * Initialize the aircraft-database from .csv file.
 */
void aircraft_CSV_load (void)
{
  struct stat st;

  if (!_stricmp(Modes.aircraft_db, "NUL"))   /* User want no .csv file */
     return;

  if (stat(Modes.aircraft_db, &st) != 0)
  {
    LOG_STDERR ("Aircraft database \"%s\" does not exist.\n", Modes.aircraft_db);
    return;
  }

  memset (&Modes.csv_ctx, '\0', sizeof(Modes.csv_ctx));
  Modes.csv_ctx.file_name  = Modes.aircraft_db;
  Modes.csv_ctx.delimiter  = ',';
  Modes.csv_ctx.callback   = aircraft_CSV_parse;
  Modes.csv_ctx.line_size  = 2000;
  if (!CSV_open_and_parse_file(&Modes.csv_ctx))
  {
    LOG_STDERR ("Parsing of \"%s\" failed: %s\n", Modes.aircraft_db, strerror(errno));
    return;
  }

  TRACE (DEBUG_GENERAL, "Parsed %u records from: \"%s\"\n", Modes.aircraft_num_CSV, Modes.aircraft_db);
  if (Modes.aircraft_num_CSV > 0)
  {
    qsort (Modes.aircraft_list, Modes.aircraft_num_CSV, sizeof(*Modes.aircraft_list), aircraft_CSV_compare_on_addr);
    if (Modes.debug)
       aircraft_CSV_test();
  }
}

/**
 * Step 1: Initialize the program with default values.
 */
void modeS_init_config (void)
{
  memset (&Modes, '\0', sizeof(Modes));
  GetCurrentDirectoryA (sizeof(Modes.where_am_I), Modes.where_am_I);
  GetModuleFileNameA (NULL, Modes.who_am_I, sizeof(Modes.who_am_I));

  strcpy (Modes.web_page, basename(GMAP_HTML));
  strcpy (Modes.web_root, dirname(Modes.who_am_I));
  strcat (Modes.web_root, "\\web_root");
  snprintf (Modes.aircraft_db, sizeof(Modes.aircraft_db), "%s\\aircraftDatabase.csv", dirname(Modes.who_am_I));

  Modes.gain_auto   = true;
  Modes.sample_rate = MODES_DEFAULT_RATE;
  Modes.freq        = MODES_DEFAULT_FREQ;
  Modes.interactive_ttl  = MODES_INTERACTIVE_TTL;
  Modes.interactive_rows = 25;
  Modes.json_interval    = 1000;
}

/**
 * Step 2:
 *  \li Open and append to the `--logfile` if specified.
 *  \li In `--net` mode (but not `--net-active` mode), check the precence of the Web-page.
 *  \li Set our home position from the env-var `%DUMP1090_HOMEPOS%`.
 *  \li Initialize the `Modes.data_mutex`.
 *  \li Setup a SIGINT/SIGBREAK handler for a clean exit.
 *  \li Allocate and initialize the needed buffers.
 *  \li Open and parse the `Modes.aircraft_db` file (unless `NUL`).
 */
int modeS_init (void)
{
  int         I, Q;
  pos_t       pos;
  const char *env;

  if (Modes.logfile)
  {
    Modes.log = fopen (Modes.logfile, "a");
    if (!Modes.log)
       LOG_STDERR ("Failed to create/append to \"%s\".\n", Modes.logfile);
    else
    {
      char   args [1000] = "";
      char   buf [sizeof(args)+MG_PATH_MAX+10];
      char  *p = args;
      size_t n, left = sizeof(args);
      int    i;

      for (i = 1; i < __argc && left > 2; i++)
      {
        n = snprintf (p, left, " %s", __argv[i]);
        p    += n;
        left -= n;
      }
      fputc ('\n', Modes.log);
      snprintf (buf, sizeof(buf), "------- Starting '%s%s' -----------\n", Modes.who_am_I, args);
      modeS_log (buf);
    }
  }

  /** By default, disable all logging from Mongoose
   */
  mg_log_set ("0");

  if (!Modes.interactive)
  {
    if (Modes.debug & DEBUG_NET)
         mg_log_set ("2");               /** Enable `LL_ERROR`, `LL_INFO` messages in Mongose */
    else if (Modes.debug & DEBUG_NET2)
         mg_log_set ("3");               /** Enable `LL_DEBUG` messages in Mongose */
  }

  env = getenv ("DUMP1090_HOMEPOS");
  if (env)
  {
    if (sscanf(env, "%lf,%lf", &pos.lat, &pos.lon) != 2 || !VALID_POS(pos))
    {
      LOG_STDERR ("Invalid home-pos %s\n", env);
      return (1);
    }
    Modes.home_pos = pos;
    Modes.home_pos_ok = true;
    spherical_to_cartesian (&Modes.home_pos_cart, Modes.home_pos);
  }

  InitializeCriticalSection (&Modes.data_mutex);
  InitializeCriticalSection (&Modes.print_mutex);
  signal (SIGINT, sigint_handler);
  signal (SIGBREAK, sigint_handler);

  /* We add a full message minus a final bit to the length, so that we
   * can carry the remaining part of the buffer that we can't process
   * in the message detection loop, back at the start of the next data
   * to process. This way we are able to also detect messages crossing
   * two reads.
   */
  Modes.data_len = MODES_DATA_LEN + 4*(MODES_FULL_LEN-1);
  Modes.data_ready = false;

  /**
   * Allocate the ICAO address cache. We use two uint32_t for every
   * entry because it's a addr / timestamp pair for every entry.
   */
  Modes.ICAO_cache    = calloc (sizeof(uint32_t)*MODES_ICAO_CACHE_LEN*2, 1);
  Modes.data          = malloc (Modes.data_len);
  Modes.magnitude     = malloc (2*Modes.data_len);
  Modes.magnitude_lut = malloc (sizeof(*Modes.magnitude_lut) * 129 * 129);

  if (!Modes.ICAO_cache || !Modes.data || !Modes.magnitude || !Modes.magnitude_lut)
  {
    LOG_STDERR ("Out of memory allocating data buffer.\n");
    return (1);
  }

  memset (Modes.data, 127, Modes.data_len);

  /* Populate the I/Q -> Magnitude lookup table. It is used because
   * hypot() or round() may be expensive and may vary a lot depending on
   * the CRT used.
   *
   * We scale to 0-255 range multiplying by 1.4 in order to ensure that
   * every different I/Q pair will result in a different magnitude value,
   * not losing any resolution.
   */
  for (I = 0; I < 129; I++)
  {
    for (Q = 0; Q < 129; Q++)
       Modes.magnitude_lut [I*129+Q] = (uint16_t) round (360 * hypot(I, Q));
  }

  aircraft_CSV_load();

  if (Modes.interactive && Modes.debug == 0)
     return console_init();
  return (0);
}

/**
 * Step 3: Initialize the RTLSDR device.
 *
 * If `Modes.rtlsdr.name` is specified, select the device that matches `manufact`.
 * Otherwise select on `Modes.rtlsdr.index` where 0 is the first device found.
 *
 * If one have > 1 RTLSDR device with the same product name and serial-number,
 * then the program `rtl_eeprom -d 1 -M "name"` is handy to set them apart.
 * Like:
 *  ```
 *   manufact: Silver, product: RTL2838UHIDIR, serial: 00000001
 *   manufact: Blue, product: RTL2838UHIDIR, serial: 00000001
 *  ```
 */
int modeS_init_RTLSDR (void)
{
  int    i, rc, device_count;
  double gain;

  device_count = rtlsdr_get_device_count();
  if (!device_count)
  {
    LOG_STDERR ("No supported RTLSDR devices found.\n");
    return (1);
  }

  LOG_STDERR ("Found %d device(s):\n", device_count);
  for (i = 0; i < device_count; i++)
  {
    char manufact [256] = "??";
    char product [256]  = "??";
    char serial [256]  = "??";
    bool selected = false;
    int  r = rtlsdr_get_device_usb_strings (i, manufact, product, serial);

    if (r == 0)
    {
      if (Modes.rtlsdr.name && manufact[0] && !_stricmp(Modes.rtlsdr.name, manufact))
      {
        selected = true;
        Modes.rtlsdr.index = i;
      }
      else
        selected = (i == Modes.rtlsdr.index);

      if (selected)
      {
        char buf [sizeof(manufact) + sizeof(product)];

        strcpy (buf, manufact);
        strcat (buf, ": ");
        strcat (buf, product);
        Modes.selected_dev = _strdup (buf);
      }
    }
    LOG_STDERR ("%d: %-10s %-20s SN: %s %s\n", i, manufact, product, serial,
                selected ? "(currently selected)" : "");
  }

#if defined(HAVE_rtlsdr_cal_imr)
  if (Modes.rtlsdr.calibrate)
     rtlsdr_cal_imr (1);
#endif

  rc = rtlsdr_open (&Modes.rtlsdr.device, Modes.rtlsdr.index);
  if (rc < 0)
  {
    LOG_STDERR ("Error opening the RTLSDR device %d: %s.\n", Modes.rtlsdr.index, get_rtlsdr_error(rc));
    return (1);
  }

  /* Set gain, frequency, sample rate, and reset the device.
   */
  if (Modes.gain_auto)
  {
    nearest_gain (Modes.rtlsdr.device, NULL);
    verbose_gain_auto (Modes.rtlsdr.device);
  }
  else
  {
    nearest_gain (Modes.rtlsdr.device, &Modes.gain);
    verbose_gain_set (Modes.rtlsdr.device, Modes.gain);
  }

  if (Modes.dig_agc)
     verbose_agc_set (Modes.rtlsdr.device, 1);

  if (Modes.rtlsdr.ppm_error)
     verbose_ppm_set (Modes.rtlsdr.device, Modes.rtlsdr.ppm_error);

  if (Modes.bias_tee)
     verbose_bias_tee (Modes.rtlsdr.device, Modes.bias_tee);

  rc = rtlsdr_set_center_freq (Modes.rtlsdr.device, Modes.freq);
  if (rc)
  {
    LOG_STDERR ("Error setting frequency: %d.\n", rc);
    return (1);
  }

  rc = rtlsdr_set_sample_rate (Modes.rtlsdr.device, Modes.sample_rate);
  if (rc)
  {
    LOG_STDERR ("Error setting sample-rate: %d.\n", rc);
    return (1);
  }

  rtlsdr_reset_buffer (Modes.rtlsdr.device);

  LOG_STDERR ("Tuned to %.03f MHz.\n", Modes.freq / 1E6);

  gain = rtlsdr_get_tuner_gain (Modes.rtlsdr.device);
  if ((unsigned int)gain == 0)
       LOG_STDERR ("Gain reported by device: AUTO.\n");
  else LOG_STDERR ("Gain reported by device: %.2f dB.\n", gain/10.0);
  return (0);
}

/**
 * This reading callback gets data from the RTLSDR or SDRplay API asynchronously.
 * We then populate the data buffer.
 *
 * A Mutex is used to avoid race-condition with the decoding thread.
 */
void rx_callback (uint8_t *buf, uint32_t len, void *ctx)
{
  volatile bool exit = *(volatile bool*) ctx;

  if (exit)
     return;

  EnterCriticalSection (&Modes.data_mutex);
  if (len > MODES_DATA_LEN)
     len = MODES_DATA_LEN;

  /* Move the last part of the previous buffer, that was not processed,
   * to the start of the new buffer.
   */
  memcpy (Modes.data, Modes.data + MODES_DATA_LEN, 4*(MODES_FULL_LEN-1));

  /* Read the new data.
   */
  memcpy (Modes.data + 4*(MODES_FULL_LEN-1), buf, len);
  Modes.data_ready = true;
  LeaveCriticalSection (&Modes.data_mutex);
}

/**
 * This is used when `--infile` is specified in order to read data from file
 * instead of using a RTLSDR / SDRplay device.
 */
int read_from_data_file (void)
{
  if (Modes.loops > 0 && Modes.fd == STDIN_FILENO)
  {
    LOG_STDERR ("Option `--loop <N>` not supported for `stdin`.\n");
    Modes.loops = 0;
  }

  do
  {
     int nread, toread;
     uint8_t *p;

     if (Modes.interactive)
     {
       /* When --infile and --interactive are used together, slow down
        * mimicking the real RTLSDR / SDRplay rate.
        */
       Sleep (1000);
     }

     /* Move the last part of the previous buffer, that was not processed,
      * on the start of the new buffer.
      */
     memcpy (Modes.data, Modes.data + MODES_DATA_LEN, 4*(MODES_FULL_LEN-1));
     toread = MODES_DATA_LEN;
     p = Modes.data + 4*(MODES_FULL_LEN-1);

     while (toread)
     {
       nread = _read (Modes.fd, p, toread);
       if (nread <= 0)
          break;
       p += nread;
       toread -= nread;
     }

     if (toread)
     {
       /* Not enough data on file to fill the buffer? Pad with
        * no signal.
        */
       memset (p, 127, toread);
     }

     compute_magnitude_vector (Modes.data);
     detect_modeS (Modes.magnitude, Modes.data_len/2);
     background_tasks();

     if (Modes.exit || Modes.fd == STDIN_FILENO)
        break;

     /* seek the file again from the start
      * and re-play it if --loop was given.
      */
     if (Modes.loops > 0)
        Modes.loops--;
     if (Modes.loops == 0 || _lseek(Modes.fd, 0, SEEK_SET) == -1)
        break;
  }
  while (1);
  return (0);  /**\todo add a check for errors above */
}

/**
 * We read RTLSDR or SDRplay data using a separate thread, so the main thread
 * only handles decoding without caring about data acquisition.
 * Ref. `main_data_loop()` below.
 */
unsigned int __stdcall data_thread_fn (void *arg)
{
  int rc;

  if (Modes.sdrplay.device)
  {
    rc = sdrplay_read_async (Modes.sdrplay.device, rx_callback, (void*)&Modes.exit,
                             MODES_ASYNC_BUF_NUMBER, MODES_DATA_LEN);

    TRACE (DEBUG_GENERAL, "sdrplay_read_async(): rc: %d / %s.\n",
           rc, sdrplay_strerror(rc));

    sigint_handler (0);   /* break out of main_data_loop() */
  }
  else if (Modes.rtlsdr.device)
  {
    rc = rtlsdr_read_async (Modes.rtlsdr.device, rx_callback, (void*)&Modes.exit,
                            MODES_ASYNC_BUF_NUMBER, MODES_DATA_LEN);

    TRACE (DEBUG_GENERAL, "rtlsdr_read_async(): rc: %d/%s.\n",
           rc, get_rtlsdr_error(rc));

    sigint_handler (0);    /* break out of main_data_loop() */
  }
  MODES_NOTUSED (arg);
  return (0);
}

/**
 * Main data processing loop.
 *
 * This runs in the main thread of the program.
 */
void main_data_loop (void)
{
  while (!Modes.exit)
  {
    background_tasks();

    if (!Modes.data_ready)
       continue;

    compute_magnitude_vector (Modes.data);

    /* Signal to the other thread that we processed the available data
     * and we want more.
     */
    Modes.data_ready = false;

    /* Process data after releasing the lock, so that the capturing
     * thread can read data while we perform computationally expensive
     * stuff at the same time. (This should only be useful with very
     * slow processors).
     */
    EnterCriticalSection (&Modes.data_mutex);

#if 0     /**\todo */
    if (Modes.sdrplay_device && Modes.sdrplay.over_sample)
    {
      struct mag_buf *buf = &Modes.mag_buffers [Modes.first_filled_buffer];
      demodulate_8000 (buf);
    }
    else
#endif
      detect_modeS (Modes.magnitude, Modes.data_len/2);

    LeaveCriticalSection (&Modes.data_mutex);

    if (/* rc > 0 && */ Modes.max_messages > 0)
    {
      if (--Modes.max_messages == 0)
      {
        LOG_STDOUT ("'Modes.max_messages' reached 0.\n");
        Modes.exit = true;
       }
    }
  }
}

/**
 * Helper function for `dump_magnitude_vector()`.
 * It prints a single bar used to display raw signals.
 *
 * Since every magnitude sample is between 0 - 255, the function uses
 * up to 63 characters for every bar. Every character represents
 * a length of 4, 3, 2, 1, specifically:
 *
 * \li "O" is 4
 * \li "o" is 3
 * \li "-" is 2
 * \li "." is 1
 */
void dump_magnitude_bar (uint16_t magnitude, int index)
{
  const char *set = " .-o";
  char        buf [256];
  uint16_t    div = (magnitude / 256) / 4;
  uint16_t    rem = (magnitude / 256) % 4;
  int         markchar = ']';

  memset (buf, 'O', div);
  buf [div] = set[rem];
  buf [div+1] = '\0';

  if (index >= 0)
  {
    /* preamble peaks are marked with ">"
     */
    if (index == 0 || index == 2 || index == 7 || index == 9)
       markchar = '>';

    /* Data peaks are marked to distinguish pairs of bits.
     */
    if (index >= 16)
       markchar = ((index - 16)/2 & 1) ? '|' : ')';
    printf ("[%3d%c |%-66s %u\n", index, markchar, buf, magnitude);
  }
  else
    printf ("[%3d] |%-66s %u\n", index, buf, magnitude);
}

/**
 * Display an *ASCII-art* alike graphical representation of the undecoded
 * message as a magnitude signal.
 *
 * The message starts at the specified offset in the `m` buffer.
 * The function will display enough data to cover a short 56 bit
 * (`MODES_SHORT_MSG_BITS`) message.
 *
 * If possible a few samples before the start of the messsage are included
 * for context.
 */
void dump_magnitude_vector (const uint16_t *m, uint32_t offset)
{
  uint32_t padding = 5;  /* Show a few samples before the actual start. */
  uint32_t start = (offset < padding) ? 0 : offset - padding;
  uint32_t end = offset + (2*MODES_PREAMBLE_US) + (2*MODES_SHORT_MSG_BITS) - 1;
  uint32_t i;

  for (i = start; i <= end; i++)
      dump_magnitude_bar (m[i], i - offset);
}

/**
 * Produce a raw representation of the message as a Javascript file
 * loadable by `debug.html`.
 */
void dump_raw_message_JS (const char *descr, uint8_t *msg, const uint16_t *m, uint32_t offset, int fixable)
{
  int   padding = 5;     /* Show a few samples before the actual start. */
  int   start = offset - padding;
  int   end = offset + (MODES_PREAMBLE_US*2)+(MODES_LONG_MSG_BITS*2) - 1;
  int   j, fix1 = -1, fix2 = -1;
  FILE *fp;

  if (fixable != -1)
  {
    fix1 = fixable & 0xFF;
    if (fixable > 255)
       fix2 = fixable >> 8;
  }
  fp = fopen ("frames.js", "a");
  if (!fp)
  {
    LOG_STDERR ("Error opening frames.js: %s\n", strerror(errno));
    exit (1);
  }

  fprintf (fp, "frames.push({\"descr\": \"%s\", \"mag\": [", descr);
  for (j = start; j <= end; j++)
  {
    fprintf (fp,"%d", j < 0 ? 0 : m[j]);
    if (j != end)
       fprintf (fp, ",");
  }
  fprintf (fp, "], \"fix1\": %d, \"fix2\": %d, \"bits\": %d, \"hex\": \"",
           fix1, fix2, modeS_message_len_by_type(msg[0] >> 3));
  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
      fprintf (fp, "\\x%02x", msg[j]);
  fprintf (fp, "\"});\n");
  fclose (fp);
}

/**
 * This is a wrapper for `dump_magnitude_vector()` that also show the message
 * in hex format with an additional description.
 *
 * \param in  descr  the additional message to show to describe the dump.
 * \param out msg    the decoded message
 * \param in  m      the original magnitude vector
 * \param in  offset the offset where the message starts
 *
 * The function also produces the Javascript file used by `debug.html` to
 * display packets in a graphical format if the Javascript output was
 * enabled.
 */
void dump_raw_message (const char *descr, uint8_t *msg, const uint16_t *m, uint32_t offset)
{
  int j;
  int msg_type = msg[0] >> 3;
  int fixable = -1;

  if (msg_type == 11 || msg_type == 17)
  {
    int msg_bits = (msg_type == 11) ? MODES_SHORT_MSG_BITS :
                                      MODES_LONG_MSG_BITS;
    fixable = fix_single_bit_errors (msg, msg_bits);
    if (fixable == -1)
       fixable = fix_two_bits_errors (msg, msg_bits);
  }

  if (Modes.debug & DEBUG_JS)
  {
    dump_raw_message_JS (descr, msg, m, offset, fixable);
    return;
  }

  EnterCriticalSection (&Modes.print_mutex);

  printf ("\n--- %s:\n    ", descr);
  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
  {
    printf ("%02X", msg[j]);
    if (j == MODES_SHORT_MSG_BYTES-1)
       printf (" ... ");
  }
  printf (" (DF %d, Fixable: %d)\n", msg_type, fixable);
  dump_magnitude_vector (m, offset);
  puts ("---\n");

  LeaveCriticalSection (&Modes.print_mutex);
}

/**
 * Parity table for MODE S Messages.
 *
 * The table contains 112 (`MODES_LONG_MSG_BITS`) elements, every element
 * corresponds to a bit set in the message, starting from the first bit of
 * actual data after the preamble.
 *
 * For messages of 112 bit, the whole table is used.
 * For messages of 56 bits only the last 56 elements are used.
 *
 * The algorithm is as simple as XOR-ing all the elements in this table
 * for which the corresponding bit on the message is set to 1.
 *
 * The last 24 elements in this table are set to 0 as the checksum at the
 * end of the message should not affect the computation.
 *
 * \note
 * This function can be used with DF11 and DF17. Other modes have
 * the CRC *XOR-ed* with the sender address as they are replies to interrogations,
 * but a casual listener can't split the address from the checksum.
 */
const uint32_t modeS_checksum_table [MODES_LONG_MSG_BITS] = {
               0x3935EA, 0x1C9AF5, 0xF1B77E, 0x78DBBF, 0xC397DB, 0x9E31E9, 0xB0E2F0, 0x587178,
               0x2C38BC, 0x161C5E, 0x0B0E2F, 0xFA7D13, 0x82C48D, 0xBE9842, 0x5F4C21, 0xD05C14,
               0x682E0A, 0x341705, 0xE5F186, 0x72F8C3, 0xC68665, 0x9CB936, 0x4E5C9B, 0xD8D449,
               0x939020, 0x49C810, 0x24E408, 0x127204, 0x093902, 0x049C81, 0xFDB444, 0x7EDA22,
               0x3F6D11, 0xE04C8C, 0x702646, 0x381323, 0xE3F395, 0x8E03CE, 0x4701E7, 0xDC7AF7,
               0x91C77F, 0xB719BB, 0xA476D9, 0xADC168, 0x56E0B4, 0x2B705A, 0x15B82D, 0xF52612,
               0x7A9309, 0xC2B380, 0x6159C0, 0x30ACE0, 0x185670, 0x0C2B38, 0x06159C, 0x030ACE,
               0x018567, 0xFF38B7, 0x80665F, 0xBFC92B, 0xA01E91, 0xAFF54C, 0x57FAA6, 0x2BFD53,
               0xEA04AD, 0x8AF852, 0x457C29, 0xDD4410, 0x6EA208, 0x375104, 0x1BA882, 0x0DD441,
               0xF91024, 0x7C8812, 0x3E4409, 0xE0D800, 0x706C00, 0x383600, 0x1C1B00, 0x0E0D80,
               0x0706C0, 0x038360, 0x01C1B0, 0x00E0D8, 0x00706C, 0x003836, 0x001C1B, 0xFFF409,
               0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
               0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
               0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000
             };

uint32_t modeS_checksum (const uint8_t *msg, int bits)
{
  uint32_t crc = 0;
  int      offset = 0;
  int      j;

  if (bits != MODES_LONG_MSG_BITS)
     offset = MODES_LONG_MSG_BITS - MODES_SHORT_MSG_BITS;

  for (j = 0; j < bits; j++)
  {
    int byte = j / 8;
    int bit  = j % 8;
    int bitmask = 1 << (7 - bit);

    /* If bit is set, XOR with corresponding table entry.
     */
    if (msg[byte] & bitmask)
       crc ^= modeS_checksum_table [j + offset];
  }
  return (crc); /* 24 bit checksum. */
}

/**
 * Given the Downlink Format (DF) of the message, return the message length
 * in bits.
 */
int modeS_message_len_by_type (int type)
{
  if (type == 16 || type == 17 || type == 19 || type == 20 || type == 21)
     return (MODES_LONG_MSG_BITS);
  return (MODES_SHORT_MSG_BITS);
}

/**
 * Try to fix single bit errors using the checksum. On success modifies
 * the original buffer with the fixed version, and returns the position
 * of the error bit. Otherwise if fixing failed, -1 is returned.
 */
int fix_single_bit_errors (uint8_t *msg, int bits)
{
  int     i;
  uint8_t aux [MODES_LONG_MSG_BITS/8];

  for (i = 0; i < bits; i++)
  {
    int      byte = i / 8;
    int      bitmask = 1 << (7-(i % 8));
    uint32_t crc1, crc2;

    memcpy (aux, msg, bits/8);
    aux[byte] ^= bitmask;   /* Flip j-th bit. */

    crc1 = ((uint32_t)aux[(bits/8)-3] << 16) |
           ((uint32_t)aux[(bits/8)-2] << 8) |
            (uint32_t)aux[(bits/8)-1];
    crc2 = modeS_checksum (aux, bits);

    if (crc1 == crc2)
    {
      /* The error is fixed. Overwrite the original buffer with
       * the corrected sequence, and returns the error bit
       * position.
       */
      memcpy (msg, aux, bits/8);
      return (i);
    }
  }
  return (-1);
}

/**
 * Similar to `fix_single_bit_errors()` but try every possible two bit combination.
 *
 * This is very slow and should be tried only against DF17 messages that
 * don't pass the checksum, and only in Aggressive Mode.
 */
int fix_two_bits_errors (uint8_t *msg, int bits)
{
  int     j, i;
  uint8_t aux [MODES_LONG_MSG_BITS/8];

  for (j = 0; j < bits; j++)
  {
    int byte1 = j / 8;
    int bitmask1 = 1 << (7-(j % 8));

    /* Don't check the same pairs multiple times, so i starts from j+1 */
    for (i = j+1; i < bits; i++)
    {
      int      byte2 = i / 8;
      int      bitmask2 = 1 << (7-(i % 8));
      uint32_t crc1, crc2;

      memcpy (aux, msg, bits/8);

      aux[byte1] ^= bitmask1; /* Flip j-th bit. */
      aux[byte2] ^= bitmask2; /* Flip i-th bit. */

      crc1 = ((uint32_t) aux [(bits/8)-3] << 16) |
             ((uint32_t) aux [(bits/8)-2] << 8) |
              (uint32_t) aux [(bits/8)-1];
      crc2 = modeS_checksum (aux, bits);

      if (crc1 == crc2)
      {
        /* The error is fixed. Overwrite the original buffer with
         * the corrected sequence, and returns the error bit
         * position.
         */
        memcpy (msg, aux, bits/8);

        /* We return the two bits as a 16 bit integer by shifting
         * 'i' on the left. This is possible since 'i' will always
         * be non-zero because i starts from j+1.
         */
        return (j | (i << 8));
      }
    }
  }
  return (-1);
}

/**
 * Hash the ICAO address to index our cache of MODES_ICAO_CACHE_LEN
 * elements, that is assumed to be a power of two.
 */
uint32_t ICAO_cache_hash_address (uint32_t a)
{
  /* The following three rounds will make sure that every bit affects
   * every output bit with ~ 50% of probability.
   */
  a = ((a >> 16) ^ a) * 0x45D9F3B;
  a = ((a >> 16) ^ a) * 0x45D9F3B;
  a = ((a >> 16) ^ a);
  return (a & (MODES_ICAO_CACHE_LEN-1));
}

/**
 * Add the specified entry to the cache of recently seen ICAO addresses.
 *
 * Note that we also add a timestamp so that we can make sure that the
 * entry is only valid for `MODES_ICAO_CACHE_TTL` seconds.
 */
void ICAO_cache_add_address (uint32_t addr)
{
  uint32_t h = ICAO_cache_hash_address (addr);

  Modes.ICAO_cache [h*2] = addr;
  Modes.ICAO_cache [h*2+1] = (uint32_t) time (NULL);
}

/**
 * Returns 1 if the specified ICAO address was seen in a DF format with
 * proper checksum (not XORed with address) no more than
 * `MODES_ICAO_CACHE_TTL` seconds ago. Otherwise returns 0.
 */
int ICAO_address_recently_seen (uint32_t addr)
{
  uint32_t h = ICAO_cache_hash_address (addr);
  uint32_t a = Modes.ICAO_cache [h*2];
  uint32_t t = Modes.ICAO_cache [h*2+1];
  time_t   now = time (NULL);

  return (a && a == addr && (now - t) <= MODES_ICAO_CACHE_TTL);
}

/**
 * Returns TRUE if the ICAO address is in one of these military ranges.
 */
static const ICAO_range military_range[] = {
     { 0xADF7C8,  0xAFFFFF },
     { 0x010070,  0x01008F },
     { 0x0A4000,  0x0A4FFF },
     { 0x33FF00,  0x33FFFF },
     { 0x350000,  0x37FFFF },
     { 0x3A8000,  0x3AFFFF },
     { 0x3B0000,  0x3BFFFF },
     { 0x3EA000,  0x3EBFFF },
     { 0x3F4000,  0x3FBFFF },
     { 0x400000,  0x40003F },
     { 0x43C000,  0x43CFFF },
     { 0x444000,  0x446FFF },
     { 0x44F000,  0x44FFFF },
     { 0x457000,  0x457FFF },
     { 0x45F400,  0x45F4FF },
     { 0x468000,  0x4683FF },
     { 0x473C00,  0x473C0F },
     { 0x478100,  0x4781FF },
     { 0x480000,  0x480FFF },
     { 0x48D800,  0x48D87F },
     { 0x497C00,  0x497CFF },
     { 0x498420,  0x49842F },
     { 0x4B7000,  0x4B7FFF },
     { 0x4B8200,  0x4B82FF },
     { 0x506F00,  0x506FFF },
     { 0x70C070,  0x70C07F },
     { 0x710258,  0x71028F },
     { 0x710380,  0x71039F },
     { 0x738A00,  0x738AFF },
     { 0x7C822E,  0x7C84FF },
     { 0x7C8800,  0x7C88FF },
     { 0x7C9000,  0x7CBFFF },
     { 0x7D0000,  0x7FFFFF },
     { 0x800200,  0x8002FF },
     { 0xC20000,  0xC3FFFF },
     { 0xE40000,  0xE41FFF }
   };

bool ICAO_is_military (uint32_t addr)
{
  for (uint16_t i = 0; i < DIM(military_range); i++)
      if (addr >= military_range[i].low && addr <= military_range[i].high)
         return (true);
  return (false);
}

/**
 * If the message type has the checksum XORed with the ICAO address, try to
 * brute force it using a list of recently seen ICAO addresses.
 *
 * Do this in a brute-force fashion by XORing the predicted CRC with
 * the address XOR checksum field in the message. This will recover the
 * address: if we found it in our cache, we can assume the message is okay.
 *
 * This function expects `mm->msg_type` and `mm->msg_bits` to be correctly
 * populated by the caller.
 *
 * On success the correct ICAO address is stored in the `modeS_message`
 * structure in the `AA3`, `AA2`, and `AA1` fields.
 *
 * \retval 1 successfully recovered a message with a correct checksum.
 * \retval 0 failed to recover a message with a correct checksum.
 */
int brute_force_AP (uint8_t *msg, modeS_message *mm)
{
  uint8_t aux [MODES_LONG_MSG_BYTES];
  int     msg_type = mm->msg_type;
  int     msg_bits = mm->msg_bits;

  if (msg_type == 0 ||         /* Short air surveillance */
      msg_type == 4 ||         /* Surveillance, altitude reply */
      msg_type == 5 ||         /* Surveillance, identity reply */
      msg_type == 16 ||        /* Long Air-Air Surveillance */
      msg_type == 20 ||        /* Comm-A, altitude request */
      msg_type == 21 ||        /* Comm-A, identity request */
      msg_type == 24)          /* Comm-C ELM */
  {
    uint32_t addr;
    uint32_t crc;
    int      last_byte = (msg_bits / 8) - 1;

    /* Work on a copy. */
    memcpy (aux, msg, msg_bits/8);

    /* Compute the CRC of the message and XOR it with the AP field
     * so that we recover the address, because:
     *
     * (ADDR xor CRC) xor CRC = ADDR.
     */
    crc = modeS_checksum (aux, msg_bits);
    aux [last_byte]   ^= crc & 0xFF;
    aux [last_byte-1] ^= (crc >> 8) & 0xFF;
    aux [last_byte-2] ^= (crc >> 16) & 0xFF;

    /* If the obtained address exists in our cache we consider
     * the message valid.
     */
    addr = aux[last_byte] |
           (aux[last_byte-1] << 8) |
           (aux[last_byte-2] << 16);
    if (ICAO_address_recently_seen(addr))
    {
      mm->AA1 = aux [last_byte-2];
      mm->AA2 = aux [last_byte-1];
      mm->AA3 = aux [last_byte];
      return (1);
    }
  }
  return (0);
}

/**
 * Decode the 13 bit AC altitude field (in DF 20 and others).
 *
 * \param in  msg   the raw message to work with.
 * \param out unit  set to either `MODES_UNIT_METERS` or `MDOES_UNIT_FEETS`.
 * \retval the altitude.
 */
int decode_AC13_field (const uint8_t *msg, int *unit)
{
  int m_bit = msg[3] & (1 << 6);
  int q_bit = msg[3] & (1 << 4);

  if (!m_bit)
  {
    *unit = MODES_UNIT_FEET;
    if (q_bit)
    {
      /* N is the 11 bit integer resulting from the removal of bit Q and M
       */
      int n = ((msg[2] & 31) << 6)   |
              ((msg[3] & 0x80) >> 2) |
              ((msg[3] & 0x20) >> 1) |
               (msg[3] & 15);

      /** The final altitude is due to the resulting number multiplied
       * by 25, minus 1000.
       */
      return (25*n - 1000);
    }
    else
    {
      /** \todo Implement altitude where Q=0 and M=0 */
    }
  }
  else
  {
    *unit = MODES_UNIT_METERS;

    /** \todo Implement altitude when meter unit is selected.
     */
  }
  return (0);
}

/**
 * Decode the 12 bit AC altitude field (in DF 17 and others).
 * Returns the altitude or 0 if it can't be decoded.
 */
int decode_AC12_field (uint8_t *msg, int *unit)
{
  int n, q_bit = msg[5] & 1;

  if (q_bit)
  {
    /* N is the 11 bit integer resulting from the removal of bit Q
     */
    *unit = MODES_UNIT_FEET;
    n = ((msg[5] >> 1) << 4) | ((msg[6] & 0xF0) >> 4);

    /* The final altitude is due to the resulting number multiplied
     * by 25, minus 1000.
     */
    return (25 * n - 1000);
  }
  return (0);
}

/**
 * Capability table.
 */
const char *capability_str[8] = {
    /* 0 */ "Level 1 (Surveillance Only)",
    /* 1 */ "Level 2 (DF0,4,5,11)",
    /* 2 */ "Level 3 (DF0,4,5,11,20,21)",
    /* 3 */ "Level 4 (DF0,4,5,11,20,21,24)",
    /* 4 */ "Level 2+3+4 (DF0,4,5,11,20,21,24,code7 - is on ground)",
    /* 5 */ "Level 2+3+4 (DF0,4,5,11,20,21,24,code7 - is airborne)",
    /* 6 */ "Level 2+3+4 (DF0,4,5,11,20,21,24,code7)",
    /* 7 */ "Level 7 ???"
};

/**
 * Flight status table.
 */
const char *flight_status_str[8] = {
    /* 0 */ "Normal, Airborne",
    /* 1 */ "Normal, On the ground",
    /* 2 */ "ALERT,  Airborne",
    /* 3 */ "ALERT,  On the ground",
    /* 4 */ "ALERT & Special Position Identification. Airborne or Ground",
    /* 5 */ "Special Position Identification. Airborne or Ground",
    /* 6 */ "Value 6 is not assigned",
    /* 7 */ "Value 7 is not assigned"
};

/**
 * Emergency state table from: <br>
 *   https://www.ll.mit.edu/mission/aviation/publications/publication-files/atc-reports/Grappel_2007_ATC-334_WW-15318.pdf
 *
 * and 1090-DO-260B_FRAC
 */
const char *emerg_state_str[8] = {
    /* 0 */ "No emergency",
    /* 1 */ "General emergency (Squawk 7700)",
    /* 2 */ "Lifeguard/Medical",
    /* 3 */ "Minimum fuel",
    /* 4 */ "No communications (Squawk 7600)",
    /* 5 */ "Unlawful interference (Squawk 7500)",
    /* 6 */ "Reserved",
    /* 7 */ "Reserved"
};

const char *get_ME_description (const modeS_message *mm)
{
  if (mm->ME_type >= 1 && mm->ME_type <= 4)
     return ("Aircraft Identification and Category");

  if (mm->ME_type >= 5 && mm->ME_type <= 8)
     return ("Surface Position");

  if (mm->ME_type >= 9 && mm->ME_type <= 18)
     return ("Airborne Position (Baro Altitude)");

  if (mm->ME_type == 19 && mm->ME_subtype >=1 && mm->ME_subtype <= 4)
     return ("Airborne Velocity");

  if (mm->ME_type >= 20 && mm->ME_type <= 22)
     return ("Airborne Position (GNSS Height)");

  if (mm->ME_type == 23 && mm->ME_subtype == 0)
     return ("Test Message");

   if (mm->ME_type == 23 && mm->ME_subtype == 7)
     return ("Test Message -- Squawk");

  if (mm->ME_type == 24 && mm->ME_subtype == 1)
     return ("Surface System Status");

  if (mm->ME_type == 28 && mm->ME_subtype == 1)
     return ("Extended Squitter Aircraft Status (Emergency)");

  if (mm->ME_type == 28 && mm->ME_subtype == 2)
     return ("Extended Squitter Aircraft Status (1090ES TCAS RA)");

  if (mm->ME_type == 29 && (mm->ME_subtype == 0 || mm->ME_subtype == 1))
     return ("Target State and Status Message");

  if (mm->ME_type == 31 && (mm->ME_subtype == 0 || mm->ME_subtype == 1))
     return ("Aircraft Operational Status Message");

  return ("Unknown");
}

/**
 * Decode a raw Mode S message demodulated as a stream of bytes by `detect_modeS()`.
 *
 * And split it into fields populating a `modeS_message` structure.
 */
void decode_modeS_message (modeS_message *mm, const uint8_t *_msg)
{
  uint32_t    crc2;   /* Computed CRC, used to verify the message CRC. */
  const char *AIS_charset = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
  uint8_t    *msg;

  memset (mm, '\0', sizeof(*mm));

  /* Work on our local copy
   */
  memcpy (mm->msg, _msg, sizeof(mm->msg));
  msg = mm->msg;

  /* Get the message type ASAP as other operations depend on this
   */
  mm->msg_type = msg[0] >> 3;    /* Downlink Format */
  mm->msg_bits = modeS_message_len_by_type (mm->msg_type);

  /* CRC is always the last three bytes.
   */
  mm->CRC = ((uint32_t)msg[(mm->msg_bits/8)-3] << 16) |
            ((uint32_t)msg[(mm->msg_bits/8)-2] << 8) |
             (uint32_t)msg[(mm->msg_bits/8)-1];
  crc2 = modeS_checksum (msg, mm->msg_bits);

  /* Check CRC and fix single bit errors using the CRC when
   * possible (DF 11 and 17).
   */
  mm->error_bit = -1;    /* No error */
  mm->CRC_ok = (mm->CRC == crc2);

  if (!mm->CRC_ok && (mm->msg_type == 11 || mm->msg_type == 17))
  {
    mm->error_bit = fix_single_bit_errors (msg, mm->msg_bits);
    if (mm->error_bit != -1)
    {
      mm->CRC = modeS_checksum (msg, mm->msg_bits);
      mm->CRC_ok = true;
    }
    else if (Modes.aggressive && mm->msg_type == 17 && (mm->error_bit = fix_two_bits_errors(msg, mm->msg_bits)) != -1)
    {
      mm->CRC = modeS_checksum (msg, mm->msg_bits);
      mm->CRC_ok = true;
    }
  }

  /* Note: most of the other computation happens **after** we fix the single bit errors.
   * Otherwise we would need to recompute the fields again.
   */
  mm->ca = msg[0] & 7;        /* Responder capabilities. */

  /* ICAO address
   */
  mm->AA1 = msg[1];
  mm->AA2 = msg[2];
  mm->AA3 = msg[3];

  /* DF17 type (assuming this is a DF17, otherwise not used)
   */
  mm->ME_type = msg[4] >> 3;         /* Extended squitter message type. */
  mm->ME_subtype = msg[4] & 7;       /* Extended squitter message subtype. */

  /* Fields for DF4,5,20,21
   */
  mm->flight_status = msg[0] & 7;         /* Flight status for DF4,5,20,21 */
  mm->DR_status = msg[1] >> 3 & 31;       /* Request extraction of downlink request. */
  mm->UM_status = ((msg[1] & 7) << 3) |   /* Request extraction of downlink request. */
                  (msg[2] >> 5);

  /*
   * In the squawk (identity) field bits are interleaved like this:
   * (message bit 20 to bit 32):
   *
   * C1-A1-C2-A2-C4-A4-ZERO-B1-D1-B2-D2-B4-D4
   *
   * So every group of three bits A, B, C, D represent an integer
   * from 0 to 7.
   *
   * The actual meaning is just 4 octal numbers, but we convert it
   * into a base ten number that happens to represent the four octal numbers.
   *
   * For more info: http://en.wikipedia.org/wiki/Gillham_code
   */
  {
    int a, b, c, d;

    a = ((msg[3] & 0x80) >> 5) |
        ((msg[2] & 0x02) >> 0) |
        ((msg[2] & 0x08) >> 3);
    b = ((msg[3] & 0x02) << 1) |
        ((msg[3] & 0x08) >> 2) |
        ((msg[3] & 0x20) >> 5);
    c = ((msg[2] & 0x01) << 2) |
        ((msg[2] & 0x04) >> 1) |
        ((msg[2] & 0x10) >> 4);
    d = ((msg[3] & 0x01) << 2) |
        ((msg[3] & 0x04) >> 1) |
        ((msg[3] & 0x10) >> 4);
    mm->identity = a*1000 + b*100 + c*10 + d;
  }

  /* DF 11 & 17: try to populate our ICAO addresses whitelist.
   * DFs with an AP field (XORed addr and CRC), try to decode it.
   */
  if (mm->msg_type != 11 && mm->msg_type != 17)
  {
    /* Check if we can check the checksum for the Downlink Formats where
     * the checksum is XORed with the aircraft ICAO address. We try to
     * brute force it using a list of recently seen aircraft addresses.
     */
    if (brute_force_AP(msg, mm))
    {
      /* We recovered the message, mark the checksum as valid.
       */
      mm->CRC_ok = true;
    }
    else
      mm->CRC_ok = false;
  }
  else
  {
    /* If this is DF 11 or DF 17 and the checksum was ok, we can add this address to the list
     * of recently seen addresses.
     */
    if (mm->CRC_ok && mm->error_bit == -1)
    {
      uint32_t addr = (mm->AA1 << 16) | (mm->AA2 << 8) | mm->AA3;
      ICAO_cache_add_address (addr);
    }
  }

  /* Decode 13 bit altitude for DF0, DF4, DF16, DF20
   */
  if (mm->msg_type == 0 || mm->msg_type == 4 || mm->msg_type == 16 || mm->msg_type == 20)
     mm->altitude = decode_AC13_field (msg, &mm->unit);

  /** Decode extended squitter specific stuff.
   */
  if (mm->msg_type == 17)
  {
    /* Decode the extended squitter message.
     */
    if (mm->ME_type >= 1 && mm->ME_type <= 4)
    {
      /* Aircraft Identification and Category
       */
      mm->aircraft_type = mm->ME_type - 1;
      mm->flight[0] = AIS_charset [msg[5] >> 2];
      mm->flight[1] = AIS_charset [((msg[5] & 3) << 4) | (msg[6] >> 4)];
      mm->flight[2] = AIS_charset [((msg[6] & 15) <<2 ) | (msg[7] >> 6)];
      mm->flight[3] = AIS_charset [msg[7] & 63];
      mm->flight[4] = AIS_charset [msg[8] >> 2];
      mm->flight[5] = AIS_charset [((msg[8] & 3) << 4) | (msg[9] >> 4)];
      mm->flight[6] = AIS_charset [((msg[9] & 15) << 2) | (msg[10] >> 6)];
      mm->flight[7] = AIS_charset [msg[10] & 63];
      mm->flight[8] = '\0';
    }
    else if (mm->ME_type >= 9 && mm->ME_type <= 18)
    {
      /* Airborne position Message
       */
      mm->odd_flag = msg[6] & (1 << 2);
      mm->UTC_flag = msg[6] & (1 << 3);
      mm->altitude = decode_AC12_field (msg, &mm->unit);
      mm->raw_latitude  = ((msg[6] & 3) << 15) | (msg[7] << 7) | (msg[8] >> 1); /* Bits 23 - 39 */
      mm->raw_longitude = ((msg[8] & 1) << 16) | (msg[9] << 8) | msg[10];       /* Bits 40 - 56 */
    }
    else if (mm->ME_type == 19 && mm->ME_subtype >= 1 && mm->ME_subtype <= 4)
    {
      /* Airborne Velocity Message
       */
      if (mm->ME_subtype == 1 || mm->ME_subtype == 2)
      {
        mm->EW_dir           = (msg[5] & 4) >> 2;
        mm->EW_velocity      = ((msg[5] & 3) << 8) | msg[6];
        mm->NS_dir           = (msg[7] & 0x80) >> 7;
        mm->NS_velocity      = ((msg[7] & 0x7F) << 3) | ((msg[8] & 0xE0) >> 5);
        mm->vert_rate_source = (msg[8] & 0x10) >> 4;
        mm->vert_rate_sign   = (msg[8] & 0x08) >> 3;
        mm->vert_rate        = ((msg[8] & 7) << 6) | ((msg[9] & 0xFC) >> 2);

        /* Compute velocity and angle from the two speed components.
         */
        mm->velocity = (int) hypot (mm->NS_velocity, mm->EW_velocity);   /* hypot(x,y) == sqrt(x*x+y*y) */

        if (mm->velocity)
        {
          int    ewV = mm->EW_velocity;
          int    nsV = mm->NS_velocity;
          double heading;

          if (mm->EW_dir)
             ewV *= -1;
          if (mm->NS_dir)
             nsV *= -1;
          heading = atan2 (ewV, nsV);

          /* Convert to degrees.
           */
          mm->heading = (int) (heading * 360 / TWO_PI);
          mm->heading_is_valid = true;

          /* We don't want negative values but a [0 .. 360> scale.
           */
          if (mm->heading < 0)
             mm->heading += 360;
        }
        else
          mm->heading = 0;
      }
      else if (mm->ME_subtype == 3 || mm->ME_subtype == 4)
      {
        mm->heading_is_valid = msg[5] & (1 << 2);
        mm->heading = (int) (360.0/128) * (((msg[5] & 3) << 5) | (msg[6] >> 3));
      }
    }
  }
  mm->phase_corrected = false;  /* Set to 'true' by the caller if needed. */
}

/**
 * Return the hex-string for a 24-bit ICAO address.
 * Also look for the registration number and manufacturer from
 * the `Modes.aircraft_list`.
 */
const char *get_ICAO_details (int AA1, int AA2, int AA3)
{
  static char ret_buf[100];
  const aircraft_CSV *a;
  char  *p = ret_buf;
  size_t n, left = sizeof(ret_buf);
  uint32_t addr = (AA1 << 16) + (AA2 << 8) + AA3;

  n = snprintf (p, left, "%02X%02X%02X", AA1, AA2, AA3);
  p    += n;
  left -= n;

  a = aircraft_CSV_lookup_entry (addr);
  if (a && a->reg_num[0])
     snprintf (p, left, " (reg-num: %s, manuf: %s, call-sign: %s%s)",
               a->reg_num, a->manufact[0] ? a->manufact : "?", a->call_sign[0] ? a->call_sign : "?",
               ICAO_is_military(addr) ? ", Military" : "");
  return (ret_buf);
}

/**
 * This function gets a decoded Mode S Message and prints it on the screen
 * in a human readable format.
 */
void display_modeS_message (const modeS_message *mm)
{
  char   buf [200];
  char  *p = buf;
  size_t left = sizeof(buf);
  int    i;

  /* Handle only addresses mode first.
   */
  if (Modes.only_addr)
  {
    puts (get_ICAO_details(mm->AA1, mm->AA2, mm->AA3));
    return;
  }

  /* Show the raw message.
   */
  *p++ = '*';
  left--;
  for (i = 0; i < mm->msg_bits/8 && left > 5; i++)
  {
    snprintf (p, left, "%02x", mm->msg[i]);
    p    += 2;
    left -= 2;
  }
  *p++ = ';';
  *p++ = '\n';
  *p = '\0';
  LOG_STDOUT ("%s", buf);

  if (Modes.raw)
     return;         /* Enough for --raw mode */

  LOG_STDOUT ("CRC: %06X (%s)\n", (int)mm->CRC, mm->CRC_ok ? "ok" : "wrong");
  if (mm->error_bit != -1)
     LOG_STDOUT ("Single bit error fixed, bit %d\n", mm->error_bit);

  if (mm->sig_level > 0)
     LOG_STDOUT ("RSSI: %.1lf dBFS\n", 10 * log10(mm->sig_level));

  if (mm->msg_type == 0)
  {
    /* DF 0 */
    LOG_STDOUT ("DF 0: Short Air-Air Surveillance.\n");
    LOG_STDOUT ("  Altitude       : %d %s\n", mm->altitude, mm->unit == MODES_UNIT_METERS ? "meters" : "feet");
    LOG_STDOUT ("  ICAO Address   : %s\n", get_ICAO_details(mm->AA1, mm->AA2, mm->AA3));
  }
  else if (mm->msg_type == 4 || mm->msg_type == 20)
  {
    LOG_STDOUT ("DF %d: %s, Altitude Reply.\n", mm->msg_type, mm->msg_type == 4 ? "Surveillance" : "Comm-B");
    LOG_STDOUT ("  Flight Status  : %s\n", flight_status_str [mm->flight_status]);
    LOG_STDOUT ("  DR             : %d\n", mm->DR_status);
    LOG_STDOUT ("  UM             : %d\n", mm->UM_status);
    LOG_STDOUT ("  Altitude       : %d %s\n", mm->altitude, mm->unit == MODES_UNIT_METERS ? "meters" : "feet");
    LOG_STDOUT ("  ICAO Address   : %s\n", get_ICAO_details(mm->AA1, mm->AA2, mm->AA3));

    if (mm->msg_type == 20)
    {
      /** \todo 56 bits DF20 MB additional field. */
    }
  }
  else if (mm->msg_type == 5 || mm->msg_type == 21)
  {
    LOG_STDOUT ("DF %d: %s, Identity Reply.\n", mm->msg_type, mm->msg_type == 5 ? "Surveillance" : "Comm-B");
    LOG_STDOUT ("  Flight Status  : %s\n", flight_status_str [mm->flight_status]);
    LOG_STDOUT ("  DR             : %d\n", mm->DR_status);
    LOG_STDOUT ("  UM             : %d\n", mm->UM_status);
    LOG_STDOUT ("  Squawk         : %d\n", mm->identity);
    LOG_STDOUT ("  ICAO Address   : %s\n", get_ICAO_details(mm->AA1, mm->AA2, mm->AA3));

    if (mm->msg_type == 21)
    {
      /** \todo 56 bits DF21 MB additional field. */
    }
  }
  else if (mm->msg_type == 11)
  {
    /* DF 11 */
    LOG_STDOUT ("DF 11: All Call Reply.\n");
    LOG_STDOUT ("  Capability  : %s\n", capability_str[mm->ca]);
    LOG_STDOUT ("  ICAO Address: %s\n", get_ICAO_details(mm->AA1, mm->AA2, mm->AA3));
  }
  else if (mm->msg_type == 17)
  {
    /* DF 17 */
    LOG_STDOUT ("DF 17: ADS-B message.\n");
    LOG_STDOUT ("  Capability     : %d (%s)\n", mm->ca, capability_str[mm->ca]);
    LOG_STDOUT ("  ICAO Address   : %s\n", get_ICAO_details(mm->AA1, mm->AA2, mm->AA3));
    LOG_STDOUT ("  Extended Squitter Type: %d\n", mm->ME_type);
    LOG_STDOUT ("  Extended Squitter Sub : %d\n", mm->ME_subtype);
    LOG_STDOUT ("  Extended Squitter Name: %s\n", get_ME_description(mm));

    /* Decode the extended squitter message. */
    if (mm->ME_type >= 1 && mm->ME_type <= 4)
    {
      /* Aircraft identification. */
      const char *ac_type_str[4] = {
                 "Aircraft Type D",
                 "Aircraft Type C",
                 "Aircraft Type B",
                 "Aircraft Type A"
             };
      LOG_STDOUT ("    Aircraft Type  : %s\n", ac_type_str[mm->aircraft_type]);
      LOG_STDOUT ("    Identification : %s\n", mm->flight);
    }
    else if (mm->ME_type >= 9 && mm->ME_type <= 18)
    {
      LOG_STDOUT ("    F flag   : %s\n", mm->odd_flag ? "odd" : "even");
      LOG_STDOUT ("    T flag   : %s\n", mm->UTC_flag ? "UTC" : "non-UTC");
      LOG_STDOUT ("    Altitude : %d feet\n", mm->altitude);
      LOG_STDOUT ("    Latitude : %d (not decoded)\n", mm->raw_latitude);
      LOG_STDOUT ("    Longitude: %d (not decoded)\n", mm->raw_longitude);
    }
    else if (mm->ME_type == 19 && mm->ME_subtype >= 1 && mm->ME_subtype <= 4)
    {
      if (mm->ME_subtype == 1 || mm->ME_subtype == 2)
      {
        /* Velocity */
        LOG_STDOUT ("    EW direction      : %d\n", mm->EW_dir);
        LOG_STDOUT ("    EW velocity       : %d\n", mm->EW_velocity);
        LOG_STDOUT ("    NS direction      : %d\n", mm->NS_dir);
        LOG_STDOUT ("    NS velocity       : %d\n", mm->NS_velocity);
        LOG_STDOUT ("    Vertical rate src : %d\n", mm->vert_rate_source);
        LOG_STDOUT ("    Vertical rate sign: %d\n", mm->vert_rate_sign);
        LOG_STDOUT ("    Vertical rate     : %d\n", mm->vert_rate);
      }
      else if (mm->ME_subtype == 3 || mm->ME_subtype == 4)
      {
        LOG_STDOUT ("    Heading status: %d\n", mm->heading_is_valid);
        LOG_STDOUT ("    Heading: %d\n", mm->heading);
      }
    }
    else if (mm->ME_type == 23)  /* Test Message */
    {
      if (mm->ME_subtype == 7)
           LOG_STDOUT ("    Squawk: %04x\n", mm->identity);
      else LOG_STDOUT ("    Unrecognized ME subtype: %d\n", mm->ME_subtype);
    }
    else if (mm->ME_type == 28)  /* Extended Squitter Aircraft Status */
    {
      if (mm->ME_subtype == 1)
      {
        LOG_STDOUT ("    Emergency State: %s\n", emerg_state_str[(mm->msg[5] & 0xE0) >> 5]);
        LOG_STDOUT ("    Squawk: %04x\n", mm->identity);
      }
      else
        LOG_STDOUT ("    Unrecognized ME subtype: %d\n", mm->ME_subtype);
    }
#if 0
    /**\todo */
    else if (mm->ME_type == 29)  /* Target State + Status Message */
    {
    }
    /**\todo Ref: chapter 8 in `The-1090MHz-riddle.pdf` */
    else if (mm->ME_type == 31)  /* Aircraft operation status */
    {
    }
#endif
    else
    {
      LOG_STDOUT ("    Unrecognized ME type: %d, subtype: %d\n", mm->ME_type, mm->ME_subtype);
      Modes.stat.unrecognized_ME++;
    }
  }
  else
  {
    LOG_STDOUT ("DF %d with good CRC received (decoding still not implemented).\n", mm->msg_type);
  }
}

/**
 * Turn I/Q samples pointed by `Modes.data` into the magnitude vector
 * pointed by `Modes.magnitude`.
 */
uint16_t *compute_magnitude_vector (const uint8_t *data)
{
  uint16_t *m = Modes.magnitude;
  uint32_t  i;

  /* Compute the magnitude vector. It's just `sqrt(I^2 + Q^2)`, but
   * we rescale to the 0-255 range to exploit the full resolution.
   */
  for (i = 0; i < Modes.data_len; i += 2)
  {
    int I = data [i] - 127;
    int Q = data [i+1] - 127;

    if (I < 0)
        I = -I;
    if (Q < 0)
        Q = -Q;
    m [i / 2] = Modes.magnitude_lut [129*I + Q];
  }
  return (m);
}

/**
 * Return -1 if the message is out of phase left-side
 * Return  1 if the message is out of phase right-size
 * Return  0 if the message is not particularly out of phase.
 *
 * Note: this function will access m[-1], so the caller should make sure to
 * call it only if we are not at the start of the current buffer.
 */
int detect_out_of_phase (const uint16_t *m)
{
  if (m[3] > m[2]/3)
     return (1);
  if (m[10] > m[9]/3)
     return (1);
  if (m[6] > m[7]/3)
     return (-1);
  if (m[-1] > m[1]/3)
     return (-1);
  return (0);
}

/**
 * This function does not really correct the phase of the message, it just
 * applies a transformation to the first sample representing a given bit:
 *
 * If the previous bit was one, we amplify it a bit.
 * If the previous bit was zero, we decrease it a bit.
 *
 * This simple transformation makes the message a bit more likely to be
 * correctly decoded for out of phase messages:
 *
 * When messages are out of phase there is more uncertainty in
 * sequences of the same bit multiple times, since `11111` will be
 * transmitted as continuously altering magnitude (high, low, high, low...)
 *
 * However because the message is out of phase some part of the high
 * is mixed in the low part, so that it is hard to distinguish if it is
 * a zero or a one.
 *
 * However when the message is out of phase passing from `0` to `1` or from
 * `1` to `0` happens in a very recognizable way, for instance in the `0 -> 1`
 * transition, magnitude goes low, high, high, low, and one of of the
 * two middle samples the high will be *very* high as part of the previous
 * or next high signal will be mixed there.
 *
 * Applying our simple transformation we make more likely if the current
 * bit is a zero, to detect another zero. Symmetrically if it is a one
 * it will be more likely to detect a one because of the transformation.
 * In this way similar levels will be interpreted more likely in the
 * correct way.
 */
void apply_phase_correction (uint16_t *m)
{
  int j;

  m += 16; /* Skip preamble. */
  for (j = 0; j < 2*(MODES_LONG_MSG_BITS-1); j += 2)
  {
    if (m[j] > m[j+1])
    {
      /* One */
      m[j+2] = (m[j+2] * 5) / 4;
    }
    else
    {
      /* Zero */
      m[j+2] = (m[j+2] * 4) / 5;
    }
  }
}

/**
 * Detect a Mode S messages inside the magnitude buffer pointed by `m` and of
 * size `mlen` bytes. Every detected Mode S message is converted into a
 * stream of bits and passed to the function to display it.
 */
int detect_modeS (uint16_t *m, uint32_t mlen)
{
  uint8_t  bits [MODES_LONG_MSG_BITS];
  uint8_t  msg [MODES_LONG_MSG_BITS/2];
  uint16_t aux [MODES_LONG_MSG_BITS*2];
  uint32_t j;
  bool     use_correction = false;
  int      rc = 0;  /**\todo fix this */

  /* The Mode S preamble is made of impulses of 0.5 microseconds at
   * the following time offsets:
   *
   * 0   - 0.5 usec: first impulse.
   * 1.0 - 1.5 usec: second impulse.
   * 3.5 - 4   usec: third impulse.
   * 4.5 - 5   usec: last impulse.
   *
   * If we are sampling at 2 MHz, every sample in our magnitude vector
   * is 0.5 usec. So the preamble will look like this, assuming there is
   * an impulse at offset 0 in the array:
   *
   * 0   -----------------
   * 1   -
   * 2   ------------------
   * 3   --
   * 4   -
   * 5   --
   * 6   -
   * 7   ------------------
   * 8   --
   * 9   -------------------
   */
  for (j = 0; j < mlen - 2*MODES_FULL_LEN; j++)
  {
    int  low, high, delta, i, errors;
    bool good_message = false;

    if (Modes.exit)
       break;

    if (use_correction)
       goto good_preamble;    /* We already checked it. */

    /* First check of relations between the first 10 samples
     * representing a valid preamble. We don't even investigate further
     * if this simple test is not passed.
     */
    if (!(m[j]   > m[j+1] &&
          m[j+1] < m[j+2] &&
          m[j+2] > m[j+3] &&
          m[j+3] < m[j]   &&
          m[j+4] < m[j]   &&
          m[j+5] < m[j]   &&
          m[j+6] < m[j]   &&
          m[j+7] > m[j+8] &&
          m[j+8] < m[j+9] &&
          m[j+9] > m[j+6]))
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Unexpected ratio among first 10 samples", msg, m, j);
      continue;
    }

    /* The samples between the two spikes must be < than the average
     * of the high spikes level. We don't test bits too near to
     * the high levels as signals can be out of phase so part of the
     * energy can be in the near samples.
     */
    high = (m[j] + m[j+2] + m[j+7] + m[j+9]) / 6;
    if (m[j+4] >= high || m[j+5] >= high)
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 3 and 6", msg, m, j);
      continue;
    }

    /* Similarly samples in the range 11-14 must be low, as it is the
     * space between the preamble and real data. Again we don't test
     * bits too near to high levels, see above.
     */
    if (m[j+11] >= high || m[j+12] >= high || m[j+13] >= high || m[j+14] >= high)
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 10 and 15", msg, m, j);
      continue;
    }

    Modes.stat.valid_preamble++;

good_preamble:

    /* If the previous attempt with this message failed, retry using
     * magnitude correction.
      */
    if (use_correction)
    {
      memcpy (aux, m + j + MODES_PREAMBLE_US*2, sizeof(aux));
      if (j && detect_out_of_phase(m+j))
      {
        apply_phase_correction (m + j);
        Modes.stat.out_of_phase++;
      }
      /** \todo Apply other kind of corrections. */
    }

    /* Decode all the next 112 bits, regardless of the actual message
     * size. We'll check the actual message type later.
     */
    errors = 0;
    for (i = 0; i < 2*MODES_LONG_MSG_BITS; i += 2)
    {
      low   = m [j + i + 2*MODES_PREAMBLE_US];
      high  = m [j + i + 2*MODES_PREAMBLE_US + 1];
      delta = low - high;
      if (delta < 0)
         delta = -delta;

      if (i > 0 && delta < 256)
         bits[i/2] = bits[i/2-1];

      else if (low == high)
      {
        /* Checking if two adjacent samples have the same magnitude
         * is an effective way to detect if it's just random noise
         * that was detected as a valid preamble.
         */
        bits[i/2] = 2;    /* error */
        if (i < 2*MODES_SHORT_MSG_BITS)
           errors++;
      }
      else if (low > high)
      {
        bits[i/2] = 1;
      }
      else
      {
        /* (low < high) for exclusion
         */
        bits[i/2] = 0;
      }
    }

    /* Restore the original message if we used magnitude correction.
     */
    if (use_correction)
       memcpy (m + j + 2*MODES_PREAMBLE_US, aux, sizeof(aux));

    /* Pack bits into bytes
     */
    for (i = 0; i < MODES_LONG_MSG_BITS; i += 8)
    {
      msg [i/8] = bits[i]   << 7 |
                  bits[i+1] << 6 |
                  bits[i+2] << 5 |
                  bits[i+3] << 4 |
                  bits[i+4] << 3 |
                  bits[i+5] << 2 |
                  bits[i+6] << 1 |
                  bits[i+7];
    }

    int msg_type = msg[0] >> 3;
    int msg_len = modeS_message_len_by_type (msg_type) / 8;

    /* Last check, high and low bits are different enough in magnitude
     * to mark this as real message and not just noise? */
    delta = 0;
    for (i = 0; i < 8 * 2 * msg_len; i += 2)
    {
      delta += abs (m[j + i + 2*MODES_PREAMBLE_US] -
                    m[j + i + 2*MODES_PREAMBLE_US + 1]);
    }
    delta /= 4 * msg_len;

    /* Filter for an average delta of three is small enough to let almost
     * every kind of message to pass, but high enough to filter some
     * random noise.
     */
    if (delta < 10*255)
    {
      use_correction = false;
      continue;
    }

    /* If we reached this point, and error is zero, we are very likely
     * with a Mode S message in our hands, but it may still be broken
     * and CRC may not be correct. This is handled by the next layer.
     */
    if (errors == 0 || (Modes.aggressive && errors <= 2))
    {
      modeS_message mm;
      double        signal_power = 0ULL;
      int           signal_len = mlen;
      uint32_t      k, mag;

      /* Decode the received message and update statistics
       */
      decode_modeS_message (&mm, msg);

      /* measure signal power
       */
      for (k = j; k < j + MODES_FULL_LEN; k++)
      {
        mag = m [k];
        signal_power += mag * mag;
      }
      mm.sig_level = signal_power / (65536.0 * signal_len);

      /* Update statistics.
       */
      if (mm.CRC_ok || use_correction)
      {
        if (errors == 0)
           Modes.stat.demodulated++;
        if (mm.error_bit == -1)
        {
          if (mm.CRC_ok)
               Modes.stat.good_CRC++;
          else Modes.stat.bad_CRC++;
        }
        else
        {
          Modes.stat.bad_CRC++;
          Modes.stat.fixed++;
          if (mm.error_bit < MODES_LONG_MSG_BITS)
               Modes.stat.single_bit_fix++;
          else Modes.stat.two_bits_fix++;
        }
      }

      /* Output debug mode info if needed.
       */
      if (!use_correction)
      {
        if (Modes.debug & DEBUG_DEMOD)
           dump_raw_message ("Demodulated with 0 errors", msg, m, j);

        else if ((Modes.debug & DEBUG_BADCRC) && mm.msg_type == 17 && (!mm.CRC_ok || mm.error_bit != -1))
           dump_raw_message ("Decoded with bad CRC", msg, m, j);

        else if ((Modes.debug & DEBUG_GOODCRC) && mm.CRC_ok && mm.error_bit == -1)
           dump_raw_message ("Decoded with good CRC", msg, m, j);
      }

      /* Skip this message if we are sure it's fine.
       */
      if (mm.CRC_ok)
      {
        j += 2 * (MODES_PREAMBLE_US + (8 * msg_len));
        good_message = true;
        if (use_correction)
           mm.phase_corrected = true;
      }

      /* Pass data to the next layer
       */
      modeS_user_message (&mm);
    }
    else
    {
      if ((Modes.debug & DEBUG_DEMODERR) && use_correction)
      {
        LOG_STDOUT ("The following message has %d demod errors", errors);
        dump_raw_message ("Demodulated with errors", msg, m, j);
      }
    }

    /* Retry with phase correction if possible.
     */
    if (!good_message && !use_correction)
    {
      j--;
      use_correction = true;
    }
    else
    {
      use_correction = false;
    }
  }
  return (rc);
}

/**
 * When a new message is available, because it was decoded from the
 * RTL/SDRplay device, file, or received in a TCP input port, or any other
 * way we can receive a decoded message, we call this function in order
 * to use the message.
 *
 * Basically this function passes a raw message to the upper layers for
 * further processing and visualization.
 */
void modeS_user_message (const modeS_message *mm)
{
  uint64_t num_clients;

  if (!mm->CRC_ok)
     return;

  Modes.stat.messages_total++;


  /* Track aircrafts in interactive mode or if we have some HTTP / SBS clients.
   */
  num_clients = Modes.stat.cli_accepted [MODES_NET_SERVICE_HTTP] +
                Modes.stat.cli_accepted [MODES_NET_SERVICE_SBS_OUT];

  if (Modes.interactive || num_clients > 0)
  {
    uint64_t  now = MSEC_TIME();
    aircraft *a = interactive_receive_data (mm, now);

    if (a && Modes.stat.cli_accepted[MODES_NET_SERVICE_SBS_OUT] > 0)
       modeS_send_SBS_output (mm, a);     /* Feed SBS output clients. */
  }

  /* In non-interactive mode, display messages on standard output.
   * In silent-mode, do nothing just to consentrate on network traces.
   */
  if (!Modes.interactive && !Modes.silent)
  {
    display_modeS_message (mm);
    if (!Modes.raw && !Modes.only_addr)
    {
      puts ("");
      modeS_log ("\n\n");
    }
  }

  /* Send data to connected clients.
   * In `--net-active` mode we have no clients.
   */
  if (Modes.net)
     modeS_send_raw_output (mm);
}

/**
 * Create a new aircraft structure.
 *
 * Store the printable hex-address as 6 digits since an ICAO address should never
 * contain more than 24 bits.
 *
 * \param in addr  the specific ICAO address.
 * \param in now   the current tick-time in milli-seconds.
 */
aircraft *aircraft_create (uint32_t addr, uint64_t now)
{
  aircraft *a = calloc (sizeof(*a), 1);

  if (a)
  {
    a->addr       = addr;
    a->seen_first = now;
    a->seen_last  = now;
    a->CSV        = aircraft_CSV_lookup_entry (addr);
    a->show       = A_SHOW_FIRST_TIME;

    /* We really can't tell if it's unique since we keep no global list of that yet
     */
    Modes.stat.unique_aircrafts++;
    if (a->CSV)
       Modes.stat.unique_aircrafts_CSV++;
  }
  return (a);
}

/**
 * Return the aircraft with the specified ICAO address, or NULL if no aircraft
 * exists with this ICAO address.
 *
 * \param in addr  the specific ICAO address.
 */
aircraft *aircraft_find (uint32_t addr)
{
  aircraft *a = Modes.aircrafts;

  while (a)
  {
    if (a->addr == addr)
       return (a);
    a = a->next;
  }
  return (NULL);
}

/**
 * Return the number of aircrafts we have now.
 */
int aircraft_numbers (void)
{
  aircraft *a = Modes.aircrafts;
  int       num;

  for (num = 0; a; num++)
      a = a->next;
  return (num);
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
  return (6371000.0 * acos(angle));
}

/**
 * Set this aircraft's distance to our home position.
 *
 * The reference time-tick is the latest of `a->odd_CPR_time` and `a->even_CPR_time`.
 */
void set_home_distance (aircraft *a)
{
  if (VALID_POS(Modes.home_pos) && VALID_POS(a->position))
  {
    double distance = great_circle_dist (a->position, Modes.home_pos);

    if (distance != 0.0)
       a->distance = distance;
    a->EST_position  = a->position;
    a->EST_seen_last = (a->even_CPR_time > a->odd_CPR_time) ? a->even_CPR_time : a->odd_CPR_time;
  }
}

/**
 * From SDRangel's 'sdrbase/util/azel.cpp':
 *
 * Convert geodetic latitude to geocentric latitude;
 * angle from centre of Earth between the point and equator.
 *
 * https://en.wikipedia.org/wiki/Latitude#Geocentric_latitude
 */
double geocentric_latitude (double lat)
{
  double e2 = 0.00669437999014;

  return atan ((1.0 - e2) * tan(lat));
}

/**
 * Convert spherical coordinate to cartesian.
 * Also calculates radius and a normal vector
 */
void spherical_to_cartesian (cartesian_t *cart, pos_t pos)
{
  double lat  = TWO_PI * pos.lat / 360.0;
  double lon  = TWO_PI * pos.lon / 360.0;
  double clat = geocentric_latitude (lat);

  cart->c_x = 6371000.0 * cos (lon) * cos (clat);
  cart->c_y = 6371000.0 * sin (lon) * cos (clat);
  cart->c_z = 6371000.0 * sin (clat);
}

/**
 * \ref https://keisan.casio.com/exec/system/1359533867
 */
void cartesian_to_spherical (pos_t *pos, cartesian_t cart)
{
  /* We do not need this; close to earth's radius = 6371000 m.
   *
   * double radius = sqrt (cart.c_x*cart.c_x + cart.c_y*cart.c_y + cart.c_z*cart->c_z);
   */
  pos->lon = 360.0 * atan2 (cart.c_y, cart.c_x) / TWO_PI;
  pos->lat = 360.0 * atan2 (hypot(cart.c_x, cart.c_y), cart.c_z) / TWO_PI;
}

/**
 * Return the distance between 2 cartesian points.
 */
double cartesian_distance (const cartesian_t *a, const cartesian_t *b)
{
  double dX = b->c_x - a->c_x;
  double dY = b->c_y - a->c_y;

  return hypot (dX, dY);
}

/**
 * Return the closest of `val1` and `val2` to `val`.
 */
double closest_to (double val, double val1, double val2)
{
  double diff1 = fabs (val1 - val);
  double diff2 = fabs (val2 - val);

  return (diff1 > diff2 ? val2 : val1);
}

/**
 * Set this aircraft's estimated distance to our home position.
 *
 * Assuming a constant good last heading and speed, calculate the
 * new position from that using the elapsed time.
 */
void set_est_home_distance (aircraft *a, uint64_t now)
{
  double      heading, distance, gc_distance, cart_distance, dX, dY;
  cartesian_t cpos;

  if (!Modes.home_pos_ok || a->speed == 0 || !a->heading_is_valid)
     return;

  if (!VALID_POS(a->EST_position) || a->EST_seen_last < a->seen_last)
     return;

  spherical_to_cartesian (&cpos, a->EST_position);

  /* Ensure heading is in range '[-Phi .. +Phi]'
   */
  if (a->heading >= 180)
       heading = TWO_PI * (a->heading - 360) / 360;
  else heading = TWO_PI * a->heading / 360;

  /* knots (1852 m/s) to distance (in meters) traveled in dT msec:
   */
  distance = 0.001852 * (double)a->speed * (now - a->EST_seen_last);
  a->EST_seen_last = now;

  dX = distance * sin (heading);
  dY = distance * cos (heading);
  cpos.c_x += dX;
  cpos.c_y += dY;

  cartesian_to_spherical (&a->EST_position, cpos);

  gc_distance   = great_circle_dist (a->EST_position, Modes.home_pos);
  cart_distance = cartesian_distance (&cpos, &Modes.home_pos_cart);
  a->EST_distance = closest_to (a->EST_distance, gc_distance, cart_distance);

#if 0
  LOG_FILEONLY ("addr %04X: heading: %+7.1lf, dX: %+8.3lf, dY: %+8.3lf, gc_distance: %6.1lf, cart_distance: %6.1lf\n",
                a->addr, 360.0*heading/TWO_PI, dX, dY, gc_distance/1000, cart_distance/1000);
#endif
}

/**
 * Return a string showing this aircraft's distance to our home position.
 *
 * If `Modes.metric == true`, return it in kilo-meters. <br>
 * Otherwise Knots.
 */
const char *get_home_distance (const aircraft *a, const char **km_kts)
{
  static char buf [20];
  double divisor = Modes.metric ? 1000.0 : 1852.0;

  if (km_kts)
     *km_kts = Modes.metric ? "km" : "kts";

  if (a->distance <= SMALL_VAL)
     return (NULL);

  snprintf (buf, sizeof(buf), "%.1lf", a->distance / divisor);
  return (buf);
}

/**
 * As for `get_home_distance()`, but return the estimated distance.
 */
const char *get_est_home_distance (const aircraft *a, const char **km_kts)
{
  static char buf [20];
  double divisor = Modes.metric ? 1000.0 : 1852.0;

  if (km_kts)
     *km_kts = Modes.metric ? "km" : "kts";

  if (a->EST_distance <= SMALL_VAL)
     return (NULL);

  snprintf (buf, sizeof(buf), "%.1lf", a->EST_distance / divisor);
  return (buf);
}

/**
 * Helper function for decoding the **CPR** (*Compact Position Reporting*). <br>
 * Always positive MOD operation, used for CPR decoding.
 */
int CPR_mod_func (int a, int b)
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
int CPR_NL_func (double lat)
{
  if (lat < 0) lat = -lat;   /* Table is symmetric about the equator. */
  if (lat < 10.47047130) return (59);
  if (lat < 14.82817437) return (58);
  if (lat < 18.18626357) return (57);
  if (lat < 21.02939493) return (56);
  if (lat < 23.54504487) return (55);
  if (lat < 25.82924707) return (54);
  if (lat < 27.93898710) return (53);
  if (lat < 29.91135686) return (52);
  if (lat < 31.77209708) return (51);
  if (lat < 33.53993436) return (50);
  if (lat < 35.22899598) return (49);
  if (lat < 36.85025108) return (48);
  if (lat < 38.41241892) return (47);
  if (lat < 39.92256684) return (46);
  if (lat < 41.38651832) return (45);
  if (lat < 42.80914012) return (44);
  if (lat < 44.19454951) return (43);
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

int CPR_N_func (double lat, int isodd)
{
  int nl = CPR_NL_func (lat) - isodd;

  if (nl < 1)
     nl = 1;
  return (nl);
}

double CPR_Dlong_func (double lat, int isodd)
{
  return 360.0 / CPR_N_func (lat, isodd);
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
void decode_CPR (aircraft *a)
{
  const double AirDlat0 = 360.0 / 60;
  const double AirDlat1 = 360.0 / 59;
  double lat0 = a->even_CPR_lat;
  double lat1 = a->odd_CPR_lat;
  double lon0 = a->even_CPR_lon;
  double lon1 = a->odd_CPR_lon;

  /* Compute the Latitude Index "j"
   */
  int    j = (int) floor (((59*lat0 - 60*lat1) / 131072) + 0.5);
  double rlat0 = AirDlat0 * (CPR_mod_func(j, 60) + lat0 / 131072);
  double rlat1 = AirDlat1 * (CPR_mod_func(j, 59) + lat1 / 131072);

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
  if (a->even_CPR_time > a->odd_CPR_time)
  {
    /* Use even packet */
    int ni = CPR_N_func (rlat0, 0);
    int m = (int) floor ((((lon0 * (CPR_NL_func(rlat0)-1)) -
                         (lon1 * CPR_NL_func(rlat0))) / 131072) + 0.5);
    a->position.lon = CPR_Dlong_func (rlat0, 0) * (CPR_mod_func(m, ni) + lon0/131072);
    a->position.lat = rlat0;
  }
  else
  {
    /* Use odd packet */
    int ni = CPR_N_func (rlat1, 1);
    int m  = (int) floor ((((lon0 * (CPR_NL_func(rlat1)-1)) -
                          (lon1 * CPR_NL_func(rlat1))) / 131072.0) + 0.5);
    a->position.lon = CPR_Dlong_func (rlat1, 1) * (CPR_mod_func (m, ni) + lon1/131072);
    a->position.lat = rlat1;
  }

  if (a->position.lon > 180)
     a->position.lon -= 360;

  set_home_distance (a);
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

  addr = (mm->AA1 << 16) | (mm->AA2 << 8) | mm->AA3;

  /* Loookup our aircraft or create a new one.
   */
  a = aircraft_find (addr);
  if (!a)
  {
    a = aircraft_create (addr, now);
    if (!a)
       return (NULL);          /* Not fatal; there could be available memory later */

    LIST_ADD_HEAD (aircraft, &Modes.aircrafts, a);
  }
  else
  {
    /* If it is an already known aircraft, move it on head
     * so we keep aircrafts ordered by received message time.
     *
     * However move it on head only if at least one second elapsed
     * since the aircraft that is currently on head sent a message,
     * otherwise with multiple aircrafts at the same time we have an
     * useless shuffle of positions on the screen.
     */
#if 0
    if (Modes.aircrafts != a && (now - a->seen_last) >= 1000)
    {
      aircraft *aux = Modes.aircrafts;

      while (aux->next != a)
         aux = aux->next;

      /* Now we are a node before the aircraft to remove.
       */
      aux->next = aux->next->next; /* removed. */

      /* Add on head */
      a->next = Modes.aircrafts;
      Modes.aircrafts = a;
    }
#endif
  }

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
         a->identity = mm->identity;       /* Set thee Squawk code. */
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
       *   This is impossible for planes slower than 1560 knots/Mach 2.3 over the ground.
       */
      int64_t t_diff = (int64_t) (a->even_CPR_time - a->odd_CPR_time);

      if (llabs(t_diff) <= 60*10*1000)
           decode_CPR (a);
   // else LOG_FILEONLY ("t_diff for '%04X' too large: %lld sec.\n", a->addr, t_diff/1000);
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

/**
 * Show information for a single aircraft.
 *
 * If `a->show == A_SHOW_FIRST_TIME`, print in GREEN colour.
 * If `a->show == A_SHOW_LAST_TIME`, print in RED colour.
 *
 * \param in a    the aircraft to show.
 * \param in now  the currect tick-timer in milli-seconds.
 */
void interactive_show_aircraft (const aircraft *a, uint64_t now)
{
  int   altitude = a->altitude;
  int   speed = a->speed;
  char  alt_buf [10]       = "  - ";
  char  lat_buf [10]       = "   - ";
  char  lon_buf [10]       = "    - ";
  char  speed_buf [8]      = " - ";
  char  heading_buf [8]    = " - ";
  char  distance_buf [10]  = " - ";
  char  RSSI_buf [7]           = " - ";
  bool  restore_colour     = false;
  const char *reg_num      = "";
  const char *call_sign    = "";
  const char *flight       = "";
  const char *distance     = NULL;
  const char *est_distance = NULL;
  const char *km_kts = NULL;
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

  if (a->position.lat)
     snprintf (lat_buf, sizeof(lat_buf), "% +7.03f", a->position.lat);

  if (a->position.lon)
     snprintf (lon_buf, sizeof(lon_buf), "% +8.03f", a->position.lon);

  if (speed)
     snprintf (speed_buf, sizeof(speed_buf), "%4d", speed);

  if (a->heading_is_valid)
     snprintf (heading_buf, sizeof(heading_buf), "%3d", a->heading);

  if (Modes.home_pos_ok)
  {
    distance     = get_home_distance (a, &km_kts);
    est_distance = get_est_home_distance (a, &km_kts);
    if (est_distance)
       snprintf (distance_buf, sizeof(distance_buf), "%s", est_distance);
  }

  if (a->CSV)
  {
    if (a->CSV->reg_num[0])
       reg_num = a->CSV->reg_num;
#if 0
    if (a->CSV->call_sign[0])
       call_sign = a->CSV->call_sign;
#endif
  }

  if (!a->flight[0] && call_sign[0])
       flight = call_sign;
  else flight = a->flight;

  if (a->show == A_SHOW_FIRST_TIME)
  {
    setcolor (COLOUR_GREEN);
    restore_colour = true;
    LOG_FILEONLY ("plane '%06X' entering.\n", a->addr);
  }
  else if (a->show == A_SHOW_LAST_TIME)
  {
    setcolor (COLOUR_RED);
    restore_colour = true;
    LOG_FILEONLY ("plane '%06X' leaving. Active for %.1lf sec. Distance: %s/%s %s.\n",
                  a->addr, (double)(now - a->seen_first) / 1000.0,
                  distance     ? distance     : "-",
                  est_distance ? est_distance : "-", km_kts);
  }

  ms_diff = (now - a->seen_last);
  if (ms_diff < 0LL)  /* clock wrapped */
     ms_diff = 0L;

  printf ("%06X %-9.9s %-8s %-5s     %-5s %-7s %-8s   %-5s ", a->addr, flight, reg_num, alt_buf, speed_buf, lat_buf, lon_buf, heading_buf);
  printf ("%6s  %5s %5u  %2llu sec \n", distance_buf, RSSI_buf, a->messages, ms_diff / 1000);

  if (restore_colour)
     setcolor (0);
}

/**
 * Show the currently captured aircraft information on screen.
 * \param in now  the currect tick-timer in mill-seconds
 */
void interactive_show_data (uint64_t now)
{
  static int spin_idx = 0;
  static int old_count = -1;
  int        count = 0;
  char       spinner[] = "|/-\\";
  aircraft  *a = Modes.aircrafts;

  /* Unless debug or raw-mode is active, clear the screen to remove old info.
   * But only if current number of aircrafts is less than last time. This is to
   * avoid an annoying blinking of the console.
   */
  if (Modes.debug == 0)
  {
    if (old_count == -1 || aircraft_numbers() < old_count)
       clrscr();
    gotoxy (1, 1);
  }

  setcolor (COLOUR_WHITE);
  printf ("ICAO   Callsign  Reg-num  Altitude  Speed   Lat      Long    Hdg     Dist   RSSI   Msg  Seen %c\n"
          "----------------------------------------------------------------------------------------------\n",
          spinner[spin_idx & 3]);
  spin_idx++;
  setcolor (0);

  while (a && count < Modes.interactive_rows && !Modes.exit)
  {
    if (a->show != A_SHOW_NONE)
    {
      set_est_home_distance (a, now);
      interactive_show_aircraft (a, now);
    }

    /* Simple state-machine for the plane's show-state
     */
    if (a->show == A_SHOW_FIRST_TIME)
       a->show = A_SHOW_NORMAL;
    else if (a->show == A_SHOW_LAST_TIME)
       a->show = A_SHOW_NONE;      /* don't show again before deleting it */

    a = a->next;
    count++;
  }
  old_count = count;
}

/**
 * Called from `background_tasks()` 4 times per second.
 *
 * If we don't receive new nessages within `Modes.interactive_ttl`
 * milli-seconds, we remove the aircraft from the list.
 */
void remove_stale_aircrafts (uint64_t now)
{
  aircraft *a, *a_next;

  for (a = Modes.aircrafts; a; a = a_next)
  {
    int64_t diff = (int64_t) (now - a->seen_last);

    a_next = a->next;

    /* Mark this plane for a "last time" view on next refresh?
     */
    if (a->show == A_SHOW_NORMAL && diff >= Modes.interactive_ttl - 1000)
    {
      a->show = A_SHOW_LAST_TIME;
    }
    else if (diff > Modes.interactive_ttl)
    {
      /* Remove the element from the linked list.
       */
      LIST_DELETE (aircraft, &Modes.aircrafts, a);
      free (a);
    }
  }
}

/**
 * Remove all active aircrafts from the list.
 */
void free_all_aircrafts (void)
{
  aircraft *a = Modes.aircrafts;
  aircraft *prev = NULL;

  while (a)
  {
    aircraft *next = a->next;

    free (a);
    if (!prev)
         Modes.aircrafts = next;
    else prev->next = next;
    a = next;
  }
}

/**
 * Read raw IQ samples from `stdin` and filter everything that is lower than the
 * specified level for more than 256 samples in order to reduce
 * example file size.
 *
 * Will print to `stdout` in BINARY-mode.
 */
int strip_mode (int level)
{
  int i, q;
  uint64_t c = 0;

  _setmode (_fileno(stdin), O_BINARY);
  _setmode (_fileno(stdout), O_BINARY);

  while ((i = getchar()) != EOF && (q = getchar()) != EOF)
  {
    if (abs(i-127) < level && abs(q-127) < level)
    {
      c++;
      if (c > 4*MODES_PREAMBLE_US)
         continue;
    }
    else
      c = 0;

    putchar (i);
    putchar (q);
  }
  return (0);
}

/*
 * Taken from Dump1090-FA's 'net_io.c':
 */
char *aircraft_json_Dump1090_OL3 (const char *url_path, int *len)
{
  MODES_NOTUSED (url_path);
  MODES_NOTUSED (len);
  return (NULL);
}

/**
 * Return a description of the receiver in JSON.
 { "version" : "0.1-gv", "refresh" : 1000, "history" : 3 }
 */
char *receiver_to_json (void)
{
  int history_size = DIM(Modes.json_aircraft_history)-1;

  /* work out number of valid history entries
   */
  if (!Modes.json_aircraft_history [history_size].ptr)
     history_size = Modes.json_aircraft_history_next;

  return mg_mprintf ("{%Q: %s, "    // "version", DUMP1090_VERSION
                      "%Q: %llu, "  // "refresh", Modes.json_interval
                      "%Q: %d, "    // "history", history_size
                      "%Q: %.6g, "  // "lat",     Modes.home_pos.lat; if 'Modes.home_pos_ok == false', this is 0.
                      "%Q: %.6g}",  // "lon",     Modes.home_pos.lon; ditto
                      "version", DUMP1090_VERSION,
                      "refresh", Modes.json_interval,
                      "history", history_size,
                      "lat",     Modes.home_pos.lat,
                      "lon",     Modes.home_pos.lon);
}

/**
 * Return a malloced JSON description of the active planes.
 * But only those whose latitude and longitude is known.
 *
 * Since various Web-clients expects different elements in this returned
 * JSON array, add those which is approprite for that Web-clients only.
 *
 * E.g. an extended web-client want an empty array like this:
 * ```
 *  { "now": 1656176445, "messages" : 1,
 *    "aircraft" : []
 *  }
 * ```
 * Or an array with 1 element like this:
 * ```
 * {
 *   "now:": 1656176445, "messages": 1,
 *   "aircraft": [{"hex":"47807D", "flight":"", "lat":60.280609, "lon":5.223715, "altitude":875, "track":199, "speed":96}]
 * }
 * ```
 */
char *aircrafts_to_json (int *num_planes, bool extended_client)
{
  struct timeval tv_now;
  aircraft      *a = Modes.aircrafts;

  *num_planes = 0;

#if 0
  char *ret_buf;
  int   aircraft_size = 1024;
  char *aircraft_json = malloc (aircraft_size);

  MODES_NOTUSED (extended_client);

  while (a)
  {
    int    altitude = a->altitude;
    int    speed    = a->speed;
    size_t f_len = strlen (a->flight);

    /* Convert units to metric if --metric was specified.
     * But option '--metric' has no effect on the Web-page yet.
     */
    if (Modes.metric)
    {
      altitude = (int) (double) (a->altitude / 3.2828);
      speed    = (int) (1.852 * (double) a->speed);
    }

    mg_asprintf (&aircraft_json, aircraft_size,
                 "%Q: %Q"    // "hex":     "addr"
                 "%Q: %Q"    // "flight":   a->flight
                 "%Q: %g"    // "lat":      a->position.lat
                 "%Q: %g"    // "lon":      a->position.lon
                 "%Q: %d"    // "altitude": altitude
                 "%Q: %d"    // "track":    a->heading
                 "%Q: %d",   // "speed":    speed
                 "hex",      a->addr,
                 "flight",   a->flight,
                 "lat",      a->position.lat,
                 "lon",      a->position.lon,
                 "altitude", altitude,
                 "track",    a->heading,
                 "speed",    speed);

    a = a->next;
  }

  mg_asprintf (&ret_buf, 0,
               "{%Q: %lu.%03lu,"  // "now":      sec.usec
                "%Q: %llu:, "     // "messages": Modes.stat.messages_total
                "%Q: [ %s ]}",    // "aircraft": aircraft_json
                "now",            tv_now.tv_sec, tv_now.tv_usec/1000,
                "messages",       Modes.stat.messages_total,
                "aircraft",       aircraft_json);

  free (aircraft_json);
  return (ret_buf);

#else
  int    buflen = 1024;        /* The initial buffer is incremented as needed. */
  char  *buf = malloc (buflen);
  char  *p = buf;
  int    l;

  if (!buf)
     return (NULL);

  if (extended_client)
  {
    _gettimeofday (&tv_now, NULL);
    l = snprintf (p, buflen, "{\"now\": %lu.%03lu, \"messages\": %llu, \"aircraft\" : [",
                  tv_now.tv_sec, tv_now.tv_usec/1000, Modes.stat.messages_total);

    p      += l;
    buflen -= l;
  }
  else
  {
    *p++ = '[';
    buflen--;
  }


  while (a)
  {
    int altitude = a->altitude;
    int speed    = a->speed;

    /* Convert units to metric if --metric was specified.
     * But option '--metric' has no effect on the Web-page yet.
     */
    if (Modes.metric)
    {
      altitude = (int) (double) (a->altitude / 3.2828);
      speed    = (int) (1.852 * (double) a->speed);
    }

    if (VALID_POS(a->position))
    {
      size_t f_len = strlen (a->flight);

      while (a->flight[f_len-1] == ' ') /* do not send trailing spaces */
         f_len--;

      l = snprintf (p, buflen,
                    "{\"hex\": \"%06X\", \"flight\": \"%.*s\", \"lat\": %f, \"lon\": %f, \"altitude\": %d, \"track\": %d, \"speed\": %d",
                    a->addr, (int)f_len, a->flight, a->position.lat, a->position.lon, altitude, a->heading, speed);
      p      += l;
      buflen -= l;

      if (extended_client)
      {
        l = snprintf (p, buflen, ", \"type\": \"%s\", \"messages\": %u, \"seen\": %lu, \"seen_pos\": %lu",
                      "adsb_icao", a->messages, 2, 1 /* tv_now.tv_sec - a->seen_first/1000 */);
        p      += l;
        buflen -= l;
      }

      strcpy (p, "},\n");
      p      += 3;
      buflen -= 3;

      (*num_planes)++;

      /* Resize if needed.
       */
      if (buflen < 256)
      {
        int used = p - buf;

        buflen += 1024;    /* Our increment. */
        buf = realloc (buf, used + buflen);
        if (!buf)
        {
          *num_planes = 0;
          return (NULL);
        }
        p = buf + used;
      }
    }
    a = a->next;
  }

  /* Remove the final comma if any, and close the json array
   */
  if (p[-2] == ',')
     p -= 2;

  *p++ = ']';
  if (extended_client)
     *p++ = '}';
  *p = '\0';
  return (buf);
#endif
}

/**
 * Returns a 'connection *' based on the remote 'addr' and 'service'.
 * This can be either client or server.
 */
connection *connection_get_addr (const mg_addr *addr, intptr_t service, bool is_server)
{
  connection *srv;

  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);

  for (srv = Modes.connections[service]; srv; srv = srv->next)
  {
    if (srv->service == service && !memcmp(&srv->addr, addr, sizeof(srv->addr)))
       return (srv);
  }
  is_server ? Modes.stat.srv_unknown [service]++ :   /* Should never happen */
              Modes.stat.cli_unknown [service]++;
  return (NULL);
}

/**
 * Free a specific connection, client or server.
 */
void connection_free (connection *this_conn, intptr_t service)
{
  connection *conn;
  uint32_t    conn_id = (uint32_t)-1;
  int         is_server = -1;

  if (!this_conn)
     return;

  for (conn = Modes.connections[service]; conn; conn = conn->next)
  {
    if (conn != this_conn)
       continue;

    LIST_DELETE (connection, &Modes.connections[service], conn);
    if (conn->conn->is_accepted)
    {
      Modes.stat.cli_removed [service]++;
      is_server = 0;
    }
    else
    {
      Modes.stat.srv_removed [service]++;
      is_server = 1;
    }
    conn_id = conn->id;
    free (conn);
    break;
  }

  TRACE (DEBUG_NET2, "Freeing %s %u for service \"%s\".\n",
         is_server == 1 ? "server" :
         is_server == 0 ? "client" : "?",
         conn_id, handler_descr(service));
}

/*
 * Free all connections in all services.
 */
unsigned connection_free_all (void)
{
  intptr_t service;
  int      num = 0;

  for (service = MODES_NET_SERVICE_RAW_OUT; service < MODES_NET_SERVICES_NUM; service++)
  {
    connection *conn, *conn_next;

    for (conn = Modes.connections[service]; conn; conn = conn_next)
    {
      conn_next = conn->next;
      connection_free (conn, service);
      num++;
    }
  }
  return (num);
}

/**
 * Iterate over all the listening connections and send a `msg` to
 * all clients in the specified `service`.
 *
 * There can only be 1 service that matches this. But this
 * service can have many clients.
 *
 * \note
 *  \li This function is not used for sending HTTP data.
 *  \li This function is not called when `--net-active` is used.
 */
void connection_send (intptr_t service, const void *msg, size_t len)
{
  connection *c;
  int         found = 0;

  for (c = Modes.connections[service]; c; c = c->next)
  {
    if (c->service != service)
       continue;

    mg_send (c->conn, msg, len);   /* if write fails, the client gets freed in connection_handler() */
    found++;
  }
  if (found > 0)
     TRACE (DEBUG_NET, "Sent %zd bytes to %d clients in service \"%s\".\n",
            len, found, handler_descr(service));
}

/**
 * Handlers for the network services.
 *
 * We use Mongoose for handling all the server and low-level network I/O. <br>
 * We register event-handlers that gets called on important network events.
 *
 * Keep the data for our 4 network services in this structure.
 */
net_service modeS_net_services [MODES_NET_SERVICES_NUM] = {
          { &Modes.raw_out,  NULL, "Raw TCP output", MODES_NET_PORT_RAW_OUT },
          { &Modes.raw_in,   NULL, "Raw TCP input",  MODES_NET_PORT_RAW_IN },
          { &Modes.sbs_out,  NULL, "SBS TCP output", MODES_NET_PORT_SBS },
          { &Modes.sbs_in,   NULL, "SBS TCP input",  MODES_NET_PORT_SBS },
          { &Modes.http_out, NULL, "HTTP server",    MODES_NET_PORT_HTTP }
        };

/* Mongoose event names.
 */
const char *event_name (int ev)
{
  if (ev >= MG_EV_USER)
  {
    static char buf[20];
    snprintf (buf, sizeof(buf), "MG_EV_USER%d", ev - MG_EV_USER);
    return (buf);
  }

  return (ev == MG_EV_OPEN       ? "MG_EV_OPEN" :     /* Event on 'connect()', 'listen()' and 'accept()'. Ignored */
          ev == MG_EV_POLL       ? "MG_EV_POLL" :
          ev == MG_EV_RESOLVE    ? "MG_EV_RESOLVE" :
          ev == MG_EV_CONNECT    ? "MG_EV_CONNECT" :
          ev == MG_EV_ACCEPT     ? "MG_EV_ACCEPT" :
          ev == MG_EV_READ       ? "MG_EV_READ" :
          ev == MG_EV_WRITE      ? "MG_EV_WRITE" :
          ev == MG_EV_CLOSE      ? "MG_EV_CLOSE" :
          ev == MG_EV_ERROR      ? "MG_EV_ERROR" :
          ev == MG_EV_HTTP_MSG   ? "MG_EV_HTTP_MSG" :
          ev == MG_EV_HTTP_CHUNK ? "MG_EV_HTTP_CHUNK" :
          ev == MG_EV_WS_OPEN    ? "MG_EV_WS_OPEN" :
          ev == MG_EV_WS_MSG     ? "MG_EV_WS_MSG" :
          ev == MG_EV_WS_CTL     ? "MG_EV_WS_CTL" :
          ev == MG_EV_MQTT_CMD   ? "MG_EV_MQTT_CMD" :   /* Can never occur here */
          ev == MG_EV_MQTT_MSG   ? "MG_EV_MQTT_MSG" :   /* Can never occur here */
          ev == MG_EV_MQTT_OPEN  ? "MG_EV_MQTT_OPEN" :  /* Can never occur here */
          ev == MG_EV_SNTP_TIME  ? "MG_EV_SNTP_TIME"    /* Can never occur here */
                                 : "?");
}

mg_connection *handler_conn (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (*modeS_net_services [service].conn);
}

uint16_t *handler_num_connections (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (&modeS_net_services [service].num_connections);
}

const char *handler_descr (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (modeS_net_services [service].descr);
}

u_short handler_port (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (modeS_net_services [service].port);
}

char *handler_error (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (modeS_net_services [service].last_err);
}

bool handler_sending (intptr_t service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM);
  return (modeS_net_services [service].active_send);
}

void net_flushall (void)
{
  mg_connection *conn;
  unsigned       num_active  = 0;
  unsigned       num_passive = 0;
  unsigned       num_unknown = 0;
  unsigned       total_rx = 0;
  unsigned       total_tx = 0;

  for (conn = Modes.mgr.conns; conn; conn = conn->next)
  {
    total_rx += conn->recv.len;
    total_tx += conn->send.len;

    mg_iobuf_free (&conn->recv);
    mg_iobuf_free (&conn->send);

    if (conn->is_accepted || conn->is_listening)
         num_passive++;
    else if (conn->is_client)
         num_active++;
    else num_unknown++;
  }
  TRACE (DEBUG_NET,
         "Flushed %u active connections, %u passive, %u unknown. Remaining bytes: %u Rx, %u Tx.\n",
         num_active, num_passive, num_unknown, total_rx, total_tx);
}

int print_server_errors (void)
{
  int   service, num = 0;
  char *err;

  for (service = MODES_NET_SERVICE_RAW_OUT; service < MODES_NET_SERVICES_NUM; service++)
  {
    err = handler_error (service);
    if (err)
    {
      LOG_STDERR ("%s\n", err);
      free (err);
      num++;
    }
  }
  return (num);
}

/**
 * \todo
 * The event handler for WebSocket control messages.
 */
void connection_handler_websocket (mg_connection *conn, const char *remote, int ev, void *ev_data)
{
  mg_ws_message *ws = ev_data;

  TRACE (DEBUG_NET, "WebSocket event %s from client at %s has %zd bytes for us.\n",
         event_name(ev), remote, conn->recv.len);

  if (ev == MG_EV_WS_MSG)
  {
  }
  else if (ev == MG_EV_WS_CTL)
  {
    Modes.stat.HTTP_websockets++;
  }
  MODES_NOTUSED (ws);
}

const char *get_client_headers (const connection *cli, const char *hdr)
{
  static char buf[100];
  int    len = snprintf (buf, sizeof(buf), "%s\r\n", hdr);

  if (cli->keep_alive)
     strncpy (buf + len, "Connection: keep-alive\r\n", sizeof(buf) - len - 1);
  return (buf);
}

/*
 * Generated arrays from
 *   xxd -i favicon.png
 *   xxd -i favicon.ico
 */
#include "favicon.c"

/**
 * The event handler for HTTP traffic.
 */
int connection_handler_http (mg_connection *conn,
                             int            ev,
                             void          *ev_data,
                             char          *request_data,
                             size_t         request_size,
                             char         **ret_data)
{
  mg_http_message *hm = ev_data;
  connection      *cli;
  bool             is_dump1090, is_extended;
  const char      *content = NULL;
  const char      *uri, *ext;
  char            *request, *end;
  char             header [1000];
  int              header_len;

  *ret_data = NULL;
  *request_data = '\0';

  if ((ev != MG_EV_HTTP_MSG && ev != MG_EV_HTTP_CHUNK) || strncmp(hm->head.ptr, "GET /", 5))
     return (400);  /* Bad Request */

  request = strncpy (alloca(hm->head.len+1), hm->head.ptr, hm->head.len);
  strncpy (request_data, request, request_size);

  uri = request + strlen ("GET ");
  end = strchr (uri, ' ');
  if (!end)
  {
    conn->is_closing = 1;
    return (400);         /* Bad Request */
  }

  *end = '\0';

  end = strchr (request_data+4, ' '); /* extract only the important file-part */
  if (end)
     *end = '\0';

  Modes.stat.HTTP_get_requests++;

  if (str_startswith(request, "GET /data/receiver.json"))
  {
    char *data = receiver_to_json();

    if (!data)
       return (444);  /* No Response */

    *ret_data = data;
    TRACE (DEBUG_NET, "Feeding client %lu with receiver-data:\n%s\n", conn->id, data);

    mg_http_reply (conn, 200, MODES_CONTENT_TYPE_JSON "\r\n", data);
    return (200);
  }

  if (str_startswith(request, "GET /chunks/chunks.json"))
  {
  }

  /* What we normally expect with the default 'web_root/gmap.html'
   */
  is_dump1090 = (str_startswith(request, "GET /data.json"));

  /* Or From an OpenLayers3/Tar1090/FlightAware web-client
   */
  is_extended = str_startswith(request, "GET /data/aircraft.json");

  if (is_dump1090 || is_extended)
  {
    int   num_planes;
    char *data = aircrafts_to_json (&num_planes, is_extended);

    if (!data)
    {
      conn->is_closing = 1;
      return (444);       /* No Response */
    }
    *ret_data = data;

    /* This is rather inefficient way to pump data over to the client.
     * Better use a WebSocket instead.
     */
    if (is_extended)
         mg_http_reply (conn, 200, NULL, "%s", data);
    else mg_http_reply (conn, 200, MODES_CONTENT_TYPE_JSON "\r\n", data);
    return (200);
  }

  cli = connection_get_addr (&conn->rem, MODES_NET_SERVICE_HTTP, false);

  /* Redirect a 'GET /' to a 'GET /' + 'web_page'
   */
  if (!strcmp(request, "GET /"))
  {
    char redirect [50+MG_PATH_MAX];

    if (hm->proto.len >= 9 && strncmp(hm->proto.ptr, "HTTP/1.1", 8))
    {
      Modes.stat.HTTP_keep_alive_recv++;
      cli->keep_alive = 1;
    }
    snprintf (redirect, sizeof(redirect),
              "%sLocation: %s\r\n", cli->keep_alive ? "Connection: keep-alive\r\n" : "", basename(Modes.web_page));
    mg_http_reply (conn, 303, redirect, "");
    return (303);
  }

  /**
   * \todo Check header for a "Upgrade: websocket" and call mg_ws_upgrade()?
   */
  if (!_stricmp(request, "GET /echo"))
  {
    TRACE (DEBUG_NET, "Got WebSocket echo:\n'%.*s'.\n", (int)hm->head.len, hm->head.ptr);
    mg_ws_upgrade (conn, hm, "WS test");
    return (200);
  }

  ext = strrchr (uri, '.');
  if (ext)
  {
    char file [MG_PATH_MAX];
    int  rc = 200;        /* Assume status 200 OK */

    if (!_stricmp(ext, ".html"))
       content = MODES_CONTENT_TYPE_HTML;
    else if (!_stricmp(ext, ".css"))
       content = MODES_CONTENT_TYPE_CSS;
    else if (!_stricmp(ext, ".js"))
       content = MODES_CONTENT_TYPE_JS;
    else if (!_stricmp(ext, ".json"))
       content = MODES_CONTENT_TYPE_JSON;
    else if (!_stricmp(ext, ".png"))
       content = MODES_CONTENT_TYPE_PNG;

    if (!_stricmp(request, "GET /favicon.png"))
    {

      TRACE (DEBUG_NET, "Sending \"favicon.png\" to cli: %lu.\n", conn->id);

      header_len = snprintf (header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                             "Content-Length: %u\r\n%s\r\n",
                             content, favicon_png_len,
                             cli->keep_alive ? "Connection: keep-alive\r\n" : "");
      mg_send (conn, header, header_len);
      mg_send (conn, favicon_png, favicon_png_len);
    }
    else if (!_stricmp(request, "GET /favicon.ico"))  /* Other browsers may want a 'favicon.ico' file */
    {
      TRACE (DEBUG_NET, "Sending \"favicon.ico\" to cli: %lu.\n", conn->id);

      header_len = snprintf (header, sizeof(header),
                             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                             "Content-Length: %u\r\n%s\r\n",
                             content, favicon_ico_len,
                             cli->keep_alive ? "Connection: keep-alive\r\n" : "");
      mg_send (conn, header, header_len);
      mg_send (conn, favicon_ico, favicon_ico_len);
    }
    else
    {
      struct mg_http_serve_opts opts;
      struct stat st;

      memset (&opts, '\0', sizeof(opts));
      opts.extra_headers = get_client_headers (cli, content);
      opts.page404       = PAGE_404_HTML;
      snprintf (file, sizeof(file), "%s\\%s", Modes.web_root, uri+1);
      mg_http_serve_file (conn, hm, file, &opts);
      if (stat(file, &st) != 0)
      {
        Modes.stat.HTTP_404_responses++;
        rc = 404;
      }
    }
    if (cli->keep_alive)
       Modes.stat.HTTP_keep_alive_sent++;

    return (rc);
  }

  mg_http_reply (conn, 404, cli->keep_alive ? "Connection: keep-alive\r\n" : "", "Not found\n");
  Modes.stat.HTTP_404_responses++;
  return (404);
}

/**
 * The timer callback for an active `connect()`.
 */
void connection_timeout (void *fn_data)
{
  INT_PTR service = (int)(INT_PTR) fn_data;
  char    err [200];
  char    host_port [100];

  snprintf (host_port, sizeof(host_port), modeS_net_services[service].is_ip6 ? "[%s]:%u" : "%s:%u",
            modeS_net_services[service].host, modeS_net_services[service].port);

  snprintf (err, sizeof(err), "Timeout in connection to service \"%s\" on host %s",
            handler_descr(service), host_port);
  modeS_net_services [service].last_err = _strdup (err);
  TRACE (DEBUG_NET, "%s.\n", err);

  sigint_handler (0);  /* break out of main_data_loop()  */
}

/**
 * The event handler for ALL network I/O.
 */
void connection_handler (mg_connection *this_conn, int ev, void *ev_data, void *fn_data)
{
  connection *conn;
  char       *remote, remote_buf [100];
  uint16_t    port;
  INT_PTR     service = (int)(INT_PTR) fn_data;   /* 'fn_data' is arbitrary user data */

  if (Modes.exit)
     return;

  if (ev == MG_EV_POLL)    /* Ignore this events */
     return;

  if (ev == MG_EV_ERROR)
  {
    char err [200];

    remote = modeS_net_services[service].host;
    port   = modeS_net_services[service].port;

    if (remote && service >= MODES_NET_SERVICE_RAW_OUT && service < MODES_NET_SERVICES_NUM)
    {
      snprintf (err, sizeof(err), "Connection to %s:%u failed: %s", remote, port, (const char*)ev_data);
      modeS_net_services [service].last_err = _strdup (err);
      TRACE (DEBUG_NET, "Error: %s\n", err);
      sigint_handler (0);   /* break out of main_data_loop()  */
    }
    return;
  }

  remote = mg_straddr (&this_conn->rem, remote_buf, sizeof(remote_buf));

  if (ev == MG_EV_OPEN)
  {
    TRACE (DEBUG_NET2, "MG_EV_OPEN for host %s\n", remote);
    return;
  }

  if (ev == MG_EV_RESOLVE)
  {
    TRACE (DEBUG_NET, "Resolved to host %s\n", remote);
    return;
  }

  if (ev == MG_EV_CONNECT)
  {
    mg_timer_free (&Modes.mgr.timers, &modeS_net_services[service].timer);
    conn = calloc (sizeof(*conn), 1);
    if (!conn)
    {
      this_conn->is_closing = 1;
      return;
    }

    conn->conn    = this_conn;      /* Keep a copy of the active connection */
    conn->service = service;
    conn->id      = this_conn->id;
    conn->addr    = this_conn->rem;

    LIST_ADD_TAIL (connection, &Modes.connections[service], conn);
    ++ (*handler_num_connections (service));  /* should never go above 1 */
    Modes.stat.srv_connected [service]++;
    TRACE (DEBUG_NET, "Connected to host %s (service \"%s\")\n", remote, handler_descr(service));
    return;
  }

  if (ev == MG_EV_ACCEPT)
  {
    conn = calloc (sizeof(*conn), 1);
    if (!conn)
    {
      this_conn->is_closing = 1;
      return;
    }

    conn->conn    = this_conn;      /* Keep a copy of the passive (listen) connection */
    conn->service = service;
    conn->id      = this_conn->id;
    conn->addr    = this_conn->rem;

    LIST_ADD_TAIL (connection, &Modes.connections[service], conn);
    ++ (*handler_num_connections (service));
    Modes.stat.cli_accepted [service]++;

    TRACE (DEBUG_NET, "New client %u (service \"%s\") from %s.\n",
           conn->id, handler_descr(service), remote);
    return;
  }

  if (ev == MG_EV_READ)
  {
    const mg_str *data = (const mg_str*) ev_data;

    Modes.stat.bytes_recv [service] += data->len;

    TRACE (DEBUG_NET2, "MG_EV_READ from %s (service \"%s\")\n", remote, handler_descr(service));

    if (service == MODES_NET_SERVICE_RAW_IN)
    {
      conn = connection_get_addr (&this_conn->rem, service, false);
      connection_read (conn, decode_hex_message, false);

      conn = connection_get_addr (&this_conn->rem, service, true);
      connection_read (conn, decode_hex_message, true);
    }
    else if (service == MODES_NET_SERVICE_SBS_IN)
    {
      conn = connection_get_addr (&this_conn->rem, service, true);
      connection_read (conn, decode_SBS_message, true);
    }
    return;
  }

  if (ev == MG_EV_WRITE)         /* Increment our own send() bytes */
  {
    Modes.stat.bytes_sent[service] += *(const int*) ev_data;
    TRACE (DEBUG_NET2, "writing %d bytes to client %lu (%s)\n", *(const int*)ev_data, this_conn->id, remote);
    return;
  }

  if (ev == MG_EV_CLOSE)
  {
    conn = connection_get_addr (&this_conn->rem, service, false);
    connection_free (conn, service);

    conn = connection_get_addr (&this_conn->rem, service, true);
    connection_free (conn, service);
    -- (*handler_num_connections (service));
    return;
  }

  if (service == MODES_NET_SERVICE_HTTP)
  {
    char  request_data [100];
    char *response_data;
    int   rc;

    if (this_conn->is_websocket && (ev == MG_EV_WS_MSG || ev == MG_EV_WS_CTL))
       connection_handler_websocket (this_conn, remote, ev, ev_data);

    rc = connection_handler_http (this_conn, ev, ev_data, request_data, sizeof(request_data)-1, &response_data);

    LOG_FILEONLY ("HTTP %d for '%s' (client %lu), response: '%.400s'.. \n",
                  rc, request_data, this_conn->id, response_data ? response_data : "<none>");

    free (response_data);
  }
}

/**
 * Setup a connection for a service.
 * Active or passive (`listen == true`).
 */
mg_connection *connection_setup (intptr_t service, bool listen, bool sending)
{
  mg_connection *conn = NULL;
  char           url [50];

  if (listen)
  {
    snprintf (url, sizeof(url), "tcp://0.0.0.0:%u", modeS_net_services[service].port);
    if (service == MODES_NET_SERVICE_HTTP)
         conn = mg_http_listen (&Modes.mgr, url, connection_handler, (void*) service);
    else conn = mg_listen (&Modes.mgr, url, connection_handler, (void*) service);
    modeS_net_services [service].active_send = sending;
  }
  else
  {
    /* For an active connect(), we'll get one of these event in connection_handler():
     *  - MG_EV_ERROR    -- the `--host-xx` argument was not resolved or the connection failed or timed out.
     *  - MG_EV_RESOLVE  -- the `--host-xx` argument was successfully resolved to an IP-address.
     *  - MG_EV_CONNECT  -- successfully connected.
     */
    const char *fmt = (modeS_net_services[service].is_ip6 ? "tcp://[%s]:%u" : "tcp://%s:%u");

    snprintf (url, sizeof(url), fmt, modeS_net_services[service].host, modeS_net_services[service].port);
    mg_timer_add (&Modes.mgr, MODES_CONNECT_TIMEOUT, 0, connection_timeout, (void*)service);
    modeS_net_services [service].active_send = sending;

    LOG_STDOUT ("Connecting to %s for service \"%s\".\n", url, handler_descr(service));
    conn = mg_connect (&Modes.mgr, url, connection_handler, (void*) service);
  }

  if (conn && (Modes.debug & DEBUG_NET2))
     conn->is_hexdumping = 1;
  return (conn);
}

/**
 * Initialize the Mongoose network manager and:
 *  \li start the 2 active network services.
 *  \li or start the 4 listening (passive) network services.
 */
int modeS_init_net (void)
{
  struct stat st;

  mg_mgr_init (&Modes.mgr);

  if (Modes.net_active)
  {
    if (modeS_net_services[MODES_NET_SERVICE_RAW_IN].host)
       Modes.raw_in = connection_setup (MODES_NET_SERVICE_RAW_IN, false, false);

    if (modeS_net_services[MODES_NET_SERVICE_SBS_IN].host)
       Modes.sbs_in = connection_setup (MODES_NET_SERVICE_SBS_IN, false, false);

    if (!Modes.raw_in && !Modes.sbs_in)
    {
      LOG_STDERR ("No hosts for any `--net-active` services specified.\n");
      return (1);
    }
  }
  else
  {
    Modes.raw_out  = connection_setup (MODES_NET_SERVICE_RAW_OUT, true, true);
    Modes.raw_in   = connection_setup (MODES_NET_SERVICE_RAW_IN, true, false);
    Modes.sbs_out  = connection_setup (MODES_NET_SERVICE_SBS_OUT, true, true);
    Modes.http_out = connection_setup (MODES_NET_SERVICE_HTTP, true, true);

    if (!Modes.raw_out || !Modes.raw_in || !Modes.sbs_out || !Modes.http_out)
    {
      LOG_STDERR ("Fail to set-up listen socket(s).\n");
      return (1);
    }
  }

  if (Modes.http_out)
  {
    char full_name [MG_PATH_MAX];

    snprintf (full_name, sizeof(full_name), "%s\\%s", Modes.web_root, basename(Modes.web_page));
    TRACE (DEBUG_NET, "Web-page: \"%s\"\n", full_name);

    if (stat(full_name, &st) != 0)
    {
      LOG_STDERR ("Web-page \"%s\" does not exist.\n", full_name);
      return (1);
    }
    if (((st.st_mode) & _S_IFMT) != _S_IFREG)
    {
      LOG_STDERR ("Web-page \"%s\" is not a regular file.\n", full_name);
      return (1);
    }
  }
  return (0);
}

/**
 * Write raw output to TCP clients.
 */
void modeS_send_raw_output (const modeS_message *mm)
{
  char  msg [10 + 2*MODES_LONG_MSG_BYTES];
  char *p = msg;

  if (!handler_sending(MODES_NET_SERVICE_RAW_OUT))
     return;

  *p++ = '*';
  mg_hex (&mm->msg, mm->msg_bits/8, p);
  p = strchr (p, '\0');
  *p++ = ';';
  *p++ = '\n';
  connection_send (MODES_NET_SERVICE_RAW_OUT, msg, p - msg);
}

/**
 * Write SBS output to TCP clients (Base Station format).
 */
void modeS_send_SBS_output (const modeS_message *mm, const aircraft *a)
{
  char msg [MODES_MAX_SBS_SIZE], *p = msg;
  int  emergency = 0, ground = 0, alert = 0, spi = 0;

  if (mm->msg_type == 4 || mm->msg_type == 5 || mm->msg_type == 21)
  {
    /**\note
     * identity is calculated/kept in base10 but is actually
     * octal (07500 is represented as 7500)
     */
    if (mm->identity == 7500 || mm->identity == 7600 || mm->identity == 7700)
       emergency = -1;
    if (mm->flight_status == 1 || mm->flight_status == 3)
       ground = -1;
    if (mm->flight_status == 2 || mm->flight_status == 3 || mm->flight_status == 4)
       alert = -1;
    if (mm->flight_status == 4 || mm->flight_status == 5)
       spi = -1;
  }

  /* Field 11 could contain the call-sign we can get from `aircraft_find()::reg_num`.
   */
  if (mm->msg_type == 0)
  {
    p += sprintf (p, "MSG,5,,,%02X%02X%02X,,,,,,,%d,,,,,,,,,,",
                  mm->AA1, mm->AA2, mm->AA3, mm->altitude);
  }
  else if (mm->msg_type == 4)
  {
    p += sprintf (p, "MSG,5,,,%02X%02X%02X,,,,,,,%d,,,,,,,%d,%d,%d,%d",
                  mm->AA1, mm->AA2, mm->AA3, mm->altitude, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 5)
  {
    p += sprintf (p, "MSG,6,,,%02X%02X%02X,,,,,,,,,,,,,%d,%d,%d,%d,%d",
                  mm->AA1, mm->AA2, mm->AA3, mm->identity, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 11)
  {
    p += sprintf (p, "MSG,8,,,%02X%02X%02X,,,,,,,,,,,,,,,,,",
                  mm->AA1, mm->AA2, mm->AA3);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 4)
  {
    p += sprintf (p, "MSG,1,,,%02X%02X%02X,,,,,,%s,,,,,,,,0,0,0,0",
                  mm->AA1, mm->AA2, mm->AA3, mm->flight);
  }
  else if (mm->msg_type == 17 && mm->ME_type >= 9 && mm->ME_type <= 18)
  {
    if (a->position.lat == 0 && a->position.lon == 0)
         p += sprintf (p, "MSG,3,,,%02X%02X%02X,,,,,,,%d,,,,,,,0,0,0,0",
                       mm->AA1, mm->AA2, mm->AA3, mm->altitude);
    else p += sprintf (p, "MSG,3,,,%02X%02X%02X,,,,,,,%d,,,%1.5f,%1.5f,,,0,0,0,0",
                       mm->AA1, mm->AA2, mm->AA3, mm->altitude, a->position.lat, a->position.lon);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 19 && mm->ME_subtype == 1)
  {
    int vr = (mm->vert_rate_sign == 0 ? 1 : -1) * 64 * (mm->vert_rate - 1);

    p += sprintf (p, "MSG,4,,,%02X%02X%02X,,,,,,,,%d,%d,,,%i,,0,0,0,0",
                  mm->AA1, mm->AA2, mm->AA3, a->speed, a->heading, vr);
  }
  else if (mm->msg_type == 21)
  {
    p += sprintf (p, "MSG,6,,,%02X%02X%02X,,,,,,,,,,,,,%d,%d,%d,%d,%d",
                  mm->AA1, mm->AA2, mm->AA3, mm->identity, alert, emergency, spi, ground);
  }
  else
    return;

  *p++ = '\n';
  connection_send (MODES_NET_SERVICE_SBS_OUT, msg, p - msg);
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

/**
 * This function decodes a string representing a Mode S message in
 * raw hex format like: `*8D4B969699155600E87406F5B69F;<eol>`
 *
 * The string is supposed to be at the start of the client buffer
 * and NUL-terminated. It accepts both '\n' and '\r\n' terminated records.
 *
 * The message is passed to the higher level layers, so it feeds
 * the selected screen output, the network output and so forth.
 *
 * If the message looks invalid, it is silently discarded.
 */
void decode_hex_message (mg_iobuf *msg, int loop_cnt)
{
  modeS_message mm;
  uint8_t       bin_msg [MODES_LONG_MSG_BYTES];
  int           len, j;
  uint8_t      *hex;
  uint8_t      *end = memchr (msg->buf, '\n', msg->len);

  if (!end)
  {
    if (!Modes.interactive)
       LOG_STDOUT ("RAW(%d): Bogus msg: '%.*s'...\n", loop_cnt, (int)msg->len, msg->buf);
    Modes.stat.unrecognized_raw++;
    mg_iobuf_del (msg, 0, msg->len);
    return;
  }

  *end++ = '\0';
  if (end[-2] == '\r')
     end[-2] = '\0';

  /* Remove spaces on the left and on the right.
   */
  hex = msg->buf;
  len = end - msg->buf - 1;
  while (len && isspace(hex[len-1]))
  {
    hex[len-1] = '\0';
    len--;
  }
  while (isspace(*hex))
  {
    hex++;
    len--;
  }

  /* Check it's format.
   */
  if (len < 2)
  {
    Modes.stat.empty_raw++;
    mg_iobuf_del (msg, 0, end - msg->buf);
    return;
  }
  if (hex[0] != '*' || !memchr(msg->buf, ';', len))
  {
    Modes.stat.unrecognized_raw++;
    mg_iobuf_del (msg, 0, end - msg->buf);
    return;
  }

  /* Turn the message into binary.
   */
  hex++;   /* Skip `*` and `;` */
  len -= 2;
  if (len > 2*MODES_LONG_MSG_BYTES)   /* Too long message... broken. */
  {
    Modes.stat.unrecognized_raw++;
    mg_iobuf_del (msg, 0, end - msg->buf);
    return;
  }

  for (j = 0; j < len; j += 2)
  {
    int high = hex_digit_val (hex[j]);
    int low  = hex_digit_val (hex[j+1]);

    if (high == -1 || low == -1)
    {
      Modes.stat.unrecognized_raw++;
      mg_iobuf_del (msg, 0, end - msg->buf);
      return;
    }
    bin_msg[j/2] = (high << 4) | low;
  }
  mg_iobuf_del (msg, 0, end - msg->buf);
  Modes.stat.good_raw++;
  decode_modeS_message (&mm, bin_msg);
  modeS_user_message (&mm);
}

/**
 * \todo
 * Ref: http://woodair.net/sbs/article/barebones42_socket_data.htm
 */
int modeS_recv_SBS_input (mg_iobuf *msg, modeS_message *mm)
{
  memset (mm, '\0', sizeof(*mm));

//decode 'msg' and fill 'mm'
//modeS_user_message (&mm);
  MODES_NOTUSED (msg);
  return (0);
}

/**
 * This function decodes a string representing a Mode S message in
 * SBS format (Base Station) like: `MSG,5,1,1,4CC52B,1,2021/09/20,23:30:43.897,2021/09/20,23:30:43.901,,38000,,,,,,,0,,0,`
 *
 * It accepts both '\n' and '\r\n' terminated records.
 */
void decode_SBS_message (mg_iobuf *msg, int loop_cnt)
{
  modeS_message mm;
  uint8_t      *end = memchr (msg->buf, '\n', msg->len);

  if (!end)
  {
    if (!Modes.interactive)
       LOG_STDOUT ("SBS(%d): Bogus msg: '%.*s'...\n", loop_cnt, (int)msg->len, msg->buf);
    Modes.stat.unrecognized_SBS++;
    mg_iobuf_del (msg, 0, msg->len);
    return;
  }
  *end++ = '\0';
  if (end[-2] == '\r')
     end[-2] = '\0';

  if (!Modes.interactive)
     LOG_STDOUT ("SBS(%d): '%s'\n", loop_cnt, msg->buf);

  if (!strncmp((char*)msg->buf, "MSG,", 4))
  {
    modeS_recv_SBS_input (msg, &mm);
    Modes.stat.good_SBS++;
  }
  mg_iobuf_del (msg, 0, end - msg->buf);
}

/**
 * This function reads client/server data for services:
 *  \li `MODES_NET_SERVICE_RAW_IN` or
 *  \li `MODES_NET_SERVICE_SBS_IN`
 *
 * when the event `MG_EV_READ` is received in `connection_handler()`.
 *
 * The message is supposed to be separated by the next message by the
 * separator 'sep', that is a NUL-terminated C string.
 *
 * The `handler` function is responsible for freeing `msg` as it consumes each record
 * in the `msg`. This `msg` can consist of several records or incomplete records since
 * Mongoose uses non-blocking sockets.
 *
 * The `tools/SBS_client.py` script is sending this in "RAW-OUT" test-mode:
 * ```
 *  *8d4b969699155600e87406f5b69f;\n
 * ```
 *
 * This message shows up as ICAO "4B9696" and Reg-num "TC-ETV" in
 * `--interactive` mode.
 */
void connection_read (connection *conn, msg_handler handler, bool is_server)
{
  mg_iobuf *msg;
  int       loops;

  if (!conn)
     return;

  msg = &conn->conn->recv;
  if (msg->len == 0)
  {
    TRACE (DEBUG_NET2, "No msg for %s.\n", is_server ? "server" : "client");
    return;
  }

  for (loops = 0; msg->len > 0; loops++)
  {
    TRACE (DEBUG_NET2, "%s msg(%d): '%.*s'.\n", is_server ? "server" : "client", loops, (int)msg->len, msg->buf);
    (*handler) (msg, loops);
  }
}

/**
 * Show the program usage
 */
void show_help (const char *fmt, ...)
{
  if (fmt)
  {
    va_list args;

    va_start (args, fmt);
    vprintf (fmt, args);
    va_end (args);
  }
  else
    printf ("A 1090 MHz receiver, decoder and web-server for\n%s.\n", ADS_B_ACRONYM);

  printf ("  Usage: %s [options]\n"
          "  General options:\n"
          "    --aggressive             Use a more aggressive CRC check (two bits fixes, ...).\n"
          "    --database <file>        The CSV file for the aircraft database\n"
          "                             (default: \"%s\").\n"
          "    --debug <flags>          Debug mode; see below for details.\n"
          "    --infile <filename>      Read data from file (use `-' for stdin).\n"
          "    --interactive            Interactive mode refreshing data on screen.\n"
          "    --interactive-rows <num> Max number of rows in interactive mode (default: 15).\n"
          "    --interactive-ttl <sec>  Remove aircraft if not seen for <sec> (default: %u).\n"
          "    --logfile <file>         Enable logging to file (default: off)\n"
          "    --loop <N>               With --infile, read the file in a loop <N> times (default: 2^63).\n"
          "    --max-messages <N>       Max number of messages to process (default: Inf).\n"
          "    --metric                 Use metric units (meters, km/h, ...).\n"
          "    --no-fix                 Disable single-bits error correction using CRC.\n"
          "    --no-crc-check           Disable checking CRC of messages (discouraged).\n"
          "    --only-addr              Show only ICAO addresses (testing purposes).\n"
          "    --raw                    Show only the raw Mode-S hex message.\n"
          "    --silent                 Silent mode for testing network I/O (together with '--debug n').\n"
          "    --strip <level>          Strip IQ file removing samples below level.\n"
          "    -h, --help               Show this help.\n\n",
          Modes.who_am_I, Modes.aircraft_db, MODES_INTERACTIVE_TTL/1000);

  printf ("  Network options:\n"
          "    --net                    Enable network listening services.\n"
          "    --net-active             Enable network active services. `--net-only` is implied.\n"
          "    --net-only               Enable just networking, no physical device or file.\n"
          "    --net-http-port <port>   HTTP server port (default: %u).\n"
          "    --net-ri-port <port>     TCP listening port for raw input  (default: %u).\n"
          "    --net-ro-port <port>     TCP listening port for raw output (default: %u).\n"
          "    --net-sbs-port <port>    TCP listening port for SBS output (default: %u).\n"
          "    --host-raw <addr:port>   Remote host/port for raw input with `--net-active`.\n"
          "    --host-sbs <addr:port>   Remote host/port for SBS input with `--net-active`.\n"
          "    --web-page <file>        The Web-page to serve for HTTP clients\n"
          "                             (default: \"%s\\%s\").\n\n",
          MODES_NET_PORT_HTTP, MODES_NET_PORT_RAW_IN, MODES_NET_PORT_RAW_OUT,
          MODES_NET_PORT_SBS, Modes.web_root, Modes.web_page);

  printf ("  RTLSDR / SDRplay options:\n"
          "    --agc                    Enable Digital AGC              (default: off)\n"
          "    --bias                   Enable Bias-T output            (default: off)\n"
          "    --calibrate              Enable calibrating R820 devices (default: off)\n"
          "    --device <N / name>      Select device                   (default: 0).\n"
          "    --freq <Hz>              Set frequency                   (default: %.0f MHz).\n"
          "    --gain <dB>              Set gain                        (default: AUTO).\n"
          "    --if-mode <ZIF | LIF>    Intermediate Frequency mode     (default: ZIF).\n"
          "    --ppm <correction>       Set frequency correction        (default: 0).\n"
          "    --samplerate <Hz>        Set sample-rate                 (default: %.0f MS/s).\n\n",
          MODES_DEFAULT_FREQ / 1E6, MODES_DEFAULT_RATE/1E6);

  printf ("  --debug <flags>: E = Log frames decoded with errors.\n"
          "                   D = Log frames decoded with 0 errors.\n"
          "                   c = Log frames with bad CRC.\n"
          "                   C = Log frames with good CRC.\n"
          "                   p = Log frames with bad preamble.\n"
          "                   n = Log network debugging information.\n"
          "                   N = A bit more network information than flag 'n'.\n"
          "                   j = Log frames to frames.js, loadable by `debug.html'.\n"
          "                   g = Log general debugging info.\n"
          "                   G = A bit more general debug info than flag 'g'.\n\n");

  printf ("  Your home-position for distance calculation can be set like:\n"
          "  'c:\\> set DUMP1090_HOMEPOS=51.5285578,-0.2420247' for London.\n");

  modeS_exit();
  exit (1);
}

/**
 * This background function is called continously by `main_data_loop()`.
 * It performs:
 *  *) Removes inactive aircrafts from the list.
 *  *) Polls the network for events blocking less than `MODES_INTERACTIVE_REFRESH_TIME`.
 *  *) Refreshes interactive data 4 times per second (`MODES_INTERACTIVE_REFRESH_TIME`).
 *  *) Refreshes the console-title with some statistics (also 4 times per second).
 */
void background_tasks (void)
{
  bool     refresh;
  uint64_t now;

  if (Modes.net)
     mg_mgr_poll (&Modes.mgr, MG_NET_POLL_TIME); /* Poll Mongoose for network events */

  if (Modes.exit)
     return;

  now = MSEC_TIME();

  refresh = (now - Modes.last_update_ms) >= MODES_INTERACTIVE_REFRESH_TIME;
  if (!refresh)
     return;

  Modes.last_update_ms = now;

  if (Modes.log)
     fflush (Modes.log);

  remove_stale_aircrafts (now);

  /* Refresh screen and console-title when in interactive mode
   */
  if (Modes.interactive)
     interactive_show_data (now);

  if (Modes.rtlsdr.device || Modes.sdrplay.device)
  {
    console_title_stats();
    console_update_gain();
  }
#if 0
  else
    console_raw_SBS_stats();
#endif
}

/**
 * The handler called in for `SIGINT` or `SIGBREAK` . <br>
 * I.e. user presses `^C`.
 */
void sigint_handler (int sig)
{
  int rc;

  if (sig > 0)
     signal (sig, SIG_DFL);   /* reset signal handler - bit extra safety */

  Modes.exit = true;          /* Signal to threads that we are done */
  console_exit();

  if (sig == SIGINT)
     LOG_STDOUT ("Caught SIGINT, shutting down ...\n");
  else if (sig == SIGBREAK)
     LOG_STDOUT ("Caught SIGBREAK, shutting down ...\n");
  else if (sig == 0)
     TRACE (DEBUG_GENERAL, "Breaking 'main_data_loop()', shutting down ...\n");

  if (Modes.rtlsdr.device)
  {
    EnterCriticalSection (&Modes.data_mutex);
    rc = rtlsdr_cancel_async (Modes.rtlsdr.device);
    TRACE (DEBUG_GENERAL, "rtlsdr_cancel_async(): rc: %d.\n", rc);

    if (rc == -2)  /* RTLSDR is not streaming data */
       Sleep (5);
    LeaveCriticalSection (&Modes.data_mutex);
  }
  else if (Modes.sdrplay.device)
  {
#if !defined(USE_RTLSDR_EMUL)
    rc = sdrplay_cancel_async (Modes.sdrplay.device);
    TRACE (DEBUG_GENERAL, "sdrplay_cancel_async(): rc: %d / %s.\n", rc, sdrplay_strerror(rc));
#endif
  }
}

void show_connection_stats (void)
{
  const char *cli_srv = (Modes.net_active ? "server" : "client");
  uint64_t    sum;
  int         s;

  LOG_STDOUT ("\nNetwork statistics:\n");

  for (s = MODES_NET_SERVICE_RAW_OUT; s < MODES_NET_SERVICES_NUM; s++)
  {
    LOG_STDOUT ("  %s (port %u):\n", handler_descr(s), handler_port(s));

    if (s == MODES_NET_SERVICE_HTTP)
    {
      if (Modes.net_active)
      {
        LOG_STDOUT ("    Not used.\n");
        continue;
      }
      LOG_STDOUT ("    %8llu HTTP GET requests received.\n", Modes.stat.HTTP_get_requests);
      LOG_STDOUT ("    %8llu HTTP 404 replies sent.\n", Modes.stat.HTTP_404_responses);
      LOG_STDOUT ("    %8llu HTTP/WebSocket upgrades.\n", Modes.stat.HTTP_websockets);
      LOG_STDOUT ("    %8llu server connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_sent);
      LOG_STDOUT ("    %8llu client connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_recv);
    }

    if (Modes.net_active)
         sum = Modes.stat.srv_connected[s] + Modes.stat.srv_removed[s] + Modes.stat.srv_unknown[s];
    else sum = Modes.stat.cli_accepted[s]  + Modes.stat.cli_removed[s] + Modes.stat.cli_unknown[s];

    sum += Modes.stat.bytes_sent[s] + Modes.stat.bytes_recv[s] + *handler_num_connections (s);

    if (sum == 0ULL)
    {
      LOG_STDOUT ("    Nothing.\n");
      continue;
    }

    if (Modes.net_active)
    {
      LOG_STDOUT ("    %8llu server connections done.\n", Modes.stat.srv_connected[s]);
      LOG_STDOUT ("    %8llu server connections removed.\n", Modes.stat.srv_removed[s]);
      LOG_STDOUT ("    %8llu server connections unknown.\n", Modes.stat.srv_unknown[s]);
    }
    else
    {
      LOG_STDOUT ("    %8llu client connections accepted.\n", Modes.stat.cli_accepted[s]);
      LOG_STDOUT ("    %8llu client connections removed.\n", Modes.stat.cli_removed[s]);
      LOG_STDOUT ("    %8llu client connections unknown.\n", Modes.stat.cli_unknown[s]);
    }

    LOG_STDOUT ("    %8llu bytes sent.\n", Modes.stat.bytes_sent[s]);
    LOG_STDOUT ("    %8llu bytes recv.\n", Modes.stat.bytes_recv[s]);
    LOG_STDOUT ("    %8u %s now.\n", *handler_num_connections(s), cli_srv);
  }
}

void show_raw_SBS_stats (void)
{
  LOG_STDOUT ("  SBS-in:  %8llu good messages.\n", Modes.stat.good_SBS);
  LOG_STDOUT ("           %8llu unrecognized messages.\n", Modes.stat.unrecognized_SBS);
  LOG_STDOUT ("           %8llu empty messages.\n", Modes.stat.empty_SBS);
  LOG_STDOUT ("  Raw-in:  %8llu good messages.\n", Modes.stat.good_raw);
  LOG_STDOUT ("           %8llu unrecognized messages.\n", Modes.stat.unrecognized_raw);
  LOG_STDOUT ("           %8llu empty messages.\n", Modes.stat.empty_raw);
  LOG_STDOUT ("  Unknown: %8llu empty messages.\n", Modes.stat.empty_unknown);
}

void show_statistics (void)
{
  if (!Modes.net_only)
  {
    LOG_STDOUT ("Decoder statistics:\n");
    LOG_STDOUT (" %8llu valid preambles.\n", Modes.stat.valid_preamble);
    LOG_STDOUT (" %8llu demodulated after phase correction.\n", Modes.stat.out_of_phase);
    LOG_STDOUT (" %8llu demodulated with 0 errors.\n", Modes.stat.demodulated);
    LOG_STDOUT (" %8llu with CRC okay.\n", Modes.stat.good_CRC);
    LOG_STDOUT (" %8llu with CRC failure.\n", Modes.stat.bad_CRC);
    LOG_STDOUT (" %8llu errors corrected.\n", Modes.stat.fixed);
    LOG_STDOUT (" %8llu messages with 1 bit errors fixed.\n", Modes.stat.single_bit_fix);
    LOG_STDOUT (" %8llu messages with 2 bit errors fixed.\n", Modes.stat.two_bits_fix);
    LOG_STDOUT (" %8llu total usable messages (%llu + %llu).\n", Modes.stat.good_CRC + Modes.stat.fixed, Modes.stat.good_CRC, Modes.stat.fixed);
    LOG_STDOUT (" %8llu unique aircrafts.\n", Modes.stat.unique_aircrafts);
    LOG_STDOUT (" %8llu unique aircrafts from CSV.\n", Modes.stat.unique_aircrafts_CSV);
    LOG_STDOUT (" %8llu unrecognized ME types.\n", Modes.stat.unrecognized_ME);
  }
  if (Modes.net)
     show_connection_stats();
  if (Modes.net_active)
     show_raw_SBS_stats();
}

/**
 * Our exit function. Free all resources here.
 */
void modeS_exit (void)
{
  int rc;

  if (Modes.net)
  {
    unsigned num = connection_free_all();

    net_flushall();
    mg_mgr_free (&Modes.mgr);
    Modes.mgr.conns = NULL;
    if (num > 0)
       Sleep (100);
  }

  if (Modes.rtlsdr.device)
  {
    if (Modes.bias_tee)
       verbose_bias_tee (Modes.rtlsdr.device, 0);
    Modes.bias_tee = 0;
    rc = rtlsdr_close (Modes.rtlsdr.device);
    free (Modes.rtlsdr.gains);
    Modes.rtlsdr.device = NULL;
    TRACE (DEBUG_GENERAL2, "rtlsdr_close(), rc: %d.\n", rc);
  }
  else if (Modes.sdrplay.device)
  {
    rc = sdrplay_exit (Modes.sdrplay.device);
    free (Modes.sdrplay.gains);
    Modes.sdrplay.device = NULL;
    TRACE (DEBUG_GENERAL2, "sdrplay_exit(), rc: %d.\n", rc);
  }

  if (Modes.reader_thread)
     CloseHandle ((HANDLE)Modes.reader_thread);

  if (Modes.fd > STDIN_FILENO)
     _close (Modes.fd);

  free_all_aircrafts();

  free (Modes.magnitude_lut);
  free (Modes.magnitude);
  free (Modes.data);
  free (Modes.ICAO_cache);
  free (Modes.aircraft_list);
  free (Modes.selected_dev);

  DeleteCriticalSection (&Modes.data_mutex);
  DeleteCriticalSection (&Modes.print_mutex);

  Modes.reader_thread = 0;
  Modes.data          = NULL;
  Modes.magnitude     = NULL;
  Modes.magnitude_lut = NULL;
  Modes.ICAO_cache    = NULL;
  Modes.selected_dev  = NULL;

  if (Modes.log)
     fclose (Modes.log);

#if defined(USE_RTLSDR_EMUL)
  RTLSDR_emul_unload_DLL();
#endif

#if defined(_DEBUG)
  crtdbug_exit();
#endif
}

static void select_device (char *arg)
{
  if (isdigit(arg[0]))
     Modes.rtlsdr.index = atoi (arg);
  else
  {
    Modes.rtlsdr.name = arg;
    Modes.rtlsdr.index = -1;  /* select on name only */
  }

  if (!_strnicmp(arg, "sdrplay", 7))
  {
    Modes.sdrplay.name = arg;
    if (isdigit(arg[+7]))
       Modes.sdrplay.index = atoi (arg+7);
  }
}

static void select_debug (const char *flags)
{
  while (*flags)
  {
    switch (*flags)
    {
      case 'D':
           Modes.debug |= DEBUG_DEMOD;
           break;
      case 'E':
           Modes.debug |= DEBUG_DEMODERR;
           break;
      case 'C':
           Modes.debug |= DEBUG_GOODCRC;
           break;
      case 'c':
           Modes.debug |= DEBUG_BADCRC;
           break;
      case 'p':
      case 'P':
           Modes.debug |= DEBUG_NOPREAMBLE;
           break;
      case 'n':
           Modes.debug |= DEBUG_NET;
           break;
      case 'N':
           Modes.debug |= (DEBUG_NET2 | DEBUG_NET);  /* A bit more network details */
           break;
      case 'j':
      case 'J':
           Modes.debug |= DEBUG_JS;
           break;
      case 'g':
           Modes.debug |= DEBUG_GENERAL;
           break;
      case 'G':
           Modes.debug |= (DEBUG_GENERAL2 | DEBUG_GENERAL);
           break;
      default:
           show_help ("Unknown debugging flag: %c\n", *flags);
           /* not reached */
           break;
    }
    flags++;
  }
}

static bool select_if_mode (const char *arg)
{
  if (!_stricmp(arg, "zif"))
  {
    Modes.sdrplay.if_mode = false;
    return (true);
  }
  if (!_stricmp(arg, "lif"))
  {
    Modes.sdrplay.if_mode = true;
    return (true);
  }
  return (false);
}

static struct option long_options[] = {
  { "agc",              no_argument,        (int*)&Modes.dig_agc,          1   },
  { "aggressive",       no_argument,        (int*)&Modes.aggressive,       1   },
  { "database",         required_argument,  NULL,                          'b' },
  { "bias",             no_argument,        (int*)&Modes.bias_tee,         1   },
  { "calibrate",        no_argument,        (int*)&Modes.rtlsdr.calibrate, 1   },
  { "debug",            required_argument,  NULL,                          'd' },
  { "device",           required_argument,  NULL,                          'D' },
  { "freq",             required_argument,  NULL,                          'f' },
  { "gain",             required_argument,  NULL,                          'g' },
  { "help",             no_argument,        NULL,                          'h' },
  { "if-mode",          required_argument,  NULL,                          'I' },
  { "infile",           required_argument,  NULL,                          'i' },
  { "interactive",      no_argument,        (int*)&Modes.interactive,      1   },
  { "interactive-rows", required_argument,  NULL,                          'r' },
  { "interactive-ttl",  required_argument,  NULL,                          't' },
  { "logfile",          required_argument,  NULL,                          'L' },
  { "loop",             optional_argument,  NULL,                          'l' },
  { "max-messages",     required_argument,  NULL,                          'm' },
  { "metric",           no_argument,        (int*)&Modes.metric,           1   },
  { "net",              no_argument,        (int*)&Modes.net,              1   },
  { "net-active",       no_argument,        (int*)&Modes.net_active,       1   },
  { "net-only",         no_argument,        (int*)&Modes.net_only,         1   },
  { "net-http-port",    required_argument,  NULL,                          'x' + MODES_NET_SERVICE_HTTP },
  { "net-ri-port",      required_argument,  NULL,                          'x' + MODES_NET_SERVICE_RAW_IN },
  { "net-ro-port",      required_argument,  NULL,                          'x' + MODES_NET_SERVICE_RAW_OUT },
  { "net-sbs-port",     required_argument,  NULL,                          'x' + MODES_NET_SERVICE_SBS_OUT },
  { "host-raw",         required_argument,  NULL,                          'Y' + MODES_NET_SERVICE_RAW_IN },
  { "host-sbs",         required_argument,  NULL,                          'Y' + MODES_NET_SERVICE_SBS_IN },
  { "only-addr",        no_argument,        (int*)&Modes.only_addr,        1   },
  { "ppm",              required_argument,  NULL,                          'p' },
  { "raw",              no_argument,        (int*)&Modes.raw,              1   },
  { "samplerate",       required_argument,  NULL,                          's' },
  { "silent",           no_argument,        (int*)&Modes.silent,           1   },
  { "strip",            required_argument,  NULL,                          'S' },
  { "web-page",         required_argument,  NULL,                          'w' },
  { NULL,               no_argument,        NULL,                          0   }
};

#ifdef NOT_YET
/**
 * The handler for long options called from `getopt_long()`.
 *
 * \param[in] o    the index into `long_options[]`.
 * \param[in] arg  the optional argument for the option.
 */
static void set_long_option (int idx, const char *arg)
{
}
#endif

void parse_cmd_line (int argc, char **argv)
{
  char *end;
  int   c, idx = 0;

  while ((c = getopt_long (argc, argv, "+h?", long_options, &idx)) != EOF)
  {
 // printf ("c: '%c' / %d, long_options[%d]: '%s'\n", c, c, idx, long_options[idx].name);

    switch (c)
    {
#ifdef NOT_YET
      case 0:
           set_long_option (idx, optarg);
           break;
#else
      case 'b':
           strncpy (Modes.aircraft_db, optarg, sizeof(Modes.aircraft_db)-1);
           break;

      case 'D':
           if (dev_selection_done)
              show_help ("Option '--device' already done.\n\n");
           select_device (optarg);
           dev_selection_done = true;
           break;

      case 'd':
           select_debug (optarg);
           break;

      case 'f':
           Modes.freq = (uint32_t) ato_hertz (optarg);
           break;

      case 'g':
           if (!_stricmp(optarg, "auto"))
              Modes.gain_auto = true;
           else
           {
             Modes.gain = (int) (10.0 * strtof(optarg, &end));  /* Gain is in tens of dBs */
             if (end == optarg || *end != '\0')
                show_help ("Illegal gain: %s.\n", optarg);
             Modes.gain_auto = false;
           }
           break;

      case 'I':
           if (!select_if_mode(optarg))
              show_help ("Illegal '--if-mode': %s.\n", optarg);
           break;

      case 'i':
           Modes.infile = optarg;
           break;

      case 'l':
           Modes.loops = optarg ? _atoi64 (optarg) : LLONG_MAX;
           break;

      case 'L':
           Modes.logfile = optarg;
           break;

      case 'm':
           Modes.max_messages = _atoi64 (optarg);
           break;

      case 'n':
           Modes.net_only = Modes.net = true;
           break;

      case 'N':
           Modes.net_active = Modes.net = true;
           break;

      case 'x' + MODES_NET_SERVICE_RAW_OUT:
           modeS_net_services [MODES_NET_SERVICE_RAW_OUT].port = atoi (optarg);
           break;

      case 'x' + MODES_NET_SERVICE_RAW_IN:
           modeS_net_services [MODES_NET_SERVICE_RAW_IN].port = atoi (optarg);
           break;

      case 'x' + MODES_NET_SERVICE_HTTP:
           modeS_net_services [MODES_NET_SERVICE_HTTP].port = atoi (optarg);
           break;

      case 'x' + MODES_NET_SERVICE_SBS_OUT:
           modeS_net_services [MODES_NET_SERVICE_SBS_OUT].port = atoi (optarg);
           break;

      case 'Y' + MODES_NET_SERVICE_RAW_OUT:
           modeS_net_services [MODES_NET_SERVICE_RAW_OUT].host = optarg;
           break;

      case 'Y' + MODES_NET_SERVICE_RAW_IN:
           set_host_port (optarg, &modeS_net_services [MODES_NET_SERVICE_RAW_IN], MODES_NET_PORT_RAW_IN);
           break;

      case 'Y' + MODES_NET_SERVICE_SBS_IN:
           set_host_port (optarg, &modeS_net_services [MODES_NET_SERVICE_SBS_IN], MODES_NET_PORT_SBS);
           break;

      case 'p':
           Modes.rtlsdr.ppm_error = atoi (optarg);
           break;

      case 'r':
           Modes.interactive_rows = atoi (optarg);
           break;

      case 's':
           Modes.sample_rate = (uint32_t) ato_hertz (optarg);
           break;

      case 'S':
           Modes.strip_level = atoi (optarg);
           if (Modes.strip_level == 0)
              show_help ("Illegal --strip level %d.\n\n", Modes.strip_level);
           break;

      case 't':
           Modes.interactive_ttl = 1000 * atoi (optarg);
           break;

      case 'w':
           strncpy (Modes.web_root, dirname(optarg), sizeof(Modes.web_root)-1);
           strncpy (Modes.web_page, basename(optarg), sizeof(Modes.web_page)-1);
           break;
#endif

      case 'h':
      case '?':
           show_help (NULL);
           break;
    }
  }
  if (Modes.net_only || Modes.net_active)
     Modes.net = Modes.net_only = true;
}

/**
 * Our main entry.
 */
int main (int argc, char **argv)
{
  int dev_opened = 0;
  int rc;

#if defined(_DEBUG)
  crtdbug_init();
#endif

  modeS_init_config();  /* Set sane defaults */

  parse_cmd_line (argc, argv);

  rc = modeS_init();    /* Initialization based on cmd-line options */
  if (rc)
     goto quit;

  if (Modes.net_only)
  {
    LOG_STDERR ("Net-only mode, no physical device or file open.\n");
  }
  else if (Modes.strip_level)
  {
    rc = strip_mode (Modes.strip_level);
  }
  else if (Modes.infile)
  {
    rc = 1;
    if (Modes.infile[0] == '-' && Modes.infile[1] == '\0')
    {
      Modes.fd = STDIN_FILENO;
    }
    else if ((Modes.fd = _open(Modes.infile, O_RDONLY)) == -1)
    {
      LOG_STDERR ("Error opening `%s`: %s\n", Modes.infile, strerror(errno));
      goto quit;
    }
  }
  else
  {
    if (Modes.sdrplay.name)
    {
#ifdef USE_RTLSDR_EMUL
      Modes.emul_loaded = RTLSDR_emul_load_DLL();
      if (!Modes.emul_loaded)
      {
        LOG_STDERR ("Cannot use device `%s` without `%s` loaded. Error: %s\n",
                    Modes.sdrplay.name, emul.dll_name, trace_strerror(emul.last_rc));
        goto quit;
      }
#endif

      rc = sdrplay_init (Modes.sdrplay.name, &Modes.sdrplay.device);
      TRACE (DEBUG_GENERAL, "sdrplay_init(): rc: %d / %s.\n", rc, sdrplay_strerror(rc));
      if (rc)
         goto quit;
    }
    else
    {
      rc = modeS_init_RTLSDR();
      TRACE (DEBUG_GENERAL, "modeS_init_RTLSDR(): rc: %d.\n", rc);
      if (rc)
         goto quit;
      dev_opened = 1;
    }
  }

  if (Modes.net)
  {
    rc = modeS_init_net();
    TRACE (DEBUG_GENERAL, "modeS_init_net(): rc: %d.\n", rc);
    if (rc)
       goto quit;
  }

  if (Modes.infile)
  {
    rc = read_from_data_file();
  }
  else if (Modes.strip_level == 0)
  {
    /* Create the thread that will read the data from the RTLSDR or SDRplay device.
     */
    Modes.reader_thread = _beginthreadex (NULL, 0, data_thread_fn, NULL, 0, NULL);
    if (!Modes.reader_thread)
    {
      rc = 1;
      LOG_STDERR ("_beginthreadex() failed: %s.\n", strerror(errno));
      goto quit;
    }
    main_data_loop();
  }

quit:
  if (print_server_errors() == 0 && dev_opened)
     show_statistics();
  modeS_exit();
  return (0);
}
