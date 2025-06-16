/**
 * \file    dump1090.c
 * \ingroup Main
 * \brief   Dump1090, a Mode-S messages decoder for RTLSDR / SDRPlay devices.
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
#include "cpr.h"
#include "crc.h"
#include "demod.h"
#include "geo.h"
#include "convert.h"
#include "sdrplay.h"
#include "speech.h"
#include "location.h"
#include "airports.h"
#include "aircraft.h"
#include "interactive.h"
#include "infile.h"

global_data Modes;

static_assert (MODES_MAG_BUFFERS < MODES_ASYNC_BUF_NUMBERS, /* 12 < 15 */
               "'MODES_MAG_BUFFERS' should be smaller than 'MODES_ASYNC_BUF_NUMBERS' for flowcontrol to work");

/**
 * \addtogroup Main         Main functions
 * \addtogroup Misc         Support functions
 * \addtogroup Samplers     SDR input functions
 * \addtogroup Demodulators Magnitude demodulators
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
 * \image html img/dump1090-blocks.png
 *
 * ### Example Web-client page:
 * \image html img/dump1090-24MSs.png
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
static void  modeS_cleanup (void);
static void  modeS_exit (int rc);
static void  modeS_send_raw_output (const modeS_message *mm);
static void  modeS_send_SBS_output (const modeS_message *mm);
static void  add_unrecognized_ME (int type, int subtype, bool test);

static bool  set_bandwidth (const char *arg);
static bool  set_bias_tee (const char *arg);
static bool  set_frequency (const char *arg);
static bool  set_gain (const char *arg);
static bool  set_if_mode (const char *arg);
static bool  set_interactive_ttl (const char *arg);
static bool  set_home_pos (const char *arg);
static bool  set_home_pos_from_location_API (const char *arg);
static bool  set_host_port_raw_in (const char *arg);
static bool  set_host_port_raw_out (const char *arg);
static bool  set_host_port_sbs_in (const char *arg);
static bool  set_logfile (const char *arg);
static bool  set_loops (const char *arg);
static bool  set_port_http (const char *arg);
static bool  set_port_raw_in (const char *arg);
static bool  set_port_raw_out (const char *arg);
static bool  set_port_sbs (const char *arg);
static bool  set_prefer_adsb_lol (const char *arg);
static bool  set_ppm (const char *arg);
static bool  set_sample_rate (const char *arg);
static bool  set_tui (const char *arg);
static bool  set_web_page (const char *arg);

static uint32_t sample_rate;
static uint64_t max_messages;

static const cfg_table config[] = {
    { "adsb-mode",        ARG_FUNC,    (void*) sdrplay_set_adsb_mode },
    { "bias-t",           ARG_FUNC,    (void*) set_bias_tee },
    { "DC-filter",        ARG_ATOB,    (void*) &Modes.DC_filter },
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
    { "fifo-bufs",        ARG_ATO_U32, (void*) &Modes.FIFO_init_bufs },
    { "fifo-acquire",     ARG_ATO_U32, (void*) &Modes.FIFO_acquire_ms },
    { "freq",             ARG_FUNC,    (void*) set_frequency },
    { "agc",              ARG_ATOB,    (void*) &Modes.dig_agc },
    { "interactive-ttl",  ARG_FUNC,    (void*) set_interactive_ttl },
    { "keep-alive",       ARG_ATOB,    (void*) &Modes.keep_alive },
    { "http-ipv6",        ARG_ATOB,    (void*) &Modes.http_ipv6 },
    { "http-ipv6-only",   ARG_ATOB,    (void*) &Modes.http_ipv6_only },
    { "logfile",          ARG_FUNC,    (void*) set_logfile },
    { "logfile-daily",    ARG_ATOB,    (void*) &Modes.logfile_daily },
    { "logfile-ignore",   ARG_FUNC,    (void*) modeS_log_add_ignore },
    { "loops",            ARG_FUNC,    (void*) set_loops },
    { "max-messages",     ARG_ATO_U64, (void*) &Modes.max_messages },
    { "max-frames",       ARG_ATO_U64, (void*) &Modes.max_frames },
    { "measure-noise",    ARG_ATOB,    (void*) &Modes.measure_noise },
    { "net-http-port",    ARG_FUNC,    (void*) set_port_http },
    { "net-ri-port",      ARG_FUNC,    (void*) set_port_raw_in },
    { "net-ro-port",      ARG_FUNC,    (void*) set_port_raw_out },
    { "net-sbs-port",     ARG_FUNC,    (void*) set_port_sbs },
    { "prefer-adsb-lol",  ARG_FUNC,    (void*) set_prefer_adsb_lol },
    { "reverse-resolve",  ARG_ATOB,    (void*) &Modes.reverse_resolve },
    { "rtlsdr-reset",     ARG_ATOB,    (void*) &Modes.rtlsdr.power_cycle },
    { "samplerate",       ARG_FUNC,    (void*) set_sample_rate },
    { "sample-rate",      ARG_FUNC,    (void*) set_sample_rate },
    { "show-hostname",    ARG_ATOB,    (void*) &Modes.show_host_name },
    { "sort",             ARG_FUNC,    (void*) aircraft_set_sort },
    { "speech-enable",    ARG_ATOB,    (void*) &Modes.speech_enable },
    { "speech-volume",    ARG_ATOI,    (void*) &Modes.speech_volume },
    { "https-enable",     ARG_ATOB,    (void*) &Modes.https_enable },
    { "silent",           ARG_ATOB,    (void*) &Modes.silent },
    { "phase-enhance",    ARG_ATOB,    (void*) &Modes.phase_enhance },
    { "ppm",              ARG_FUNC,    (void*) set_ppm },
    { "host-raw-in",      ARG_FUNC,    (void*) set_host_port_raw_in },
    { "host-raw-out",     ARG_FUNC,    (void*) set_host_port_raw_out },
    { "host-sbs-in",      ARG_FUNC,    (void*) set_host_port_sbs_in },
    { "error-correct1",   ARG_ATOB,    (void*) &Modes.error_correct_1 },
    { "error-correct2",   ARG_ATOB,    (void*) &Modes.error_correct_2 },
    { "web-send-rssi",    ARG_ATOB,    (void*) &Modes.web_send_rssi },
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

#if defined(NOT_USED_YET)
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
 *
 * Used in convert.c.
 */
static uint16_t *gen_magnitude_lut (void)
{
  uint16_t *lut = malloc (sizeof(*lut) * 129 * 129);
  int       I, Q;

  if (!lut)
     return (NULL);

  for (I = 0; I < 129; I++)
  {
    for (Q = 0; Q < 129; Q++)
       lut [I*129 + Q] = (uint16_t) round (360 * hypot(I, Q));
  }
  return (lut);
}

/**
 * For same reason as above, allocate and populate a
 * log10(x) lookup-table.
 *
 * Used by demod-2000.c only to calculate the SNR.
 */
static uint16_t *gen_log10_lut (void)
{
  uint16_t *lut = calloc (sizeof(uint16_t), 65536);
  int       i;

  if (!lut)
     return (NULL);

  /* Prepare the log10 lookup table: 100*log10 (x)
   */
  for (i = 1; i < 65536; i++)
      lut [i] = (uint16_t) round (100.0 * log10(i));
  return (lut);
}

/**
 * Initialize our temporary directory == `%TEMP%\\dump1090` and
 * `results_dir == `%TEMP%\\dump1090\\standing-data\\results`.
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

  /* And now 'results_dir'
   */
  strcpy (Modes.results_dir, Modes.tmp_dir);
  strcat_s (Modes.results_dir, sizeof(Modes.results_dir), "\\standing-data\\results");

  /* Do not call `CreateDirectory (Modes.results_dir, 0)`.
   * That should be done by `Modes.where_am_I/tools/gen_data.py`.
   */
}

/**
 * Trap some possible dev-errors.
 */
static void dummy_converter (const void    *iq_input,
                             uint16_t      *mag_output,
                             unsigned       nsamples,
                             convert_state *state,
                             double        *out_power)
{
  MODES_NOTUSED (iq_input);
  MODES_NOTUSED (mag_output);
  MODES_NOTUSED (nsamples);
  MODES_NOTUSED (state);
  MODES_NOTUSED (out_power);
  LOG_STDERR ("Development error: calling 'dummy_converter()'!\n");
  exit (1);
}

static void dummy_demod (const mag_buf *mag)
{
  MODES_NOTUSED (mag);
  LOG_STDERR ("Development error: calling 'dummy_demod()'!\n");
  exit (1);
}

/**
 * Step 1: Initialize the program with default values.
 */
static void modeS_init_config (void)
{
  memset (&Modes, '\0', sizeof(Modes));
  GetModuleFileNameA (NULL, Modes.who_am_I, sizeof(Modes.who_am_I));
  snprintf (Modes.where_am_I, sizeof(Modes.where_am_I), "%s", dirname(Modes.who_am_I));

  GetSystemDirectory (Modes.sys_dir, sizeof(Modes.sys_dir));

  modeS_init_temp();

  snprintf (Modes.cfg_file, sizeof(Modes.cfg_file), "%s\\dump1090.cfg", Modes.where_am_I);
  strcpy (Modes.web_page, basename(INDEX_HTML));
  snprintf (Modes.web_root, sizeof(Modes.web_root), "%s\\web_root", Modes.where_am_I);

  snprintf (Modes.aircraft_db, sizeof(Modes.aircraft_db), "%s\\%s", Modes.where_am_I, AIRCRAFT_DATABASE_CSV);
  snprintf (Modes.airport_db, sizeof(Modes.airport_db), "%s\\%s", Modes.where_am_I, AIRPORT_DATABASE_CSV);

  snprintf (Modes.airport_freq_db, sizeof(Modes.airport_freq_db), "%s\\%s", Modes.where_am_I, AIRPORT_FREQ_CSV);
  snprintf (Modes.airport_cache, sizeof(Modes.airport_cache), "%s\\%s", Modes.tmp_dir, AIRPORT_DATABASE_CACHE);

  /* No device selected yet
   */
  Modes.rtlsdr.index  = 0;    /* but the first RTLSDR device found is the default */
  Modes.sdrplay.index = -1;

  /* Defaults for SDRPlay:
   */
  strcpy (Modes.sdrplay.dll_name, "sdrplay_api.dll");  /* Assumed to be on PATH */
  Modes.sdrplay.min_version = SDRPLAY_API_VERSION;     /* = 3.14F */

  Modes.infile_fd        = -1;      /* no --infile */
  Modes.gain_auto        = true;
  Modes.bytes_per_sample = 2;       /* I + Q == 2 bytes */
  Modes.converter_func   = dummy_converter;
  Modes.demod_func       = dummy_demod;
  Modes.sample_rate      = MODES_DEFAULT_RATE;
  Modes.freq             = MODES_DEFAULT_FREQ;
  Modes.interactive_ttl  = MODES_INTERACTIVE_TTL;
  Modes.a_sort           = INTERACTIVE_SORT_NONE;
  Modes.json_interval    = 1000;
  Modes.tui_interface    = TUI_WINCON;
  Modes.min_dist         = 0.0;                /* 0 Km default min distance */
  Modes.max_dist         = 500000.0;           /* 500 Km default max distance */
  Modes.FIFO_acquire_ms  = 100;                /* timeout for `fifo_acquire()` */
  Modes.FIFO_init_bufs   = MODES_MAG_BUFFERS;  /* # of buffers for `fifo_init()` */
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
  bool   write_BOM = false;
  static const BYTE BOM[] = { 0xEF, 0xBB, 0xBF };

  if (!modeS_log_init())
     return;

  /* Write an UTF-8 BOM at the start of the .log file?
   */
  if (_filelength(fileno(Modes.log)) == 0)
     write_BOM = true;

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

  if (write_BOM)
       fwrite (&BOM, sizeof(BOM), 1, Modes.log);
  else fputc ('\n', Modes.log);

  fputs ("---------------------------------------------------------------------------------\n", Modes.log);
  modeS_log (args);
  fputs ("\n\n", Modes.log);
}

/**
 * Initialize hardware stuff **after** to initialising RTLSDR/SDRPlay.
 *
 * Also needed for a remote `rtl_tcp / rtl_tcp2 /rtl_udp` program. <br>
 * A remote connection to `rtl_tcp` will be done in `net_init()` later in main.
 *
 * This function is not called for `--net-only` (i.e. `Modes.net_only == true`)
 * and services like RAW_IN or SBS_IN.
 */
