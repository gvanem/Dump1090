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
#include <process.h>

#include "misc.h"
#include "net_io.h"
#include "cfg_file.h"
#include "sdrplay.h"
#include "location.h"
#include "airports.h"
#include "interactive.h"

global_data Modes;

/**
 * \addtogroup Main      Main functions
 * \addtogroup Misc      Support functions
 * \addtogroup Samplers  SDR input functions
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
static void      modeS_send_raw_output (const modeS_message *mm);
static void      modeS_send_SBS_output (const modeS_message *mm, const aircraft *a);
static void      modeS_user_message (const modeS_message *mm);

static bool      set_bandwidth (const char *arg);
static bool      set_bias_tee (const char *arg);
static bool      set_frequency (const char *arg);
static bool      set_gain (const char *arg);
static bool      set_if_mode (const char *arg);
static bool      set_infile (const char *arg);
static bool      set_interactive_ttl (const char *arg);
static bool      set_home_pos (const char *arg);
static bool      set_home_pos_from_location_API (const char *arg);
static bool      set_host_port_raw_in (const char *arg);
static bool      set_host_port_raw_out (const char *arg);
static bool      set_host_port_sbs_in (const char *arg);
static bool      set_logfile (const char *arg);
static bool      set_loops (const char *arg);
static bool      set_port_http (const char *arg);
static bool      set_port_raw_in (const char *arg);
static bool      set_port_raw_out (const char *arg);
static bool      set_port_sbs (const char *arg);
static bool      set_prefer_adsb_lol (const char *arg);
static bool      set_ppm (const char *arg);
static bool      set_sample_rate (const char *arg);
static bool      set_tui (const char *arg);
static bool      set_web_page (const char *arg);

static int       fix_single_bit_errors (uint8_t *msg, int bits);
static int       fix_two_bits_errors (uint8_t *msg, int bits);
static uint32_t  detect_modeS (uint16_t *m, uint32_t mlen);
static int       modeS_message_len_by_type (int type);
static uint16_t *compute_magnitude_vector (const uint8_t *data);
static void      background_tasks (void);
static void      modeS_exit (void);

static const struct cfg_table config[] = {
    { "adsb-mode",        ARG_FUNC,    (void*) sdrplay_set_adsb_mode },
    { "bias-t",           ARG_FUNC,    (void*) set_bias_tee },
    { "usb-bulk",         ARG_ATOB,    (void*) &Modes.sdrplay.USB_bulk_mode },
    { "sdrplay-dll",      ARG_FUNC,    (void*) sdrplay_set_dll_name },
    { "sdrplay-minver",   ARG_FUNC,    (void*) sdrplay_set_minver },
    { "calibrate",        ARG_ATOB,    (void*) &Modes.rtlsdr.calibrate },
    { "deny4",            ARG_FUNC,    (void*) net_deny4 },
    { "deny6",            ARG_FUNC,    (void*) net_deny6 },
    { "gain",             ARG_FUNC,    (void*) set_gain },
    { "homepos",          ARG_FUNC,    (void*) set_home_pos },
    { "location",         ARG_FUNC,    (void*) set_home_pos_from_location_API },
    { "if-mode",          ARG_FUNC,    (void*) set_if_mode },
    { "metric",           ARG_ATOB,    (void*) &Modes.metric },
    { "web-page",         ARG_FUNC,    (void*) set_web_page },
    { "web-touch",        ARG_ATOB,    (void*) &Modes.web_root_touch },
    { "tui",              ARG_FUNC,    (void*) set_tui },
    { "airports",         ARG_STRCPY,  (void*) &Modes.airport_db },
    { "aircrafts",        ARG_STRCPY,  (void*) &Modes.aircraft_db },
    { "aircrafts-url",    ARG_STRDUP,  (void*) &Modes.aircraft_db_url },
    { "bandwidth",        ARG_FUNC,    (void*) set_bandwidth },
    { "freq",             ARG_FUNC,    (void*) set_frequency },
    { "agc",              ARG_ATOB,    (void*) &Modes.dig_agc },
    { "interactive-ttl",  ARG_FUNC,    (void*) set_interactive_ttl },
    { "keep-alive",       ARG_ATOB,    (void*) &Modes.keep_alive },
    { "logfile",          ARG_FUNC,    (void*) set_logfile },
    { "logfile-daily",    ARG_ATOB,    (void*) &Modes.logfile_daily },
    { "logfile-ignore",   ARG_FUNC,    (void*) modeS_log_add_ignore },
    { "loops",            ARG_FUNC,    (void*) set_loops },
    { "max-messages",     ARG_ATO_U64, (void*) &Modes.max_messages },
    { "max-frames",       ARG_ATO_U64, (void*) &Modes.max_frames },
    { "net-http-port",    ARG_FUNC,    (void*) set_port_http },
    { "net-ri-port",      ARG_FUNC,    (void*) set_port_raw_in },
    { "net-ro-port",      ARG_FUNC,    (void*) set_port_raw_out },
    { "net-sbs-port",     ARG_FUNC,    (void*) set_port_sbs },
    { "prefer-adsb-lol",  ARG_FUNC,    (void*) set_prefer_adsb_lol },
    { "rtl-reset",        ARG_ATOB,    (void*) &Modes.rtlsdr.power_cycle },
    { "samplerate",       ARG_FUNC,    (void*) set_sample_rate },
    { "silent",           ARG_ATOB,    (void*) &Modes.silent },
    { "ppm",              ARG_FUNC,    (void*) set_ppm },
    { "host-raw-in",      ARG_FUNC,    (void*) set_host_port_raw_in },
    { "host-raw-out",     ARG_FUNC,    (void*) set_host_port_raw_out },
    { "host-sbs-in",      ARG_FUNC,    (void*) set_host_port_sbs_in },
    { "error-correct1",   ARG_ATOB,    (void*) &Modes.error_correct_1 },
    { "error-correct2",   ARG_ATOB,    (void*) &Modes.error_correct_2 },
    { NULL,               0,           NULL }
  };

/**
 * Set the RTLSDR manual gain verbosively.
 */
static void verbose_gain_set (rtlsdr_dev_t *dev, int gain)
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
  else LOG_STDOUT ("Tuner gain set to %.0f dB.\n", gain/10.0);
}

/**
 * Set the RTLSDR gain verbosively to AUTO.
 */
static void verbose_gain_auto (rtlsdr_dev_t *dev)
{
  int r = rtlsdr_set_tuner_gain_mode (dev, 0);

  if (r)
       LOG_STDERR ("WARNING: Failed to enable automatic gain.\n");
  else LOG_STDOUT ("Tuner gain set to automatic.\n");
}

/**
 * Set the RTLSDR gain verbosively to the nearest available
 * gain value given in `*target_gain`.
 */
static bool nearest_gain (rtlsdr_dev_t *dev, uint16_t *target_gain)
{
  int    gain_in;
  int    i, err1, err2, nearest;
  int    r = rtlsdr_set_tuner_gain_mode (dev, 1);
  char   gbuf [200], *p = gbuf;
  size_t left = sizeof(gbuf);

  if (r)
  {
    LOG_STDERR ("WARNING: Failed to enable manual gain.\n");
    return (false);
  }

  Modes.rtlsdr.gain_count = rtlsdr_get_tuner_gains (dev, NULL);
  if (Modes.rtlsdr.gain_count <= 0)
     return (false);

  Modes.rtlsdr.gains = malloc (sizeof(int) * Modes.rtlsdr.gain_count);
  Modes.rtlsdr.gain_count = rtlsdr_get_tuner_gains (dev, Modes.rtlsdr.gains);
  nearest = Modes.rtlsdr.gains[0];
  if (!target_gain)
     return (true);

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
  p [-2] = '\0';
  LOG_STDOUT ("Supported gains: %s.\n", gbuf);
  *target_gain = (uint16_t) nearest;
  return (true);
}

#ifdef NOT_USED_YET
/**
 * Enable RTLSDR direct sampling mode.
 */
static void verbose_direct_sampling (rtlsdr_dev_t *dev, int on)
{
  int r = rtlsdr_set_direct_sampling (dev, on);

  if (r)
  {
    LOG_STDERR ("WARNING: Failed to set direct sampling mode.\n");
    return;
  }
  if (on == 0)
     LOG_STDOUT ("Direct sampling mode disabled.\n");
  else if (on == 1)
     LOG_STDOUT ("Enabled direct sampling mode, input 1/I.\n");
  else if (on == 2)
     LOG_STDOUT ("Enabled direct sampling mode, input 2/Q.\n");
}
#endif

/**
 * Set RTLSDR PPM error-correction.
 */
static void verbose_ppm_set (rtlsdr_dev_t *dev, int ppm_error)
{
  double tuner_freq = 0.0;
  int    r;

  r = rtlsdr_set_freq_correction (dev, ppm_error);
  if (r < 0)
     LOG_STDERR ("WARNING: Failed to set PPM correction.\n");
  else
  {
    rtlsdr_get_xtal_freq (dev, NULL, &tuner_freq);
    LOG_STDOUT ("Tuner correction set to %d PPM; %.3lf MHz.\n", ppm_error, tuner_freq / 1E6);
  }
}

/**
 * Set RTLSDR automatic gain control.
 */
static void verbose_agc_set (rtlsdr_dev_t *dev, int agc)
{
  int r = rtlsdr_set_agc_mode (dev, agc);

  if (r < 0)
       LOG_STDERR ("WARNING: Failed to set AGC.\n");
  else LOG_STDOUT ("AGC %s okay.\n", agc ? "enabled" : "disabled");
}

/**
 * Set RTLSDR Bias-T
 */
static void verbose_bias_tee (rtlsdr_dev_t *dev, int bias_t)
{
  int r = rtlsdr_set_bias_tee (dev, bias_t);

  if (bias_t && r)
     LOG_STDERR ("Failed to activate Bias-T.\n");
}

/**
 * \todo power down and up again before calling RTLSDR API
 */
static bool rtlsdr_power_cycle (void)
{
  return (false);
}

/**
 * Populate a I/Q -> Magnitude lookup table. It is used because
 * hypot() or round() may be expensive and may vary a lot depending on
 * the CRT used.
 *
 * We scale to 0-255 range multiplying by 1.4 in order to ensure that
 * every different I/Q pair will result in a different magnitude value,
 * not losing any resolution.
 */
static uint16_t *gen_magnitude_lut (void)
{
  int       I, Q;
  uint16_t *lut = malloc (sizeof(*lut) * 129 * 129);

  if (!lut)
  {
    LOG_STDERR ("Out of memory in 'gen_magnitude_lut()'.\n");
    modeS_exit();
    exit (1);
  }
  for (I = 0; I < 129; I++)
  {
    for (Q = 0; Q < 129; Q++)
       lut [I*129 + Q] = (uint16_t) round (360 * hypot(I, Q));
  }
  return (lut);
}

/**
 * Initialize our temporary directory: == `%TEMP%\\dump1090`.
 */
static void modeS_init_temp (void)
{
  DWORD len_temp  = GetTempPath (sizeof(Modes.tmp_dir)-1, Modes.tmp_dir);
  bool  have_temp = false;

  if (len_temp > 0 && len_temp < sizeof(Modes.tmp_dir) - sizeof("dump1090") - 1)
     have_temp = true;

  if (!have_temp)
  {
    LOG_STDERR ("have_temp == false!\n");
    strcpy (Modes.tmp_dir, "c:\\dump1090");   /* use this as '%TEMP%'! */
  }
  else
    strcat_s (Modes.tmp_dir, sizeof(Modes.tmp_dir), "dump1090");

  if (!CreateDirectory(Modes.tmp_dir, 0) && GetLastError() != ERROR_ALREADY_EXISTS)
     LOG_STDERR ("'CreateDirectory(\"%s\")' failed; %s.\n", Modes.tmp_dir, win_strerror(GetLastError()));
}

/**
 * Step 1: Initialize the program with default values.
 */