static bool modeS_init_hardware (void)
{
  uint32_t    samplerate = Modes.sample_rate;
  bool        use_rtltcp;
  const char *p;

  if (Modes.net_only || Modes.tests)  /* no need for this */
     return (true);

  /* If `--device tcp://host:port' specified
   */
  p = net_handler_host (MODES_NET_SERVICE_RTL_TCP);
  use_rtltcp = (p && *p);

  switch (samplerate)
  {
    case 2000000:
         Modes.sample_rate = 2000000;
         Modes.demod_func  = demod_2000;
         break;

    case 2400000:
         Modes.sample_rate = 2400000;
         Modes.demod_func  = demod_2400;
         break;

    case 8000000:
         if (Modes.rtlsdr.index >= 0)
         {
           LOG_STDERR ("RTLSDR cannot use 8 MS/s sample-rate.\n");
           return (false);
         }
         if (!demod_8000_alloc())
         {
           LOG_STDERR ("Out of memory allocating `demod_8000()` buffers.\n");
           return (false);
         }
         Modes.sample_rate = 8000000;
         Modes.demod_func  = demod_8000;
         break;

    default:
         LOG_STDERR ("Illegal samplerate %.1lf MS/s selected\n", (double)samplerate / 1E6);
         return (false);
  }

  Modes.trailing_samples = (MODES_PREAMBLE_US + MODES_LONG_MSG_BITS + 16) * 1E-6 * Modes.sample_rate;

  if (Modes.infile[0])
  {
    /* Unless not set, use whatever '--informat' was set to 'Modes.input_format'
     */
    if (Modes.input_format == INPUT_ILLEGAL)
       Modes.input_format = INPUT_UC8;
  }
  else if (Modes.rtlsdr.index >= 0 ||                    /* --device N */
           Modes.rtlsdr.name       ||                    /* --device name */
           use_rtltcp)
  {
    /* A local or remote RTLSDR device
     */
    Modes.input_format = INPUT_UC8;   /* Unsigned, Complex, 8 bit per sample. Always */
    Modes.bytes_per_sample = 2;
  }
  else if (Modes.sdrplay.name || Modes.sdrplay.index >= 0)
  {
    /* A local SDRPlay device
     */
    Modes.input_format = INPUT_SC16;  /* Signed, Complex, 16 bit per sample. Always */
    Modes.bytes_per_sample = 4;
  }

  if (!fifo_init(Modes.FIFO_init_bufs, MODES_MAG_BUF_SAMPLES + Modes.trailing_samples, Modes.trailing_samples))
  {
    LOG_STDERR ("Out of memory allocating FIFO\n");
    return (false);
  }

  Modes.FIFO_active = true;

  Modes.converter_func = convert_init (Modes.input_format,
                                       Modes.sample_rate,
                                       Modes.DC_filter,
                                       Modes.measure_noise,   /* total power is interesting if we want noise */
                                       &Modes.converter_state);
  if (!Modes.converter_func)
  {
    LOG_STDERR ("Can't initialize sample converter for %s\n", Modes.selected_dev);
    return (false);
  }

  if (use_rtltcp)
     Modes.rtltcp.remote = mg_mprintf ("%s://%s:%u",
                                       net_handler_protocol(MODES_NET_SERVICE_RTL_TCP),
                                       net_handler_host(MODES_NET_SERVICE_RTL_TCP),
                                       net_handler_port(MODES_NET_SERVICE_RTL_TCP));

  LOG_FILEONLY ("Modes.rtlsdr.index:     %2d (name: '%s')\n"
                "              Modes.sdrplay.index:    %2d (name: '%s')\n"
                "              Modes.sample_rate:      %.1lf MS/s\n"
                "              Modes.rtltcp.remote:    %s\n"
                "              Modes.selected_dev:     %s\n"
                "              Modes.bytes_per_sample: %d\n"
                "              Modes.trailing_samples: %u\n"
                "              Modes.input_format:     %d / %s\n"
                "              Modes.DC_filter:        %d\n"
                "              Modes.measure_noise:    %d\n"
                "              Modes.phase_enhance:    %d\n"
                "              Modes.demod_func:       demod_%u()\n"
                "              Modes.FIFO_init_bufs:   %u\n"
                "              Modes.FIFO_acquire_ms:  %u\n"
                "              Using converter:        %s(), '%s'\n\n",
                Modes.rtlsdr.index, Modes.rtlsdr.name ? Modes.rtlsdr.name : "<none>",
                Modes.sdrplay.index, Modes.sdrplay.name ? Modes.sdrplay.name : "<none>",
                (double)Modes.sample_rate / 1E6,
                Modes.rtltcp.remote ? Modes.rtltcp.remote : "<n/a>",
                Modes.selected_dev,
                Modes.bytes_per_sample,
                Modes.trailing_samples,
                Modes.input_format, convert_format_name(Modes.input_format),
                Modes.DC_filter,
                Modes.measure_noise,
                Modes.phase_enhance,
                Modes.sample_rate / 1000,
                Modes.FIFO_init_bufs,
                Modes.FIFO_acquire_ms,
                Modes.converter_state->func_name, Modes.converter_state->description);
  return (true);
}

/**
 * Step 2:
 *  \li Initialize the start_time, timezone, DST-adjust and QueryPerformanceFrequency() values.
 *  \li Open and parse `Modes.cfg_file`.
 *  \li Open and append to the `--logfile` if specified.
 *  \li Set the Mongoose log-level based on `--debug m|M`.
 *  \li Check if we have the Aircrafts SQL file.
 *  \li Initialize (and update) the aircrafts structures / files.
 *  \li Initialize (and update) the airports structures / files.
 *  \li Allocate and initialize the needed buffers.
 *  \li Unless `Modes.net_only` is set:
 *  \li   Check and set the sample-rate and correct demodulator function.
 *  \li   Initialize the converter function.
 *  \li   Create the FIFO for enqueueing IQ-data by the `data_thread_fn()`.
 *  \li Setup a SIGINT/SIGBREAK handler for a clean exit.
 */
static bool modeS_init (void)
{
  bool rc = true;

  if (!init_misc())
     return (false);

  if (strcmp(Modes.cfg_file, "NUL") && !cfg_open_and_parse(Modes.cfg_file, config))
     return (false);

  if (Modes.http_ipv6_only)
     Modes.http_ipv6 = true;

  if (Modes.logfile_initial[0] && stricmp(Modes.logfile_initial, "NUL"))
     modeS_init_log();

  if (Modes.speech_enable && !speak_init(0, Modes.speech_volume))
  {
    LOG_FILEONLY ("speak_init(): failed.\n");
    Modes.speech_enable = false;
  }

  /* Command-line options `--samplerate X` and `--max-messages Y`
   * overrides the config-file setting
   */
  if (sample_rate > 0)
     Modes.sample_rate = sample_rate;

  if (max_messages > 0)
     Modes.max_messages = max_messages;

  if (Modes.max_frames > 0)
     Modes.max_messages = Modes.max_frames;

  modeS_log_set();

  crc_init ((int)Modes.error_correct_1 + (int)Modes.error_correct_2);

  if (!aircraft_init())
     return (false);

  if (!airports_init())
     return (false);

  /**
   * Allocate the ICAO address cache. We use two uint32_t for every
   * entry because it's a addr / timestamp pair for every entry.
   */
  Modes.ICAO_cache = calloc (2 * sizeof(uint32_t) * MODES_ICAO_CACHE_LEN, 1);
  Modes.mag_lut    = gen_magnitude_lut();
  Modes.log10_lut  = gen_log10_lut();

  if (!Modes.mag_lut || !Modes.log10_lut || !Modes.ICAO_cache)
  {
    LOG_STDERR ("Out of memory allocating buffers.\n");
    return (false);
  }

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

  if (test_contains(Modes.tests, "net"))
     Modes.net = true;    /* Will force `net_init()` and it's tests to be called */

  if (test_contains(Modes.tests, "cpr"))
     cpr_do_tests();

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
 * \note Not called for a remote RTL_TCP / RTL_UDP device.
 */
static bool modeS_init_RTLSDR (void)
{
  int    i, rc, device_count;
  bool   gain_ok;
  double gain;

  device_count = rtlsdr_get_device_count();
  if (device_count <= 0)
  {
    LOG_STDERR ("No supported RTLSDR devices found. Error: %s\n", get_rtlsdr_error(0));
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
    const char *err = get_rtlsdr_error (rc);

    if (Modes.rtlsdr.name)
         LOG_STDERR ("Error opening the RTLSDR device `%s`: %s\n", Modes.rtlsdr.name, err);
    else LOG_STDERR ("Error opening the RTLSDR device `%d`: %s\n", Modes.rtlsdr.index, err);
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
  if (rc != 0)
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
 * Acquire a FIFO buffer and return the head of list.
 */
static mag_buf *rx_callback_to_fifo (uint32_t in_len, unsigned *to_convert)
{
  static uint32_t dropped = 0;
  uint32_t        samples_read = in_len / Modes.bytes_per_sample;
  uint64_t        block_duration;
  mag_buf        *out_buf;

  out_buf = fifo_acquire (Modes.FIFO_acquire_ms);
  if (!out_buf)
  {
    /* FIFO is full. Drop this block.
     */
    dropped += samples_read;
    return (NULL);
  }

  out_buf->flags = MAGBUF_ZERO;

  /* We previously dropped some samples due to no buffers being available
   */
  if (dropped)
  {
    out_buf->flags   = MAGBUF_DISCONTINUOUS;
    out_buf->dropped = dropped;
  }

  dropped = 0;

  /* Compute the sample timestamp and system timestamp for the start of the block
   */
  out_buf->sample_timestamp = Modes.sample_counter * 12E6 / Modes.sample_rate;
  Modes.sample_counter += samples_read;

  /* Get the approx system time for the start of this block
   */
  block_duration = (1E3 * samples_read) / Modes.sample_rate;
  out_buf->sys_timestamp = MSEC_TIME() - block_duration;

  /* Convert the new data
   */
  *to_convert = samples_read;
  if (*to_convert + out_buf->overlap > out_buf->total_length) /* how did that happen? */
  {
    *to_convert = out_buf->total_length - out_buf->overlap;
    dropped = samples_read - *to_convert;
  }

  out_buf->valid_length = out_buf->overlap + *to_convert;
  return (out_buf);
}

/**
 * This RX-data callback gets data from the local RTLSDR, a remote RTLSDR
 * device or a local SDRplay device asynchronously.
 *
 * We then allocate a FIFO-buffer, call the "IQ to magnitude" converter function and
 * depending on sample-rate call the correct `Modes.demod_func` function.
 * ADS-B is all about pulse-power; hence "Pulse Position Modulation" decoding
 * depends highly on sample-rate.
 *
 * \note A Mutex is used to avoid race-condition with the decoding thread.
 */
void rx_callback (uint8_t *in_buf, uint32_t in_len, void *ctx)
{
  volatile bool exit = *(volatile bool*) ctx;
  mag_buf      *out_buf;
  unsigned      to_convert = 0;    /* samples to convert */
  uint32_t      samples_read = in_len / Modes.bytes_per_sample;

  if (exit)
  {
    if (Modes.rtlsdr.device)
       rtlsdr_cancel_async (Modes.rtlsdr.device);    /* ask our caller to exit */
    SleepEx (100, TRUE);
    return;
  }

  if (samples_read == 0)
     return;

  out_buf = rx_callback_to_fifo (in_len, &to_convert);
  if (out_buf)
  {
    /**
     * Convert the raw I/Q `in_buf` data to a "magnitude-squared"
     * buffer into `out_buf->data [ofs]`. The main-thread will pick up
     * this buffer by `fifo_dequeue()` and use it to feed the selected
     * `Modes.demod_func()` function. \ref demod.h
     */
    fifo_enqueue (out_buf);

    /**
     * Since `sizeof(*out_buf->overlap) == Modes.bytes_per_sample`,
     * this is valid pointer arithmetics. But what about SDRPlay?
     */
    out_buf->valid_length = out_buf->overlap + to_convert;

    (*Modes.converter_func) (in_buf, &out_buf->data [out_buf->overlap], to_convert,
                             Modes.converter_state, &out_buf->mean_power);
  }
}

/**
 * We read RTLSDR, SDRplay or remote RTL_TCP data using a separate thread,
 * so the main thread only handles decoding without caring about data acquisition.
 *
 * \ref `main_data_loop()` below.
 */
static DWORD WINAPI data_thread_fn (void *arg)
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

    LOG_STDERR ("rtlsdr_read_async(): rc: %d/%s\n", rc, get_rtlsdr_error(rc));
    modeS_signal_handler (0);    /* break out of main_data_loop() */
  }
  else if (Modes.rtl_tcp_in || Modes.raw_in)
  {
    while (!Modes.exit)
    {
     /* Not much to do here. For RTL_TCP, enqueueing to the FIFO is
      * done in `rx_callback()` via `rtl_tcp_recv_data()` in net_io.c.
      * For RAW_IN, everything runs out of `background_tasks()`.
      */
      Sleep (100);
    }
  }

  Modes.exit = true;   /* just in case */
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

    if (!Modes.FIFO_active)
    {
      Sleep (100);
    }
    else
    {
     /* Wait max. 100 msec for a magnitude buffer
      */
      mag_buf *buf = fifo_dequeue (100);

      if (!buf)
         continue;

      (*Modes.demod_func) (buf);   /* call `demod_2000()` etc. */

      Modes.stat.samples_processed += (buf->valid_length - buf->overlap) / Modes.bytes_per_sample;
      Modes.stat.samples_dropped   += buf->dropped / Modes.bytes_per_sample;

      fifo_release (buf);
    }

    /* Have we shown enough messages?
     */
    if (Modes.max_messages > 0 &&
        Modes.stat.messages_shown >= Modes.max_messages)
    {
      LOG_STDOUT ("Reached 'Modes.max_messages'.\n");
      Modes.exit = true;
    }
  }

  if (Modes.FIFO_active)
     fifo_halt();

  /* Wait on reader thread exit
   */
  WaitForSingleObject (Modes.reader_thread, INFINITE);
}

/**
 * Given the Downlink Format (DF) of the message, return the
 * message length in bits.
 */
int modeS_message_len_by_type (int type)
{
  if (type == 16 || type == 17 || type == 19 || type == 20 || type == 21)
     return (MODES_LONG_MSG_BITS);
  return (MODES_SHORT_MSG_BITS);
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
  uint32_t h_idx = ICAO_cache_hash_address (addr);

  Modes.ICAO_cache [2*h_idx]     = addr;
  Modes.ICAO_cache [2*h_idx + 1] = (uint32_t) time (NULL);
}

/**
 * Returns true if the specified ICAO address was seen in a DFx format with
 * proper checksum (not XORed with address) no more than
 * `MODES_ICAO_CACHE_TTL` seconds ago.
 * Otherwise returns false.
 */
static bool ICAO_address_recently_seen (uint32_t a)
{
  uint32_t h_idx = ICAO_cache_hash_address (a);
  uint32_t addr  = Modes.ICAO_cache [2*h_idx];
  uint32_t seen  = Modes.ICAO_cache [2*h_idx + 1];

  return (addr && addr == a && (time(NULL) - seen) <= MODES_ICAO_CACHE_TTL);
}