static void modeS_init_config (void)
{
  memset (&Modes, '\0', sizeof(Modes));
  GetModuleFileNameA (NULL, Modes.who_am_I, sizeof(Modes.who_am_I));
  snprintf (Modes.where_am_I, sizeof(Modes.where_am_I), "%s", dirname(Modes.who_am_I));

  modeS_init_temp();

  snprintf (Modes.cfg_file, sizeof(Modes.cfg_file), "%s\\dump1090.cfg", Modes.where_am_I);
  strcpy (Modes.web_page, basename(INDEX_HTML));
  snprintf (Modes.web_root, sizeof(Modes.web_root), "%s\\web_root", Modes.where_am_I);

  snprintf (Modes.aircraft_db, sizeof(Modes.aircraft_db), "%s\\%s", Modes.where_am_I, AIRCRAFT_DATABASE_CSV);
  snprintf (Modes.airport_db, sizeof(Modes.airport_db), "%s\\%s", Modes.where_am_I, AIRPORT_DATABASE_CSV);

  snprintf (Modes.airport_freq_db, sizeof(Modes.airport_freq_db), "%s\\%s", Modes.where_am_I, AIRPORT_FREQ_CSV);
  snprintf (Modes.airport_cache, sizeof(Modes.airport_cache), "%s\\%s", Modes.tmp_dir, AIRPORT_DATABASE_CACHE);

  /* Defaults for SDRPlay:
   */
  strcpy (Modes.sdrplay.dll_name, "sdrplay_api.dll");  /* Assumed to be on PATH */
  Modes.sdrplay.min_version = SDRPLAY_API_VERSION;     /* = 3.14F */

  Modes.infile_fd       = -1;      /* no --infile */
  Modes.gain_auto       = true;
  Modes.sample_rate     = MODES_DEFAULT_RATE;
  Modes.freq            = MODES_DEFAULT_FREQ;
  Modes.interactive_ttl = MODES_INTERACTIVE_TTL;
  Modes.json_interval   = 1000;
  Modes.tui_interface   = TUI_WINCON;

  Modes.error_correct_1 = true;
  Modes.error_correct_2 = false;

  InitializeCriticalSection (&Modes.data_mutex);
  InitializeCriticalSection (&Modes.print_mutex);
}

/**
 * Create or append to `Modes.logfile_initial` and write the
 * start-up command-line into it.
 */
static void modeS_init_log (void)
{
  char   args [2000] = "";
  char  *ptr = args;
  size_t line_len, left = sizeof(args);
  int    i, n;

  if (!modeS_log_init())
     return;

  /* Print this a bit nicer. Split into multiple lines (< 120 character per line).
   */
  #define FILLER "             "

  n = snprintf (ptr, left, "Starting: %s", Modes.who_am_I);
  ptr  += n;
  left -= n;
  line_len = strlen (FILLER) + strlen ("Starting: ");

  for (i = 1; i < __argc && left > 2; i++)
  {
    if (i >= 2 && line_len + strlen(__argv[i]) > 120)
    {
      n = snprintf (ptr, left, "\n%s", FILLER);
      ptr  += n;
      left -= n;
      line_len = 0;
    }
    n = snprintf (ptr, left, " %s", __argv[i]);
    line_len += n + 1;
    ptr  += n;
    left -= n;
  }
  fputs ("\n---------------------------------------------------------------------------------\n", Modes.log);
  modeS_log (args);
  fputs ("\n\n", Modes.log);
}

/**
 * Step 2:
 *  \li Initialize the start_time, timezone, DST-adjust and QueryPerformanceFrequency() values.
 *  \li Open and parse `Modes.cfg_file`.
 *  \li Open and append to the `--logfile` if specified.
 *  \li Set the Mongoose log-level based on `--debug m|M`.
 *  \li Check if we have the Aircrafts SQL file.
 *  \li Set our home position from the env-var `%DUMP1090_HOMEPOS%`.
 *  \li Initialize (and update) the aircrafts structures / files.
 *  \li Initialize (and update) the airports structures / files.
 *  \li Setup a SIGINT/SIGBREAK handler for a clean exit.
 *  \li Allocate and initialize the needed buffers.
 */
static bool modeS_init (void)
{
  bool rc = true;

  init_timings();

  if (strcmp(Modes.cfg_file, "NUL") && !cfg_open_and_parse(Modes.cfg_file, config))
     return (false);

  if (Modes.logfile_initial[0])
     modeS_init_log();

  modeS_log_set();
  aircraft_SQL_set_name();

  if (strcmp(Modes.aircraft_db, "NUL") && (Modes.aircraft_db_url || Modes.update))
  {
    aircraft_CSV_update (Modes.aircraft_db, Modes.aircraft_db_url);
    if (!aircraft_CSV_load() || Modes.update)
       return (false);
  }

  airports_init();

#if 0
  /**
   * \todo
   * Regenerate AIRPORT_DATABASE_CSV by:
   * ```
   *  python tools/gen_airport_codes_csv.py > %TEMP%\dump1090\airport-codes.csv
   *  if NOT errorlevel copy %TEMP%\dump1090\airport-codes.csv %CD%
   * ```
   * (and convert it to `airport-database.csv.sqlite` ?)
   */
  if (strcmp(Modes.airport_db, "NUL") && (Modes.airport_db_url || Modes.update))
  {
    airports_update_CSV (Modes.airport_db);
    airports_init_CSV();
    return (false);
  }
#endif

  signal (SIGINT, modeS_signal_handler);
  signal (SIGBREAK, modeS_signal_handler);
  signal (SIGABRT, modeS_signal_handler);

  /* We add a full message minus a final bit to the length, so that we
   * can carry the remaining part of the buffer that we can't process
   * in the message detection loop, back at the start of the next data
   * to process. This way we are able to also detect messages crossing
   * two reads.
   */
  Modes.data_len = MODES_ASYNC_BUF_SIZE + 4*(MODES_FULL_LEN-1);
  Modes.data_ready = false;

  /**
   * Allocate the ICAO address cache. We use two uint32_t for every
   * entry because it's a addr / timestamp pair for every entry.
   */
  Modes.ICAO_cache = calloc (2 * sizeof(uint32_t) * MODES_ICAO_CACHE_LEN, 1);
  Modes.data       = malloc (Modes.data_len);
  Modes.magnitude  = malloc (2 * Modes.data_len);

  if (!Modes.ICAO_cache || !Modes.data || !Modes.magnitude)
  {
    LOG_STDERR ("Out of memory allocating data buffer.\n");
    return (false);
  }

  memset (Modes.data, 127, Modes.data_len);
  Modes.magnitude_lut = gen_magnitude_lut();

  if (Modes.max_frames > 0)
     Modes.max_messages = Modes.max_frames;

  if (test_contains(Modes.tests, "net"))
     Modes.net = true;    /* Will force `net_init()` and it's tests to be called */

  if (!rc)
     return (false);

  if (Modes.interactive)
     return interactive_init();
  return (true);
}

/**
 * Step 3: Initialize the RTLSDR device.
 *
 * If `Modes.rtlsdr.name` is specified, select the device that matches `product`.
 * Otherwise select on `Modes.rtlsdr.index` where 0 is the first device found.
 *
 * If you have > 1 RTLSDR device with the same product name and serial-number,
 * then the command `rtl_eeprom -d 1 -p RTL2838-Silver` is handy to set them apart.
 * Like:
 *  ```
 *   product: RTL2838-Silver, serial: 00000001
 *   product: RTL2838-Blue,   serial: 00000001
 *  ```
 *
 * \note Not called for a remote RTL_TCP device.
 */
static bool modeS_init_RTLSDR (void)
{
  int    i, rc, device_count;
  bool   gain_ok;
  double gain;

  device_count = rtlsdr_get_device_count();
  if (device_count <= 0)
  {
    LOG_STDERR ("No supported RTLSDR devices found. Error: %s\n", get_rtlsdr_error());
    return (false);
  }

  LOG_STDOUT ("Found %d device(s):\n", device_count);
  for (i = 0; i < device_count; i++)
  {
    char manufact [256] = "??";
    char product  [256] = "??";
    char serial   [256] = "??";
    bool selected = false;
    int  r = rtlsdr_get_device_usb_strings (i, manufact, product, serial);

    if (r == 0)
    {
      if (Modes.rtlsdr.name && product[0] && !stricmp(Modes.rtlsdr.name, product))
      {
        selected = true;
        Modes.rtlsdr.index = i;
      }
      else
        selected = (i == Modes.rtlsdr.index);

      if (selected)
         Modes.selected_dev = mg_mprintf ("%s (%s)", product, manufact);
    }
    LOG_STDOUT ("%d: %-10s %-20s SN: %s%s\n", i, manufact, product, serial,
                selected ? " (currently selected)" : "");
  }

  if (Modes.rtlsdr.power_cycle)
     rtlsdr_power_cycle();

  if (Modes.rtlsdr.calibrate)
     rtlsdr_cal_imr (1);

  rc = rtlsdr_open (&Modes.rtlsdr.device, Modes.rtlsdr.index);
  if (rc)
  {
    const char *err = get_rtlsdr_error();

    if (Modes.rtlsdr.name)
         LOG_STDERR ("Error opening the RTLSDR device %s: %s\n", Modes.rtlsdr.name, err);
    else LOG_STDERR ("Error opening the RTLSDR device %d: %s\n", Modes.rtlsdr.index, err);
    return (false);
  }

  /* Set gain, AGC, frequency correction, Bias-T, frequency, sample rate, and reset the buffers.
   */
  gain_ok = nearest_gain (Modes.rtlsdr.device, Modes.gain_auto ? NULL : &Modes.gain);
  if (gain_ok)
  {
    if (Modes.gain_auto)
         verbose_gain_auto (Modes.rtlsdr.device);
    else verbose_gain_set (Modes.rtlsdr.device, Modes.gain);
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
    return (false);
  }

  rc = rtlsdr_set_sample_rate (Modes.rtlsdr.device, Modes.sample_rate);
  if (rc)
  {
    LOG_STDERR ("Error setting sample-rate: %d.\n", rc);
    return (false);
  }

  if (Modes.band_width > 0)
  {
    uint32_t applied_bw = 0;

    rc = rtlsdr_set_and_get_tuner_bandwidth (Modes.rtlsdr.device, 0, &applied_bw, 0);
    if (rc == 0)
         LOG_STDOUT ("Bandwidth reported by device: %.3f MHz.\n", applied_bw/1E6);
    else LOG_STDOUT ("Bandwidth reported by device: <unknown>.\n");

    LOG_STDOUT ("Setting Bandwidth to: %.3f MHz.\n", Modes.band_width/1E6);
    rc = rtlsdr_set_tuner_bandwidth (Modes.rtlsdr.device, Modes.band_width);
    if (rc != 0)
    {
      LOG_STDERR ("Error setting bandwidth: %d.\n", rc);
      return (false);
    }
  }

  LOG_STDOUT ("Tuned to %.03f MHz.\n", Modes.freq / 1E6);

  gain = rtlsdr_get_tuner_gain (Modes.rtlsdr.device);
  if ((unsigned int)gain == 0)
       LOG_STDOUT ("Gain reported by device: AUTO.\n");
  else LOG_STDOUT ("Gain reported by device: %.2f dB.\n", gain/10.0);

  rtlsdr_reset_buffer (Modes.rtlsdr.device);

  return (true);
}

/**
 * This RX-data callback gets data from the local RTLSDR, a remote RTLSDR
 * device or a local SDRplay device asynchronously.
 * We then populate the data buffer for "Pulse Position Modulation" decoding in
 * `detect_modeS()`.
 *
 * \note A Mutex is used to avoid race-condition with the decoding thread.
 * \node "Mode S" is "Mode Select Beacon System" (\ref "docs/The-1090MHz-riddle.pdf" chapter 1.4.)
 */
void rx_callback (uint8_t *buf, uint32_t len, void *ctx)
{
  volatile bool exit = *(volatile bool*) ctx;

  if (exit)
     return;

  EnterCriticalSection (&Modes.data_mutex);
  if (len > MODES_ASYNC_BUF_SIZE)
     len = MODES_ASYNC_BUF_SIZE;

  /* Move the last part of the previous buffer, that was not processed,
   * to the start of the new buffer.
   */
  memcpy (Modes.data, Modes.data + MODES_ASYNC_BUF_SIZE, 4*(MODES_FULL_LEN-1));

  /* Read the new data.
   */
  memcpy (Modes.data + 4*(MODES_FULL_LEN-1), buf, len);
  Modes.data_ready = true;
  LeaveCriticalSection (&Modes.data_mutex);
}

/**
 * Close the `--infile file` handle.
 */
static void infile_exit (void)
{
  if (Modes.infile_fd == STDIN_FILENO)
        SETMODE (STDIN_FILENO, O_TEXT);
  else _close (Modes.infile_fd);

  Modes.infile_fd = -1;
}

/**
 * This is used when `--infile` is specified in order to read data from file
 * instead of using a RTLSDR / SDRplay device.
 */