#define icao_filter_test(addr) ICAO_address_recently_seen (addr)
#define icao_filter_add(addr)  ICAO_cache_add_address (addr)

/**
 * In the squawk (identity) field bits are interleaved as follows in
 * (message bit 20 to bit 32):
 *
 * C1-A1-C2-A2-C4-A4-ZERO-B1-D1-B2-D2-B4-D4
 *
 * So every group of three bits A, B, C, D represent an integer from 0 to 7.
 *
 * The actual meaning is just 4 octal numbers, but we convert it into a hex
 * number that happens to represent the four octal numbers.
 *
 * For more info: http://en.wikipedia.org/wiki/Gillham_code
 */
static int decode_ID13_field (int ID13_field)
{
  int hex_gillham = 0;

  if (ID13_field & 0x1000) hex_gillham |= 0x0010;     /* Bit 12 = C1 */
  if (ID13_field & 0x0800) hex_gillham |= 0x1000;     /* Bit 11 = A1 */
  if (ID13_field & 0x0400) hex_gillham |= 0x0020;     /* Bit 10 = C2 */
  if (ID13_field & 0x0200) hex_gillham |= 0x2000;     /* Bit  9 = A2 */
  if (ID13_field & 0x0100) hex_gillham |= 0x0040;     /* Bit  8 = C4 */
  if (ID13_field & 0x0080) hex_gillham |= 0x4000;     /* Bit  7 = A4 */
/*if (ID13_field & 0x0040) hex_gillham |= 0x0800; */  /* Bit  6 = X  or M */
  if (ID13_field & 0x0020) hex_gillham |= 0x0100;     /* Bit  5 = B1 */
  if (ID13_field & 0x0010) hex_gillham |= 0x0001;     /* Bit  4 = D1 or Q */
  if (ID13_field & 0x0008) hex_gillham |= 0x0200;     /* Bit  3 = B2 */
  if (ID13_field & 0x0004) hex_gillham |= 0x0002;     /* Bit  2 = D2 */
  if (ID13_field & 0x0002) hex_gillham |= 0x0400;     /* Bit  1 = B4 */
  if (ID13_field & 0x0001) hex_gillham |= 0x0004;     /* Bit  0 = D4 */

  return (hex_gillham);
}

#define INVALID_ALTITUDE (-9999)

/**
 * Decode the 13 bit AC altitude field (in DF20 and others).
 *
 * \param in  msg   the raw message to work with.
 * \param out unit  set to either `MODES_UNIT_METERS` or `MODES_UNIT_FEETS`.
 * \retval the altitude.
 */
static int decode_AC13_field (int AC13_field, metric_unit_t *unit)
{
  int m_bit = AC13_field & 0x0040;  /* set = meters, clear = feet */
  int q_bit = AC13_field & 0x0010;  /* set = 25 ft encoding, clear = Gillham Mode C encoding */
  int n;

  if (!m_bit)
  {
    *unit = MODES_UNIT_FEET;
    if (q_bit)
    {
      /* N is the 11 bit integer resulting from the removal of bit Q and M
       */
      n = ((AC13_field & 0x1F80) >> 2) |
          ((AC13_field & 0x0020) >> 1) |
          (AC13_field & 0x000F);

      /* The final altitude is resulting number multiplied by 25, minus 1000.
       */
      return ((n * 25) - 1000);
    }

    /* N is an 11 bit Gillham coded altitude
     */
    n = mode_A_to_mode_C (decode_ID13_field(AC13_field));
    if (n < -12)
       return (INVALID_ALTITUDE);
    return (100 * n);
  }

  *unit = MODES_UNIT_METERS;

  /**< \todo Implement altitude when meter unit is selected
   */
  return (INVALID_ALTITUDE);
}

/**
 * Decode the 12 bit AC altitude field (in DF17 and others).
 * Returns the altitude or 0 if it can't be decoded.
 */
static int decode_AC12_field (int AC12_field, metric_unit_t *unit)
{
  int ret, n;
  int q_bit = AC12_field & 0x10;

  *unit = MODES_UNIT_FEET;
  if (q_bit)
  {
    /* N is the 11 bit integer resulting from the removal of bit Q at bit 4
     */
    n = ((AC12_field & 0x0FE0) >> 1) | (AC12_field & 0x000F);

    /* The final altitude is the resulting number multiplied by 25, minus 1000.
     */
    ret = (n * 25) - 1000;
    return (ret);
  }

  /* Make N a 13 bit Gillham coded altitude by inserting M=0 at bit 6
   */
  n = ((AC12_field & 0x0FC0) << 1) | (AC12_field & 0x003F);
  n = mode_A_to_mode_C (decode_ID13_field(n));
  if (n < -12)
     return (INVALID_ALTITUDE);

  ret = 100 * n;
  return (ret);
}

/**
 * Decode the 7 bit ground movement field PWL exponential style scale
 */
static int decode_movement_field (int movement)
{
  int gspeed;

  if (movement > 123)
     gspeed = 199;  /* > 175kt */

  else if (movement > 108)
     gspeed = ((movement - 108) * 5) + 100;

  else if (movement > 93)
     gspeed = ((movement - 93) * 2) + 70;

  else if (movement > 38)
     gspeed = ((movement - 38)) + 15;

  else if (movement > 12)
     gspeed = ((movement - 11) >> 1) + 2;

  else if (movement > 8)
     gspeed = ((movement - 6) >> 2) + 1;

  else
     gspeed = 0;

  return (gspeed);
}

#if 0 /**\ todo Add this? */
/**
 * Extract one bit from a message.
 */
static __inline unsigned get_bit (const uint8_t *data, unsigned bitnum)
{
  unsigned bi = bitnum - 1;
  unsigned by = bi >> 3;
  unsigned mask = 1 << (7 - (bi & 7));

  return (data[by] & mask) != 0;
}

/**
 * Extract some bits  (first_bit .. last_bit inclusive) from a message.
 */
static __inline unsigned get_bits (const uint8_t *data, unsigned first_bit, unsigned last_bit)
{
  unsigned num_bits   = (last_bit - first_bit + 1);
  unsigned first_bi   = first_bit - 1;
  unsigned last_bi    = last_bit - 1;
  unsigned first_byte = first_bi >> 3;
  unsigned last_byte  = last_bi >> 3;
  unsigned num_bytes  = (last_byte - first_byte) + 1;
  unsigned shift      = 7 - (last_bi & 7);
  unsigned top_mask   = 0xFF >> (first_bi & 7);

  assert (first_bi <= last_bi);
  assert (num_bits <= 32);
  assert (num_bytes <= 5);

  if (num_bytes == 5)
     return ((data[first_byte] & top_mask) << (32 - shift)) |
            (data[first_byte + 1] << (24 - shift)) |
            (data[first_byte + 2] << (16 - shift)) |
            (data[first_byte + 3] << (8 - shift)) |
            (data[first_byte + 4] >> shift);

  if (num_bytes == 4)
     return ((data[first_byte] & top_mask) << (24 - shift)) |
            (data[first_byte + 1] << (16 - shift)) |
            (data[first_byte + 2] << (8 - shift)) |
            (data[first_byte + 3] >> shift);

  if (num_bytes == 3)
     return ((data[first_byte] & top_mask) << (16 - shift)) |
            (data[first_byte + 1] << (8 - shift)) |
            (data[first_byte + 2] >> shift);

  if (num_bytes == 2)
     return ((data[first_byte] & top_mask) << (8 - shift)) | (data[first_byte + 1] >> shift);

  if (num_bytes == 1)
     return (data[first_byte] & top_mask) >> shift;

  return 0;
}

/**
 * Handle setting a non-ICAO address
 */
static void set_IMF (struct modeS_message *mm)
{
  mm->addr |= MODES_NON_ICAO_ADDRESS;
  switch (mm->addrtype)
  {
    case ADDR_ADSB_ICAO:
    case ADDR_ADSB_ICAO_NT:
         mm->addrtype = ADDR_ADSB_OTHER;
         break;

    case ADDR_TISB_ICAO:
         mm->addrtype = ADDR_TISB_TRACKFILE;
         break;

    case ADDR_ADSR_ICAO:
         mm->addrtype = ADDR_ADSR_OTHER;
         break;

    default:
        break;
  }
}

static void decode_ES_airborne_velocity (modeS_message *mm, bool check_imf)
{
  uint8_t *me = mm->ME;
  uint16_t ew_raw, ns_raw, airspeed;

  /* 1-5: ME type */
  /* 6-8: ME subtype */
  mm->ME_subtype = get_bits (me, 6, 8);

  if (mm->ME_subtype < 1 || mm->ME_subtype > 4)
     return;

  /* 9: IMF or Intent Change */
  if (check_imf && get_bit(me, 9))
     set_IMF (mm);

  /* 10: reserved */
  /* 11-13: NACv (NUCr in v0, maps directly to NACv in v2) */
  mm->accuracy.nac_v_valid = 1;
  mm->accuracy.nac_v = get_bits (me, 11, 13);

  /* 14-35: speed/velocity depending on subtype
   */
  switch (mm->ME_subtype)
  {
    case 1:
    case 2:
         /* 14:    E/W direction */
         /* 15-24: E/W speed */
         /* 25:    N/S direction */
         /* 26-35: N/S speed */
         ew_raw = get_bits (me, 15, 24);
         ns_raw = get_bits (me, 26, 35);

         if (ew_raw && ns_raw)
         {
           int ew_vel = (ew_raw - 1) * (get_bit(me, 14) ? -1 : 1) * ((mm->ME_subtype == 2) ? 4 : 1);
           int ns_vel = (ns_raw - 1) * (get_bit(me, 25) ? -1 : 1) * ((mm->ME_subtype == 2) ? 4 : 1);

           /* Compute velocity and angle from the two speed components
            */
           mm->gs.v0 = mm->gs.v2 = mm->gs.selected = sqrtf ((ns_vel * ns_vel) + (ew_vel * ew_vel) + 0.5);
           mm->gs_valid = 1;

           if (mm->gs.selected > 0)
           {
             double ground_track = (atan2 (ew_vel, ns_vel) * 180.0) / M_PI;

             /* We don't want negative values but a 0-360 scale
              */
             if (ground_track < 0.0)
                ground_track += 360.0;
             mm->heading       = ground_track;
             mm->heading_type  = HEADING_GROUND_TRACK;
             mm->heading_valid = 1;
           }
         }
         break;

    case 3:
    case 4:
         /* 14:    heading status */
         /* 15-24: heading */
         if (get_bit(me, 14))
         {
           mm->heading_valid = 1;
           mm->heading = get_bits (me, 15, 24) * 360.0 / 1024.0;
           mm->heading_type = HEADING_MAGNETIC_OR_TRUE;
         }

         /* 25: airspeed type */
         /* 26-35: airspeed */
         airspeed = get_bits (me, 26, 35);
         if (airspeed)
         {
             unsigned speed = (airspeed - 1) * (mm->ME_subtype == 4 ? 4 : 1);

             if (get_bit(me, 25))
             {
               mm->tas_valid = 1;
               mm->tas = speed;
             }
             else
             {
               mm->ias_valid = 1;
               mm->ias = speed;
             }
         }
         break;
  }

  /* 36: vert rate source */
  /* 37: vert rate sign */
  /* 38-46: vert rate magnitude */
  unsigned vert_rate         = get_bits (me, 38, 46);
  unsigned vert_rate_is_baro = get_bit (me, 36);

  if (vert_rate)
  {
    int rate = (vert_rate - 1) * (get_bit(me, 37) ? -64 : 64);

    if (vert_rate_is_baro)
    {
      mm->baro_rate = rate;
      mm->baro_rate_valid = 1;
    }
    else
    {
      mm->geom_rate = rate;
      mm->geom_rate_valid = 1;
    }
  }

  /* 47-48: reserved */

  /* 49: baro/geom delta sign */
  /* 50-56: baro/geom delta magnitude */
  unsigned raw_delta = get_bits (me, 50, 56);

  if (raw_delta)
  {
    mm->geom_delta_valid = 1;
    mm->geom_delta = (raw_delta - 1) * (get_bit(me, 49) ? -25 : 25);
  }
}

/**
 * Decode DF16 message?
 * \sa https://github.com/e5150/msdec/blob/master/df16.c
 */
static void decode_DF16 (modeS_message *mm)
{
}
#endif // 0

static bool set_callsign (modeS_message *mm, uint32_t chars1, uint32_t chars2)
{
  static const char *AIS_charset = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?";
  static const char *AIS_junk    = "[\\]^_!\"#$%&'()*+,-./:;<=>?";

  char flight [sizeof(mm->flight)];
  bool valid;

  /* A common failure mode seems to be to intermittently send
   * all zeros. Catch that here.
   */
  if (chars1 == 0 && chars2 == 0)
     return (false);

  flight [3] = AIS_charset [chars1 & 0x3F]; chars1 = chars1 >> 6;
  flight [2] = AIS_charset [chars1 & 0x3F]; chars1 = chars1 >> 6;
  flight [1] = AIS_charset [chars1 & 0x3F]; chars1 = chars1 >> 6;
  flight [0] = AIS_charset [chars1 & 0x3F];

  flight [7] = AIS_charset [chars2 & 0x3F]; chars2 = chars2 >> 6;
  flight [6] = AIS_charset [chars2 & 0x3F]; chars2 = chars2 >> 6;
  flight [5] = AIS_charset [chars2 & 0x3F]; chars2 = chars2 >> 6;
  flight [4] = AIS_charset [chars2 & 0x3F];
  flight [8] = '\0';

  valid = (strpbrk(flight, AIS_junk) == NULL);
  if (valid)
  {
    mm->AC_flags |= MODES_ACFLAGS_CALLSIGN_VALID;
    strcpy (mm->flight, str_trim(flight));
  }
  return (valid);
}

/**
 * Decode BDS2,0 carried in Comm-B or ES
 */
static void decode_BDS20 (modeS_message *mm)
{
  uint8_t *msg = mm->msg;
  uint32_t chars1 = (msg[5] << 16) | (msg[6] << 8) | (msg[7]);
  uint32_t chars2 = (msg[8] << 16) | (msg[9] << 8) | (msg[10]);

  set_callsign (mm, chars1, chars2);
}

static void decode_comm_B (modeS_message *mm)
{
  uint8_t *msg = mm->msg;

  /* This is a bit hairy as we don't know what the requested register was
   */
  if (msg[4] == 0x20)  /* BDS 2,0 Aircraft Identification */
     decode_BDS20 (mm);
}

static void decode_extended_squitter (modeS_message *mm)
{
  uint8_t *msg = mm->msg;
  int      ME_type, ME_subtype;
  bool     check_imf = false;

  memcpy (mm->ME, &msg[4], sizeof(mm->ME));

  /* Extended squitter message type
   */
  ME_type = mm->ME_type = (msg[4] >> 3);

  /* Extended squitter message subtype
   */
  ME_subtype = mm->ME_subtype = (ME_type == 29 ? ((msg[4] & 6) >> 1) : (msg[4] & 7));

  /* Check CF on DF18 to work out the format of the ES and whether we need to look for an IMF bit
   */
  if (mm->msg_type == 18)
  {
    switch (mm->cf)
    {
      case 0:      /* ADS-B ES/NT devices that report the ICAO 24-bit address in the AA field */
           mm->addrtype = ADDR_ADSB_ICAO_NT;
           break;

      case 1:      /* Reserved for ADS-B for ES/NT devices that use other addressing techniques in the AA field */
           mm->addr |= MODES_NON_ICAO_ADDRESS;
           mm->addrtype = ADDR_ADSB_OTHER;
           break;

      case 2:      /* Fine TIS-B message (formats are close enough to DF17 for our purposes) */
           mm->AC_flags |= MODES_ACFLAGS_FROM_TISB;
           mm->addrtype = ADDR_TISB_ICAO;
           check_imf = true;
           break;

      case 3:      /* Coarse TIS-B airborne position and velocity. */
           /* \todo decode me.
            * For now we only look at the IMF bit.
            */
           mm->AC_flags |= MODES_ACFLAGS_FROM_TISB;
           mm->addrtype = ADDR_TISB_ICAO;
           if (msg[4] & 0x80)
              mm->addr |= MODES_NON_ICAO_ADDRESS;
           return;

      case 5:      /* TIS-B messages that relay ADS-B Messages using anonymous 24-bit addresses (format not explicitly defined, but it seems to follow DF17) */
           mm->AC_flags |= MODES_ACFLAGS_FROM_TISB;
           mm->addr |= MODES_NON_ICAO_ADDRESS;
           mm->addrtype = ADDR_TISB_OTHER;
           break;

      case 6:      /* ADS-B rebroadcast using the same type codes and message formats as defined for DF = 17 ADS-B messages */
           mm->addrtype = ADDR_ADSR_ICAO;
           check_imf = true;
           break;

      default:     /* All others, we don't know the format */
           mm->addr |= MODES_NON_ICAO_ADDRESS;    /* assume non-ICAO */
           mm->addrtype = ADDR_UNKNOWN;
           return;
    }
  }

  uint32_t chars1, chars2;
  int      vert_rate, airspeed;
  int      movement, AC12_field, ID13_field;
  int      ew_raw, ew_vel, ns_raw, ns_vel;

  switch (ME_type)
  {
    case 1:
    case 2:
    case 3:
    case 4:
         /* Aircraft Identification and Category
          */
         chars1 = (msg[5] << 16) | (msg[6] << 8) | (msg[7]);
         chars2 = (msg[8] << 16) | (msg[9] << 8) | (msg[10]);

         set_callsign (mm, chars1, chars2);
         mm->category = ((0x0E - ME_type) << 4) | ME_subtype;
         mm->AC_flags |= MODES_ACFLAGS_CATEGORY_VALID;
         break;

    case 19:   /* Airborne Velocity Message */
#if 0
         decode_ES_airborne_velocity (mm, check_imf);    /** \todo */
#else
         if (check_imf && (msg[5] & 0x80))
            mm->addr |= MODES_NON_ICAO_ADDRESS;

         /* Presumably airborne if we get an Airborne Velocity Message */
         mm->AC_flags |= MODES_ACFLAGS_AOG_VALID;

         if (ME_subtype >= 1 && ME_subtype <= 4)
         {
           vert_rate = ((msg[8] & 0x07) << 6) | (msg[9] >> 2);
           if (vert_rate)
           {
             vert_rate--;
             if (msg[8] & 0x08)
                vert_rate = 0 - vert_rate;
             mm->vert_rate =  vert_rate * 64;
             mm->AC_flags |= MODES_ACFLAGS_VERTRATE_VALID;
           }
         }

         if (ME_subtype == 1 || ME_subtype == 2)
         {
           ew_raw = ((msg[5] & 0x03) << 8) | msg[6];
           ew_vel = ew_raw - 1;
           ns_raw = ((msg[7] & 0x7F) << 3) | (msg[8] >> 5);
           ns_vel = ns_raw - 1;

           if (ME_subtype == 2)   /* If (supersonic) unit is 4 kts */
           {
             ns_vel = ns_vel << 2;
             ew_vel = ew_vel << 2;
           }
           if (ew_raw)            /* Do East/West */
           {
             mm->AC_flags |= MODES_ACFLAGS_EWSPEED_VALID;
             if (msg[5] & 0x04)
                ew_vel = 0 - ew_vel;    /* Flying west */
             mm->EW_velocity = ew_vel;
             mm->EW_dir      = (msg[5] & 4) >> 2;
           }
           if (ns_raw)            /* Do North/South */
           {
             mm->AC_flags |= MODES_ACFLAGS_NSSPEED_VALID;
             if (msg[7] & 0x80)   /* Flying south */
                ns_vel = 0 - ns_vel;
             mm->NS_velocity = ns_vel;
             mm->NS_dir      = (msg[7] & 0x80) >> 7;
           }

           if (ew_raw && ns_raw)
           {
             /* Compute velocity and angle from the two speed components
              */
             mm->AC_flags |= (MODES_ACFLAGS_SPEED_VALID | MODES_ACFLAGS_HEADING_VALID | MODES_ACFLAGS_NSEWSPD_VALID);
             mm->velocity = sqrt ((ns_vel * ns_vel) + (ew_vel * ew_vel) + 0.5);
             if (mm->velocity > SMALL_VAL)
             {
               mm->heading = atan2 (ew_vel, ns_vel) * 180.0 / M_PI;

               /* We don't want negative values but a 0 - 360 scale
                */
               if (mm->heading < 0.0)
                  mm->heading += 360.0;
             }
           }
         }
         else if (ME_subtype == 3 || ME_subtype == 4)
         {
           airspeed = ((msg[7] & 0x7f) << 3) | (msg[8] >> 5);
           if (airspeed)
           {
             mm->AC_flags |= MODES_ACFLAGS_SPEED_VALID;
             airspeed--;
             if (ME_subtype == 4)  /* If (supersonic) unit is 4 kts */
                airspeed = airspeed << 2;
             mm->velocity =  airspeed;
           }
           if (msg[5] & 0x04)
           {
             mm->AC_flags |= MODES_ACFLAGS_HEADING_VALID;
             mm->heading = ((((msg[5] & 0x03) << 8) | msg[6]) * 45) >> 7;
           }
         }
         if (msg[10] != 0)
         {
           mm->AC_flags |= MODES_ACFLAGS_HAE_DELTA_VALID;
           mm->HAE_delta = ((msg[10] & 0x80) ? -25 : 25) * ((msg[10] & 0x7f) - 1);
         }
#endif
         break;

    case 5:
    case 6:
    case 7:
    case 8:
         /* Ground position */
#if 0
         decode_ES_surface_position (mm, check_imf);    /** \todo */
#else
         if (check_imf && (msg[6] & 0x08))
            mm->addr |= MODES_NON_ICAO_ADDRESS;

         mm->AC_flags |= MODES_ACFLAGS_AOG_VALID | MODES_ACFLAGS_AOG;
         mm->raw_latitude  = ((msg[6] & 3) << 15) | (msg[7] << 7) | (msg[8] >> 1);
         mm->raw_longitude = ((msg[8] & 1) << 16) | (msg[9] << 8) | (msg[10]);
         mm->AC_flags |= (mm->msg[6] & 0x04) ? MODES_ACFLAGS_LLODD_VALID : MODES_ACFLAGS_LLEVEN_VALID;

         movement = ((msg[4] << 4) | (msg[5] >> 4)) & 0x007F;
         if (movement && movement < 125)
         {
           mm->AC_flags |= MODES_ACFLAGS_SPEED_VALID;
           mm->velocity = decode_movement_field (movement);
         }
         if (msg[5] & 0x08)
         {
           mm->AC_flags |= MODES_ACFLAGS_HEADING_VALID;
           mm->heading = ((((msg[5] << 4) | (msg[6] >> 4)) & 0x007F) * 45) >> 4;
         }
         mm->nuc_p = (14 - ME_type);
#endif
         break;

    case 0:                                        /* Airborne position, baro altitude only  */
    case 9:  case 10: case 11: case 12: case 13:   /* Airborne position, baro */
    case 14: case 15: case 16: case 17: case 18:
    case 20: case 21: case 22:                     /* Airborne position, geometric altitude (HAE or MSL) */
#if 0
         decode_ES_airborne_position (mm, check_imf);    /** \todo */
#else

         AC12_field = ((msg[5] << 4) | (msg[6] >> 4)) & 0x0FFF;

         if (check_imf && (msg[4] & 0x01))
            mm->addr |= MODES_NON_ICAO_ADDRESS;

         mm->AC_flags |= MODES_ACFLAGS_AOG_VALID;

         if (ME_type != 0)
         {
           /* Catch some common failure modes and don't mark them as valid
            * (so they won't be used for positioning)
            */
           mm->raw_latitude  = ((msg[6] & 3) << 15) | (msg[7] << 7) | (msg[8] >> 1);
           mm->raw_longitude = ((msg[8] & 1) << 16) | (msg[9] << 8) | (msg[10]);

           if (AC12_field == 0 && mm->raw_longitude == 0 && (mm->raw_latitude & 0x0FFF) == 0 && mm->ME_type == 15)
           {
             /* Seen from at least:
              *   400F3F (Eurocopter ECC155 B1) - Bristow Helicopters
              *   4008F3 (BAE ATP) - Atlantic Airlines
              *   400648 (BAE ATP) - Atlantic Airlines
              * altitude == 0, longitude == 0, type == 15 and zeros in latitude LSB.
              * Can alternate with valid reports having type == 14
              */
             Modes.stat.cpr_filtered++;
           }
           else
           {
             /* Otherwise, assume it's valid
              */
             mm->AC_flags |= (mm->msg[6] & 0x04) ? MODES_ACFLAGS_LLODD_VALID : MODES_ACFLAGS_LLEVEN_VALID;
           }
         }

         if (AC12_field) /* Only attempt to decode if a valid (non zero) altitude is present */
         {
           if (ME_type == 20 || ME_type == 21 || ME_type == 22)
           {
             /* Position reported as HAE
              */
             mm->altitude_HAE = decode_AC12_field (AC12_field, &mm->unit);
             if (mm->altitude_HAE != INVALID_ALTITUDE)
                mm->AC_flags |= MODES_ACFLAGS_ALTITUDE_HAE_VALID;

           }
           else
           {
             mm->altitude = decode_AC12_field (AC12_field, &mm->unit);
             if (mm->altitude != INVALID_ALTITUDE)
                mm->AC_flags |= MODES_ACFLAGS_ALTITUDE_VALID;
           }
         }

         if (ME_type == 0 || ME_type == 18 || ME_type == 22)
              mm->nuc_p = 0;
         else if (ME_type < 18)
              mm->nuc_p = (18 - ME_type);
         else mm->nuc_p = (29 - ME_type);
#endif
         break;

    case 23:   /* Test message */
#if 0
         decode_ES_test_message (mm);        /** \todo */
#else
         if (ME_subtype == 7)                /* (see 1090-WP-15-20) */
         {
           ID13_field = (((msg[5] << 8) | msg[6]) & 0xFFF1) >> 3;
           if (ID13_field)
           {
             mm->AC_flags |= MODES_ACFLAGS_SQUAWK_VALID;
             mm->identity = decode_ID13_field (ID13_field);
           }
         }
#endif
         break;

    case 24:   /* Reserved for Surface System Status */
         break;

    case 28:   /* Extended Squitter Aircraft Status */
#if 0
         decode_ES_aircraft_status (mm, check_imf);   /** \todo */
#else
         if (ME_subtype == 1)       /* Emergency status squawk field */
         {
           ID13_field = (((msg[5] << 8) | msg[6]) & 0x1FFF);
           if (ID13_field)
           {
             mm->AC_flags |= MODES_ACFLAGS_SQUAWK_VALID;
             mm->identity = decode_ID13_field (ID13_field);
           }
           if (check_imf && (msg[10] & 0x01))
              mm->addr |= MODES_NON_ICAO_ADDRESS;
         }
#endif
         break;

    case 29:   /* Aircraft Trajectory Intent */
#if 0
         decode_ES_target_status (mm, check_imf);   /** \todo */
#endif
         break;

    case 30:   /* Aircraft Operational Coordination */
         break;

    case 31:   /* Aircraft Operational Status */
#if 0
         decode_ES_operational_status (mm, check_imf);  /** \todo */
#else
         if (check_imf && (msg[10] & 0x01))
            mm->addr |= MODES_NON_ICAO_ADDRESS;
#endif
         break;

    default:
        mm->reliable = false;
        add_unrecognized_ME (ME_type, ME_subtype, false);
        break;
  }
}