static int infile_read (void)
{
  uint32_t rc = 0;

  if (Modes.loops > 0 && Modes.infile_fd == STDIN_FILENO)
  {
    LOG_STDERR ("Option `--loops <N>' not supported for `stdin'.\n");
    Modes.loops = 0;
  }

  do
  {
     int      nread, toread;
     uint8_t *data;

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
     memcpy (Modes.data, Modes.data + MODES_ASYNC_BUF_SIZE, 4*(MODES_FULL_LEN-1));
     toread = MODES_ASYNC_BUF_SIZE;
     data   = Modes.data + 4*(MODES_FULL_LEN-1);

     while (toread)
     {
       nread = _read (Modes.infile_fd, data, toread);
       if (nread <= 0)
          break;
       data   += nread;
       toread -= nread;
     }

     if (toread)
     {
       /* Not enough data on file to fill the buffer? Pad with
        * no signal.
        */
       memset (data, 127, toread);
     }

     compute_magnitude_vector (Modes.data);
     rc += detect_modeS (Modes.magnitude, Modes.data_len/2);
     background_tasks();

     if (Modes.exit || Modes.infile_fd == STDIN_FILENO)
        break;

     /* seek the file again from the start
      * and re-play it if --loops was given.
      */
     if (Modes.loops > 0)
        Modes.loops--;
     if (Modes.loops == 0 || _lseek(Modes.infile_fd, 0, SEEK_SET) == -1)
        break;
  }
  while (1);
  return (rc);
}

/**
 * We read RTLSDR or SDRplay data using a separate thread, so the main
 * thread only handles decoding without caring about data acquisition.
 * \ref `main_data_loop()` below.
 */
static unsigned int __stdcall data_thread_fn (void *arg)
{
  int rc;

#if 0  /** \todo see below */
  if (Modes.infile[0])
  {
    rc = infile_read_async (Modes.infile, rx_callback, (void*)&Modes.exit,
                            MODES_ASYNC_BUF_NUMBERS, MODES_ASYNC_BUF_SIZE);

    modeS_signal_handler (0);   /* break out of main_data_loop() */
    LOG_STDERR  ("infile_read_async(): rc: %d / %s.\n", rc, strerror(rc));
  }
  else
#endif
  if (Modes.sdrplay.device)
  {
    rc = sdrplay_read_async (Modes.sdrplay.device, rx_callback, (void*)&Modes.exit,
                             MODES_ASYNC_BUF_NUMBERS, MODES_ASYNC_BUF_SIZE);

    LOG_STDERR ("sdrplay_read_async(): rc: %d / %s.\n", rc, sdrplay_strerror(rc));
    modeS_signal_handler (0);   /* break out of main_data_loop() */
  }
  else if (Modes.rtlsdr.device)
  {
    rc = rtlsdr_read_async (Modes.rtlsdr.device, rx_callback, (void*)&Modes.exit,
                            MODES_ASYNC_BUF_NUMBERS, MODES_ASYNC_BUF_SIZE);

    LOG_STDERR ("rtlsdr_read_async(): rc: %d/%s\n", rc, get_rtlsdr_error());
    modeS_signal_handler (0);    /* break out of main_data_loop() */
  }
  MODES_NOTUSED (arg);
  return (0);
}

/**
 * Main data processing loop.
 *
 * This runs in the main thread of the program.
 */
static void main_data_loop (void)
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

    if (Modes.max_messages > 0 && --Modes.max_messages == 0)
    {
      LOG_STDOUT ("'Modes.max_messages' reached 0.\n");
      Modes.exit = true;
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
static void dump_magnitude_bar (uint16_t magnitude, int index)
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
static void dump_magnitude_vector (const uint16_t *m, uint32_t offset)
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
static void dump_raw_message_JS (const char *descr, uint8_t *msg, const uint16_t *m, uint32_t offset,
                                 int fixable, uint32_t frame)
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
  fp = fopen_excl ("frames.js", "a");
  if (!fp)
  {
    LOG_STDERR ("Error opening frames.js: %s.\n", strerror(errno));
    Modes.exit = 1;
    return;
  }

  fprintf (fp, "frames.push({\"descr\": \"%s\", \"mag\": [", descr);
  for (j = start; j <= end; j++)
  {
    fprintf (fp, "%d", j < 0 ? 0 : m[j]);
    if (j != end)
       fprintf (fp, ",");
  }
  fprintf (fp, "], \"fix1\": %d, \"fix2\": %d, \"bits\": %d, \"hex\": \"",
           fix1, fix2, modeS_message_len_by_type(msg[0] >> 3));

  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
      fprintf (fp, "\\x%02x", msg[j]);
  fprintf (fp, "\"});\n");
  fclose (fp);
  (void) frame;
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
static void dump_raw_message (const char *descr, uint8_t *msg, const uint16_t *m,
                              uint32_t offset, uint32_t frame)
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
    dump_raw_message_JS (descr, msg, m, offset, fixable, frame);
    return;
  }

  EnterCriticalSection (&Modes.print_mutex);

  printf ("\n--- %s:\n    ", descr);
  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
  {
    printf ("%02X", msg[j]);
    if (j == MODES_SHORT_MSG_BYTES - 1)
       printf (" ... ");
  }
  printf (" (DF %d, Fixable: %d, frame: %u)\n", msg_type, fixable, frame);
  dump_magnitude_vector (m, offset);
  puts ("---");

  LeaveCriticalSection (&Modes.print_mutex);
}

/*
 * Return the CRC in a message.
 * CRC is always the last three bytes.
 */
static __inline uint32_t CRC_get (const uint8_t *msg, int bits)
{
  uint32_t CRC = ((uint32_t) msg [(bits / 8) - 3] << 16) |
                 ((uint32_t) msg [(bits / 8) - 2] << 8) |
                  (uint32_t) msg [(bits / 8) - 1];
  return (CRC);
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
static const uint32_t checksum_table [MODES_LONG_MSG_BITS] = {
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

static uint32_t CRC_check (const uint8_t *msg, int bits)
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
    int mask = 1 << (7 - bit);

    /* If bit is set, XOR with corresponding table entry.
     */
    if (msg[byte] & mask)
       crc ^= checksum_table [j + offset];
  }
  return (crc); /* 24 bit checksum. */
}

/**
 * Given the Downlink Format (DF) of the message, return the
 * message length in bits.
 */
static int modeS_message_len_by_type (int type)
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
static int fix_single_bit_errors (uint8_t *msg, int bits)
{
  int     i;
  uint8_t aux [MODES_LONG_MSG_BITS / 8];

  for (i = 0; i < bits; i++)
  {
    int      byte = i / 8;
    int      mask = 1 << (7-(i % 8));
    uint32_t crc1, crc2;

    memcpy (aux, msg, bits/8);
    aux [byte] ^= mask;   /* Flip j-th bit. */

    crc1 = CRC_get (aux, bits);
    crc2 = CRC_check (aux, bits);

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
 * don't pass the checksum, and only with `Modes.error_correct_2` setting.
 */
static int fix_two_bits_errors (uint8_t *msg, int bits)
{
  int     j, i;
  uint8_t aux [MODES_LONG_MSG_BITS / 8];

  for (j = 0; j < bits; j++)
  {
    int byte1 = j / 8;
    int mask1 = 1 << (7-(j % 8));

    /* Don't check the same pairs multiple times, so i starts from j+1 */
    for (i = j+1; i < bits; i++)
    {
      int      byte2 = i / 8;
      int      mask2 = 1 << (7 - (i % 8));
      uint32_t crc1, crc2;

      memcpy (aux, msg, bits/8);

      aux [byte1] ^= mask1;  /* Flip j-th bit. */
      aux [byte2] ^= mask2;  /* Flip i-th bit. */

      crc1 = CRC_get (aux, bits);
      crc2 = CRC_check (aux, bits);

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
static uint32_t ICAO_cache_hash_address (uint32_t a)
{
  /* The following three rounds will make sure that every bit affects
   * every output bit with ~ 50% of probability.
   */
  a = ((a >> 16) ^ a) * 0x45D9F3B;
  a = ((a >> 16) ^ a) * 0x45D9F3B;
  a = ((a >> 16) ^ a);
  return (a & (MODES_ICAO_CACHE_LEN - 1));
}

/**
 * Add the specified entry to the cache of recently seen ICAO addresses.
 *
 * Note that we also add a timestamp so that we can make sure that the
 * entry is only valid for `MODES_ICAO_CACHE_TTL` seconds.
 */
static void ICAO_cache_add_address (uint32_t addr)
{
  uint32_t h = ICAO_cache_hash_address (addr);

  Modes.ICAO_cache [h*2]   = addr;
  Modes.ICAO_cache [h*2+1] = (uint32_t) time (NULL);
}

/**
 * Returns true if the specified ICAO address was seen in a DF format with
 * proper checksum (not XORed with address) no more than
 * `MODES_ICAO_CACHE_TTL` seconds ago.
 * Otherwise returns false.
 */
static bool ICAO_address_recently_seen (uint32_t addr)
{
  uint32_t h_idx = ICAO_cache_hash_address (addr);
  uint32_t _addr = Modes.ICAO_cache [2*h_idx];
  uint32_t seen  = Modes.ICAO_cache [2*h_idx + 1];

  return (_addr && _addr == addr && (time(NULL) - seen) <= MODES_ICAO_CACHE_TTL);
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
 * structure in the `AA [0..2]` fields.
 *
 * \retval true   successfully recovered a message with a correct checksum.
 * \retval false  failed to recover a message with a correct checksum.
 */
static bool brute_force_AP (const uint8_t *msg, modeS_message *mm)
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
    uint32_t CRC;
    int      last_byte = (msg_bits / 8) - 1;

    /* Work on a copy. */
    memcpy (aux, msg, msg_bits/8);

    /* Compute the CRC of the message and XOR it with the AP field
     * so that we recover the address, because:
     *
     * (ADDR xor CRC) xor CRC = ADDR.
     */
    CRC = CRC_check (aux, msg_bits);
    aux [last_byte]   ^= CRC & 0xFF;
    aux [last_byte-1] ^= (CRC >> 8) & 0xFF;
    aux [last_byte-2] ^= (CRC >> 16) & 0xFF;

    /* If the obtained address exists in our cache we consider
     * the message valid.
     */
    addr = aircraft_get_addr (aux[last_byte-2], aux[last_byte-1], aux[last_byte]);
    if (ICAO_address_recently_seen(addr))
    {
      mm->AA [0] = aux [last_byte-2];
      mm->AA [1] = aux [last_byte-1];
      mm->AA [2] = aux [last_byte];
      return (true);
    }
  }
  return (false);
}

/**
 * Decode the 13 bit AC altitude field (in DF 20 and others).
 *
 * \param in  msg   the raw message to work with.
 * \param out unit  set to either `MODES_UNIT_METERS` or `MODES_UNIT_FEETS`.
 * \retval the altitude.
 */
static int decode_AC13_field (const uint8_t *msg, metric_unit_t *unit)
{
  int m_bit = msg[3] & (1 << 6);
  int q_bit = msg[3] & (1 << 4);
  int ret;

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

      /**
       * The final altitude is due to the resulting number multiplied by 25, minus 1000.
       */
      ret = 25 * n - 1000;
      if (ret < 0)
         ret = 0;
      return (ret);
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
static int decode_AC12_field (uint8_t *msg, metric_unit_t *unit)
{
  int ret, n, q_bit = msg[5] & 1;

  if (q_bit)
  {
    /* N is the 11 bit integer resulting from the removal of bit Q
     */
    *unit = MODES_UNIT_FEET;
    n = ((msg[5] >> 1) << 4) | ((msg[6] & 0xF0) >> 4);

    /* The final altitude is due to the resulting number multiplied
     * by 25, minus 1000.
     */
    ret = 25 * n - 1000;
    if (ret < 0)
       ret = 0;
    return (ret);
  }
  return (0);
}

/**
 * Capability table.
 */
static const char *capability_str[8] = {
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
static const char *flight_status_str[8] = {
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
static const char *emerg_state_str[8] = {
    /* 0 */ "No emergency",
    /* 1 */ "General emergency (Squawk 7700)",
    /* 2 */ "Lifeguard/Medical",
    /* 3 */ "Minimum fuel",
    /* 4 */ "No communications (Squawk 7600)",
    /* 5 */ "Unlawful interference (Squawk 7500)",
    /* 6 */ "Reserved",
    /* 7 */ "Reserved"
};

static const char *get_ME_description (const modeS_message *mm)
{
  static char buf [100];

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

  snprintf (buf, sizeof(buf), "Unknown: %d/%d", mm->ME_type, mm->ME_subtype);
  return (buf);
}

/*
 * From readasb's mode_s.c
 */
static void decode_ES_surface_position (struct modeS_message *mm, bool check_imf)
{
#if 0
  // Surface position and movement
  uint8_t *me = mm->ME;

  mm->airground = AG_GROUND; // definitely.
  mm->cpr_valid = 1;
  mm->cpr_type  = CPR_SURFACE;

  // 6-12: Movement
  unsigned movement = getbits (me, 6, 12);
  if (movement > 0 && movement < 125)
  {
    mm->gs_valid    = 1;
    mm->gs.selected = mm->gs.v0 = decode_movement_field_V0 (movement); // assumed v0 until told otherwise
    mm->gs.v2       = decode_movement_field_V2 (movement);
  }

  // 13: Heading/track status
  // 14-20: Heading/track
  if (getbit(me, 13))
  {
    mm->heading_valid = 1;
    mm->heading       = getbits (me, 14, 20) * 360.0 / 128.0;
    mm->heading_type  = HEADING_TRACK_OR_HEADING;
  }

  // 21: IMF or T flag
  if (check_imf && getbit (me, 21))
     setIMF (mm);

  // 22: F flag (odd/even)
  mm->cpr_odd = getbit (me, 22);

  // 23-39: CPR encoded latitude
  mm->cpr_lat = getbits (me, 23, 39);

  // 40-56: CPR encoded longitude
  mm->cpr_lon = getbits (me, 40, 56);
#else
  (void) mm;
  (void) check_imf;
#endif
}

/**
 * Decode a raw Mode S message demodulated as a stream of bytes by `detect_modeS()`.
 *
 * And split it into fields populating a `modeS_message` structure.
 */
static int decode_modeS_message (modeS_message *mm, const uint8_t *_msg)
{
  uint32_t    CRC;   /* Computed CRC, used to verify the message CRC. */
  const char *AIS_charset = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";
  uint8_t    *msg;
  bool        check_imf = false;

  memset (mm, '\0', sizeof(*mm));

  /* Work on our local copy
   */
  memcpy (mm->msg, _msg, sizeof(mm->msg));
  msg = mm->msg;

  /* Get the message type ASAP as other operations depend on this
   */
  mm->msg_type = msg[0] >> 3;    /* Downlink Format */
  mm->msg_bits = modeS_message_len_by_type (mm->msg_type);
  mm->CRC      = CRC_get (msg, mm->msg_bits);
  CRC = CRC_check (msg, mm->msg_bits);

  /* Check CRC and fix single bit errors using the CRC when
   * possible (DF 11 and 17).
   */
  mm->error_bit = -1;    /* No error */
  mm->CRC_ok = (mm->CRC == CRC);

  if (!mm->CRC_ok && Modes.error_correct_1 && (mm->msg_type == 11 || mm->msg_type == 17))
  {
    mm->error_bit = fix_single_bit_errors (msg, mm->msg_bits);
    if (mm->error_bit != -1)
    {
      mm->CRC    = CRC_check (msg, mm->msg_bits);
      mm->CRC_ok = true;
      Modes.stat.single_bit_fix++;
    }
    else if (Modes.error_correct_2 && mm->msg_type == 17 && (mm->error_bit = fix_two_bits_errors(msg, mm->msg_bits)) != -1)
    {
      mm->CRC    = CRC_check (msg, mm->msg_bits);
      mm->CRC_ok = true;
      Modes.stat.two_bits_fix++;
    }
  }

  /* Note: most of the other computation happens **after** we fix the single bit errors.
   * Otherwise we would need to recompute the fields again.
   */
  mm->ca = msg[0] & 7;        /* Responder capabilities. */

  /* ICAO address
   */
  mm->AA [0] = msg [1];
  mm->AA [1] = msg [2];
  mm->AA [2] = msg [3];

  /* DF17 type (assuming this is a DF17, otherwise not used)
   */
  mm->ME_type    = msg[4] >> 3;      /* Extended squitter message type. */
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
    /* If this is DF 11 or DF 17 and the checksum was ok, we can add this address
     * to the list of recently seen addresses.
     */
    if (mm->CRC_ok && mm->error_bit == -1)
       ICAO_cache_add_address (aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]));
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
      mm->flight [0] = AIS_charset [msg[5] >> 2];
      mm->flight [1] = AIS_charset [((msg[5] & 3) << 4) | (msg[6] >> 4)];
      mm->flight [2] = AIS_charset [((msg[6] & 15) <<2 ) | (msg[7] >> 6)];
      mm->flight [3] = AIS_charset [msg[7] & 63];
      mm->flight [4] = AIS_charset [msg[8] >> 2];
      mm->flight [5] = AIS_charset [((msg[8] & 3) << 4) | (msg[9] >> 4)];
      mm->flight [6] = AIS_charset [((msg[9] & 15) << 2) | (msg[10] >> 6)];
      mm->flight [7] = AIS_charset [msg[10] & 63];
      mm->flight [8] = '\0';

      char *p = mm->flight + 7;
      while (*p == ' ')    /* Remove trailing spaces */
        *p-- = '\0';

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
         * hypot(x,y) == sqrt(x*x+y*y)
         */
        mm->velocity = (int) hypot ((double)mm->NS_velocity, (double)mm->EW_velocity);

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
    else if (mm->ME_type == 19 && mm->ME_subtype >= 5 && mm->ME_subtype <= 8)
    {
      decode_ES_surface_position (mm, check_imf);
    }
  }
  mm->phase_corrected = false;  /* Set to 'true' by the caller if needed. */
  return (mm->CRC_ok);
}

/**
 * Accumulate statistics of unrecognized ME types and sub-types.
 */
static void add_unrecognized_ME (int type, int subtype)
{
  unrecognized_ME *me;

  if (type >= 0 && type < MAX_ME_TYPE && subtype >= 0 && subtype < MAX_ME_SUBTYPE)
  {
    me = &Modes.stat.unrecognized_ME [type];
    me->sub_type [subtype]++;
  }
}

/**
 * Sum the number of unrecognized ME sub-types for a type.
 */
static uint64_t sum_unrecognized_ME (int type)
{
  unrecognized_ME *me = &Modes.stat.unrecognized_ME [type];
  uint64_t         sum = 0;
  int              i;

  for (i = 0; i < MAX_ME_SUBTYPE; i++)
      sum += me->sub_type [i];
  return (sum);
}

/**
 * Print statistics of unrecognized ME types and sub-types.
 */
static void print_unrecognized_ME (void)
{
  int       t, num_totals = 0;
  uint64_t  totals = 0;
  uint64_t  totals_ME [MAX_ME_TYPE];

  for (t = 0; t < MAX_ME_TYPE; t++)
  {
    totals_ME [t] = sum_unrecognized_ME (t);
    totals += totals_ME [t];
  }

  LOG_STDOUT (" %8llu unrecognized ME types:", totals);
  if (totals == 0ULL)
  {
    LOG_STDOUT ("! \n");
    return;
  }

  for (t = 0; t < MAX_ME_TYPE; t++)
  {
    char   sub_types [200];
    char  *p = sub_types;
    size_t j, left = sizeof(sub_types);
    int    n;

    if (totals_ME[t] == 0ULL)
       continue;

    *p = '\0';
    for (j = 0; j < MAX_ME_SUBTYPE; j++)
    {
      const unrecognized_ME *me = &Modes.stat.unrecognized_ME [t];

      if (me->sub_type[j] > 0ULL)
      {
        n = snprintf (p, left, "%zd,", j);
        left -= n;
        p    += n;
      }
    }

    if (p > sub_types) /* remove the comma */
         p[-1] = '\0';
    else *p = '\0';

    /* indent next line to print like:
     *   45 unrecognized ME types: 29: 20 (2)
     *                             31: 25 (3)
     */
    if (num_totals++ >= 1)
       LOG_STDOUT ("! \n                                ");

    if (sub_types[0])
         LOG_STDOUT ("! %3llu: %2d (%s)", totals, t, sub_types);
    else LOG_STDOUT ("! %3llu: %2d", totals, t);
  }
  LOG_STDOUT ("! \n");
}

/**
 * This function gets a decoded Mode S Message and prints it on the screen
 * in a human readable format.
 */
static void display_modeS_message (const modeS_message *mm)
{
  char   buf [200];
  char  *p = buf;
  size_t left = sizeof(buf);
  int    i;

  /* Handle only addresses mode first.
   */
  if (Modes.only_addr)
  {
    puts (aircraft_get_details(&mm->AA[0]));
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
    LOG_STDOUT ("  Altitude       : %d %s\n", mm->altitude, UNIT_NAME(mm->unit));
    LOG_STDOUT ("  ICAO Address   : %s\n", aircraft_get_details(&mm->AA[0]));
  }
  else if (mm->msg_type == 4 || mm->msg_type == 20)
  {
    LOG_STDOUT ("DF %d: %s, Altitude Reply.\n", mm->msg_type, mm->msg_type == 4 ? "Surveillance" : "Comm-B");
    LOG_STDOUT ("  Flight Status  : %s\n", flight_status_str [mm->flight_status]);
    LOG_STDOUT ("  DR             : %d\n", mm->DR_status);
    LOG_STDOUT ("  UM             : %d\n", mm->UM_status);
    LOG_STDOUT ("  Altitude       : %d %s\n", mm->altitude, UNIT_NAME(mm->unit));
    LOG_STDOUT ("  ICAO Address   : %s\n", aircraft_get_details(&mm->AA[0]));

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
    LOG_STDOUT ("  ICAO Address   : %s\n", aircraft_get_details(&mm->AA[0]));

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
    LOG_STDOUT ("  ICAO Address: %s\n", aircraft_get_details(&mm->AA[0]));
  }
  else if (mm->msg_type == 17)
  {
    /* DF 17 */
    LOG_STDOUT ("DF 17: ADS-B message.\n");
    LOG_STDOUT ("  Capability     : %d (%s)\n", mm->ca, capability_str[mm->ca]);
    LOG_STDOUT ("  ICAO Address   : %s\n", aircraft_get_details(&mm->AA[0]));
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
#if 1
    else if (mm->ME_type == 29)
    {
      /**\todo
       * Target State + Status Message
       */
      add_unrecognized_ME (29, mm->ME_subtype);
    }
    else if (mm->ME_type == 31)  /* Aircraft operation status */
    {
      /**\todo Ref: chapter 8 in `The-1090MHz-riddle.pdf`
       */
      add_unrecognized_ME (31, mm->ME_subtype);
    }
#endif
    else
    {
      LOG_STDOUT ("    Unrecognized ME type: %d, subtype: %d\n", mm->ME_type, mm->ME_subtype);
      add_unrecognized_ME (mm->ME_type, mm->ME_subtype);
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
static uint16_t *compute_magnitude_vector (const uint8_t *data)
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
static int detect_out_of_phase (const uint16_t *m)
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
static void apply_phase_correction (uint16_t *m)
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

#if defined(USE_READSB_DEMOD)
/**
 * Use a rewrite of the 'demodulate2400()' function from
 * https://github.com/wiedehopf/readsb.git
 */
static uint32_t detect_modeS (uint16_t *m, uint32_t mlen)
{
  struct mag_buf mag;

  memset (&mag, '\0', sizeof(mag));
  mag.data   = m;
  mag.length = mlen;
  mag.sysTimestamp = MSEC_TIME();
  demodulate2400 (&mag);
}

#else
/**
 * Detect a Mode S messages inside the magnitude buffer pointed by `m`
 * and of size `mlen` bytes. Every detected Mode S message is converted
 * into a stream of bits and passed to the function to display it.
 *
 * In the outer loop to find the preamble and a data-frame:
 *   `mlen == 131310` bits, but `j == [0 .. mlen - (2*120)]`.
 *   Hence `j == [0 .. 131070]`.
 *
 * In the inner loop to extract the bits in a frame:
 *   index `i == [0 .. 2*112]`.
 *
 * \todo Use the pulse_slicer_ppm() function from the RTL-433 project.
 * \ref https://github.com/merbanan/rtl_433/blob/master/src/pulse_slicer.c#L259
 */
static uint32_t detect_modeS (uint16_t *m, uint32_t mlen)
{
  uint8_t  bits [MODES_LONG_MSG_BITS];
  uint8_t  msg [MODES_LONG_MSG_BITS / 2];
  uint16_t aux [MODES_LONG_MSG_BITS * 2];
  uint32_t j;
  uint32_t frame = 0;
  bool     use_correction = false;
  uint32_t rc = 0;  /**\todo fix this */

  /**
   * The Mode S preamble is made of pulses of 0.5 microseconds
   * at the following time offsets:
   *
   * 0   - 0.5 usec: first pulse.
   * 1.0 - 1.5 usec: second pulse.
   * 3.5 - 4   usec: third pulse.
   * 4.5 - 5   usec: last pulse.
   *
   * Like this  (\ref ../docs/The-1090MHz-riddle.pdf, "1.4.2 Mode S replies"):
   *  ```
   *    < ----------- 8 usec / 16 bits ---------> < ---- data -- ... >
   *    __  __         __  __
   *    | | | |        | | | |
   *    | |_| |________| |_| |__________________  ....
   *
   *    ----|----|----|----|----|----|----|----|
   *    10   10   00   01   01   00   00   00
   * j: 0 1 2 3 4 5 6 7 8 9 10 ...
   * ```
   *
   * If we are sampling at 2 MHz, every sample in our magnitude vector
   * is 0.5 usec. So the preamble will look like this, assuming there is
   * an pulse at offset 0 in the array:
   *
   * ```
   *   0   -----------------
   *   1   -
   *   2   ------------------
   *   3   --
   *   4   -
   *   5   --
   *   6   -
   *   7   ------------------
   *   8   --
   *   9   -------------------
   * ```
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
         dump_raw_message ("Unexpected ratio among first 10 samples", msg, m, j, frame);

      if (Modes.max_frames > 0 && ++frame > Modes.max_frames)
         return (rc);
      continue;
    }

    /* The samples between the two spikes must be lower than the average
     * of the high spikes level. We don't test bits too near to
     * the high levels as signals can be out of phase so part of the
     * energy can be in the near samples.
     */
    high = (m[j] + m[j+2] + m[j+7] + m[j+9]) / 6;
    if (m[j+4] >= high || m[j+5] >= high)
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 3 and 6", msg, m, j, frame);

      if (Modes.max_frames > 0 && ++frame > Modes.max_frames)
         return (rc);
      continue;
    }

    /* Similarly samples in the range 11-14 must be low, as it is the
     * space between the preamble and real data. Again we don't test
     * bits too near to high levels, see above.
     */
    if (m[j+11] >= high || m[j+12] >= high || m[j+13] >= high || m[j+14] >= high)
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 10 and 15", msg, m, j, frame);

      if (Modes.max_frames > 0 && ++frame > Modes.max_frames)
         return (rc);
      continue;
    }

    Modes.stat.valid_preamble++;

good_preamble:

    /* If the previous attempt with this message failed, retry using
     * magnitude correction.
      */
    if (use_correction)
    {
      memcpy (aux, m + j + MODES_PREAMBLE_US * 2, sizeof(aux));
      if (j && detect_out_of_phase(m + j))
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
    for (i = 0; i < 2 * MODES_LONG_MSG_BITS; i += 2)
    {
      low   = m [j + i + 2*MODES_PREAMBLE_US];
      high  = m [j + i + 2*MODES_PREAMBLE_US + 1];
      delta = low - high;
      if (delta < 0)
         delta = -delta;

      if (i > 0 && delta < 256)
         bits [i/2] = bits [i/2-1];

      else if (low == high)
      {
        /* Checking if two adjacent samples have the same magnitude
         * is an effective way to detect if it's just random noise
         * that was detected as a valid preamble.
         */
        bits [i/2] = 2;    /* error */
        if (i < 2*MODES_SHORT_MSG_BITS)
           errors++;
      }
      else if (low > high)
      {
        bits [i/2] = 1;
      }
      else
      {
        /* (low < high) for exclusion
         */
        bits [i/2] = 0;
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
      msg [i/8] = bits [i]   << 7 |
                  bits [i+1] << 6 |
                  bits [i+2] << 5 |
                  bits [i+3] << 4 |
                  bits [i+4] << 3 |
                  bits [i+5] << 2 |
                  bits [i+6] << 1 |
                  bits [i+7];
    }

    int msg_type = msg[0] >> 3;
    int msg_len  = modeS_message_len_by_type (msg_type) / 8;

    /* Last check, high and low bits are different enough in magnitude
     * to mark this as real message and not just noise?
     */
    delta = 0;
    for (i = 0; i < 8 * 2 * msg_len; i += 2)
    {
      delta += abs (m[j + i + 2 * MODES_PREAMBLE_US] -
                    m[j + i + 2 * MODES_PREAMBLE_US + 1]);
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
    if (errors == 0 || (Modes.error_correct_2 && errors <= 2))
    {
      modeS_message mm;
      double        signal_power = 0.0;
      int           signal_len   = mlen;
      uint32_t      k, mag;

      /* Decode the received message and update statistics
       */
      rc += decode_modeS_message (&mm, msg);

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
#if 0
          if (mm.error_bit < MODES_LONG_MSG_BITS)
               Modes.stat.single_bit_fix++;
          else Modes.stat.two_bits_fix++;
#endif
        }
      }

      /* Output debug mode info if needed.
       */
      if (!use_correction)
      {
        if (Modes.debug & DEBUG_DEMOD)
           dump_raw_message ("Demodulated with 0 errors", msg, m, j, frame);

        else if ((Modes.debug & DEBUG_BADCRC) && mm.msg_type == 17 && (!mm.CRC_ok || mm.error_bit != -1))
           dump_raw_message ("Decoded with bad CRC", msg, m, j, frame);

        else if ((Modes.debug & DEBUG_GOODCRC) && mm.CRC_ok && mm.error_bit == -1)
           dump_raw_message ("Decoded with good CRC", msg, m, j, frame);
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
      if (mm.CRC_ok)
         modeS_user_message (&mm);
    }
    else
    {
      if ((Modes.debug & DEBUG_DEMODERR) && use_correction)
      {
        LOG_STDOUT ("The following message has %d demod errors:", errors);
        dump_raw_message ("Demodulated with errors", msg, m, j, frame);
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
#endif  /* USE_READSB_DEMOD */

/**
 * When a new message is available, because it was decoded from the
 * RTLSDR/SDRplay device, file, or received on a TCP input port
 * (from a SBS-IN or RAW-IN service), we call this function in order
 * to use the message.
 *
 * Basically this function passes a raw message to the upper layers for
 * further processing and visualization.
 */
static void modeS_user_message (const modeS_message *mm)
{
  uint64_t  now = MSEC_TIME();
  aircraft *a;

  Modes.stat.messages_total++;
  a = interactive_receive_data (mm, now);

  if (a &&
      Modes.stat.cli_accepted [MODES_NET_SERVICE_SBS_OUT] > 0 && /* If we have accepted >=1 client */
      net_handler_sending(MODES_NET_SERVICE_SBS_OUT))            /* and we're still sending */
     modeS_send_SBS_output (mm, a);                              /* Feed SBS output clients. */

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
 * Read raw IQ samples from `stdin` and filter everything that is lower than the
 * specified level for more than 256 samples in order to reduce
 * example file size.
 *
 * Will print to `stdout` in BINARY-mode.
 */
static bool strip_mode (int level)
{
  uint64_t c = 0;
  int      I, Q;

  SETMODE (_fileno(stdin), O_BINARY);
  SETMODE (_fileno(stdout), O_BINARY);

  while ((I = getchar()) != EOF && (Q = getchar()) != EOF)
  {
    if (abs(I-127) < level && abs(Q-127) < level)
    {
      c++;
      if (c > 4*MODES_PREAMBLE_US)
         continue;
    }
    else
      c = 0;

    putchar (I);
    putchar (Q);
  }
  SETMODE (_fileno(stdin), O_TEXT);
  SETMODE (_fileno(stdout), O_TEXT);
  return (true);
}

/**
 * Write raw output to TCP clients.
 */
static void modeS_send_raw_output (const modeS_message *mm)
{
  char  msg [10 + 2*MODES_LONG_MSG_BYTES];
  char *p = msg;

  if (!net_handler_sending(MODES_NET_SERVICE_RAW_OUT))
     return;

  *p++ = '*';
  mg_hex (&mm->msg, mm->msg_bits/8, p);
  p = strchr (p, '\0');
  *p++ = ';';
  *p++ = '\n';
  net_connection_send (MODES_NET_SERVICE_RAW_OUT, msg, p - msg);
}

/**
 * Return a double-timestamp for the SBS output.
 */
static const char *get_SBS_timestamp (void)
{
  int         ts_len;
  char        ts_buf [30];
  static char timestamp [2*sizeof(ts_buf)];

  FILETIME    ft;
  SYSTEMTIME  st;

  get_FILETIME_now (&ft);
  FileTimeToSystemTime (&ft, &st);
  ts_len = snprintf (ts_buf, sizeof(ts_buf), "%04u/%02u/%02u,%02u:%02u:%02u.%03u,",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

  /* Since the date,time,date,time is just a repeat, build the whole string once and
   * then add it to each MSG output.
   */
  strcpy (timestamp, ts_buf);
  strcat (timestamp, ts_buf);

  if (ts_len >= 1)
     timestamp [ts_len - 1] = '\0';    /* remove last ',' */
  return (timestamp);
}

/**
 * Write SBS output to TCP clients (Base Station format).
 */
static void modeS_send_SBS_output (const modeS_message *mm, const aircraft *a)
{
  char  msg [MODES_MAX_SBS_SIZE], *p = msg;
  int   emergency = 0, ground = 0, alert = 0, spi = 0;
  const char *date_str;

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

  /* Field 11 could contain the call-sign we can get from `aircraft_find_or_create()::reg_num`.
   *
   * Need to output the current date and time into the SBS output
   *          1   2 3 4 5      6 7          8            9          10           11 12   13 14  15       16       17 18 19 20 21 22
   * example: MSG,3,1,1,4CA7B6,1,2023/10/20,22:33:49.364,2023/10/20,22:33:49.403,  ,7250,  ,   ,53.26917,-2.17755,  ,  ,  ,  ,  ,0
   */
  date_str = get_SBS_timestamp();

  if (mm->msg_type == 0)
  {
    p += sprintf (p, "MSG,5,1,1,%06X,1,%s,,%d,,,,,,,,,,",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  date_str, mm->altitude);
  }
  else if (mm->msg_type == 4)
  {
    p += sprintf (p, "MSG,5,1,1,%06X,1,%s,,%d,,,,,,,%d,%d,%d,%d",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  date_str, mm->altitude, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 5)
  {
    p += sprintf (p, "MSG,6,1,1,%06X,1,%s,,,,,,,,%d,%d,%d,%d,%d",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  date_str, mm->identity, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 11)
  {
    p += sprintf (p, "MSG,8,1,1,%06X,1,%s,,,,,,,,,,,,",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]), date_str);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 4)
  {
    p += sprintf (p, "MSG,1,1,1,%06X,1,%s,%s,,,,,,,,0,0,0,0",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  date_str, mm->flight);
  }
  else if (mm->msg_type == 17 && mm->ME_type >= 9 && mm->ME_type <= 18)
  {
    if (!VALID_POS(a->position))
         p += sprintf (p, "MSG,3,1,1,%06X,1,%s,,%d,,,,,,,0,0,0,0",
                       aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                       date_str, mm->altitude);
    else p += sprintf (p, "MSG,3,1,1,%06X,1,%s,,%d,,,%1.5f,%1.5f,,,0,0,0,0",
                       aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                       date_str, mm->altitude, a->position.lat, a->position.lon);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 19 && mm->ME_subtype == 1)
  {
    int vr = (mm->vert_rate_sign == 0 ? 1 : -1) * 64 * (mm->vert_rate - 1);

    p += sprintf (p, "MSG,4,1,1,%06X,1,%s,,,%d,%d,,,%i,,0,0,0,0",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  date_str, a->speed, a->heading, vr);
  }
  else if (mm->msg_type == 21)
  {
    p += sprintf (p, "MSG,6,1,1,%06X,1,%s,,,,,,,,%d,%d,%d,%d,%d",
                  aircraft_get_addr(mm->AA[0], mm->AA[1], mm->AA[2]),
                  date_str, mm->identity, alert, emergency, spi, ground);
  }
  else
    return;

  *p++ = '\n';
  net_connection_send (MODES_NET_SERVICE_SBS_OUT, msg, p - msg);
}

/**
 * \def LOG_GOOD_RAW()
 *      if `--debug g` is active, log a good RAW message.
 */
#define LOG_GOOD_RAW(fmt, ...)  TRACE ("RAW(%d): " fmt, \
                                       loop_cnt, __VA_ARGS__)

/**
 * \def LOG_BOGUS_RAW()
 *      if `--debug g` is active, log a bad / bogus RAW message.
 */
#define LOG_BOGUS_RAW(_msg, fmt, ...)  TRACE ("RAW(%d), Bogus msg %d: " fmt, \
                                              loop_cnt, _msg, __VA_ARGS__);  \
                                       Modes.stat.RAW_unrecognized++

/**
 * This function decodes a string representing a Mode S message in
 * raw hex format like: `*8D4B969699155600E87406F5B69F;<eol>`
 *
 * The string is supposed to be at the start of the client buffer
 * and NUL-terminated. It accepts both `\n` and `\r\n` terminated records.
 *
 * The message is passed to the higher level layers, so it feeds
 * the selected screen output, the network output and so forth.
 *
 * If the message looks invalid, it is silently discarded.
 *
 * The `readsb` program will send 5 heart-beats like this:
 *  `*0000;\n*0000;\n*0000;\n*0000;\n*0000;\n` (35 bytes)
 *
 * on accepting a new client. Hence check for that too.
 */
bool decode_RAW_message (mg_iobuf *msg, int loop_cnt)
{
  modeS_message mm;
  uint8_t       bin_msg [MODES_LONG_MSG_BYTES];
  int           len, j;
  uint8_t      *hex, *end;

  if (msg->len == 0)  /* all was consumed */
     return (false);

  end = memchr (msg->buf, '\n', msg->len);
  if (!end)
  {
    mg_iobuf_del (msg, 0, msg->len);
    return (false);
  }

  *end++ = '\0';
  hex = msg->buf;
  len = end - msg->buf - 1;

  if (msg->len >= 2 && end[-2] == '\r')
  {
    end [-2] = '\0';
    len--;
  }

  /* Remove spaces on the left and on the right.
   */
  if (!strcmp((const char*)hex, MODES_RAW_HEART_BEAT))
  {
    LOG_GOOD_RAW ("Got heart-beat signal");
    Modes.stat.RAW_good++;
    mg_iobuf_del (msg, 0, msg->len);
    return (true);
  }

  while (len && isspace(hex[len-1]))
  {
    hex [len-1] = '\0';
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
    Modes.stat.RAW_empty++;
    LOG_BOGUS_RAW (1, "'%.*s'", (int)msg->len, msg->buf);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  if (hex[0] != '*' || !memchr(msg->buf, ';', len))
  {
    LOG_BOGUS_RAW (2, "hex[0]: '%c', '%.*s'", hex[0], (int)msg->len, msg->buf);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  /* Turn the message into binary.
   */
  hex++;     /* Skip `*` and `;` */
  len -= 2;

  if (len > 2 * MODES_LONG_MSG_BYTES)   /* Too long message (> 28 bytes)... broken. */
  {
    LOG_BOGUS_RAW (3, "len=%d, '%.*s'", len, len, hex);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  for (j = 0; j < len; j += 2)
  {
    int high = hex_digit_val (hex[j]);
    int low  = hex_digit_val (hex[j+1]);

    if (high == -1 || low == -1)
    {
      LOG_BOGUS_RAW (4, "high='%c', low='%c'", hex[j], hex[j+1]);
      mg_iobuf_del (msg, 0, end - msg->buf);
      return (false);
    }
    bin_msg[j/2] = (high << 4) | low;
  }

  mg_iobuf_del (msg, 0, end - msg->buf);
  Modes.stat.RAW_good++;

  decode_modeS_message (&mm, bin_msg);
  if (mm.CRC_ok)
     modeS_user_message (&mm);
  return (true);
}

#define USE_str_sep 1

/**
 * The decoder for SBS input of `MSG,x` messages.
 * Details at: http://woodair.net/sbs/article/barebones42_socket_data.htm
 */
static void modeS_recv_SBS_input (char *msg, modeS_message *mm)
{
  char  *fields [23]; /* leave 0 indexed entry empty, place 22 tokens into array */
  char  *p = msg;
  size_t i;

  fields [0] = "?";

  /* E.g.:
   *   MSG,5,111,11111,45D068,111111,2024/03/16,18:53:45.000,2024/03/16,18:53:45.000,,7125,,,,,,,,,,0
   */
#if (USE_str_sep == 0)
  strtok (p, ", ");
#endif

  for (i = 1; i < DIM(fields); i++)
  {
#if USE_str_sep
    fields [i] = str_sep (&p, ",");
    if (!p && i < DIM(fields) - 1)
#else
    fields [i] = strtok (NULL, ",");
    if (!fields[i] && i < DIM(fields) - 1)
#endif
    {
      TRACE ("Missing field %zd: ", i);
      goto SBS_invalid;
    }
  }

//TRACE ("field-2: '%s', field-5: '%s' ", fields[2], fields[5]);
  memset (mm, '\0', sizeof(*mm));
  mm->CRC_ok = true;
  Modes.stat.SBS_good++;

#if 0
  /**\todo
   * Decode 'msg' and fill 'mm'. Use `decodeSbsLine()` from readsb.
   */
  modeS_user_message (&mm);
#else
  MODES_NOTUSED (msg);
#endif
  return;

SBS_invalid:
  Modes.stat.SBS_unrecognized++;
}

/**
 * \def LOG_GOOD_SBS()
 *      if `--debug g` is active, log a good SBS message.
 */
#define LOG_GOOD_SBS(fmt, ...)  TRACE ("SBS(%d): " fmt, loop_cnt, __VA_ARGS__)

/**
 * \def LOG_BOGUS_SBS()
 *      if `--debug g` is active, log a bad / bogus SBS message.
 */
#define LOG_BOGUS_SBS(fmt, ...)  TRACE ("SBS(%d), Bogus msg: " fmt, loop_cnt, __VA_ARGS__); \
                                 Modes.stat.SBS_unrecognized++

/**
 * From http://woodair.net/sbs/article/barebones42_socket_data.htm:
 * ```
 *  Message Types:
 *  There are six message types - MSG, SEL, ID, AIR, STA, CLK
 * ```
 */
static bool modeS_SBS_valid_msg (const mg_iobuf *io, bool *ignore)
{
  const char *m = (const char*) io->buf;

  *ignore = true;   /* Assume we ignore */

  if (io->len < 4)
     return (false);

  if (!strncmp(m, "MSG,", 4))
  {
    *ignore = false;
    Modes.stat.SBS_MSG_msg++;
  }

  if (strncmp(m, "AIR,", 4) == 0)
  {
    Modes.stat.SBS_AIR_msg++;
    return (true);
  }
  if (strncmp(m, "STA,", 4) == 0)
  {
    Modes.stat.SBS_STA_msg++;
    return (true);
  }
  return (!strncmp(m, "MSG,", 4) || !strncmp(m, "SEL,", 4) ||
          !strncmp(m, "CLK,", 4) || !strncmp(m, "ID,", 3));
}

/**
 * This function is called from `net_io.c` on a `MG_EV_READ` event
 * from Mongoose. Potentially multiple times until all lines in the
 * event-chunk gets consumes; `loop_cnt` is at-least 0.
 *
 * This function shall decode a string representing a Mode S message
 * in SBS format (Base Station) like:
 * ```
 * MSG,5,1,1,4CC52B,1,2021/09/20,23:30:43.897,2021/09/20,23:30:43.901,,38000,,,,,,,0,,0,
 * ```
 *
 * It accepts both '\n' and '\r\n' terminated records.
 * It checks for all 6 valid Message Types, but it handles only `"MSG"` records.
 *
 * \todo Move the handling of SBS-IN data to the `data_thread_fn()`.
 *       Add a `struct mg_queue *sbs_in_data` to `ModeS.sbs_in::fn_data`?
 */
bool decode_SBS_message (mg_iobuf *msg, int loop_cnt)
{
  modeS_message mm;
  uint8_t      *end;
  bool          ignore;

  if (Modes.debug & DEBUG_NET2)
  {
    /* with '--debug N', the trace will look like this
     *   net_io.c(938): MG_EV_READ: 703 bytes from 127.0.0.1:30003 (service "SBS TCP input")
     *   0000   4d 53 47 2c 35 2c 31 31 31 2c 31 31 31 31 31 2c   MSG,5,111,11111,
     *   0010   33 43 36 35 38 36 2c 31 31 31 31 31 31 2c 32 30   3C6586,111111,20
     *   0020   32 34 2f 30 33 2f 31 36 2c 30 36 3a 31 35 3a 33   24/03/16,06:15:3
     *   0030   39 2e 30 30 30 2c 32 30 32 34 2f 30 33 2f 31 36   9.000,2024/03/16
     *   0040   2c 30 36 3a 31 35 3a 33 39 2e 30 30 30 2c 2c 34   ,06:15:39.000,,4
     *   0050   31 37 35 2c 2c 2c 2c 2c 2c 2c 2c 2c 2c 30 0d 0a   175,,,,,,,,,,0..
     *                                                    ^_ end of this line
     *   0060   4d 53 47 2c 36 2c 31 31 31 2c 31 31 31 31 31 2c   MSG,6,111,11111,
     */
    mg_hexdump (msg->buf, msg->len);
  }

  if (msg->len == 0)  /* all was consumed */
     return (false);

  end = memchr (msg->buf, '\n', msg->len);
  if (!end)
     return (true);   /* The end-of-line could come in next message */

  *end++ = '\0';
  if (end [-2] == '\r')
     end [-2] = '\0';

  if (modeS_SBS_valid_msg(msg, &ignore))
  {
    if (!ignore)
    {
      LOG_GOOD_SBS ("'%.*s'", (int)(end - msg->buf), msg->buf);
      modeS_recv_SBS_input ((char*) msg->buf, &mm);
    }
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (true);
  }

  LOG_BOGUS_SBS ("'%.*s'", (int)(end - msg->buf), msg->buf);
  mg_iobuf_del (msg, 0, msg->len);  /* recover */
  return (false);
}

/**
 * Show the program usage
 */
static void NO_RETURN show_help (const char *fmt, ...)
{
  if (fmt)
  {
    va_list args;

    va_start (args, fmt);
    vprintf (fmt, args);
    va_end (args);
  }
  else
  {
    printf ("A 1090 MHz receiver, decoder and web-server for ADS-B (Automatic Dependent Surveillance - Broadcast).\n"
            "Usage: %s [options]:\n"
            "  --config <file>       Select config-file (default: `%s')\n"
            "  --debug <flags>       A = Log the the ADSB-LOL details to log-file.\n"
            "                        c = Log frames with bad CRC.\n"
            "                        C = Log frames with good CRC.\n"
            "                        D = Log frames decoded with 0 errors.\n"
            "                        E = Log frames decoded with errors.\n"
            "                        f = Log actions in `cfg_file.c'.\n"
            "                        g = Log general debugging info.\n"
            "                        G = A bit more general debug info than flag `g'.\n"
            "                        j = Log frames to `frames.js', loadable by `tools/debug.html'.\n"
            "                        m = Log activity in `externals/mongoose.c'.\n"
            "                        M = Log more activity in `externals/mongoose.c'.\n"
            "                        n = Log network debugging information.\n"
            "                        N = A bit more network information than flag `n'.\n"
            "                        p = Log frames with bad preamble.\n"
            "  --device <N / name>   Select RTLSDR/SDRPlay device (default: 0; first found).\n"
            "                        e.g. `--device 0'               - select first RTLSDR device found.\n"
            "                             `--device RTL2838-silver'  - select on RTLSDR name.\n"
            "                             `--device tcp://host:port' - select a remote RTLSDR tcp service (default port=%u).\n"
            "                             `--device udp://host:port' - select a remote RTLSDR udp service (default port=%u).\n"
            "                             `--device sdrplay'         - select first SDRPlay device found.\n"
            "                             `--device sdrplay1'        - select on SDRPlay index.\n"
            "                             `--device sdrplayRSP1A'    - select on SDRPlay name.\n"
            "  --infile <filename>   Read data from file (use `-' for stdin).\n"
            "  --interactive         Enable interactive mode.\n"
            "  --net                 Enable network listening services.\n"
            "  --net-active          Enable network active services.\n"
            "  --net-only            Enable only networking, no physical device or file.\n"
            "  --only-addr           Show only ICAO addresses.\n"
            "  --raw                 Output raw hexadecimal messages only.\n"
            "  --strip <level>       Output missing the I/Q parts that are below the specified level.\n"
            "  --test <test-spec>    A comma-list of tests to perform (`airport', `aircraft', `config', `locale', `net' or `*')\n"
            "  --update              Update missing or old \"*.csv\" files and exit.\n"
            "  --version, -V, -VV    Show version info. `-VV' for details.\n"
            "  --help, -h            Show this help.\n\n",
            Modes.who_am_I, Modes.cfg_file, MODES_NET_PORT_RTL_TCP, MODES_NET_PORT_RTL_TCP);

    printf ("  Refer the `%s` file for other settings.\n", Modes.cfg_file);
  }
  modeS_exit();
  exit (0);
}

/**
 * This background function is called continously by `main_data_loop()`.
 * It performs:
 *  \li Polls the network for events blocking less than 125 msec.
 *  \li Polls the `Windows Location API` for a location every 250 msec.
 *  \li Removes inactive aircrafts from the list.
 *  \li Refreshes interactive data every 250 msec (`MODES_INTERACTIVE_REFRESH_TIME`).
 *  \li Refreshes the console-title with some statistics (also 4 times per second).
 */
static void background_tasks (void)
{
  bool     refresh;
  pos_t    pos;
  uint64_t now;

  if (Modes.net)
     net_poll();

  if (Modes.exit)
     return;

  now = MSEC_TIME();

  refresh = (now - Modes.last_update_ms) >= MODES_INTERACTIVE_REFRESH_TIME;
  if (!refresh)
     return;

  Modes.last_update_ms = now;

  /* Check the asynchronous result from `Location API`.
   */
  if (Modes.win_location && location_poll(&pos))
  {
    /* Assume our location won't change while running this program.
     * Hence just stop the `Location API` event-handler.
     */
    location_exit();
    Modes.home_pos = pos;

    spherical_to_cartesian (&Modes.home_pos, &Modes.home_pos_cart);
    if (Modes.home_pos_ok)
       LOG_FILEONLY ("Ignoring the 'homepos' config value since we use the 'Windows Location API':"
                     " Latitude: %.8f, Longitude: %.8f.\n",
                     Modes.home_pos.lat, Modes.home_pos.lon);
    Modes.home_pos_ok = true;
  }

  aircraft_remove_stale (now);
  airports_background (now);

  /* Refresh screen and console-title when in interactive mode
   */
  if (Modes.interactive)
     interactive_show_data (now);

  if (Modes.rtlsdr.device || Modes.rtl_tcp_in || Modes.sdrplay.device)
  {
    interactive_title_stats();
    interactive_update_gain();
    interactive_other_stats();  /* Only effective if 'tui = curses' was used */
  }
#if 0
  else
    interactive_raw_SBS_stats();
#endif
}

/**
 * The handler called in for `SIGINT` or `SIGBREAK`. <br>
 * I.e. user presses `^C`.
 */
void modeS_signal_handler (int sig)
{
  int rc;

  if (sig > 0)
     signal (sig, SIG_DFL);   /* reset signal handler - bit extra safety */

  Modes.exit = true;          /* Signal to threads that we are done */

  /* When PDCurses exists, it restores the startup console-screen.
   * Hence make it clear what is printed on exit by separating the
   * startup and shutdown messages with a dotted "----" bar.
   */
  if ((sig == SIGINT || sig == SIGBREAK || sig == SIGABRT) && Modes.tui_interface == TUI_CURSES)
     puts ("----------------------------------------------------------------------------------");

  if (sig == SIGINT)
     LOG_STDOUT ("Caught SIGINT, shutting down ...\n");
  else if (sig == SIGBREAK)
     LOG_STDOUT ("Caught SIGBREAK, shutting down ...\n");
  else if (sig == SIGABRT)
  {
    LOG_STDOUT ("Caught SIGABRT, shutting down ...\n");
    airports_exit (false);
  }
  else if (sig == 0)
  {
    DEBUG (DEBUG_GENERAL, "Breaking 'main_data_loop()', shutting down ...\n");
  }

  if (Modes.rtlsdr.device)
  {
    EnterCriticalSection (&Modes.data_mutex);
    rtlsdr_reset_buffer (Modes.rtlsdr.device);
    rc = rtlsdr_cancel_async (Modes.rtlsdr.device);
    DEBUG (DEBUG_GENERAL, "rtlsdr_cancel_async(): rc: %d.\n", rc);

    if (rc == -2)  /* RTLSDR is not streaming data */
       Sleep (5);
    LeaveCriticalSection (&Modes.data_mutex);
  }
  else if (Modes.sdrplay.device)
  {
    rc = sdrplay_cancel_async (Modes.sdrplay.device);
    DEBUG (DEBUG_GENERAL, "sdrplay_cancel_async(): rc: %d / %s.\n", rc, sdrplay_strerror(rc));
  }
}

/*
 * Show decoder statistics for a RTLSDR / SDRPlay device.
 */
static void show_decoder_stats (void)
{
  LOG_STDOUT ("Decoder statistics:\n");
  interactive_clreol();  /* to clear the lines from startup messages */

  LOG_STDOUT (" %8llu valid preambles.\n", Modes.stat.valid_preamble);
  interactive_clreol();

  LOG_STDOUT (" %8llu demodulated after phase correction.\n", Modes.stat.out_of_phase);
  interactive_clreol();

  LOG_STDOUT (" %8llu demodulated with 0 errors.\n", Modes.stat.demodulated);
  interactive_clreol();

  LOG_STDOUT (" %8llu with CRC okay.\n", Modes.stat.good_CRC);
  interactive_clreol();

  LOG_STDOUT (" %8llu with CRC failure.\n", Modes.stat.bad_CRC);
  interactive_clreol();

  LOG_STDOUT (" %8llu errors corrected.\n", Modes.stat.fixed);
  interactive_clreol();

  LOG_STDOUT (" %8llu messages with 1 bit errors fixed.\n", Modes.stat.single_bit_fix);
  interactive_clreol();

  LOG_STDOUT (" %8llu messages with 2 bit errors fixed.\n", Modes.stat.two_bits_fix);
  interactive_clreol();

  LOG_STDOUT (" %8llu total usable messages (%llu + %llu).\n", Modes.stat.good_CRC + Modes.stat.fixed, Modes.stat.good_CRC, Modes.stat.fixed);
  interactive_clreol();

  /**\todo Move to `aircraft_show_stats()`
   */
  LOG_STDOUT (" %8llu unique aircrafts of which %llu was in CSV-file and %llu in SQL-file.\n",
              Modes.stat.unique_aircrafts, Modes.stat.unique_aircrafts_CSV, Modes.stat.unique_aircrafts_SQL);

  print_unrecognized_ME();
}

/**
 * Show some statistics at program exit.
 * If at least 1 device got opened, show some decoder statistics.
 */
static void show_statistics (void)
{
  bool any_device = (Modes.rtlsdr.device || Modes.sdrplay.device || Modes.infile_fd > -1);

  if (Modes.rtl_tcp_in && !modeS_net_services[MODES_NET_SERVICE_RTL_TCP].last_err)
     any_device = true;  /* connect() OK */

  if (any_device)  /* assume we got some data */
     show_decoder_stats();

  if (Modes.net)
     net_show_stats();

  if (Modes.airports_priv)
     airports_show_stats();

#ifdef USE_MIMALLOC
  mimalloc_stats();
#endif
}

/**
 * Our exit function. Free all resources here.
 */
static void modeS_exit (void)
{
  int rc;

  net_exit();

  if (Modes.rtlsdr.device)
  {
    if (Modes.bias_tee)
       verbose_bias_tee (Modes.rtlsdr.device, 0);
    Modes.bias_tee = 0;

    rc = rtlsdr_close (Modes.rtlsdr.device);
    free (Modes.rtlsdr.gains);
    Modes.rtlsdr.device = NULL;
    DEBUG (DEBUG_GENERAL, "rtlsdr_close(), rc: %d.\n", rc);
  }
  else if (Modes.rtl_tcp_in)
  {
    free (Modes.rtltcp.gains);
    Modes.rtl_tcp_in = NULL;
  }
  else if (Modes.sdrplay.device)
  {
    rc = sdrplay_exit (Modes.sdrplay.device);
    free (Modes.sdrplay.gains);
    Modes.sdrplay.device = NULL;
    DEBUG (DEBUG_GENERAL2, "sdrplay_exit(), rc: %d.\n", rc);
  }

  if (Modes.reader_thread)
     CloseHandle ((HANDLE)Modes.reader_thread);

  if (Modes.infile_fd > -1)
     infile_exit();

  if (Modes.interactive)
     interactive_exit();

  aircraft_exit (true);
  airports_exit (true);

  free (Modes.magnitude_lut);
  free (Modes.magnitude);
  free (Modes.data);
  free (Modes.ICAO_cache);
  free (Modes.selected_dev);
  free (Modes.rtlsdr.name);
  free (Modes.sdrplay.name);
  free (Modes.aircraft_db_url);
  free (Modes.tests);

  DeleteCriticalSection (&Modes.data_mutex);
  DeleteCriticalSection (&Modes.print_mutex);

  Modes.reader_thread = 0;
  Modes.data          = NULL;
  Modes.magnitude     = NULL;
  Modes.magnitude_lut = NULL;
  Modes.ICAO_cache    = NULL;
  Modes.selected_dev  = NULL;
  Modes.tests         = NULL;

  if (Modes.win_location)
     location_exit();

  modeS_log_exit();

#if defined(USE_MIMALLOC)
  mimalloc_exit();

#elif defined(_DEBUG)
  crtdbug_exit();
#endif
}

static void set_device (const char *arg)
{
  static bool dev_selection_done = false;

  if (dev_selection_done)
     show_help ("Option '--device' already done.\n\n");

  if (isdigit(arg[0]))
  {
    Modes.rtlsdr.index = atoi (arg);
  }
  else if (!strnicmp(arg, "tcp://", 6) || !strnicmp(arg, "udp://", 6))
  {
    net_set_host_port (arg, &modeS_net_services [MODES_NET_SERVICE_RTL_TCP], MODES_NET_PORT_RTL_TCP);
    Modes.selected_dev = mg_mprintf ("%s", modeS_net_services [MODES_NET_SERVICE_RTL_TCP].descr);
    Modes.rtlsdr.index = -1;      /* select on host+port only */
    Modes.net = true;
  }
  else
  {
    Modes.rtlsdr.name  = strdup (arg);
    Modes.rtlsdr.index = -1;  /* select on name only */
  }

  if (!strnicmp(arg, "sdrplay", 7))
  {
    Modes.sdrplay.name = strdup (arg);
    if (isdigit(arg[7]))
    {
      Modes.sdrplay.index   = atoi (arg + 7);
      Modes.sdrplay.name[7] = '\0';
    }
    else
      Modes.sdrplay.index = -1;
  }
  dev_selection_done = true;
}

static bool set_gain (const char *arg)
{
  uint16_t gain;
  char    *end;

  if (!stricmp(arg, "auto"))
     Modes.gain_auto = true;
  else
  {
    /* Gain is in tens of dBs
     */
    gain = (uint16_t) (10.0 * strtof(arg, &end));
    if (end == arg || *end != '\0')
       show_help ("Illegal gain: %s.\n", arg);
    Modes.gain = gain;
    Modes.gain_auto = false;
  }
  return (true);
}

static bool set_sample_rate (const char *arg)
{
  Modes.sample_rate = ato_hertz (arg);
  if (Modes.sample_rate == 0)
     show_help ("Illegal sample_rate: %s.\n", arg);

  if (Modes.sample_rate != MODES_DEFAULT_RATE)
  {
    if (Modes.sample_rate == 2400000)
         show_help ("2.4 MB/s sample_rate is not yet supported.\n");
    else show_help ("Illegal sample_rate: %s. Use '%uM' or leave empty.\n", arg, MODES_DEFAULT_RATE/1000000);
  }
  return (true);
}

static bool set_tui (const char *arg)
{
  if (!stricmp(arg, "wincon"))
  {
    Modes.tui_interface = TUI_WINCON;
    return (true);
  }
  if (!stricmp(arg, "curses"))
  {
    Modes.tui_interface = TUI_CURSES;
    return (true);
  }
  show_help ("Unknown `tui %s' mode.\n", arg);
  /* not reached */
  return (false);
}

static void set_debug_bits (const char *flags)
{
  while (*flags)
  {
    switch (*flags)
    {
      case 'A':
           Modes.debug |= DEBUG_ADSB_LOL;
           break;
      case 'C':
           Modes.debug |= DEBUG_GOODCRC;
           break;
      case 'c':
           Modes.debug |= DEBUG_BADCRC;
           break;
      case 'D':
           Modes.debug |= DEBUG_DEMOD;
           break;
      case 'E':
           Modes.debug |= DEBUG_DEMODERR;
           break;
      case 'f':
           Modes.debug |= DEBUG_CFG_FILE;
           break;
      case 'g':
           Modes.debug |= DEBUG_GENERAL;
           break;
      case 'G':
           Modes.debug |= (DEBUG_GENERAL2 | DEBUG_GENERAL);
           break;
      case 'j':
      case 'J':
           Modes.debug |= DEBUG_JS;
           break;
      case 'm':
           Modes.debug |= DEBUG_MONGOOSE;
           break;
      case 'M':
           Modes.debug |= DEBUG_MONGOOSE2;
           break;
      case 'n':
           Modes.debug |= DEBUG_NET;
           break;
      case 'N':
           Modes.debug |= (DEBUG_NET2 | DEBUG_NET);  /* A bit more network details */
           break;
      case 'p':
      case 'P':
           Modes.debug |= DEBUG_NOPREAMBLE;
           break;
      default:
           show_help ("Unknown debugging flag: %c\n", *flags);
           /* not reached */
           break;
    }
    flags++;
  }
}

static bool set_home_pos (const char *arg)
{
  pos_t pos;

  if (arg)
  {
    if (sscanf(arg, "%lf,%lf", &pos.lat, &pos.lon) != 2 || !VALID_POS(pos))
    {
      LOG_STDERR ("Invalid home-pos %s.\n", arg);
      return (false);
    }
    Modes.home_pos    = pos;
    Modes.home_pos_ok = true;
    spherical_to_cartesian (&Modes.home_pos, &Modes.home_pos_cart);
  }
  return (true);
}

/**
 * Use `Windows Location API` to set `Modes.home_pos`.
 * If an error happened, the error was already reported.
 * Otherwise poll the result in 'location_poll()' called
 * from `background_tasks()`.
 */
static bool set_home_pos_from_location_API (const char *arg)
{
  if (arg && cfg_true(arg))
  {
    Modes.win_location = true;
    if (!location_get_async())
       return (false);
  }
  return (true);
}

static bool set_bandwidth (const char *arg)
{
  Modes.band_width = ato_hertz (arg);
  if (Modes.band_width == 0)
     show_help ("Illegal band-width: %s\n", arg);
  return (true);
}

static bool set_bias_tee (const char *arg)
{
  Modes.bias_tee = cfg_true (arg);
  return (true);
}

static bool set_frequency (const char *arg)
{
  Modes.freq = ato_hertz (arg);
  if (Modes.freq == 0)
     show_help ("Illegal frequency: %s\n", arg);
  return (true);
}

static bool set_if_mode (const char *arg)
{
  if (!stricmp(arg, "zif"))
       Modes.sdrplay.if_mode = false;
  else if (!stricmp(arg, "lif"))
       Modes.sdrplay.if_mode = true;
  else printf ("%s(%u): Ignoring illegal '--if-mode': '%s'.\n",  cfg_current_file(), cfg_current_line(), arg);
  return (true);
}

static bool set_interactive_ttl (const char *arg)
{
  Modes.interactive_ttl = 1000 * atoi (arg);
  return (true);
}

static bool set_infile (const char *arg)
{
  strcpy_s (Modes.infile, sizeof(Modes.infile), arg);
  return (true);
}

static bool set_logfile (const char *arg)
{
  strcpy_s (Modes.logfile_initial, sizeof(Modes.logfile_initial), arg);
  return (true);
}

static bool set_loops (const char *arg)
{
  Modes.loops = arg ? _atoi64 (arg) : LLONG_MAX;
  return (true);
}

static bool set_port_http (const char *arg)
{
  modeS_net_services [MODES_NET_SERVICE_HTTP].port = (uint16_t) atoi (arg);
  return (true);
}

static bool set_port_raw_in (const char *arg)
{
  modeS_net_services [MODES_NET_SERVICE_RAW_IN].port = (uint16_t) atoi (arg);
  return (true);
}

static bool set_port_raw_out (const char *arg)
{
  modeS_net_services [MODES_NET_SERVICE_RAW_OUT].port = (uint16_t) atoi (arg);
  return (true);
}

static bool set_port_sbs (const char *arg)
{
  modeS_net_services [MODES_NET_SERVICE_SBS_OUT].port = (uint16_t) atoi (arg);
  return (true);
}

static bool set_host_port_raw_in (const char *arg)
{
  if (!net_set_host_port(arg, &modeS_net_services [MODES_NET_SERVICE_RAW_IN], MODES_NET_PORT_RAW_IN))
     return (false);
  return (true);
}

static bool set_host_port_raw_out (const char *arg)
{
  if (!net_set_host_port(arg, &modeS_net_services [MODES_NET_SERVICE_RAW_OUT], MODES_NET_PORT_RAW_OUT))
     return (false);
  return (true);
}

static bool set_host_port_sbs_in (const char *arg)
{
  if (!net_set_host_port(arg, &modeS_net_services [MODES_NET_SERVICE_SBS_IN], MODES_NET_PORT_SBS))
     return (false);
  return (true);
}

static bool set_ppm (const char *arg)
{
  Modes.rtlsdr.ppm_error = atoi (arg);
  Modes.rtltcp.ppm_error = Modes.rtlsdr.ppm_error;
  return (true);
}

static bool set_prefer_adsb_lol (const char *arg)
{
  Modes.prefer_adsb_lol = cfg_true (arg);

#if !defined(USE_GEN_ROUTES)
  DEBUG (DEBUG_GENERAL,
         "Config value 'prefer-adsb-lol=%d' has no meaning.\n"
         "Will always use ADSB-LOL API to lookup routes in 'airports.c'.\n",
         Modes.prefer_adsb_lol);
#endif

  return (true);
}

static bool set_web_page (const char *arg)
{
  strcpy_s (Modes.web_root, sizeof(Modes.web_root), dirname(arg));
  strcpy_s (Modes.web_page, sizeof(Modes.web_page), basename(arg));
  DEBUG (DEBUG_GENERAL, "Full-name of web_page: '%s/%s'\n", Modes.web_root, Modes.web_page);
  return (true);
}

static struct option long_options[] = {
  { "config",      required_argument,  NULL,               'c' },
  { "debug",       required_argument,  NULL,               'd' },
  { "device",      required_argument,  NULL,               'D' },
  { "help",        no_argument,        NULL,               'h' },
  { "infile",      required_argument,  NULL,               'i' },
  { "interactive", no_argument,        &Modes.interactive,  1  },
  { "net",         no_argument,        &Modes.net,          1  },
  { "net-active",  no_argument,        &Modes.net_active,  'N' },
  { "net-only",    no_argument,        &Modes.net_only,    'n' },
  { "only-addr",   no_argument,        &Modes.only_addr,    1  },
  { "raw",         no_argument,        &Modes.raw,          1  },
  { "strip",       required_argument,  NULL,               'S' },
  { "test",        required_argument,  NULL,               'T' },
  { "update",      no_argument,        NULL,               'u' },
  { "version",     no_argument,        NULL,               'V' },
  { NULL,          no_argument,        NULL,                0  }
};

static bool parse_cmd_line (int argc, char **argv)
{
  int   c, show_ver = 0, idx = 0;
  bool  rc = true;
  char *non_options;

  while ((c = getopt_long (argc, argv, "+h?V", long_options, &idx)) != EOF)
  {
    switch (c)
    {
      case 'c':
           strcpy_s (Modes.cfg_file, sizeof(Modes.cfg_file), optarg);
           break;

      case 'd':
           set_debug_bits (optarg);
           break;

      case 'D':
           set_device (optarg);
           break;

      case 'h':
      case '?':
           show_help (NULL);
           /* not reached */
           break;

      case 'i':
           set_infile (optarg);
           break;

      case 'N':
           Modes.net_active = Modes.net = true;
           break;

      case 'n':
           Modes.net_only = Modes.net = true;
           break;

      case 'S':
           Modes.strip_level = atoi (optarg);
           if (Modes.strip_level == 0)
              show_help ("Illegal level for `--strip %d'.\n", Modes.strip_level);
           break;

      case 'T':
           test_add (&Modes.tests, optarg);
           break;

      case 'u':
           Modes.update = true;
           break;

      case 'V':
           show_ver++;
           break;
    }
  }

  if (show_ver > 0)
  {
    show_version_info (show_ver >= 2);
    rc = false;
  }
  else if (Modes.net_only || Modes.net_active)
  {
    Modes.net = Modes.net_only = true;
  }

  argv += optind;
  if (*argv)
  {
    non_options = str_join (argv, " ");
    fprintf (stderr, "Argments ('%s') after options was unexpected!\n", non_options);
    free (non_options);
    rc = false;
  }
  return (rc);
}

/**
 * Our main entry.
 */
int main (int argc, char **argv)
{
  bool init_error = true;   /* assume some 'x_init()' failure */
  int  rc;

#if defined(USE_MIMALLOC)
  mimalloc_init();

#elif defined(_DEBUG)
  crtdbug_init();
#endif

  modeS_init_config();  /* Set sane defaults */

  if (!parse_cmd_line(argc, argv))
     goto quit;

  rc = modeS_init();    /* Initialization based on cmd-line options */
  if (!rc)
     goto quit;

  if (Modes.net_only)
  {
    char notice [100] = "";

    if ((Modes.rtlsdr.name  || Modes.rtlsdr.index > -1 ||
         Modes.sdrplay.name || Modes.sdrplay.index > -1) &&
        !Modes.rtl_tcp_in)
    {
      strcpy (notice, " The `--device x' option has no effect now.");
    }
    LOG_STDERR ("Net-only mode, no physical device or file open.%s\n", notice);
  }
  else if (Modes.strip_level)
  {
    rc = strip_mode (Modes.strip_level);
  }
  else if (Modes.infile[0])
  {
    rc = 1;
    if (Modes.infile[0] == '-' && Modes.infile[1] == '\0')
    {
      Modes.infile_fd = STDIN_FILENO;
      SETMODE (Modes.infile_fd, O_BINARY);
    }
    else if ((Modes.infile_fd = _open(Modes.infile, O_RDONLY | O_BINARY)) == -1)
    {
      LOG_STDERR ("Error opening `%s`: %s\n", Modes.infile, strerror(errno));
      goto quit;
    }
  }
  else if (!Modes.tests)     /* for testing, do not initialize RTLSDR/SDRPlay */
  {
    if (Modes.sdrplay.name)
    {
      rc = sdrplay_init (Modes.sdrplay.name, Modes.sdrplay.index, &Modes.sdrplay.device);
      DEBUG (DEBUG_GENERAL, "sdrplay_init(): rc: %d / %s.\n", rc, sdrplay_strerror(rc));
      if (rc)
         goto quit;
    }
    else if (!modeS_net_services[MODES_NET_SERVICE_RTL_TCP].host[0])
    {
      rc = modeS_init_RTLSDR();  /* not using a remote RTL_TCP input device */
      DEBUG (DEBUG_GENERAL, "modeS_init_RTLSDR(): rc: %d.\n", rc);
      if (!rc)
         goto quit;
    }
  }

  if (Modes.net)
  {
    /* This will also setup a service for the remote RTL_TCP input device.
     */
    rc = net_init();
    DEBUG (DEBUG_GENERAL, "net_init(): rc: %d.\n", rc);
    if (!rc)
       goto quit;
  }

  init_error = false;

  if (Modes.tests)
     goto quit;

  /**
   * \todo Move processing of `Modes.infile` to the same thread
   * for consistent handling of all sample-sources.
   */
  if (Modes.infile[0])
  {
    if (infile_read() == 0)
       LOG_STDERR ("No good messages found in '%s'.\n", Modes.infile);
  }
  else if (Modes.strip_level == 0 || !Modes.rtl_tcp_in)
  {
    /* Create the thread that will read the data from a physical RTLSDR or SDRplay device.
     * No need for a data-thread with RTL_TCP.
     */
    Modes.reader_thread = _beginthreadex (NULL, 0, data_thread_fn, NULL, 0, NULL);
    if (!Modes.reader_thread)
    {
      LOG_STDERR ("_beginthreadex() failed: %s.\n", strerror(errno));
      goto quit;
    }
    main_data_loop();
  }

quit:
  if (!init_error)
     show_statistics();
  modeS_exit();
  return (0);
}