/**
 * Capability table.
 */
static const char *capability_str [8] = {
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
static const char *flight_status_str [8] = {
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
static const char *emerg_state_str [8] = {
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

  if (mm->ME_type == 30)
     return ("Aircraft Operational Coordination");

  if (mm->ME_type == 31 && mm->ME_subtype == 0)
     return ("Aircraft Operational Status (airborne)");

  if (mm->ME_type == 31 && mm->ME_subtype == 1)
     return ("Aircraft Operational Status (surface)");

  snprintf (buf, sizeof(buf), "Unknown: %d/%d", mm->ME_type, mm->ME_subtype);
  return (buf);
}

/**
 * Decode a raw Mode S message demodulated as a stream of bytes by
 * `demod_2000()`, `demod_2400()`, `demod_8000()`, .CSV-infile or a remote
 * RAW-IN network service via `decode_RAW_message()`.
 *
 * Split it into fields populating a `modeS_message` structure.
 *
 * \retval 0   on success
 * \retval < 0 on failure
 */
static int _decode_mode_S_message (modeS_message *mm, const uint8_t *_msg)
{
  uint8_t   *msg;
  errorinfo *ei;
  uint32_t   addr, addr1, addr2;

#if 0
  memset (mm, '\0', sizeof(*mm));
#else
  /*
   * Do not clear the whole `mm` structure.
   * Preserve the time-stamps.
   */
  #define MEMSET_MM_OFS  (sizeof(mm->timestamp_msg) + sizeof(mm->sys_timestamp_msg))
  memset ((char*)mm + MEMSET_MM_OFS, '\0', sizeof(*mm) - MEMSET_MM_OFS);
#endif

  /* Work on our local copy
   */
  memcpy (mm->msg, _msg, sizeof(mm->msg));
  msg = mm->msg;

  mm->AC_flags = 0;

  /* Get the message type ASAP as other operations depend on this
   */
  mm->msg_type = msg[0] >> 3;    /* Downlink Format */
  mm->msg_bits = modeS_message_len_by_type (mm->msg_type);
  mm->CRC      = crc_checksum (msg, mm->msg_bits);

  /* Check CRC and fix single bit errors using the CRC when
   * possible (DF11 and 17).
   */
  mm->error_bits = 0;
  mm->addr       = 0;

  /* Do checksum work and set fields that depend on the CRC
   */
  switch (mm->msg_type)
  {
    case 0:  /* Short air-air surveillance */
    case 4:  /* Surveillance, altitude reply */
    case 5:  /* Surveillance, altitude reply */
    case 16: /* Long air-air surveillance */
         /* These message types use Address/Parity, i.e. our CRC syndrome is the sender's ICAO address.
          * We can't tell if the CRC is correct or not as we don't know the correct address.
          * Accept the message if it appears to be from a previously-seen aircraft
          */
         if (!icao_filter_test(mm->CRC))
            return (-1);

         mm->addr = mm->CRC;
         mm->reliable = false;
         break;

    case 11: /* All-call reply */
         /* This message type uses Parity/Interrogator, i.e. our CRC syndrome is CL + IC from
          * the uplink message which we can't see. So we don't know if the CRC is correct or not.
          *
          * However! CL + IC only occupy the lower 7 bits of the CRC. So if we ignore those bits when testing
          * the CRC we can still try to detect/correct errors.
          */
         mm->IID = mm->CRC & 0x7F;
         if (mm->CRC & 0xFFFF80)
         {
           ei = crc_checksum_diagnose (mm->CRC & 0xFFFF80, mm->msg_bits);
           if (!ei)
              return (-2);   /* couldn't fix it */

           /* See crc.c comments: we do not attempt to fix more than single-bit errors,
            * as two-bit errors are ambiguous in DF11.
            */
           if (ei->errors > 1)
              return (-2);   /* can't correct errors */

           mm->error_bits = ei->errors;
           crc_checksum_fix (msg, ei);

           mm->reliable = (mm->IID == 0 && mm->error_bits == 0);

           /* Check whether the corrected message looks sensible
            * we are conservative here: only accept corrected messages that
            * match an existing aircraft.
            */
           addr = AIRCRAFT_GET_ADDR (msg + 1);
           if (!icao_filter_test(addr))
              return (-1);
         }
         break;

    case 17:   /* Extended squitter */
    case 18:   /* Extended squitter/non-transponder */
         mm->reliable = (mm->error_bits == 0);
         if (mm->CRC == 0)
            break;  /* all good */

         ei = crc_checksum_diagnose (mm->CRC, mm->msg_bits);
         if (!ei)
            return (-2); /* couldn't fix it */

         addr1 = AIRCRAFT_GET_ADDR (msg + 1);
         mm->error_bits = ei->errors;
         crc_checksum_fix (msg, ei);
         addr2 = AIRCRAFT_GET_ADDR (msg + 1);

         /* We are conservative here: only accept corrected messages that
          * match an existing aircraft.
          */
         if (addr1 != addr2 && !icao_filter_test(addr2))
            return (-1);
         break;

    case 20: /* Comm-B, altitude reply */
    case 21: /* Comm-B, identity reply */
         /* These message types either use Address/Parity (see DF0 etc)
          * or Data Parity where the requested BDS is also XORed into the top byte.
          * So not only do we not know whether the CRC is right, we also don't know if
          * the ICAO is right! Ow.
          */
         if (icao_filter_test(mm->CRC)) /* Try an exact match */
         {
           mm->addr = mm->CRC;
           mm->BDS  = 0;  /* unknown */
           mm->reliable = false;
           break;
         }

#if 0
        /* This doesn't seem useful, as we mistake a lot of CRC errors for overlay control.
         * Try a fuzzy match
         */
         mm->addr = icao_filter_test_fuzzy (mm->CRC);
         if (mm->addr != 0)
         {
           /* We have an address that would match, assume it's correct.
            * Derive the BDS value based on what we think the address is
            */
           mm->BDS = (mm->CRC ^ mm->addr) >> 16;
           break;
         }
#endif
         return (-1); /* no good */

    case 24: /* Comm-D (ELM) */
    case 25: /* Comm-D (ELM) */
    case 26: /* Comm-D (ELM) */
    case 27: /* Comm-D (ELM) */
    case 28: /* Comm-D (ELM) */
    case 29: /* Comm-D (ELM) */
    case 30: /* Comm-D (ELM) */
    case 31: /* Comm-D (ELM) */
        /* These messages use Address/Parity,
         * and also use some of the DF bits to carry data. Remap them all to a single
         * DF for simplicity.
         */
        mm->msg_type = 24;
        mm->source   = SOURCE_MODE_S;
        mm->addr     = mm->CRC;
        mm->reliable = false;
        break;

    default:
         /* All other message types, we don't know how to handle their CRCs, give up
          */
         return (-2);
  }

  /* Now decode the bulk of the message
   */
  mm->AC_flags = 0;

  /* AA (Address announced)
   */
  if (mm->msg_type == 11 || mm->msg_type == 17 || mm->msg_type == 18)
     mm->addr = AIRCRAFT_GET_ADDR (msg+1);

  /* AC (Altitude Code)
   */
  if (mm->msg_type == 0 || mm->msg_type == 4 || mm->msg_type == 16 || mm->msg_type == 20)
  {
    int AC13_field = ((msg[2] << 8) | msg[3]) & 0x1FFF;

    if (AC13_field)  /* Only attempt to decode if a valid (non zero) altitude is present */
    {
      mm->altitude = decode_AC13_field (AC13_field, &mm->unit);
      if (mm->altitude != INVALID_ALTITUDE)
          mm->AC_flags |= MODES_ACFLAGS_ALTITUDE_VALID;
    }
  }

  /* CA (Capability), responder capabilities:
   */
  if (mm->msg_type == 11 || mm->msg_type == 17)
  {
    mm->capa = msg[0] & 7;
    if (mm->capa == 4)
        mm->AC_flags |= MODES_ACFLAGS_AOG_VALID | MODES_ACFLAGS_AOG;
    else if (mm->capa == 5)
        mm->AC_flags |= MODES_ACFLAGS_AOG_VALID;
  }

  /* CC (Cross-link capability) not decoded */

  /* CF (Control field) */
  if (mm->msg_type == 18)
     mm->cf = msg[0] & 7;

  /* DR (Downlink Request) not decoded */

  /* FS (Flight Status) */
  if (mm->msg_type == 4 || mm->msg_type == 5 || mm->msg_type == 20 || mm->msg_type == 21)
  {
    mm->AC_flags |= MODES_ACFLAGS_FS_VALID;
    mm->flight_status = msg[0] & 7;
    if (mm->flight_status <= 3)
    {
      mm->AC_flags |= MODES_ACFLAGS_AOG_VALID;
      if (mm->flight_status & 1)
         mm->AC_flags |= MODES_ACFLAGS_AOG;
    }
  }

  /* ID (Identity) */
  if (mm->msg_type == 5 || mm->msg_type == 21)
  {
    /* Gillham encoded Squawk */
    int ID13_field = ((msg[2] << 8) | msg[3]) & 0x1FFF;
    if (ID13_field)
    {
      mm->AC_flags |= MODES_ACFLAGS_SQUAWK_VALID;
      mm->identity = decode_ID13_field (ID13_field);
    }
  }

  /* KE (Control, ELM) not decoded */

  /* MB (messsage, Comm-B) */
  if (mm->msg_type == 20 || mm->msg_type == 21)
     decode_comm_B (mm);

  /* MD (message, Comm-D) */
  if (mm->msg_type == 24)
     memcpy (mm->MD, &msg[1], sizeof(mm->MD));

  /* ME (message, extended squitter) */
  if (mm->msg_type == 17 ||   /* Extended squitter */
      mm->msg_type == 18)     /* Extended squitter/non-transponder: */
     decode_extended_squitter (mm);

  /* MV (message, ACAS) not decoded */
  /* ND (number of D-segment) not decoded */
  /* RI (Reply information) not decoded */
  /* SL (Sensitivity level, ACAS) not decoded */
  /* UM (Utility Message) not decoded */

  /* VS (Vertical Status) */
  if (mm->msg_type == 0 || mm->msg_type == 16)
  {
    mm->AC_flags |= MODES_ACFLAGS_AOG_VALID;
    if (msg[0] & 0x04)
       mm->AC_flags |= MODES_ACFLAGS_AOG;
  }

  if (!mm->error_bits && (mm->msg_type == 17 || mm->msg_type == 18 || (mm->msg_type == 11 && mm->IID == 0)))
  {
    /* No CRC errors seen, and either it was an DF17/18 extended squitter
     * or a DF11 acquisition squitter with II = 0. We probably have the right address.
     *
     * We wait until here to do this as we may have needed to decode an ES to note
     * the type of address in DF18 messages.
     *
     * NB this is the only place that adds addresses!
     */
    icao_filter_add (mm->addr);
  }
  return (0);  /* all done */
}

/**
 * Common entry for all decoding to keep the CRC-statistics simpler.
 */
int decode_mode_S_message (modeS_message *mm, const uint8_t *msg)
{
  int rc = _decode_mode_S_message (mm, msg);

  if (rc < 0)
  {
    mm->CRC_ok = false;
    Modes.stat.CRC_bad++;
    if (rc == -1)
       Modes.stat.demod_rejected_unknown++;
  }
  else
  {
    mm->CRC_ok = true;
    if (mm->error_bits > 0)
    {
      Modes.stat.CRC_fixed++;
      if (mm->error_bits == 1)
         Modes.stat.CRC_single_bit_fix++;
      else if (mm->error_bits == 2)
         Modes.stat.CRC_two_bits_fix++;
    }
    else
      Modes.stat.CRC_good++;   /* good CRC, not fixed */
  }
  return (rc);
}

void decode_mode_A_message (modeS_message *mm, int ModeA)
{
 /* Valid Mode S DF's are DF-00 to DF-31.
  * so use 32 to indicate Mode A/C
  */
  mm->msg_type = 32;
  mm->addrtype = ADDR_MODE_A;

  mm->msg_bits = 16;   /* Fudge up a Mode S style data stream */
  mm->msg [0] = (ModeA >> 8);
  mm->msg [1] = (ModeA);

  /* Fudge an address based on Mode A (remove the Ident bit)
   */
  mm->addr = (ModeA & 0x0000FF7F) | MODES_NON_ICAO_ADDRESS;

  /* Set the Identity field to ModeA
   */
  mm->identity = ModeA & 0x7777;
  mm->AC_flags |= MODES_ACFLAGS_SQUAWK_VALID;

  /* Flag ident in flight status
   */
  mm->flight_status = ModeA & 0x0080;

#if 0
  /* Decode an altitude if this looks like a possible mode C
   */
  if (!mm->spi)
  {
    int mode_C = mode_A_to_mode_C (ModeA);
    if (mode_C != INVALID_ALTITUDE)
    {
      mm->altitude_baro = modeC * 100;
      mm->altitude_baro_unit = UNIT_FEET;
      mm->altitude_baro_valid = 1;
    }
  }
#endif

  /* Not much else we can tell from a Mode A/C reply.
   * Just fudge up a few bits to keep other code happy
   */
  mm->error_bits = 0;
}

/**
 * Input format is: 00:A4:A2:A1:00:B4:B2:B1:00:C4:C2:C1:00:D4:D2:D1
 */
int mode_A_to_mode_C (u_int ModeA)
{
  u_int five_hundreds = 0;
  u_int one_hundreds  = 0;

  if ((ModeA & 0xFFFF8889) ||      /* check zero bits are zero, D1 set is illegal */
      (ModeA & 0x000000F0) == 0)   /* C1,,C4 cannot be Zero */
    return (-9999);

  if (ModeA & 0x0010) one_hundreds ^= 0x007; /* C1 */
  if (ModeA & 0x0020) one_hundreds ^= 0x003; /* C2 */
  if (ModeA & 0x0040) one_hundreds ^= 0x001; /* C4 */

  /* Remove 7s from one_hundreds (Make 7->5, snd 5->7)
   */
  if ((one_hundreds & 5) == 5)
     one_hundreds ^= 2;

  /* Check for invalid codes, only 1 to 5 are valid
   */
  if (one_hundreds > 5)
     return (-9999);

/*if (ModeA & 0x0001) five_hundreds ^= 0x1FF; */  /* D1 - never used for altitude */
  if (ModeA & 0x0002) five_hundreds ^= 0x0FF;     /* D2 */
  if (ModeA & 0x0004) five_hundreds ^= 0x07F;     /* D4 */

  if (ModeA & 0x1000) five_hundreds ^= 0x03F;     /* A1 */
  if (ModeA & 0x2000) five_hundreds ^= 0x01F;     /* A2 */
  if (ModeA & 0x4000) five_hundreds ^= 0x00F;     /* A4 */

  if (ModeA & 0x0100) five_hundreds ^= 0x007;     /* B1 */
  if (ModeA & 0x0200) five_hundreds ^= 0x003;     /* B2 */
  if (ModeA & 0x0400) five_hundreds ^= 0x001;     /* B4 */

  /* Correct order of one_hundreds
   */
  if (five_hundreds & 1)
     one_hundreds = 6 - one_hundreds;

  return ((five_hundreds * 5) + one_hundreds - 13);
}

/**
 * Accumulate statistics of unrecognized ME types and sub-types.
 */
static void add_unrecognized_ME (int type, int subtype, bool test)
{
  unrecognized_ME *me;

  if (type >= 0 && type < MAX_ME_TYPE && subtype >= 0 && subtype < MAX_ME_SUBTYPE)
  {
    me = &Modes.stat.unrecognized_ME [type];
    me->sub_type [subtype]++;
    if (test)
       printf ("(%2d, %2d) -> sub_type [%d]=%llu\n", type, subtype, subtype, me->sub_type[subtype]);
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
  int      t, num_totals = 0;
  uint64_t totals = 0;
  uint64_t totals_ME [MAX_ME_TYPE];
  bool     indented = false;

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
    char  *end = p + sizeof(sub_types);
    size_t j;

    if (totals_ME[t] == 0ULL)
       continue;

    *p = '\0';
    for (j = 0; j < MAX_ME_SUBTYPE; j++)
    {
      const unrecognized_ME *me = &Modes.stat.unrecognized_ME [t];

      if (me->sub_type[j] > 0ULL)
         p += snprintf (p, end - p, "%zd,", j);
    }

    if (p > sub_types) /* remove the comma */
         p[-1] = '\0';
    else *p = '\0';

    /* indent next line to print like:
     *   45 unrecognized ME types: 29: 20 (2)
     *                             31: 25 (3)
     */
    if (num_totals++ >= 1)
    {
      if (indented)
           LOG_STDOUT ("! \n                                ");
      else LOG_STDOUT ("!  ");
    }
    if (sub_types[0])
         LOG_STDOUT ("! %3llu: %2d (%s)", totals, t, sub_types);
    else LOG_STDOUT ("! %3llu: %2d", totals, t);
    indented = true;
  }
  LOG_STDOUT ("! \n");
}

/*
 * Test the above functions
 */
static void test_print_unrecognized_ME (void)
{
  int i, type, subtype;

  for (i = 0; i < 50; i++)
  {
    type    = random_range2 (0, MAX_ME_TYPE - 1);
    subtype = random_range2 (0, MAX_ME_SUBTYPE - 1);
    add_unrecognized_ME (type, subtype, true);
  }
  add_unrecognized_ME (1, 2, true);
  add_unrecognized_ME (1, 2, true);
  add_unrecognized_ME (1, 2, true);

  puts ("                              ME: sub (num, ..)");
  print_unrecognized_ME();
  modeS_exit (0);
}

static bool display_brief_message (modeS_message *mm)
{
  const char     *dist = "-";
  char            buf [200];
  const aircraft *a = aircraft_find (mm->addr);

  if (!a)
     return (false);

  if (a->distance_ok &&
      (Modes.min_dist == 0 || a->distance < Modes.min_dist))
  {
    snprintf (buf, sizeof(buf), "%7.3lf km", a->distance / 1000.0);
    dist = buf;
  }
  printf ("%06X dist: %s\n", a->addr, dist);
  return (true);
}

static const char *ac_type_str[] = {
                  "Aircraft Type D",
                  "Aircraft Type C",
                  "Aircraft Type B",
                  "Aircraft Type A"
                 };

static bool display_extended_squitter (const modeS_message *mm)
{
  /* Decode the extended squitter message. */
  if (mm->ME_type >= 1 && mm->ME_type <= 4)
  {
    /* Aircraft identification
     */
    LOG_STDOUT ("    Aircraft Type  : %s\n", ac_type_str[mm->aircraft_type]);
    LOG_STDOUT ("    Identification : %s\n", mm->flight);
    return (true);
  }

  if (mm->ME_type >= 9 && mm->ME_type <= 18)
  {
    LOG_STDOUT ("    F flag   : %s\n", mm->odd_flag ? "odd" : "even");
    LOG_STDOUT ("    T flag   : %s\n", mm->UTC_flag ? "UTC" : "non-UTC");
    LOG_STDOUT ("    Altitude : %d feet\n", mm->altitude);
    LOG_STDOUT ("    Latitude : %d (not decoded)\n", mm->raw_latitude);
    LOG_STDOUT ("    Longitude: %d (not decoded)\n", mm->raw_longitude);
    return (true);
  }

  if (mm->ME_type == 19 && mm->ME_subtype >= 1 && mm->ME_subtype <= 4)
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
      LOG_STDOUT ("    Heading status: %d\n", (mm->AC_flags & MODES_ACFLAGS_HEADING_VALID) ? 1 : 0);
      LOG_STDOUT ("    Heading: %d\n", (int)mm->heading);
    }
    return (true);
  }

  if (mm->ME_type == 23)  /* Test Message */
  {
    if (mm->ME_subtype == 7)
         LOG_STDOUT ("    Squawk: %04x\n", mm->identity);
    else LOG_STDOUT ("    Unrecognized ME subtype: %d\n", mm->ME_subtype);
    return (true);
  }

  if (mm->ME_type == 28)  /* Extended Squitter Aircraft Status */
  {
    if (mm->ME_subtype == 1)
    {
      LOG_STDOUT ("    Emergency State: %s\n", emerg_state_str[(mm->msg[5] & 0xE0) >> 5]);
      LOG_STDOUT ("    Squawk: %04x\n", mm->identity);
    }
    else
      LOG_STDOUT ("    Unrecognized ME subtype: %d\n", mm->ME_subtype);
    return (true);
  }

#if 1
  if (mm->ME_type == 29)
  {
    /**\todo
     * Target State + Status Message
     *
     * \ref
     *   the `if tc == 29:` part in `pyModeS/decoder/__init__.py`
     */
    add_unrecognized_ME (29, mm->ME_subtype, false);
    return (false);
  }

  if (mm->ME_type == 31)  /* Aircraft operation status */
  {
    /**\todo Ref: chapter 8 in `The-1090MHz-riddle.pdf`
     */
    add_unrecognized_ME (31, mm->ME_subtype, false);
    return (false);
  }
#endif

  LOG_STDOUT ("    Unrecognized ME type: %d, subtype: %d\n", mm->ME_type, mm->ME_subtype);
  add_unrecognized_ME (mm->ME_type, mm->ME_subtype, false);
  return (false);
}

static void display_addr (const modeS_message *mm, int extra_space)
{
  if (mm->addr & MODES_NON_ICAO_ADDRESS)
       LOG_STDOUT ("  Other Address%*s: %06X (%s)\n", extra_space, "", mm->addr & MODES_ICAO_ADDRESS_MASK, aircraft_get_details(mm));
  else LOG_STDOUT ("  ICAO Address%*s: %06X (%s)\n", extra_space, "", mm->addr, aircraft_get_details(mm));
}

/**
 * This function gets a decoded Mode S Message and prints it on the screen
 * in a human readable format.
 */
static bool modeS_message_display (modeS_message *mm)
{
  char   buf [200];
  char  *p = buf;
  size_t left = sizeof(buf);
  int    i;

  /* Did we specify a filter?
   */
  if (!aircraft_match(mm->addr))
  {
    TRACE ("aircraft_match() did not match %06X\n", mm->addr);
    return (false);
  }

  /* Handle only addresses mode first.
   */
  if (Modes.only_addr)
     return display_brief_message (mm);

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
     return (true);       /* Enough for --raw mode */

  LOG_STDOUT ("CRC: %06X (%s)\n", (int)mm->CRC, mm->CRC_ok ? "ok" : "wrong");
  if (mm->error_bits > 0)
     LOG_STDOUT ("No. of bit errors fixed: %d\n", mm->error_bits);

  if (mm->sig_level > 0)
     LOG_STDOUT ("RSSI: %.1lf dBFS\n", 10 * log10(mm->sig_level));

  const char *ground = "";

  if ((mm->AC_flags & MODES_ACFLAGS_AOG_VALID) && (mm->AC_flags & MODES_ACFLAGS_AOG))
     ground = ", Ground";

  if (mm->msg_type == 0)
  {
    /* DF0 */
    LOG_STDOUT ("DF 0: Short Air-Air Surveillance.\n");
    LOG_STDOUT ("  Altitude    : %d %s%s\n", mm->altitude, UNIT_NAME(mm->unit), ground);
    display_addr (mm, 0);
  }
  else if (mm->msg_type == 4 || mm->msg_type == 20)
  {
    LOG_STDOUT ("DF %d: %s, Altitude Reply.\n", mm->msg_type, mm->msg_type == 4 ? "Surveillance" : "Comm-B");
    LOG_STDOUT ("  Flight Status: %s\n", flight_status_str [mm->flight_status]);
    LOG_STDOUT ("  DR           : %d\n", mm->DR_status);
    LOG_STDOUT ("  UM           : %d\n", mm->UM_status);
    LOG_STDOUT ("  Altitude     : %d %s\n", mm->altitude, UNIT_NAME(mm->unit));
    display_addr (mm, 1);

    if (mm->msg_type == 20)
    {
      /** \todo 56 bits DF20 MB additional field. */
    }
  }
  else if (mm->msg_type == 5 || mm->msg_type == 21)
  {
    LOG_STDOUT ("DF %d: %s, Identity Reply.\n", mm->msg_type, mm->msg_type == 5 ? "Surveillance" : "Comm-B");
    LOG_STDOUT ("  Flight Status: %s\n", flight_status_str [mm->flight_status]);
    LOG_STDOUT ("  DR           : %d\n", mm->DR_status);
    LOG_STDOUT ("  UM           : %d\n", mm->UM_status);
    LOG_STDOUT ("  Squawk       : %d\n", mm->identity);
    display_addr (mm, 1);

    if (mm->msg_type == 21)
    {
      /** \todo 56 bits DF21 MB additional field. */
    }
  }
  else if (mm->msg_type == 11)
  {
    /* DF11 */
    LOG_STDOUT ("DF 11: All Call Reply.\n");
    LOG_STDOUT ("  Capability  : %d (%s)\n", mm->capa, capability_str[mm->capa]);
    display_addr (mm, 0);
  }
  else if (mm->msg_type == 17)
  {
    /* DF17 */
    LOG_STDOUT ("DF 17: ADS-B message.\n");
    LOG_STDOUT ("  Capability  : %d (%s)\n", mm->capa, capability_str[mm->capa]);
    display_addr (mm, 0);
    LOG_STDOUT ("  Extended Squitter Type: %d\n", mm->ME_type);
    LOG_STDOUT ("  Extended Squitter Sub : %d\n", mm->ME_subtype);
    LOG_STDOUT ("  Extended Squitter Name: %s\n", get_ME_description(mm));

    display_extended_squitter (mm);
  }
  else
  {
    LOG_STDOUT ("DF %d with good CRC received (decoding not implemented).\n", mm->msg_type);
  }
  return (true);
}

/**
 * Based on errorinfo` from crc.c, correct a decoded native-endian
 * Address Announced (AA) field (from bits 8-31).
 * if it is affected by the given error syndrome.
 * Updates *addr and returns >0 if changed, 0 if it was unaffected.
 */
static int correct_aa_field (uint32_t *addr, const errorinfo *ei)
{
  int i, addr_errors = 0;

  if (!ei)
     return (0);

  for (i = 0; i < ei->errors; i++)
  {
    if (ei->bit[i] >= 8 && ei->bit[i] <= 31)
    {
      *addr ^= 1 << (31 - ei->bit[i]);
      ++addr_errors;
    }
  }
  return (addr_errors);
}

/**
 * Score how plausible this ModeS message looks.
 * The more positive, the more reliable the message is.
 *
 * 1000: DF 0/4/5/16/24 with a CRC-derived address matching a known aircraft
 *
 * 1800: DF17/18 with good CRC and an address matching a known aircraft
 * 1400: DF17/18 with good CRC and an address not matching a known aircraft
 *  900: DF17/18 with 1-bit error and an address matching a known aircraft
 *  700: DF17/18 with 1-bit error and an address not matching a known aircraft
 *  450: DF17/18 with 2-bit error and an address matching a known aircraft
 *  350: DF17/18 with 2-bit error and an address not matching a known aircraft
 *
 * 1600: DF11 with IID==0, good CRC and an address matching a known aircraft
 *  800: DF11 with IID==0, 1-bit error and an address matching a known aircraft
 *  750: DF11 with IID==0, good CRC and an address not matching a known aircraft
 *  375: DF11 with IID==0, 1-bit error and an address not matching a known aircraft
 *
 * 1000: DF11 with IID!=0, good CRC and an address matching a known aircraft
 *  500: DF11 with IID!=0, 1-bit error and an address matching a known aircraft
 *
 * 1000: DF20/21 with a CRC-derived address matching a known aircraft
 *  500: DF20/21 with a CRC-derived address matching a known aircraft (bottom 16 bits only - overlay control in use)
 *
 *   -1: message might be valid, but we couldn't validate the CRC against a known ICAO
 *   -2: bad message or unrepairable CRC error
 *
 * Called from a `demod*.c` function.
 */
#if 0 /**<\todo Add a `score_rank` enum
       */
  typedef enum score_rank {
         // ...
        } score_rank;
#endif

int modeS_message_score (const uint8_t *msg, int valid_bits)
{
  static uint8_t all_zeros[14] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  int        msg_type, msg_bits, CRC, IID;
  uint32_t   addr;
  errorinfo *ei;

  if (valid_bits < 56)
     return (-2);

  msg_type = msg [0] >> 3; /* Downlink Format */
  msg_bits = modeS_message_len_by_type (msg_type);

  if (valid_bits < msg_bits)
     return (-2);

  if (!memcmp(all_zeros, msg, msg_bits/8))
     return (-2);

  CRC = crc_checksum (msg, msg_bits);

  switch (msg_type)
  {
    case 0:   /* short air-air surveillance */
    case 4:   /* surveillance, altitude reply */
    case 5:   /* surveillance, altitude reply */
    case 16:  /* long air-air surveillance */
    case 24:  /* Comm-D (ELM) */
         return icao_filter_test (CRC) ? 1000 : -1;

    case 11:  /* All-call reply */
         IID = CRC & 0x7F;
         CRC = CRC & 0xFFFF80;
         addr = AIRCRAFT_GET_ADDR (msg + 1);

         ei = crc_checksum_diagnose (CRC, msg_bits);
         if (!ei)
            return (-2);  /* can't correct errors */

         /* See crc.c comments: we do not attempt to fix
          * more than single-bit errors, as two-bit
          * errors are ambiguous in DF11
          */
         if (ei->errors > 1)
            return (-2);  /* can't correct errors */

         /* fix any errors in the address field
          */
         correct_aa_field (&addr, ei);

         /* validate address */
         if (IID == 0)
         {
           if (icao_filter_test(addr))
              return (1600 / (ei->errors + 1));
           return (750 / (ei->errors + 1));
         }
         if (icao_filter_test(addr))
            return (1000 / (ei->errors + 1));
         return (-1);

    case 17:   /* Extended squitter */
    case 18:   /* Extended squitter/non-transponder */
         ei = crc_checksum_diagnose (CRC, msg_bits);
         if (!ei)
             return (-2);   /* can't correct errors */

         /* fix any errors in the address field
          */
         addr = AIRCRAFT_GET_ADDR (msg + 1);
         correct_aa_field (&addr, ei);

         if (icao_filter_test(addr))
            return (1800 / (ei->errors + 1));
         return (1400 / (ei->errors + 1));

    case 20:   /* Comm-B, altitude reply */
    case 21:   /* Comm-B, identity reply */
         if (icao_filter_test(CRC))
            return (1000);  /* Address/Parity */
         return (-2);

    default:
        /* unknown message type */
        return (-2);
  }
}

/**
 * When a new message is available, because it was decoded from the
 * RTLSDR/SDRplay device, file, or received on a TCP input port
 * (from a SBS-IN or RAW-IN service), we call this function in order
 * to use the message.
 *
 * Basically this function passes a raw message to the upper layers for
 * further processing and visualization.
 */
void modeS_user_message (modeS_message *mm)
{
  aircraft *a = aircraft_update_from_message (mm);

  Modes.stat.messages_total++;

  if (a &&
      Modes.stat.cli_accepted [MODES_NET_SERVICE_SBS_OUT] > 0 &&  /* If we have accepted >=1 client */
      net_handler_sending(MODES_NET_SERVICE_SBS_OUT))             /* and we're still sending */
     modeS_send_SBS_output (mm);                                  /* Feed SBS output clients. */

  /* In non-interactive mode, display messages on standard output.
   * In silent-mode, do nothing just to consentrate on network traces.
   */
  if (!Modes.interactive && !Modes.silent)
  {
    if (modeS_message_display (mm))
       Modes.stat.messages_shown++;
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
 * specified level for more than 256 samples in order to reduce example file size.
 *
 * Will print to `stdout` in BINARY-mode.
 */
static bool strip_mode (int level)
{
  uint64_t c = 0;
  int      I, Q;

  _setmode (_fileno(stdin), O_BINARY);
  _setmode (_fileno(stdout), O_BINARY);

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
  _setmode (_fileno(stdin), O_TEXT);
  _setmode (_fileno(stdout), O_TEXT);
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
  net_handler_send (MODES_NET_SERVICE_RAW_OUT, msg, p - msg);
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
     timestamp [strlen(timestamp) - 1] = '\0';    /* remove last ',' */
  return (timestamp);
}

/**
 * Write SBS output to TCP clients (Base Station format).
 */
static void modeS_send_SBS_output (const modeS_message *mm)
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
                  mm->addr, date_str, mm->altitude);
  }
  else if (mm->msg_type == 4)
  {
    p += sprintf (p, "MSG,5,1,1,%06X,1,%s,,%d,,,,,,,%d,%d,%d,%d",
                  mm->addr, date_str, mm->altitude, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 5)
  {
    p += sprintf (p, "MSG,6,1,1,%06X,1,%s,,,,,,,,%d,%d,%d,%d,%d",
                  mm->addr, date_str, mm->identity, alert, emergency, spi, ground);
  }
  else if (mm->msg_type == 11)
  {
    p += sprintf (p, "MSG,8,1,1,%06X,1,%s,,,,,,,,,,,,",
                  mm->addr, date_str);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 4)
  {
    p += sprintf (p, "MSG,1,1,1,%06X,1,%s,%s,,,,,,,,0,0,0,0",
                  mm->addr, date_str, mm->flight);
  }
  else if (mm->msg_type == 17 && mm->ME_type >= 9 && mm->ME_type <= 18)
  {
    if (mm->AC_flags & MODES_ACFLAGS_LATLON_VALID)
         p += sprintf (p, "MSG,3,1,1,%06X,1,%s,,%d,,,%1.5f,%1.5f,,,0,0,0,0",
                       mm->addr, date_str, mm->altitude, mm->position.lat, mm->position.lon);
    else p += sprintf (p, "MSG,3,1,1,%06X,1,%s,,%d,,,,,,,0,0,0,0",
                       mm->addr, date_str, mm->altitude);
  }
  else if (mm->msg_type == 17 && mm->ME_type == 19 && mm->ME_subtype == 1)
  {
    int vr = (mm->vert_rate_sign == 0 ? 1 : -1) * 64 * (mm->vert_rate - 1);

    p += sprintf (p, "MSG,4,1,1,%06X,1,%s,,,%d,%d,,,%i,,0,0,0,0",
                  mm->addr, date_str, (int)round(mm->velocity), (int)round(mm->heading), vr);
  }
  else if (mm->msg_type == 21)
  {
    p += sprintf (p, "MSG,6,1,1,%06X,1,%s,,,,,,,,%d,%d,%d,%d,%d",
                  mm->addr, date_str, mm->identity, alert, emergency, spi, ground);
  }
  else
    return;

  *p++ = '\n';
  net_handler_send (MODES_NET_SERVICE_SBS_OUT, msg, p - msg);
}

/**
 * \def LOG_GOOD_RAW()
 *      if `--debug g` is active, log a good RAW message.
 */
#define LOG_GOOD_RAW(fmt, ...)  TRACE ("RAW(%d): " fmt, \
                                       loop_cnt, ## __VA_ARGS__)

/**
 * \def LOG_BOGUS_RAW()
 *      if `--debug g` is active, log a bad / bogus RAW message.
 */
#define LOG_BOGUS_RAW(_msg, fmt, ...)  TRACE ("RAW(%d), Bogus msg %d: " fmt,   \
                                              loop_cnt, _msg, ## __VA_ARGS__); \
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
    LOG_GOOD_RAW ("Got heart-beat signal\n");
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
    LOG_BOGUS_RAW (1, "'%.*s'\n", (int)msg->len, msg->buf);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  if (hex[0] != '*' || !memchr(msg->buf, ';', len))
  {
    LOG_BOGUS_RAW (2, "hex[0]: '%c', '%.*s'\n", hex[0], (int)msg->len, msg->buf);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  /* Turn the message into binary.
   */
  hex++;     /* Skip `*` and `;` */
  len -= 2;

  if (len > 2 * MODES_LONG_MSG_BYTES)   /* Too long message (> 28 bytes)... broken. */
  {
    LOG_BOGUS_RAW (3, "len=%d, '%.*s'\n", len, len, hex);
    mg_iobuf_del (msg, 0, end - msg->buf);
    return (false);
  }

  for (j = 0; j < len; j += 2)
  {
    int high = hex_digit_val (hex[j]);
    int low  = hex_digit_val (hex[j+1]);

    if (high == -1 || low == -1)
    {
      LOG_BOGUS_RAW (4, "high='%c', low='%c'\n", hex[j], hex[j+1]);
      mg_iobuf_del (msg, 0, end - msg->buf);
      return (false);
    }
    bin_msg [j/2] = (high << 4) | low;
  }

  mg_iobuf_del (msg, 0, end - msg->buf);
  Modes.stat.RAW_good++;

  decode_mode_S_message (&mm, bin_msg);
  if (mm.CRC_ok)
     modeS_user_message (&mm);
  return (true);
}

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
  for (i = 1; i < DIM(fields); i++)
  {
    fields [i] = str_sep (&p, ",");
    if (!p && i < DIM(fields) - 1)
    {
      TRACE ("Missing field %zd\n", i);
      goto SBS_invalid;
    }
  }

/*TRACE ("field-2: '%s', field-5: '%s'\n", fields[2], fields[5]); */

  memset (mm, '\0', sizeof(*mm));
  mm->CRC_ok = true;
  Modes.stat.SBS_good++;

  /**
   *\todo
   * Decode `msg` and fill `mm` using `decodeSbsLine()` from readsb.
   */
#if 0
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
 * It accepts both `\n` and `\r\n` terminated records.
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
  if ((end - 2 > msg->buf) && end [-2] == '\r')
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
static void show_help (const char *fmt, ...)
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
            "Usage: %s [options] <filter-spec>:\n"
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
            "                        P = Log a single plane at a time with details (ref `LOG_FOLLOW()`).\n"
            "  --device <N / name>   Select RTLSDR/SDRPlay device (default: 0; first found).\n"
            "                        e.g. `--device 0'               - select first RTLSDR device found.\n"
            "                             `--device RTL2838-silver'  - select on RTLSDR name.\n"
            "                             `--device tcp://host:port' - select a remote RTLSDR tcp service (default port=%u).\n"
            "                             `--device udp://host:port' - select a remote RTLSDR udp service (default port=%u).\n"
            "                             `--device sdrplay'         - select first SDRPlay device found.\n"
            "                             `--device sdrplay1'        - select on SDRPlay index.\n"
            "                             `--device sdrplayRSP1A'    - select on SDRPlay name.\n"
            "  --infile <filename>   Read data from file (use `-' for stdin).\n"
            "  --informat <format>   Format for `--infile`; `UC8`, `SC16` or `SC16Q11` (default: `UC8`)\n"
            "  --interactive         Enable interactive mode.\n"
            "  --max-messages        Maximum number of messages to process.\n"
            "  --net                 Enable network listening services.\n"
            "  --net-active          Enable network active services.\n"
            "  --net-only            Enable only networking, no physical device or file.\n"
            "  --only-addr           Show only ICAO addresses.\n"
            "  --raw                 Output raw hexadecimal messages only.\n"
            "  --samplerate/-s <S/s> Sample-rate (2M, 2.4M, 8M). Overrides setting in config-file.\n"
            "  --strip <level>       Output missing the I/Q parts that are below the specified level.\n"
            "  --test <test-spec>    A comma-list of tests to perform (`airport', `aircraft', `cpr', `locale', `misc`, `net' or `*')\n"
            "  --update              Update missing or old \"*.csv\" files and exit.\n"
            "  --version, -V, -VV    Show version info. `-VV' for details.\n"
            "  --help, -h            Show this help.\n\n",
            Modes.who_am_I, Modes.cfg_file, MODES_NET_PORT_RTL_TCP, MODES_NET_PORT_RTL_TCP);

    printf ("  Shows only matching ICAO-addresses;     `dump1090.exe --only-addr 4A*`.\n"
            "  Shows only non-matching ICAO-addresses; `dump1090.exe --only-addr !48*`.\n\n"
            "  Refer the `%s` file for other settings.\n", Modes.cfg_file);
  }
  modeS_exit (0);
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
void background_tasks (void)
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

    geo_spherical_to_cartesian (NULL, &Modes.home_pos, &Modes.home_pos_cart);
    if (Modes.home_pos_ok)
       LOG_FILEONLY ("Ignoring the 'homepos' config value since we use the 'Windows Location API':"
                     " Latitude: %.8f, Longitude: %.8f.\n",
                     Modes.home_pos.lat, Modes.home_pos.lon);
    Modes.home_pos_ok = true;
  }

  static bool shown_home_pos = false;

  if (Modes.home_pos_ok && !shown_home_pos)
  {
    LOG_FILEONLY ("Modes.home_pos: %.07lf %s, %.07lf %s\n",
                  fabs(Modes.home_pos.lon), Modes.home_pos.lon > 0.0 ? "E" : "W",
                  fabs(Modes.home_pos.lat), Modes.home_pos.lat > 0.0 ? "N" : "S");
    shown_home_pos = true;
  }

  static uint32_t fifo_stat = 0;

  if ((++fifo_stat % 240) == 0)     /* every 240th time; approx 1 min */
     fifo_stats();

  aircraft_remove_stale (now);
  airports_background (now);

  /* Refresh screen and console-title when in interactive mode
   */
  if (Modes.interactive)
     interactive_show_data (now);

  if (Modes.rtlsdr.device || Modes.rtl_tcp_in || Modes.sdrplay.device || Modes.raw_in)
  {
    interactive_title_stats();
    interactive_update_gain();
    interactive_other_stats();  /* Only effective if 'tui = curses' was used */
  }
  else
    interactive_raw_SBS_stats();
}

/**
 * The handler called for `SIGINT` or `SIGBREAK`. <br>
 * I.e. user presses `^C`.
 *
 * It also handles `SIGABRT`.
 * I.e. `abort()` (or `assert()` triggered false) was called.
 */
void modeS_signal_handler (int sig)
{
  int rc;

  if (sig > 0)                /* Restore signal handler */
     signal (sig,modeS_signal_handler);

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
    DEBUG (DEBUG_GENERAL, "Breaking 'main_data_loop()'%s, shutting down ...\n",
           Modes.internal_error ? " due to internal error" : "");
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

 /* SetEvent (Modes.reader_event); */
  }
}

/*
 * Show decoder statistics for a RTLSDR / RTLTCP / SDRPlay device.
 */
static void show_decoder_stats (void)
{
  LOG_STDOUT ("Decoder statistics:\n");
  interactive_clreol();  /* to clear the lines from startup messages */

  if (Modes.stat.FIFO_enqueue + Modes.stat.FIFO_dequeue + Modes.stat.FIFO_full > 0ULL)
  {
    double   percent, delta_s;
    FILETIME now;

    get_FILETIME_now (&now);

    /* Program runtime; in 100 nsec units to seconds as a double
     */
    delta_s = (double) (*(ULONGLONG*)&now - *(ULONGLONG*)&Modes.start_FILETIME) / 1E7;
    if (delta_s < 1.0)
       delta_s = 1.0;    /* fat chance, but avoid a divide by zero */

    LOG_STDOUT (" %8llu FIFO enqueue events (%.1lf/sec).\n", Modes.stat.FIFO_enqueue, (double)Modes.stat.FIFO_enqueue / delta_s);
    interactive_clreol();

    LOG_STDOUT (" %8llu FIFO dequeue events (%.1lf/sec).\n", Modes.stat.FIFO_dequeue, (double)Modes.stat.FIFO_dequeue / delta_s);
    interactive_clreol();

    if (Modes.stat.FIFO_enqueue > 0)
         percent = 100.0 * ((double)Modes.stat.FIFO_full / (double)Modes.stat.FIFO_enqueue);
    else percent = 0.0;
    LOG_STDOUT (" %8llu FIFO full events (%.2lf%%).\n", Modes.stat.FIFO_full, percent);
    interactive_clreol();
  }

  if (Modes.rtl_tcp_in)
  {
    LOG_STDOUT (" %8llu RTLTCP samples, processed %llu.\n",
                Modes.stat.samples_recv_rtltcp, Modes.stat.samples_processed);
    interactive_clreol();
  }

  char   buf [20];
  double val = (double) Modes.stat.valid_preamble;

  if (val >= 1E9)
       snprintf (buf, sizeof(buf), "%6.1lf M", val / 1E9);
  else if (val >= 1E6)
       snprintf (buf, sizeof(buf), "%6.1lf M", val / 1E6);
  else snprintf (buf, sizeof(buf), "%8llu", Modes.stat.valid_preamble);

  LOG_STDOUT (" %s valid preambles.\n", buf);
  interactive_clreol();

  LOG_STDOUT (" %8llu demodulated after phase correction.\n", Modes.stat.out_of_phase);
  interactive_clreol();

  LOG_STDOUT (" %8llu demodulated with 0 errors.\n", Modes.stat.demodulated);
  interactive_clreol();

  LOG_STDOUT (" %8llu with CRC okay.\n", Modes.stat.CRC_good);
  interactive_clreol();

  LOG_STDOUT (" %8llu with CRC failure.\n", Modes.stat.CRC_bad);
  interactive_clreol();

  LOG_STDOUT (" %8llu messages with 1 bit errors fixed.\n", Modes.stat.CRC_single_bit_fix);
  interactive_clreol();

  LOG_STDOUT (" %8llu messages with 2 bit errors fixed.\n", Modes.stat.CRC_two_bits_fix);
  interactive_clreol();

  LOG_STDOUT (" %8llu errors corrected (%llu + %llu).\n", Modes.stat.CRC_fixed, Modes.stat.CRC_single_bit_fix, Modes.stat.CRC_two_bits_fix);
  interactive_clreol();

  LOG_STDOUT (" %8llu total usable messages (%llu + %llu).\n", Modes.stat.CRC_good + Modes.stat.CRC_fixed, Modes.stat.CRC_good, Modes.stat.CRC_fixed);

  if (Modes.icao_spec)
     LOG_STDOUT (" %8llu ICAO-addresses filtered.\n", Modes.stat.addr_filtered);

  if (Modes.stat.cart_errors)
     LOG_STDOUT (" %8llu Cartesian errors.\n", Modes.stat.cart_errors);

  if (Modes.stat.cpr_errors)
     LOG_STDOUT (" %8llu CPR errors.\n", Modes.stat.cpr_errors);

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

  if (Modes.rtl_tcp_in)
     any_device = true;  /* connect() OK */

  if (any_device)        /* assume we got some data */
     show_decoder_stats();

  if (Modes.net)
     net_show_stats();

  if (Modes.airports_priv)
     airports_show_stats();
}

/**
 * Our exit function. Free all resources and `exit()`
 * with specific error-level (0 or 1).
 */
static void __declspec(noreturn) modeS_exit (int rc)
{
  modeS_cleanup();
  exit (rc);
}

/**
 * Free all resources.
 */
static void modeS_cleanup (void)
{
  int rc;

  if (!Modes.internal_error)
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
     CloseHandle (Modes.reader_thread);

  if (Modes.infile_fd > -1)
     infile_exit();

  if (Modes.interactive)
     interactive_exit();

  aircraft_exit (true);
  airports_exit (true);
  crc_exit();

  free (Modes.mag_lut);
  free (Modes.log10_lut);
  free (Modes.ICAO_cache);
  free (Modes.selected_dev);
  free (Modes.rtlsdr.name);
  free (Modes.rtltcp.remote);
  free (Modes.sdrplay.name);
  free (Modes.aircraft_db_url);
  free (Modes.tests);
  free (Modes.icao_spec);

  demod_8000_free();

  if (Modes.FIFO_active)
     fifo_exit();
  Modes.FIFO_active = false;

  convert_cleanup (&Modes.converter_state);

  DeleteCriticalSection (&Modes.data_mutex);
  DeleteCriticalSection (&Modes.print_mutex);

  Modes.reader_thread = NULL;
  Modes.mag_lut       = NULL;
  Modes.log10_lut     = NULL;
  Modes.ICAO_cache    = NULL;
  Modes.selected_dev  = NULL;
  Modes.tests         = NULL;

  if (Modes.win_location)
     location_exit();

  speak_exit();

  modeS_log_exit();

#if defined(_DEBUG)
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

    if (modeS_net_services [MODES_NET_SERVICE_RTL_TCP].is_ip6)
    {
      LOG_STDERR ("IPv6 is not yet supported for RTL_TCP.\n");
      modeS_exit (1);
    }
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

  if (Modes.sample_rate != MODES_DEFAULT_RATE &&
      Modes.sample_rate != 2400000            &&
      Modes.sample_rate != 8000000)
     show_help ("Illegal sample_rate: %s. Use '2M, 2.4M or 8M (for SDRPlay)'.\n", arg);

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
  char buf [200], *p, *end;

  while (*flags)
  {
    switch (*flags)
    {
      case 'a':
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
           Modes.debug |= DEBUG_NOPREAMBLE;
           break;
      case 'P':
           Modes.debug |= DEBUG_PLANE;
           break;
      default:
           p = buf;
           end = p + sizeof(buf);
           p += snprintf (p, end - p, "Unknown debugging flag: `%c`", *flags);
           if (*flags == '-' && flags[1])
              p += snprintf (p, end - p, ". Did you mean `--debug %s`", flags+1);
           show_help ("%s\n", buf);
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
    geo_spherical_to_cartesian (NULL, &Modes.home_pos, &Modes.home_pos_cart);
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
  uint16_t port = atoi (arg);

  modeS_net_services [MODES_NET_SERVICE_HTTP4].port = port;
  modeS_net_services [MODES_NET_SERVICE_HTTP6].port = port;
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

#if !defined(USE_BIN_FILES)
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
  { "config",       required_argument,  NULL,               'c' },
  { "debug",        required_argument,  NULL,               'd' },
  { "device",       required_argument,  NULL,               'D' },
  { "help",         no_argument,        NULL,               'h' },
  { "infile",       required_argument,  NULL,               'i' },
  { "informat",     required_argument,  NULL,               'I' },
  { "interactive",  no_argument,        &Modes.interactive,  1  },
  { "max-messages", required_argument,  NULL,               'm' },
  { "net",          no_argument,        &Modes.net,          1  },
  { "net-active",   no_argument,        &Modes.net_active,  'N' },
  { "net-only",     no_argument,        &Modes.net_only,    'n' },
  { "only-addr",    no_argument,        &Modes.only_addr,    1  },
  { "raw",          no_argument,        &Modes.raw,          1  },
  { "samplerate",   required_argument,  NULL,               's' },
  { "strip",        required_argument,  NULL,               'S' },
  { "test",         required_argument,  NULL,               'T' },
  { "update",       no_argument,        NULL,               'u' },
  { "version",      no_argument,        NULL,               'V' },
  { NULL,           no_argument,        NULL,                0  }
};

static bool parse_cmd_line (int argc, char **argv)
{
  int   c, show_ver = 0, idx = 0;
  char *end;
  bool  rc = true;

  while ((c = getopt_long (argc, argv, "+hs:?V", long_options, &idx)) != EOF)
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
           infile_set (optarg);
           break;

      case 'I':
           if (!informat_set(optarg))
              show_help ("Illegal `--informat %s`. Use `UC8`, `SC16` or `SC16Q11`.\n", optarg);
           break;

      case 'm':
           max_messages = strtoull (optarg, &end, 10);
           if (end == optarg)
              show_help ("Illegal value for `--max-messages %s'.\n", optarg);
           break;

      case 'N':
           Modes.net_active = Modes.net = true;
           break;

      case 'n':
           Modes.net_only = Modes.net = true;
           break;

      case 's':
           set_sample_rate (optarg);
           break;

      case 'S':
           Modes.strip_level = atoi (optarg);
           if (Modes.strip_level == 0)
              show_help ("Illegal level for `--strip %s'.\n", optarg);
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
  if (*argv && !aircraft_match_init (*argv))
     rc = false;
  return (rc);
}

/**
 * Our main entry.
 */
int main (int argc, char **argv)
{
  bool init_error = true;   /* assume some 'x_init()' failure */
  int  rc;

#if defined(_DEBUG)
  crtdbug_init();
#endif

  modeS_init_config();  /* Set sane defaults */

  if (!parse_cmd_line(argc, argv))
     goto quit;

  rc = modeS_init();    /* Initialization based on cmd-line and config-file options */
  if (!rc)
     goto quit;

  if (Modes.net_only)
  {
    char notice [100] = "";

    if ((Modes.rtlsdr.name  || Modes.rtlsdr.index > -1 ||
         Modes.sdrplay.name || Modes.sdrplay.index > -1) &&
        !net_handler_host(MODES_NET_SERVICE_RTL_TCP))
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
    rc = infile_init();
    if (!rc)
         goto quit;
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
    else if (!net_handler_host(MODES_NET_SERVICE_RTL_TCP))
    {
      rc = modeS_init_RTLSDR();  /* not using a remote RTL_TCP input device */
      DEBUG (DEBUG_GENERAL, "modeS_init_RTLSDR(): rc: %d.\n", rc);
      if (!rc)
         goto quit;
    }
  }

  if (!modeS_init_hardware())
     goto quit;

  if (Modes.net)
  {
    /* This will also setup a service for the remote RTL_TCP input device.
     */
    rc = net_init();
    if (!rc)
    {
      LOG_STDERR ("net_init() failed.\n");
      if (!Modes.tests)      /* not fatal for test-modes */
         goto quit;
    }
  }

  init_error = false;

  if (Modes.tests)
     goto quit;

  /**
   * \todo
   * Move processing of `Modes.infile` to the same thread
   * for consistent handling of all sample-sources.
   */
  if (Modes.infile[0])
  {
    if (infile_read() == 0)
       LOG_STDERR ("No good messages found in '%s'.\n", Modes.infile);
  }
  else if (Modes.strip_level == 0)
  {
    /* Create the thread that will read the data from a RTLSDR device,
     * SDRplay device or a remote RTL_TCP service.
     */
    Modes.reader_thread = CreateThread (NULL, 0, data_thread_fn, NULL, 0, NULL);
    if (!Modes.reader_thread)
    {
      LOG_STDERR ("CreateThread() failed; err=%lu", GetLastError());
      goto quit;
    }
    Modes.reader_event = CreateEvent (NULL, FALSE, FALSE, NULL);
    if (!Modes.reader_event)
    {
      LOG_STDERR ("CreateEvent() failed; err=%lu", GetLastError());
      goto quit;
    }

    main_data_loop();
  }

quit:
  if (!init_error && !Modes.no_stats)
     show_statistics();
  modeS_cleanup();
  return (0);
}
