/**
 * \file    dump1090.c
 * \ingroup Main
 * \brief   Dump1090, a Mode S messages decoder for RTLSDR devices.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
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

#include <rtl-sdr.h>
#include <mongoose.h>

/**
 * \addtogroup Main      Main decoder
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

#define MG_NET_POLL_TIME  500   /* milli-sec */
#define MG_NET_DEBUG      "3"   /* LL_DEBUG */

#define MODES_DEFAULT_RATE         2000000
#define MODES_DEFAULT_FREQ         1090000000
#define MODES_ASYNC_BUF_NUMBER     12
#define MODES_DATA_LEN             (16*16384)   /* 256k */
#define MODES_AUTO_GAIN            -100         /* Use automatic gain. */

#define MODES_PREAMBLE_US             8         /* microseconds */
#define MODES_LONG_MSG_BITS         112
#define MODES_SHORT_MSG_BITS         56
#define MODES_FULL_LEN             (MODES_PREAMBLE_US + MODES_LONG_MSG_BITS)
#define MODES_LONG_MSG_BYTES       (MODES_LONG_MSG_BITS / 8)
#define MODES_SHORT_MSG_BYTES      (MODES_SHORT_MSG_BITS / 8)

#define MODES_ICAO_CACHE_LEN       1024   /* Power of two required. */
#define MODES_ICAO_CACHE_TTL         60   /* Time to live of cached addresses (sec). */
#define MODES_UNIT_FEET               0
#define MODES_UNIT_METERS             1

/**
 * Bits for 'Modes.debug':
 */
#define DEBUG_DEMOD      (1 << 0)
#define DEBUG_DEMODERR   (1 << 1)
#define DEBUG_BADCRC     (1 << 2)
#define DEBUG_GOODCRC    (1 << 3)
#define DEBUG_NOPREAMBLE (1 << 4)
#define DEBUG_JS         (1 << 5)
#define DEBUG_GENERAL    (1 << 6)
#define DEBUG_NET        (1 << 7)
#define DEBUG_NET2       (1 << 8)

/**
 * When debug is set to DEBUG_NOPREAMBLE, the first sample must be
 * at least greater than a given level for us to dump the signal.
 */
#define DEBUG_NOPREAMBLE_LEVEL         25

#define MODES_INTERACTIVE_REFRESH_TIME 250   /* Milliseconds */
#define MODES_INTERACTIVE_ROWS          15   /* Rows on screen */
#define MODES_INTERACTIVE_TTL           60   /* TTL before being removed */

#define MODES_NET_OUTPUT_SBS_PORT    30003
#define MODES_NET_OUTPUT_RAW_PORT    30002
#define MODES_NET_INPUT_RAW_PORT     30001
#define MODES_NET_HTTP_PORT           8080
#define MODES_CLIENT_BUF_SIZE         1024

#define MODES_NET_SERVICE_RAW_OUT 0
#define MODES_NET_SERVICE_RAW_IN  1
#define MODES_NET_SERVICE_SBS     2
#define MODES_NET_SERVICE_HTTP    3
#define MODES_NET_SERVICES_NUM    4

#define MODES_CONTENT_TYPE_CSS    "text/css;charset=utf-8"
#define MODES_CONTENT_TYPE_HTML   "text/html;charset=utf-8"
#define MODES_CONTENT_TYPE_JSON   "application/json;charset=utf-8"
#define MODES_CONTENT_TYPE_JS     "application/javascript;charset=utf-8"
#define MODES_CONTENT_TYPE_PNG    "image/png"

#define ADS_B_ACRONYM             "ADS-B; Automatic Dependent Surveillance - Broadcast"

/**
 * \def MSEC_TIME()
 * Returns a 64-bit tick-time value with 1 millisec granularity.
 */
#define MSEC_TIME() GetTickCount64()

/**
 * \struct net_service
 * A structure defining a network listening service.
 */
struct net_service {
    struct mg_connection **conn;  /**< A pointer to the returned Mongoose connection */
    const char *descr;            /**< A textual description of this service */
    unsigned    port;             /**< The listening port number */
    unsigned    num_clients;      /**< Number of active clients connected to it */
  };

/**
 * \def GMAP_HTML
 * Our default main server page relative to `Mode.where_am_I`.
 */
#define GMAP_HTML    "web_root/gmap.html"

/**
 * Various helper macros.
 */
#define IS_SLASH(c)        ((c) == '\\' || (c) == '/')
#define MODES_NOTUSED(V)   ((void)V)
#define TWO_PI             (2 * M_PI)

/**
 * \def SAFE_COND_SIGNAL(cond, mutex)
 * \def SAFE_COND_WAIT(cond, mutex)
 *
 * Signals are not threadsafe by default.
 * Taken from the Osmocom-SDR code and modified.
 */
#define SAFE_COND_SIGNAL(cond, mutex)   \
        do {                            \
          pthread_mutex_lock (mutex);   \
          pthread_cond_signal (cond);   \
          pthread_mutex_unlock (mutex); \
        } while (0)

#define SAFE_COND_WAIT(cond, mutex)        \
        do {                               \
          pthread_mutex_lock (mutex);      \
          pthread_cond_wait (cond, mutex); \
          pthread_mutex_unlock (mutex);    \
        } while (0)

/**
 * \def TRACE(bit, fmt, ...)
 * A more compact tracing macro
 */
#define TRACE(bit, fmt, ...)               \
        do {                               \
          if (Modes.debug & bit)           \
             printf ("%u: " fmt, __LINE__, \
                     __VA_ARGS__);         \
        } while (0)

/**
 * On Windows, a `pthread_t` is not a scalar but a structure like this:
 *  ```
 *   typedef struct
 *   {
 *     void *p;
 *     size_t x;
 *   } __ptw32_handle_t;
 *  ```
 *
 * So we cannot do e.g. `if (pthread_x == pthread_y) ..`
 *
 * But we can use `pthread_equal (pthread_x, PTHREAD_NULL)` to compare.
 */
static pthread_t PTHREAD_NULL;

/**
 * \struct client
 * Structure used to describe a networking client.
 */
struct client {
    struct mg_connection *conn;                          /**< Remember which connection the client is in */
    int                   service;                       /**< This client's service membership */
    uint32_t              id;                            /**< A copy of `conn->id` */
    struct mg_addr        addr;                          /**< A copy of `conn->peer` */
    int                   keep_alive;                    /**< Client request contains "Connection: keep-alive" */
    char                  buf [MODES_CLIENT_BUF_SIZE+1]; /**< Read buffer. */
    int                   buflen;                        /**< Amount of data on buffer. */
    struct client        *next;
};

/**
 * \struct aircraft
 * Structure used to describe an aircraft in interactive mode.
 */
struct aircraft {
       uint32_t addr;           /**< ICAO address */
       char     hexaddr [7];    /**< Printable ICAO address */
       char     flight [9];     /**< Flight number */
       int      altitude;       /**< Altitude */
       uint32_t speed;          /**< Velocity computed from EW and NS components. */
       int      heading;        /**< Horizontal angle of flight. */
       time_t   seen;           /**< Time at which the last packet was received. */
       long     messages;       /**< Number of Mode S messages received. */
       int      identity;       /**< 13 bits identity (Squawk). */

       /* Encoded latitude and longitude as extracted by odd and even
        * CPR encoded messages.
        */
       int      odd_CPR_lat;    /**< Encoded odd CPR latitude */
       int      odd_CPR_lon;    /**< Encoded odd CPR longitude */
       int      even_CPR_lat;   /**< Encoded even CPR latitude */
       int      even_CPR_lon;   /**< Encoded even CPR longitude */
       uint64_t odd_CPR_time;   /**< Tick-time for reception of an odd CPR message */
       uint64_t even_CPR_time;  /**< Tick-time for reception of an even CPR message */
       double   lat, lon;       /**< Coordinates obtained from decoded CPR data. */

       struct aircraft *next;   /**< Next aircraft in our linked list. */
     };

/**
 * \struct statistics
 * Keep all collected statistics in this structure.
 */
struct statistics {
       uint64_t valid_preamble;
       uint64_t demodulated;
       uint64_t good_CRC;
       uint64_t bad_CRC;
       uint64_t fixed;
       uint64_t single_bit_fix;
       uint64_t two_bits_fix;
       uint64_t out_of_phase;
       uint64_t unique_aircrafts;
       uint64_t unrecognized_ME;

       /* Network statistics:
        */
       uint64_t cli_removed  [MODES_NET_SERVICES_NUM];
       uint64_t cli_accepted [MODES_NET_SERVICES_NUM];
       uint64_t cli_unknown [MODES_NET_SERVICES_NUM];
       uint64_t bytes_sent [MODES_NET_SERVICES_NUM];
       uint64_t bytes_recv [MODES_NET_SERVICES_NUM];
       uint64_t HTTP_get_requests;
       uint64_t HTTP_keep_alive_recv;
       uint64_t HTTP_keep_alive_sent;
       uint64_t HTTP_websockets;
     };

/**
 * \struct global_data
 * All program global state is in this structure.
 */
struct global_data {
       char              who_am_I [MG_PATH_MAX];   /**< The full name of this program */
       char              where_am_I [MG_PATH_MAX]; /**< The current directory with a trailing `\\` */
       pthread_t         reader_thread;            /**< Device reader thread ID */
       pthread_mutex_t   data_mutex;               /**< Mutex to synchronize buffer access. */
       uint8_t          *data;                     /**< Raw IQ samples buffer */
       uint32_t          data_len;                 /**< Length of raw IQ buffer. */
       uint16_t         *magnitude;                /**< Magnitude vector */
       uint16_t         *magnitude_lut;            /**< I/Q -> Magnitude lookup table. */
       int               fd;                       /**< `--infile` option file descriptor. */
       volatile int      exit;                     /**< Exit from the main loop when true. */
       volatile int      data_ready;               /**< Data ready to be processed. */
       uint32_t         *ICAO_cache;               /**< Recently seen ICAO addresses. */
       struct statistics stat;                     /**< Decoding and network statistics */
       struct aircraft  *aircrafts;                /**< Linked list of active aircrafts */
       uint64_t          last_update_ms;           /**< Last screen update in milliseconds */

       int               dev_index;                /**< The index of the RTLSDR device used */
       int               gain;                     /**< The gain setting for this device. Default is MODES_AUTO_GAIN. */
       rtlsdr_dev_t     *dev;                      /**< The RTLSDR handle from `rtlsdr_open()` */
       uint32_t          freq;                     /**< The tuned frequency. Default is MODES_DEFAULT_FREQ. */
       uint32_t          sample_rate;              /**< The sample-rate. Default is MODES_DEFAULT_RATE. <br>
                                                     *  \note This cannot be used yet since the code assumes a
                                                     *        pulse-width of 0.5 usec based on a fixed rate of 2 MS/s.
                                                     */

       /** Lists of clients for each network service
        */
       struct client        *clients [MODES_NET_SERVICES_NUM];
       struct mg_connection *sbsos;       /**< SBS output listening connection. */
       struct mg_connection *ros;         /**< Raw output listening connection. */
       struct mg_connection *ris;         /**< Raw input listening connection. */
       struct mg_connection *http;        /**< HTTP listening connection. */
       struct mg_mgr         mgr;         /**< Only one connection manager */

       /* Configuration
        */
       const char *infile;                 /**< Input IQ samples from file with option `--infile file`. */
       uint64_t    loops;                  /**< Read input file in a loop. */
       int         fix_errors;             /**< Single bit error correction if true. */
       int         check_crc;              /**< Only display messages with good CRC. */
       int         raw;                    /**< Raw output format. */
       int         debug;                  /**< Debugging mode. */
       int         net;                    /**< Enable networking. */
       int         net_only;               /**< Enable just networking. */
       int         interactive;            /**< Interactive mode */
       int         interactive_rows;       /**< Interactive mode: max number of rows. */
       int         interactive_ttl;        /**< Interactive mode: TTL before deletion. */
       int         only_addr;              /**< Print only ICAO addresses. */
       int         metric;                 /**< Use metric units. */
       int         aggressive;             /**< Aggressive detection algorithm. */
       char        web_page [MG_PATH_MAX]; /**< The base-name of the web-page to server for HTTP clients */
       char        web_root [MG_PATH_MAX]; /**< And it's directory */
       int         strip_level;            /**< For '--strip X' mode */
     };

struct global_data Modes;

/**
 * \struct modeS_message
 * The structure we use to store information about a decoded message.
 */
struct modeS_message {
    uint8_t  msg [MODES_LONG_MSG_BYTES]; /**< Binary message. */
    int      msg_bits;                   /**< Number of bits in message */
    int      msg_type;                   /**< Downlink format # */
    int      CRC_ok;                     /**< True if CRC was valid */
    uint32_t CRC;                        /**< Message CRC */
    int      error_bit;                  /**< Bit corrected. -1 if no bit corrected. */
    int      AA1, AA2, AA3;              /**< ICAO Address bytes 1, 2 and 3 */
    int      phase_corrected;            /**< True if phase correction was applied. */

    /** DF11
     */
    int ca;                      /**< Responder capabilities. */

    /** DF 17
     */
    int  ME_type;                /**< Extended squitter message type. */
    int  ME_subtype;             /**< Extended squitter message subtype. */
    int  heading_is_valid;
    int  heading;                /**< Horizontal angle of flight. */
    int  aircraft_type;
    int  odd_flag;               /**< 1 = Odd, 0 = Even CPR message. */
    int  UTC_flag;               /**< UTC synchronized? */
    int  raw_latitude;           /**< Non decoded latitude */
    int  raw_longitude;          /**< Non decoded longitude */
    char flight [9];             /**< 8 chars flight number. */
    int  EW_dir;                 /**< 0 = East, 1 = West. */
    int  EW_velocity;            /**< E/W velocity. */
    int  NS_dir;                 /**< 0 = North, 1 = South. */
    int  NS_velocity;            /**< N/S velocity. */
    int  vert_rate_source;       /**< Vertical rate source. */
    int  vert_rate_sign;         /**< Vertical rate sign. */
    int  vert_rate;              /**< Vertical rate. */
    int  velocity;               /**< Computed from EW and NS velocity. */

    /** DF4, DF5, DF20, DF21
     */
    int flight_status;           /**< Flight status for DF4, 5, 20 and 21 */
    int DR_status;               /**< Request extraction of downlink request. */
    int UM_status;               /**< Request extraction of downlink request. */
    int identity;                /**< 13 bits identity (Squawk). */

    /** Fields used by multiple message types.
     */
    int altitude, unit;
};

struct aircraft *interactive_receive_data (const struct modeS_message *mm);
void read_from_client (struct client *cli, char *sep, int (*handler)(struct client *));
void modeS_send_raw_output (const struct modeS_message *mm);
void modeS_send_SBS_output (const struct modeS_message *mm, struct aircraft *a);
void modeS_user_message (const struct modeS_message *mm);

int  fix_single_bit_errors (uint8_t *msg, int bits);
int  fix_two_bits_errors (uint8_t *msg, int bits);
void detect_modeS (uint16_t *m, uint32_t mlen);
int  decode_hex_message (struct client *c);
int  modeS_message_len_by_type (int type);
void compute_magnitude_vector (void);
void background_tasks (void);
void modeS_exit (void);
void sigint_handler (int sig);

u_short               handler_port (int service);
const char           *handler_descr (int service);
struct mg_connection *handler_conn (int service);

static CONSOLE_SCREEN_BUFFER_INFO console_info;
static HANDLE console_hnd = INVALID_HANDLE_VALUE;
static DWORD  console_mode = 0;

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

  while (y < console_info.srWindow.Bottom)
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

#if defined(USE_VLD)
void crtdbug_init (void)
{
  VLDSetReportOptions (VLD_OPT_REPORT_TO_STDOUT, NULL);  /* Force all reports to "stdout" in "ASCII" */
  VLDSetOptions (VLDGetOptions(), 100, 4);   /* Dump max 100 bytes data. And walk max 4 stack frames */
}
void crtdbug_exit (void)
{
}

#elif defined(_DEBUG)
static _CrtMemState last_state;

void crtdbug_exit (void)
{
  _CrtMemState new_state, diff_state;

  _CrtMemCheckpoint (&new_state);
  if (!_CrtMemDifference(&diff_state, &last_state, &new_state))
  {
    TRACE (DEBUG_GENERAL, "No leaks detected.\n");
    return;
  }
  TRACE (DEBUG_GENERAL, "Leak report:\n");
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
#endif  /* USE_VLD */

/**
 * Convert standard suffixes (k, M, G) to double
 *
 * \param in Hertz   a string to be parsed
 * \retval the fereuency as a `double`
 * \note Taken from Osmo-SDR's `convenience.c` and modified.
 */
double ato_hertz (const char *Hertz)
{
  char   tmp [20], last_ch;
  int    len;
  double multiplier = 1.0;

  strncpy (tmp, Hertz, sizeof(tmp));
  len = strlen (tmp);
  last_ch = tmp [len-1];
  tmp [len-1] = '\0';

  switch (last_ch)
  {
    case 'g':
    case 'G':
          multiplier *= 1E3;
          /* fall-through */
    case 'm':
    case 'M':
          multiplier *= 1E3;
          /* fall-through */
    case 'k':
    case 'K':
          multiplier *= 1E3;
          break;
    default:
          break;
  }
  tmp [len-1] = last_ch;
  return (multiplier * atof(tmp));
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
 * Return TRUE if string `s1` starts with `s2`.
 *
 * Ignore casing of boith strings.
 * And drop leading blanks in `s1` first.
 */
bool str_startswith (const char *s1, const char *s2)
{
  size_t s1_len = strlen (s1);
  size_t s2_len = strlen (s2);

  if (s2_len > s1_len)
     return (FALSE);

  if (!strncmp(s1, s2, s2_len))
     return (true);
  return (false);
}

/**
 * Return a string describing an error-code from RTLSDR
 *
 * This can be from `librtlsdr` itself or from `libusb`.
 */
const char *get_rtlsdr_libusb_error (int err)
{
  extern const char *WINAPI libusb_error_name (int);

  if (err >= 0)
     return ("No error");
  if (err == -ENOMEM)
     return strerror (-err);
  return libusb_error_name (err);
}

void verbose_gain_set (rtlsdr_dev_t *dev, int gain)
{
  int r = rtlsdr_set_tuner_gain_mode(dev, 1);

  if (r < 0)
  {
    fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
    return;
  }
  r = rtlsdr_set_tuner_gain (dev, gain);
  if (r)
       fprintf (stderr, "WARNING: Failed to set tuner gain.\n");
  else fprintf (stderr, "Tuner gain set to %0.2f dB.\n", gain/10.0);
}

void verbose_auto_gain (rtlsdr_dev_t *dev)
{
  int r = rtlsdr_set_tuner_gain_mode (dev, 0);

  if (r != 0)
       fprintf (stderr, "WARNING: Failed to enable automatic gain.\n");
  else fprintf (stderr, "Tuner gain set to automatic.\n");
}

int nearest_gain (rtlsdr_dev_t *dev, int target_gain)
{
  int  i, r, err1, err2, count, nearest;
  int *gains;

  r = rtlsdr_set_tuner_gain_mode (dev, 1);
  if (r < 0)
  {
    fprintf (stderr, "WARNING: Failed to enable manual gain.\n");
    return (r);
  }

  count = rtlsdr_get_tuner_gains (dev, NULL);
  if (count <= 0)
     return (0);

  gains = alloca (sizeof(int) * count);
  count = rtlsdr_get_tuner_gains (dev, gains);
  nearest = gains[0];
  for (i = 0; i < count; i++)
  {
    err1 = abs (target_gain - nearest);
    err2 = abs (target_gain - gains[i]);
    if (err2 < err1)
       nearest = gains[i];
  }
  return (nearest);
}


/**
 * Step 1: Initialize the program with default values.
 */
void modeS_init_config (void)
{
  GetCurrentDirectoryA (sizeof(Modes.where_am_I), Modes.where_am_I);
  GetModuleFileNameA (NULL, Modes.who_am_I, sizeof(Modes.who_am_I));

  strcpy (Modes.web_page, basename(GMAP_HTML));
  strcpy (Modes.web_root, dirname(Modes.who_am_I));
  strcat (Modes.web_root, "\\web_root");

  memset (&Modes.stat, '\0', sizeof(Modes.stat));
  Modes.gain = MODES_AUTO_GAIN;
  Modes.dev_index = 0;
  Modes.sample_rate = MODES_DEFAULT_RATE;
  Modes.freq = MODES_DEFAULT_FREQ;
  Modes.infile = NULL;
  Modes.fix_errors = 1;
  Modes.check_crc = 1;
  Modes.raw = 0;
  Modes.net = 0;
  Modes.net_only = 0;
  Modes.only_addr = 0;
  Modes.debug = 0;
  Modes.exit = 0;
  Modes.aggressive = 0;
  Modes.interactive = 0;
  Modes.interactive_ttl = MODES_INTERACTIVE_TTL;
  Modes.reader_thread = PTHREAD_NULL;
  Modes.strip_level = 0;
  Modes.interactive_rows = 0;  /* set in `console_init()` in `--interactive` mode */
  Modes.loops = 0;
  Modes.aircrafts = NULL;
  Modes.last_update_ms = 0;
}

/**
 * Step 2:
 *  \li In `--net` mode, check the precence of the Web-page.
 *  \li Initialize the `Modes.data_mutex`.
 *  \li Setup a SIGINT handler for a clean exit.
 *  \li Allocate and initialize the needed buffers.
 */
int modeS_init (void)
{
  int i, q, rc;

  if (Modes.net)
  {
    struct stat st;
    char   full_name [MG_PATH_MAX];

    snprintf (full_name, sizeof(full_name), "%s\\%s", Modes.web_root, basename(Modes.web_page));
    TRACE (DEBUG_NET, "Full web-page: \"%s\"\n", full_name);

    if (stat(full_name, &st) != 0)
    {
      fprintf (stderr, "Web-page \"%s\" does not exist.\n", full_name);
      return (1);
    }
    if (((st.st_mode) & _S_IFMT) != _S_IFREG)
    {
      fprintf (stderr, "Web-page \"%s\" is not a regular file.\n", full_name);
      return (1);
    }
  }

  rc = pthread_mutex_init (&Modes.data_mutex, NULL);
  if (rc)
  {
    fprintf (stderr, "Failed to create mutex: %s.\n", strerror(rc));
    return (1);
  }

  signal (SIGINT, sigint_handler);

  /* We add a full message minus a final bit to the length, so that we
   * can carry the remaining part of the buffer that we can't process
   * in the message detection loop, back at the start of the next data
   * to process. This way we are able to also detect messages crossing
   * two reads.
   */
  Modes.data_len = MODES_DATA_LEN + 4*(MODES_FULL_LEN-1);
  Modes.data_ready = 0;

  /**
   * Allocate the ICAO address cache. We use two uint32_t for every
   * entry because it's a addr / timestamp pair for every entry.
   */
  Modes.ICAO_cache = calloc (sizeof(uint32_t)*MODES_ICAO_CACHE_LEN*2, 1);
  Modes.data = malloc (Modes.data_len);
  Modes.magnitude = malloc (2*Modes.data_len);

  if (!Modes.ICAO_cache || !Modes.data || !Modes.magnitude)
  {
    fprintf (stderr, "Out of memory allocating data buffer.\n");
    return (1);
  }

  memset (Modes.data, 127, Modes.data_len);

  /* Populate the I/Q -> Magnitude lookup table. It is used because
   * sqrt() or round() may be expensive and may vary a lot depending on
   * the CRT used.
   *
   * We scale to 0-255 range multiplying by 1.4 in order to ensure that
   * every different I/Q pair will result in a different magnitude value,
   * not losing any resolution.
   */
  Modes.magnitude_lut = malloc (sizeof(*Modes.magnitude_lut) * 129 * 129);
  if (!Modes.magnitude_lut)
  {
    fprintf (stderr, "Out of memory allocating `Modes.magnitude_lut`.\n");
    return (1);
  }

  for (i = 0; i <= 128; i++)
  {
    for (q = 0; q <= 128; q++)
        Modes.magnitude_lut [i*129+q] = (uint16_t) round (360 * sqrt(i*i + q*q));
  }

  if (Modes.debug == 0 && !Modes.raw && Modes.interactive)
     return console_init();
  return (0);
}

/**
 * Step 3: Initialize the RTLSDR device.
 */
int modeS_init_RTLSDR (void)
{
  int    i, rc, device_count, ppm_error = 0;
  char   vendor[256], product[256], serial[256];
  double gain;

  device_count = rtlsdr_get_device_count();
  if (!device_count)
  {
    fprintf (stderr, "No supported RTLSDR devices found.\n");
    return (1);
  }

  fprintf (stderr, "Found %d device(s):\n", device_count);
  for (i = 0; i < device_count; i++)
  {
    rtlsdr_get_device_usb_strings (i, vendor, product, serial);
    fprintf (stderr, "%d: %s, %s, SN: %s %s\n", i, vendor, product, serial,
             (i == Modes.dev_index) ? "(currently selected)" : "");
  }

  rc = rtlsdr_open (&Modes.dev, Modes.dev_index);
  if (rc < 0)
  {
    fprintf (stderr, "Error opening the RTLSDR device: %s.\n", get_rtlsdr_libusb_error(rc));
    return (1);
  }

  /* Set gain, frequency, sample rate, and reset the device.
   */
  if (Modes.gain == MODES_AUTO_GAIN)
     verbose_auto_gain (Modes.dev);
  else
  {
    Modes.gain = nearest_gain (Modes.dev, Modes.gain);
    verbose_gain_set (Modes.dev, Modes.gain);
  }

  rtlsdr_set_freq_correction (Modes.dev, ppm_error);
  rtlsdr_set_agc_mode (Modes.dev, 1);

  rtlsdr_set_center_freq (Modes.dev, Modes.freq);
  rtlsdr_set_sample_rate (Modes.dev, Modes.sample_rate);
  rtlsdr_reset_buffer (Modes.dev);

  fprintf (stderr, "Tuned to %.03f MHz. ", Modes.freq/1E6);

  gain = rtlsdr_get_tuner_gain (Modes.dev);
  if ((unsigned int)gain == 0)
       fprintf (stderr, "Gain reported by device: AUTO.\n");
  else fprintf (stderr, "Gain reported by device: %.2f dB.\n", gain/10.0);
  return (0);
}

/**
 * This reading callback gets data from the RTLSDR API asynchronously.
 * We then populate the data buffer. <br>
 * A Mutex is used to avoid race-condition with the decoding thread.
 */
void rtlsdr_callback (uint8_t *buf, uint32_t len, void *ctx)
{
  MODES_NOTUSED (ctx);

  if (Modes.exit)
     return;

  pthread_mutex_lock (&Modes.data_mutex);
  if (len > MODES_DATA_LEN)
     len = MODES_DATA_LEN;

  /* Move the last part of the previous buffer, that was not processed,
   * to the start of the new buffer.
   */
  memcpy (Modes.data, Modes.data+MODES_DATA_LEN, 4*(MODES_FULL_LEN-1));

  /* Read the new data.
   */
  memcpy (Modes.data + 4*(MODES_FULL_LEN-1), buf, len);
  Modes.data_ready = 1;
  pthread_mutex_unlock (&Modes.data_mutex);
}

/**
 * This is used when `--infile` is specified in order to read data from file
 * instead of using an RTLSDR device.
 */
int read_from_data_file (void)
{
  if (Modes.loops > 0 && Modes.fd == STDIN_FILENO)
  {
    fprintf (stderr, "Option `--loop <N>` not supported for `stdin`.\n");
    Modes.loops = 0;
  }

  do
  {
     int nread, toread;
     uint8_t *p;

     if (Modes.interactive)
     {
       /* When --infile and --interactive are used together, slow down
        * playing at the natural rate of the RTLSDR received.
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
       nread = read (Modes.fd, p, toread);
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

     compute_magnitude_vector();
     detect_modeS (Modes.magnitude, Modes.data_len/2);
     background_tasks();

     if (Modes.exit || Modes.fd == STDIN_FILENO)
        break;

     /* seek the file again from the start
      * and re-play it if --loop was given.
      */
     if (Modes.loops > 0)
        Modes.loops--;
     if (Modes.loops == 0 || lseek(Modes.fd, 0, SEEK_SET) == -1)
        break;
  }
  while (1);
  return (0);  /**\todo add a check for errors above */
}

/**
 * We read RTLSDR data using a separate thread, so the main thread only handles decoding
 * without caring about data acquisition. Ref. `main_data_loop()` below.
 */
void *data_thread_fn (void *arg)
{
  int rc;

  MODES_NOTUSED (arg);

  if (Modes.dev)
  {
    TRACE (DEBUG_GENERAL, "Calling rtlsdr_read_async().\n");

    rc = rtlsdr_read_async (Modes.dev, rtlsdr_callback, NULL,
                            MODES_ASYNC_BUF_NUMBER, MODES_DATA_LEN);
    TRACE (DEBUG_GENERAL, "rtlsdr_read_async(): rc: %d/%s.\n",
           rc, get_rtlsdr_libusb_error(rc));
  }

  pthread_exit (NULL);
  return (NULL);
}

/**
 * Main data processing loop. <br>
 * This runs in the main thrad of the program.
 */
void main_data_loop (void)
{
  while (!Modes.exit)
  {
    background_tasks();

    if (!Modes.data_ready)
       continue;

    compute_magnitude_vector();

    /* Signal to the other thread that we processed the available data
     * and we want more.
     */
    Modes.data_ready = 0;

    /* Process data after releasing the lock, so that the capturing
     * thread can read data while we perform computationally expensive
     * stuff * at the same time. (This should only be useful with very
     * slow processors).
     */
    pthread_mutex_lock (&Modes.data_mutex);
    detect_modeS (Modes.magnitude, Modes.data_len/2);
    pthread_mutex_unlock (&Modes.data_mutex);
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
void dump_magnitude_bar (int index, int magnitude)
{
  const char *set = " .-o";
  char  buf [256];
  int   div = magnitude / 256 / 4;
  int   rem = magnitude / 256 % 4;

  memset (buf, 'O', div);
  buf[div] = set[rem];
  buf[div+1] = '\0';

  if (index >= 0)
  {
    int markchar = ']';

    /* preamble peaks are marked with ">"
     */
    if (index == 0 || index == 2 || index == 7 || index == 9)
       markchar = '>';

    /* Data peaks are marked to distinguish pairs of bits.
     */
    if (index >= 16)
       markchar = ((index-16)/2 & 1) ? '|' : ')';
    printf ("[%.3d%c |%-66s %d\n", index, markchar, buf, magnitude);
  }
  else
    printf ("[%.2d] |%-66s %d\n", index, buf, magnitude);
}

/**
 * Display an *ASCII-art* alike graphical representation of the undecoded
 * message as a magnitude signal.
 *
 * The message starts at the specified offset in the `m` buffer.
 * The function will display enough data to cover a short 56 bit message.
 *
 * If possible a few samples before the start of the messsage are included
 * for context.
 */
void dump_magnitude_vector (const uint16_t *m, uint32_t offset)
{
  uint32_t padding = 5;  /* Show a few samples before the actual start. */
  uint32_t start = (offset < padding) ? 0 : offset-padding;
  uint32_t end = offset + (2*MODES_PREAMBLE_US) + (2*MODES_SHORT_MSG_BITS) - 1;
  uint32_t j;

  for (j = start; j <= end; j++)
      dump_magnitude_bar (j-offset, m[j]);
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
    fprintf (stderr, "Error opening frames.js: %s\n", strerror(errno));
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

  printf ("\n--- %s\n    ", descr);
  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
  {
    printf ("%02x",msg[j]);
    if (j == MODES_SHORT_MSG_BYTES-1)
       printf (" ... ");
  }
  printf (" (DF %d, Fixable: %d)\n", msg_type, fixable);
  dump_magnitude_vector (m, offset);
  printf ("---\n\n");
}

/**
 * Parity table for MODE S Messages. s<br>
 * The table contains 112 elements, every element corresponds to a bit set
 * in the message, starting from the first bit of actual data after the
 * preamble.
 *
 * For messages of 112 bit, the whole table is used.
 * For messages of 56 bits only the last 56 elements are used.
 *
 * The algorithm is as simple as xoring all the elements in this table
 * for which the corresponding bit on the message is set to 1.
 *
 * The latest 24 elements in this table are set to 0 as the checksum at the
 * end of the message should not affect the computation.
 *
 * \note
 * This function can be used with DF11 and DF17. Other modes have
 * the CRC *XORed* with the sender address as they are replies to interrogations,
 * but a casual listener can't split the address from the checksum.
 */
const uint32_t modeS_checksum_table[112] = {
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
  int      offset = (bits == 112) ? 0 : (112 - 56);
  int      j;

  for (j = 0; j < bits; j++)
  {
    int byte = j / 8;
    int bit  = j % 8;
    int bitmask = 1 << (7 - bit);

    /* If bit is set, XOR with corresponding table entry. */
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
     return MODES_LONG_MSG_BITS;
  return (MODES_SHORT_MSG_BITS);
}

/**
 * Try to fix single bit errors using the checksum. On success modifies
 * the original buffer with the fixed version, and returns the position
 * of the error bit. Otherwise if fixing failed -1 is returned.
 */
int fix_single_bit_errors (uint8_t *msg, int bits)
{
  int j;
  uint8_t aux[MODES_LONG_MSG_BITS/8];

  for (j = 0; j < bits; j++)
  {
    int      byte = j / 8;
    int      bitmask = 1 << (7-(j % 8));
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
      return (j);
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
int brute_force_AP (uint8_t *msg, struct modeS_message *mm)
{
  uint8_t aux [MODES_LONG_MSG_BYTES];
  int     msg_type = mm->msg_type;
  int     msg_bits = mm->msg_bits;

  if (msg_type == 0 ||         /* Short air surveillance */
      msg_type == 4 ||         /* Surveillance, altitude reply */
      msg_type == 5 ||         /* Surveillance, identity reply */
      msg_type == 16 ||        /* Long Air-Air survillance */
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
    aux [last_byte] ^= crc & 0xFF;
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
    /**\todo Implement altitude when meter unit is selected.
     */
  }
  return (0);
}

/*
 * Decode the 12 bit AC altitude field (in DF 17 and others).
 * Returns the altitude or 0 if it can't be decoded.
 */
int decode_AC12_field (uint8_t *msg, int *unit)
{
  int q_bit = msg[5] & 1;

  if (q_bit)
  {
    /* N is the 11 bit integer resulting from the removal of bit Q
     */
    *unit = MODES_UNIT_FEET;
    int n = ((msg[5] >> 1) << 4) | ((msg[6] & 0xF0) >> 4);

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
    /* 0 */ "Level 1 (Survillance Only)",
    /* 1 */ "Level 2 (DF0,4,5,11)",
    /* 2 */ "Level 3 (DF0,4,5,11,20,21)",
    /* 3 */ "Level 4 (DF0,4,5,11,20,21,24)",
    /* 4 */ "Level 2+3+4 (DF0,4,5,11,20,21,24,code7 - is on ground)",
    /* 5 */ "Level 2+3+4 (DF0,4,5,11,20,21,24,code7 - is on airborne)",
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

const char *get_ME_description (const struct modeS_message *mm)
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
 * Decode a raw Mode S message demodulated as a stream of bytes by `detect_modeS()`. <br>
 * And split it into fields populating a `modeS_message` structure.
 */
void decode_modeS_message (struct modeS_message *mm, uint8_t *msg)
{
  uint32_t crc2;   /* Computed CRC, used to verify the message CRC. */
  char    *AIS_charset = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ????? ???????????????0123456789??????";

  /* Work on our local copy
   */
  memcpy (mm->msg, msg, MODES_LONG_MSG_BYTES);
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

  if (!mm->CRC_ok && Modes.fix_errors && (mm->msg_type == 11 || mm->msg_type == 17))
  {
    mm->error_bit = fix_single_bit_errors (msg, mm->msg_bits);
    if (mm->error_bit != -1)
    {
      mm->CRC = modeS_checksum (msg, mm->msg_bits);
      mm->CRC_ok = 1;
    }
    else if (Modes.aggressive && mm->msg_type == 17 && (mm->error_bit = fix_two_bits_errors(msg, mm->msg_bits)) != -1)
    {
      mm->CRC = modeS_checksum (msg, mm->msg_bits);
      mm->CRC_ok = 1;
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
  mm->ME_type = msg[4] >> 3;     /* Extended squitter message type. */
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
      mm->CRC_ok = 1;
    }
    else
      mm->CRC_ok = 0;
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
      mm->raw_latitude = ((msg[6] & 3) << 15) | (msg[7] << 7) | (msg[8] >> 1);
      mm->raw_longitude = ((msg[8] & 1) << 16) | (msg[9] << 8) | msg[10];
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
          int ewv = mm->EW_velocity;
          int nsv = mm->NS_velocity;
          double heading;

          if (mm->EW_dir)
             ewv *= -1;
          if (mm->NS_dir)
             nsv *= -1;
          heading = atan2 (ewv, nsv);

          /* Convert to degrees.
           */
          mm->heading = (int) (heading * 360 / TWO_PI);

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
  mm->phase_corrected = 0; /* Set to 1 by the caller if needed. */
}

/**
 * This function gets a decoded Mode S Message and prints it on the screen
 * in a human readable format.
 */
void display_modeS_message (const struct modeS_message *mm)
{
  int j;

  /* Handle only addresses mode first.
   */
  if (Modes.only_addr)
  {
    printf ("%02x%02x%02x\n", mm->AA1, mm->AA2, mm->AA3);
    return;
  }

  /* Show the raw message.
   */
  printf ("*");
  for (j = 0; j < mm->msg_bits/8; j++)
      printf ("%02x", mm->msg[j]);
  printf (";\n");

  if (Modes.raw)
  {
    fflush (stdout);  /* Provide data to the reader ASAP. */
    return;           /* Enough for --raw mode */
  }

  printf ("CRC: %06x (%s)\n", (int)mm->CRC, mm->CRC_ok ? "ok" : "wrong");
  if (mm->error_bit != -1)
     printf ("Single bit error fixed, bit %d\n", mm->error_bit);

  if (mm->msg_type == 0)
  {
    /* DF 0 */
    printf ("DF 0: Short Air-Air Surveillance.\n");
    printf ("  Altitude       : %d %s\n", mm->altitude, mm->unit == MODES_UNIT_METERS ? "meters" : "feet");
    printf ("  ICAO Address   : %02x%02x%02x\n", mm->AA1, mm->AA2, mm->AA3);
  }
  else if (mm->msg_type == 4 || mm->msg_type == 20)
  {
    printf ("DF %d: %s, Altitude Reply.\n", mm->msg_type, mm->msg_type == 4 ? "Surveillance" : "Comm-B");
    printf ("  Flight Status  : %s\n", flight_status_str [mm->flight_status]);
    printf ("  DR             : %d\n", mm->DR_status);
    printf ("  UM             : %d\n", mm->UM_status);
    printf ("  Altitude       : %d %s\n", mm->altitude, mm->unit == MODES_UNIT_METERS ? "meters" : "feet");
    printf ("  ICAO Address   : %02x%02x%02x\n", mm->AA1, mm->AA2, mm->AA3);

    if (mm->msg_type == 20)
    {
      /** \todo 56 bits DF20 MB additional field. */
    }
  }
  else if (mm->msg_type == 5 || mm->msg_type == 21)
  {
    printf ("DF %d: %s, Identity Reply.\n", mm->msg_type, mm->msg_type == 5 ? "Surveillance" : "Comm-B");
    printf ("  Flight Status  : %s\n", flight_status_str [mm->flight_status]);
    printf ("  DR             : %d\n", mm->DR_status);
    printf ("  UM             : %d\n", mm->UM_status);
    printf ("  Squawk         : %d\n", mm->identity);
    printf ("  ICAO Address   : %02x%02x%02x\n", mm->AA1, mm->AA2, mm->AA3);

    if (mm->msg_type == 21)
    {
      /** \todo 56 bits DF21 MB additional field. */
    }
  }
  else if (mm->msg_type == 11)
  {
    /* DF 11 */
    printf ("DF 11: All Call Reply.\n");
    printf ("  Capability  : %s\n", capability_str[mm->ca]);
    printf ("  ICAO Address: %02x%02x%02x\n", mm->AA1, mm->AA2, mm->AA3);
  }
  else if (mm->msg_type == 17)
  {
    /* DF 17 */
    printf ("DF 17: ADS-B message.\n");
    printf ("  Capability     : %d (%s)\n", mm->ca, capability_str[mm->ca]);
    printf ("  ICAO Address   : %02x%02x%02x\n", mm->AA1, mm->AA2, mm->AA3);
    printf ("  Extended Squitter  Type: %d\n", mm->ME_type);
    printf ("  Extended Squitter  Sub : %d\n", mm->ME_subtype);
    printf ("  Extended Squitter  Name: %s\n", get_ME_description(mm));

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
      printf ("    Aircraft Type  : %s\n", ac_type_str[mm->aircraft_type]);
      printf ("    Identification : %s\n", mm->flight);
    }
    else if (mm->ME_type >= 9 && mm->ME_type <= 18)
    {
      printf ("    F flag   : %s\n", mm->odd_flag ? "odd" : "even");
      printf ("    T flag   : %s\n", mm->UTC_flag ? "UTC" : "non-UTC");
      printf ("    Altitude : %d feet\n", mm->altitude);
      printf ("    Latitude : %d (not decoded)\n", mm->raw_latitude);
      printf ("    Longitude: %d (not decoded)\n", mm->raw_longitude);
    }
    else if (mm->ME_type == 19 && mm->ME_subtype >= 1 && mm->ME_subtype <= 4)
    {
      if (mm->ME_subtype == 1 || mm->ME_subtype == 2)
      {
        /* Velocity */
        printf ("    EW direction      : %d\n", mm->EW_dir);
        printf ("    EW velocity       : %d\n", mm->EW_velocity);
        printf ("    NS direction      : %d\n", mm->NS_dir);
        printf ("    NS velocity       : %d\n", mm->NS_velocity);
        printf ("    Vertical rate src : %d\n", mm->vert_rate_source);
        printf ("    Vertical rate sign: %d\n", mm->vert_rate_sign);
        printf ("    Vertical rate     : %d\n", mm->vert_rate);
      }
      else if (mm->ME_subtype == 3 || mm->ME_subtype == 4)
      {
        printf ("    Heading status: %d", mm->heading_is_valid);
        printf ("    Heading: %d", mm->heading);
      }
    }
    else
    {
      printf ("    Unrecognized ME type: %d subtype: %d\n", mm->ME_type, mm->ME_subtype);
      Modes.stat.unrecognized_ME++;
    }
  }
  else
  {
    if (Modes.check_crc)
       printf ("DF %d with good CRC received (decoding still not implemented).\n", mm->msg_type);
  }
}

/**
 * Turn I/Q samples pointed by `Modes.data` into the magnitude vector
 * pointed by `Modes.magnitude`.
 */
void compute_magnitude_vector (void)
{
  uint16_t *m = Modes.magnitude;
  uint32_t  j;
  const uint8_t *p = Modes.data;

  /* Compute the magnitudo vector. It's just `sqrt(I^2 + Q^2)`, but
   * we rescale to the 0-255 range to exploit the full resolution.
   */
  for (j = 0; j < Modes.data_len; j += 2)
  {
    int i = p[j] - 127;
    int q = p[j+1] - 127;

    if (i < 0)
       i = -i;
    if (q < 0)
       q = -q;
    m [j / 2] = Modes.magnitude_lut [129*i + q];
  }
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
void detect_modeS (uint16_t *m, uint32_t mlen)
{
  uint8_t  bits [MODES_LONG_MSG_BITS];
  uint8_t  msg [MODES_LONG_MSG_BITS/2];
  uint16_t aux [MODES_LONG_MSG_BITS*2];
  uint32_t j;
  int      use_correction = 0;

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
    int low, high, delta, i, errors;
    int good_message = 0;

    if (Modes.exit)
       break;

    if (use_correction)
       goto good_preamble; /* We already checked it. */

    /* First check of relations between the first 10 samples
     * representing a valid preamble. We don't even investigate further
     * if this simple test is not passed.
     */
    if (!(m[j] > m[j+1] &&
        m[j+1] < m[j+2] &&
        m[j+2] > m[j+3] &&
        m[j+3] < m[j] &&
        m[j+4] < m[j] &&
        m[j+5] < m[j] &&
        m[j+6] < m[j] &&
        m[j+7] > m[j+8] &&
        m[j+8] < m[j+9] &&
        m[j+9] > m[j+6]))
    {
      if (Modes.debug & DEBUG_NOPREAMBLE && m[j] > DEBUG_NOPREAMBLE_LEVEL)
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
      if (Modes.debug & DEBUG_NOPREAMBLE && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 3 and 6", msg, m, j);
      continue;
    }

    /* Similarly samples in the range 11-14 must be low, as it is the
     * space between the preamble and real data. Again we don't test
     * bits too near to high levels, see above.
     */
    if (m[j+11] >= high || m[j+12] >= high || m[j+13] >= high || m[j+14] >= high)
    {
      if (Modes.debug & DEBUG_NOPREAMBLE && m[j] > DEBUG_NOPREAMBLE_LEVEL)
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
      use_correction = 0;
      continue;
    }

    /* If we reached this point, and error is zero, we are very likely
     * with a Mode S message in our hands, but it may still be broken
     * and CRC may not be correct. This is handled by the next layer.
     */
    if (errors == 0 || (Modes.aggressive && errors < 3))
    {
      struct modeS_message mm;

      /* Decode the received message and update statistics
       */
      decode_modeS_message (&mm, msg);

      /* Update statistics. */
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
      if (use_correction == 0)
      {
        if (Modes.debug & DEBUG_DEMOD)
           dump_raw_message ("Demodulated with 0 errors", msg, m, j);

        else if (Modes.debug & DEBUG_BADCRC && mm.msg_type == 17 && (!mm.CRC_ok || mm.error_bit != -1))
           dump_raw_message ("Decoded with bad CRC", msg, m, j);

        else if (Modes.debug & DEBUG_GOODCRC && mm.CRC_ok && mm.error_bit == -1)
           dump_raw_message ("Decoded with good CRC", msg, m, j);
      }

      /* Skip this message if we are sure it's fine.
       */
      if (mm.CRC_ok)
      {
        j += 2 * (MODES_PREAMBLE_US + (8 * msg_len));
        good_message = 1;
        if (use_correction)
           mm.phase_corrected = 1;
      }

      /* Pass data to the next layer
       */
      modeS_user_message (&mm);
    }
    else
    {
      if (Modes.debug & DEBUG_DEMODERR && use_correction)
      {
        printf ("The following message has %d demod errors\n", errors);
        dump_raw_message ("Demodulated with errors", msg, m, j);
      }
    }

    /* Retry with phase correction if possible.
     */
    if (!good_message && !use_correction)
    {
      j--;
      use_correction = 1;
    }
    else
    {
      use_correction = 0;
    }
  }
}

/**
 * When a new message is available, because it was decoded from the
 * RTL device, file, or received in the TCP input port, or any other
 * way we can receive a decoded message, we call this function in order
 * to use the message.
 *
 * Basically this function passes a raw message to the upper layers for
 * further processing and visualization.
 */
void modeS_user_message (const struct modeS_message *mm)
{
  if (Modes.check_crc == 0 || mm->CRC_ok)
  {
    /* Track aircrafts in interactive mode or if we have some HTTP / SBS clients.
     */
    uint64_t num_clients = Modes.stat.cli_accepted [MODES_NET_SERVICE_HTTP] +
                           Modes.stat.cli_accepted [MODES_NET_SERVICE_SBS];
    if (Modes.interactive || num_clients > 0)
    {
      struct aircraft *a = interactive_receive_data (mm);

      if (a && Modes.stat.cli_accepted[MODES_NET_SERVICE_SBS] > 0)
         modeS_send_SBS_output (mm, a);     /* Feed SBS output clients. */
    }

    /* In non-interactive way, display messages on standard output.
     */
    if (!Modes.interactive)
    {
      display_modeS_message (mm);
      if (!Modes.raw && !Modes.only_addr)
         printf ("\n");
    }

    /* Send data to connected clients.
     */
    if (Modes.net)
       modeS_send_raw_output (mm);
  }
}

/**
 * Create a new dynamic aircraft structure.<br>
 * Store the printable hex-address as 6 digits since an ICAO address should never
 * contain more than 24 bits.
 *
 * \param in addr  the specific ICAO address.
 */
struct aircraft *create_aircraft (uint32_t addr)
{
  struct aircraft *a = calloc (sizeof(*a), 1);

  if (a)
  {
    a->addr = addr;
    snprintf (a->hexaddr, sizeof(a->hexaddr), "%06X", (int)addr);
    a->seen = MSEC_TIME() / 1000;
  }
  return (a);
}

/**
 * Return the aircraft with the specified ICAO address, or NULL if no aircraft
 * exists with this ICAO address.
 *
 * \param in addr  the specific ICAO address.
 */
struct aircraft *find_aircraft (uint32_t addr)
{
  struct aircraft *a = Modes.aircrafts;

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
int num_aircrafts (void)
{
  struct aircraft *a = Modes.aircrafts;
  int    num;

  for (num = 0; a; num++)
      a = a->next;
  return (num);
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
  return res;
}

/**
 * Helper function for decoding the **CPR** (*Compact Position Reporting*). <br>
 *
 * Calculates **NL** *(lat)*; *Number of Longitude* zone. <br>
 * Given the latitude, this function returns the number of longitude zones between 1 and 59. <br>
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
void decode_CPR (struct aircraft *a)
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

  /* Check that both are in the same latitude zone, or abort.
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
    a->lon = CPR_Dlong_func (rlat0, 0) * (CPR_mod_func(m, ni) + lon0/131072);
    a->lat = rlat0;
  }
  else
  {
    /* Use odd packet */
    int ni = CPR_N_func (rlat1, 1);
    int m  = (int) floor ((((lon0 * (CPR_NL_func(rlat1)-1)) -
                          (lon1 * CPR_NL_func(rlat1))) / 131072.0) + 0.5);
    a->lon = CPR_Dlong_func (rlat1, 1) * (CPR_mod_func (m, ni)+lon1/131072);
    a->lat = rlat1;
  }
  if (a->lon > 180)
     a->lon -= 360;
}

/**
 * Receive new messages and populate the interactive mode with more info.
 */
struct aircraft *interactive_receive_data (const struct modeS_message *mm)
{
  struct aircraft *a, *aux;
  uint32_t addr;

  if (Modes.check_crc && mm->CRC_ok == 0)
     return (NULL);

  addr = (mm->AA1 << 16) | (mm->AA2 << 8) | mm->AA3;

  /* Loookup our aircraft or create a new one.
   */
  a = find_aircraft (addr);
  if (!a)
  {
    a = create_aircraft (addr);
    if (!a)
       return (NULL);  /* Not fatal; there could be available memory later */
    a->next = Modes.aircrafts;
    Modes.aircrafts = a;

    /* We really can't tell if it's unique since we keep no global list of that yet
     */
    Modes.stat.unique_aircrafts++;
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
    if (0 && Modes.aircrafts != a && (MSEC_TIME()/1000 - a->seen) >= 1)
    {
      aux = Modes.aircrafts;
      while (aux->next != a)
         aux = aux->next;

      /* Now we are a node before the aircraft to remove.
       */
      aux->next = aux->next->next; /* removed. */

      /* Add on head */
      a->next = Modes.aircrafts;
      Modes.aircrafts = a;
    }
  }

  a->seen = MSEC_TIME() / 1000;
  a->messages++;

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
    }
    else if (mm->ME_type >= 9 && mm->ME_type <= 18)
    {
      a->altitude = mm->altitude;
      if (mm->odd_flag)
      {
        a->odd_CPR_lat  = mm->raw_latitude;
        a->odd_CPR_lon  = mm->raw_longitude;
        a->odd_CPR_time = MSEC_TIME();
      }
      else
      {
        a->even_CPR_lat  = mm->raw_latitude;
        a->even_CPR_lon  = mm->raw_longitude;
        a->even_CPR_time = MSEC_TIME();
      }

      /* If the two reports are less than 10 seconds apart, compute the position.
       */
      int64_t t_diff = (int64_t) (a->even_CPR_time - a->odd_CPR_time);

      if (llabs(t_diff) <= 10000)
         decode_CPR (a);
    }
    else if (mm->ME_type == 19)
    {
      if (mm->ME_subtype == 1 || mm->ME_subtype == 2)
      {
        a->speed   = mm->velocity;
        a->heading = mm->heading;
      }
    }
  }
  return (a);
}

/**
 * Show the currently captured aircraft information on screen.
 * \param in now     the currect tick-timer
 * \param in next_a  the next aircraft to be removed. We print this in RED
 *                   colour here to make this clearer.
 */
void interactive_show_data (uint64_t now, const struct aircraft *next_a)
{
  static int spin_idx = 0;
  static int old_count = -1;
  int    count = 0;
  char   spinner[] = "|/-\\";
  struct aircraft *a = Modes.aircrafts;

  /* Unless debug or raw-mode is active, clear the screen to remove old info.
   * But only if current number of aircrafts is less than last time. This is to
   * avoid an annoying blinking of the console.
   */
  if (Modes.debug == 0 && !Modes.raw)
  {
    if (old_count == -1 || old_count > num_aircrafts())
       clrscr();
    gotoxy (1, 1);
  }

  setcolor (COLOUR_WHITE);
  printf ("ICAO   Flight   Sqwk   Altitude  Speed    Lat       Long     Heading  Messages Seen %c\n"
          "-------------------------------------------------------------------------------------\n",
          spinner[spin_idx++ % 4]);
  setcolor (0);

  while (a && count < Modes.interactive_rows && !Modes.exit)
  {
    int  altitude = a->altitude;
    int  speed = a->speed;
    char alt_buf [10] = "  - ";
    char squawk [6] = "  - ";
    char lat_buf [10] = "  - ";
    char lon_buf [10] = "  - ";
    char speed_buf [8] = " - ";

    /* Convert units to metric if --metric was specified. */
    if (Modes.metric)
    {
      double altitudeM, speedKmH;

      altitudeM = (double) altitude / 3.2828;
      altitude  = (int) altitudeM;
      speedKmH  = (double) speed * 1.852;
      speed     = (int) speedKmH;
    }

    if (altitude)
       snprintf (alt_buf, sizeof(alt_buf), "%5d", altitude);

    if (a->identity)
       snprintf (squawk, 5, "%05d", a->identity);

    if (a->lat)
       snprintf (lat_buf, sizeof(lat_buf), "%.03f", a->lat);

    if (a->lon)
       snprintf (lon_buf, sizeof(lon_buf), "%.03f", a->lon);

    if (a->speed)
       snprintf (speed_buf, sizeof(speed_buf), "%d", a->speed);

    if (next_a)
       setcolor (COLOUR_RED);

    printf ("%-6s %-8s %-5s  %-5s     %-7s  %-7s %7s    %3d      %-8ld %2d sec  \n",
            a->hexaddr, a->flight, squawk, alt_buf, speed_buf,
            lat_buf, lon_buf, a->heading, a->messages, (int)(now - a->seen));

    if (next_a)
       setcolor (0);
    next_a = NULL;

    a = a->next;
    count++;
  }
  old_count = count;
}

/**
 * When in interactive mode, if we don't receive new nessages within
 * `Modes.interactive_ttl` seconds, we remove the aircraft from the list.
 *
 * \retval return a pointer to the aircraft that is to be removed
 *         on the next call to this function. So we can write this
 *         aircraft information in red colour.
 */
const struct aircraft *remove_stale_aircrafts (uint64_t now)
{
  struct aircraft *a = Modes.aircrafts;
  struct aircraft *prev = NULL;
  const struct aircraft *rc = NULL;
  uint32_t sec_now = (int) (now / 1000);

  while (a)
  {
    if ((sec_now - a->seen) > Modes.interactive_ttl)
    {
      struct aircraft *next = a->next;

      /* Remove the element from the linked list, with care
       * if we are removing the first element.
       */
      free (a);
      if (!prev)
           Modes.aircrafts = next;
      else prev->next = next;
      a = next;
    }
    else
    {
      if (sec_now - a->seen >= (Modes.interactive_ttl + MODES_INTERACTIVE_REFRESH_TIME/1000))
         rc = a;
      prev = a;
      a = a->next;
    }
  }
  return (rc);
}

/**
 * Remove all active aircrafts from the list.
 */
void free_all_aircrafts (void)
{
  struct aircraft *a = Modes.aircrafts;
  struct aircraft *prev = NULL;

  while (a)
  {
    struct aircraft *next = a->next;

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

  setmode (fileno(stdin), O_BINARY);
  setmode (fileno(stdout), O_BINARY);

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

/**
 * Return a malloced JSON description of the active planes.
 */
char *aircrafts_to_json (int *len, int *num_planes)
{
  struct aircraft *a = Modes.aircrafts;
  int   l, num = 0;
  int   buflen = 1024;        /* The initial buffer is incremented as needed. */
  char *buf = malloc (buflen);
  char *p = buf;

  l = snprintf (p, buflen, "[\n");
  p += l;
  buflen -= l;

  while (a)
  {
    int altitude = a->altitude;
    int speed = a->speed;

    /* Convert units to metric if --metric was specified.
     * But option '--metric' has no effect on the Web-page yet.
     */
    if (Modes.metric)
    {
      double altitudeM, speedKmH;

      altitudeM = (double) altitude / 3.2828;
      altitude  = (int) altitudeM;
      speedKmH  = (double) speed * 1.852;
      speed     = (int) speedKmH;
    }

    if (a->lat != 0 && a->lon != 0)
    {
      l = snprintf (p, buflen,
                    "{\"hex\":\"%s\", \"flight\":\"%s\", \"lat\":%f, "
                    "\"lon\":%f, \"altitude\":%d, \"track\":%d, "
                    "\"speed\":%d},\n",
                    a->hexaddr, a->flight, a->lat, a->lon, a->altitude, a->heading, a->speed);
      p += l;
      buflen -= l;
      num++;

      /* Resize if needed.
       */
      if (buflen < 256)
      {
        int used = p - buf;

        buflen += 1024; /* Our increment. */
        buf = realloc (buf, used+buflen);
        p = buf + used;
      }
    }
    a = a->next;
  }

  /* Remove the final comma if any, and closes the json array
   */
  if (p[-2] == ',')
  {
    p[-2] = '\n';
    p--;
    buflen++;
  }
  l = snprintf (p, buflen, "]\n");
  p += l;
  buflen -= l;

  *len = p - buf;
  *num_planes = num;
  return (buf);
}

/**
 * Returns a 'struct client *' based on the remote 'addr' and 'service'.
 */
struct client *get_client_addr (const struct mg_addr *addr, int service)
{
  struct client *cli;

  assert (service >= MODES_NET_SERVICE_RAW_OUT && service <= MODES_NET_SERVICE_HTTP);

  for (cli = Modes.clients[service]; cli; cli = cli->next)
  {
    if (cli->service == service && !memcmp(&cli->addr, addr, sizeof(cli->addr)))
       return (cli);
  }
  Modes.stat.cli_unknown [service]++;   /* Should never happen */
  return (NULL);
}

/**
 * Free a specific '_client'.
 */
void free_client (struct client *_cli, int service)
{
  struct client *cli;
  uint32_t cli_id = (uint32_t)-1;

  assert (_cli);

  for (cli = Modes.clients[service]; cli; cli = cli->next)
  {
    if (cli != _cli)
       continue;

    LIST_DELETE (struct client, &Modes.clients[service], cli);
    Modes.stat.cli_removed [service]++;
    cli_id = cli->id;
    free (cli);
    break;
  }
  TRACE (DEBUG_NET2, "Freeing client %u for service %d.\n", cli_id, service);
}

/*
 * Free all clients in all services.
 */
unsigned free_all_clients (void)
{
  int service, num = 0;

  for (service = MODES_NET_SERVICE_RAW_OUT; service <= MODES_NET_SERVICE_HTTP; service++)
  {
    struct client *cli, *cli_next;

    for (cli = Modes.clients[service]; cli; cli = cli_next)
    {
      cli_next = cli->next;
      free_client (cli, service);
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
 *  This function is not used for sending HTTP data.
 */
int send_all_clients (int service, const void *msg, size_t len)
{
  struct mg_connection *conn = handler_conn (service);
  struct client *cli, *cli_next;
  int    rc, found = 0;

  for (cli = Modes.clients[service]; cli; cli = cli_next)
  {
    cli_next = cli->next;   /* Since 'free_client()' could 'free(cli->next)' */

    if (cli->service != service)
       continue;

    rc = mg_send (cli->conn, msg, len);
    if (rc != len)
         free_client (cli, service);    /* write failed; assume client is gone */
    else found++;
    TRACE (DEBUG_NET, "Sent to client service \"%s\", rc: %d.\n", handler_descr(service), rc);
  }
  if (found == 0)
     TRACE (DEBUG_NET2, "No client found for service: \"%s\"\n", handler_descr(service));
  return (found);
}

/**
 * Handlers for the network services.
 *
 * We use Mongoose for handling all the server and low-level network I/O. <br>
 * We register event-handlers that gets called on important network events.
 *
 * Keep the data for our 4 network services in this structure.
 */
struct net_service modeS_net_services [MODES_NET_SERVICES_NUM] = {
                 { &Modes.ros,   "Raw TCP output",         MODES_NET_OUTPUT_RAW_PORT },
                 { &Modes.ris,   "Raw TCP input",          MODES_NET_INPUT_RAW_PORT },
                 { &Modes.sbsos, "Basestation TCP output", MODES_NET_OUTPUT_SBS_PORT },
                 { &Modes.http,  "HTTP server",            MODES_NET_HTTP_PORT }
               };

/* Mongoose event names.
 */
const char *event_name (int ev)
{
  return (ev == MG_EV_POLL       ? "MG_EV_POLL" :
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
          ev == MG_EV_WS_CTL     ? "MG_EV_WS_CTL" : "?");
}

struct mg_connection *handler_conn (int service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service <= MODES_NET_SERVICE_HTTP);
  return (*modeS_net_services [service].conn);
}

unsigned *handler_num_clients (int service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service <= MODES_NET_SERVICE_HTTP);
  return (&modeS_net_services [service].num_clients);
}

const char *handler_descr (int service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service <= MODES_NET_SERVICE_HTTP);
  return (modeS_net_services [service].descr);
}

u_short handler_port (int service)
{
  assert (service >= MODES_NET_SERVICE_RAW_OUT && service <= MODES_NET_SERVICE_HTTP);
  return (modeS_net_services [service].port);
}

void net_flushall (void)
{
  struct mg_connection *conn;
  unsigned num_active = 0;
  unsigned num_passive = 0;
  unsigned total_rx = 0;
  unsigned total_tx = 0;

  for (conn = Modes.mgr.conns; conn; conn = conn->next)
  {
    total_rx += conn->recv.len;
    total_tx += conn->send.len;

    mg_iobuf_free (&conn->recv);
    mg_iobuf_free (&conn->send);

    if (conn->is_accepted)
         num_active++;
    else num_passive++;
  }
  TRACE (DEBUG_NET,
         "%s(): flushed %u active connections. %u passive.\n"
         "      remaining bytes: %u Rx, %u Tx\n",
         __FUNCTION__, num_active, num_passive, total_rx, total_tx);
}

/**
 * \todo
 * The event handler for WebSocket control messages.
 */
void websocket_ctrl_handler (struct mg_connection *conn, const char *remote, int ev, void *ev_data)
{
  struct mg_ws_message *ws = ev_data;

  TRACE (DEBUG_NET, "Web-socket event %s from client at %s has %d bytes for us.\n",
         event_name(ev), remote, conn->recv.len);

  if (ev == MG_EV_WS_MSG)
  {
  }
  else if (ev == MG_EV_WS_CTL)
  {
    Modes.stat.HTTP_websockets++;
  }
}

/**
 * Return true if event is a WebSocket control event.
 * \note A `MG_EV_WS_OPEN` is handled like a `MG_EV_HTTP_*` event.
 */
bool is_websocket_ctrl_event (int ev)
{
  return (ev == MG_EV_WS_MSG || ev == MG_EV_WS_CTL);
}

/**
 * The event handler for HTTP traffic.
 */
void http_handler (struct mg_connection *conn, const char *remote, int ev, void *ev_data)
{
  struct mg_http_message *hm = ev_data;
  struct client          *cli;
  bool   data_json;
  const char *content;
  const char *uri;
  const char *keep_alive = "";
  const char *ext;
  char  *request, *end;

  if ((ev != MG_EV_HTTP_MSG && ev != MG_EV_HTTP_CHUNK) || strncmp(hm->head.ptr, "GET /", 5))
     return;

  Modes.stat.HTTP_get_requests++;
  request = strncpy (alloca(hm->head.len+1), hm->head.ptr, hm->head.len);

  uri = request + strlen("GET ");
  end = strchr (uri, ' ');
  if (!end)
  {
    TRACE (DEBUG_NET, "Bad request from %s: '%.20s'...\n\n", remote, hm->head.ptr);
    conn->is_closing = 1;
    return;
  }

  *end = '\0';
  data_json = (stricmp(request, "GET /data.json") == 0);

  cli = get_client_addr (&conn->peer, MODES_NET_SERVICE_HTTP);

  /* Do not trace these '/data.json' requests since it would be too much
   */
  if (!data_json)
     TRACE (DEBUG_NET, "'%s' from client %lu (cli: 0x%p) at %s.\n", request, conn->id, cli, remote);

  /* Redirect a 'GET /' to a 'GET /' + 'web_page'
   */
  if (!strcmp(request, "GET /"))
  {
    char redirect [10+MG_PATH_MAX];

    if (str_startswith(hm->proto.ptr, "HTTP/1.1"))
    {
      keep_alive = "Connection: keep-alive\r\n";
      Modes.stat.HTTP_keep_alive_recv++;
      cli->keep_alive = 1;
    }
    snprintf (redirect, sizeof(redirect), "Location: %s\r\n%s", basename(Modes.web_page), keep_alive);
    mg_http_reply (conn, 303, redirect, "");
    TRACE (DEBUG_NET, "Redirecting client %lu (cli: 0x%p): \"%s\"...\n\n", conn->id, cli, redirect);
    return;
  }

  if (data_json)
  {
    int   data_len, num_planes;
    char *data = aircrafts_to_json (&data_len, &num_planes);

    if (num_planes >= 1)
       TRACE (DEBUG_NET2, "Feeding client %lu (cli: 0x%p) with \"%s\", num_planes: %d.\n",
              conn->id, cli, uri, num_planes);

    /* This is rather inefficient way to pump data over to the client.
     * Better use a WebSocket instead.
     */
    mg_http_reply (conn, 200, MODES_CONTENT_TYPE_JSON "\r\n", data);
    free (data);
    return;
  }

  /**
   * \todo Check header for a "Upgrade: websocket" and call mg_ws_upgrade()?
   */
  if (!stricmp(request, "GET /echo"))
  {
    TRACE (DEBUG_NET, "Got Web-socket echo:\n'%.*s'.\n", hm->head.len, hm->head.ptr);
    mg_ws_upgrade (conn, hm, "WS test");
    return;
  }

  ext = strrchr (uri, '.');
  if (ext)
  {
    char file [MG_PATH_MAX];

    if (!stricmp(ext, ".html"))
       content = MODES_CONTENT_TYPE_HTML;
    else if (!stricmp(ext, ".css"))
       content = MODES_CONTENT_TYPE_CSS;
    else if (!stricmp(ext, ".js"))
       content = MODES_CONTENT_TYPE_JS;
    else if (!stricmp(ext, ".json"))
       content = MODES_CONTENT_TYPE_JSON;
    else if (!stricmp(ext, ".png"))
       content = MODES_CONTENT_TYPE_PNG;

    if (!stricmp(request, "GET /favicon.png"))
    {
      static const uint8_t favicon_array[] = {     /* generated array from a 'favicon.png' */
             #include "favicon.c"
           };

      TRACE (DEBUG_NET, "Sending \"favicon.png\" to cli: %lu.\n", conn->id);
      mg_printf (conn,
                 "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                 "Content-Length: %zu\r\n%s\r\n",
                 content, sizeof(favicon_array), cli->keep_alive ? "Connection: keep-alive\r\n" : "");
      mg_send (conn, favicon_array, sizeof(favicon_array));
    }
    else
    {
      snprintf (file, sizeof(file), "%s\\%s", Modes.web_root, uri+1);
      mg_http_serve_file (conn, hm, file, content,
                          cli->keep_alive ? "Connection: keep-alive\r\n" : "");
    }
    if (cli->keep_alive)
       Modes.stat.HTTP_keep_alive_sent++;

    TRACE (DEBUG_NET, "Serving HTTP client %lu (cli: 0x%p) with \"%s\".\n", conn->id, cli, uri);
    return;
  }

  mg_http_reply (conn, 404, "", "Not found\n");
  TRACE (DEBUG_NET, "HTTP 404 for URI: \"%s\".\n", uri);
}

/**
 * A hack to get the socket-fd from a
 * `struct mg_connection::fd` in mongoose.h.
 */
static SOCKET _ptr2sock (void *ptr)
{
  union {
    SOCKET s;
    void  *ptr;
  } u = { 0 };
  u.ptr = ptr;
  return (u.s);
}

/**
 * The event handler for ALL network I/O.
 */
void net_handler (struct mg_connection *conn, int ev, void *ev_data, void *fn_data)
{
  struct client *cli;
  char *remote, remote_buf [100];
  int   service = (int) fn_data;     /* 'fn_data' is arbitrary user data */

  if (Modes.exit)
     return;

  if (ev == MG_EV_POLL || ev == MG_EV_ERROR)  /* Ignore these events */
     return;

  if (ev == MG_EV_WRITE)
  {
    Modes.stat.bytes_sent[service] += *(const int*) ev_data;
    return;
  }

  remote = mg_straddr (conn, remote_buf, sizeof(remote_buf));

  if (ev == MG_EV_ACCEPT)
  {
    cli = calloc (sizeof(*cli), 1);
    cli->conn    = conn;      /* Keep a copy of the servicing connection */
    cli->service = service;
    cli->id      = conn->id;
    cli->addr    = conn->peer;

    LIST_ADD_HEAD (struct client, &Modes.clients[service], cli);
    ++ (*handler_num_clients (service));
    Modes.stat.cli_accepted [service]++;

    TRACE (DEBUG_NET, "New client %u (service \"%s\") from %s (socket %u).\n",
           cli->id, handler_descr(service), remote, _ptr2sock(conn->fd));
  }
  else if (ev == MG_EV_READ)
  {
    const struct mg_str *data = (const struct mg_str*) ev_data;

    Modes.stat.bytes_recv [service] += data->len;

    if (service == MODES_NET_SERVICE_RAW_IN)
    {
      cli = get_client_addr (&conn->peer, service);
      read_from_client (cli, "\n", decode_hex_message);
    }
  }
  else if (ev == MG_EV_CLOSE)
  {
    cli = get_client_addr (&conn->peer, service);
    TRACE (DEBUG_NET, "Freeing client %u for service %s.\n", cli->id, handler_descr(service));
    free_client (cli, service);
    -- (*handler_num_clients (service));
  }

  if (service == MODES_NET_SERVICE_HTTP)
  {
    if (conn->is_websocket && is_websocket_ctrl_event(ev))
       websocket_ctrl_handler (conn, remote, ev, ev_data);
    http_handler (conn, remote, ev, ev_data);
  }
}

/**
 * Initialize the Mongoose network manager and
 * start the 4 listening network services.
 */
int modeS_init_net (void)
{
  char url [50];

  mg_mgr_init (&Modes.mgr);

  snprintf (url, sizeof(url), "tcp://0.0.0.0:%u", modeS_net_services[MODES_NET_SERVICE_RAW_OUT].port);
  Modes.ros = mg_listen (&Modes.mgr, url, net_handler, (void*) MODES_NET_SERVICE_RAW_OUT);
  if (!Modes.ros)
     return (1);

  snprintf (url, sizeof(url), "tcp://0.0.0.0:%u", modeS_net_services[MODES_NET_SERVICE_RAW_IN].port);
  Modes.ris = mg_listen (&Modes.mgr, url, net_handler, (void*) MODES_NET_SERVICE_RAW_IN);
  if (!Modes.ris)
     return (1);

  snprintf (url, sizeof(url), "tcp://0.0.0.0:%u", modeS_net_services[MODES_NET_SERVICE_SBS].port);
  Modes.sbsos = mg_listen (&Modes.mgr, url, net_handler, (void*) MODES_NET_SERVICE_SBS);
  if (!Modes.sbsos)
     return (1);

  snprintf (url, sizeof(url), "http://0.0.0.0:%u", modeS_net_services[MODES_NET_SERVICE_HTTP].port);
  Modes.http = mg_http_listen (&Modes.mgr, url, net_handler, (void*) MODES_NET_SERVICE_HTTP);
  if (!Modes.http)
     return (1);
  return (0);
}

/**
 * Write raw output to TCP clients.
 */
void modeS_send_raw_output (const struct modeS_message *mm)
{
  char  msg [10 + MODES_LONG_MSG_BYTES];
  char *p = msg;

//TRACE (DEBUG_GENERAL, "sizeof(msg): %u, mm->msg_bits: %u\n", sizeof(msg), mm->msg_bits);

#if 1
  *p++ = '*';
  mg_hex (&mm->msg, mm->msg_bits/8, p);
  p = strchr (p, '\0');
#else
  static char hex[] = "0123456789abcdef--";
  int    i;

  *p++ = '*';
  for (i = 0; i < mm->msg_bits/8; i++)
  {
    uint8_t byte = mm->msg[i];
    *p++ = hex [byte >> 4];
    *p++ = hex [byte & 0x0F];
  }
#endif

  *p++ = ';';
  *p++ = '\n';

  send_all_clients (MODES_NET_SERVICE_RAW_OUT, msg, p - msg);
}

/**
 * Write SBS output to TCP clients (Base Station format).
 */
void modeS_send_SBS_output (const struct modeS_message *mm, struct aircraft *a)
{
  char msg[256], *p = msg;
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
    if (a->lat == 0 && a->lon == 0)
         p += sprintf (p, "MSG,3,,,%02X%02X%02X,,,,,,,%d,,,,,,,0,0,0,0",
                       mm->AA1, mm->AA2, mm->AA3, mm->altitude);
    else p += sprintf (p, "MSG,3,,,%02X%02X%02X,,,,,,,%d,,,%1.5f,%1.5f,,,0,0,0,0",
                       mm->AA1, mm->AA2, mm->AA3, mm->altitude, a->lat, a->lon);
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
  send_all_clients (MODES_NET_SERVICE_SBS, msg, p - msg);
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
 * raw hex format like: `*8D4B969699155600E87406F5B69F;`
 *
 * The string is supposed to be at the start of the client buffer
 * and NUL-terminated.
 *
 * The message is passed to the higher level layers, so it feeds
 * the selected screen output, the network output and so forth.
 *
 * If the message looks invalid is silently discarded.
 *
 * The function always returns 0 (success) to the caller as there is
 * no case where we want broken messages here to close the client
 * connection.
 */
int decode_hex_message (struct client *c)
{
  struct modeS_message mm;
  char *hex = c->buf;
  int   l = strlen (hex), j;
  uint8_t msg [MODES_LONG_MSG_BYTES];

  /* Remove spaces on the left and on the right.
   */
  while (l && isspace(hex[l-1]))
  {
    hex[l-1] = '\0';
    l--;
  }
  while (isspace(*hex))
  {
    hex++;
    l--;
  }

  /* Turn the message into binary.
   */
  if (l < 2 || hex[0] != '*' || hex[l-1] != ';')
     return (0);

  hex++;   /* Skip * and ; */
  l -= 2;
  if (l > MODES_LONG_MSG_BYTES*2)
     return (0); /* Too long message... broken. */

  for (j = 0; j < l; j += 2)
  {
    int high = hex_digit_val (hex[j]);
    int low  = hex_digit_val (hex[j+1]);

    if (high == -1 || low == -1)
       return (0);
    msg[j/2] = (high << 4) | low;
  }
  decode_modeS_message (&mm, msg);
  modeS_user_message (&mm);
  return (0);
}

#if !defined(USE_MONGOOSE)
/**
 * Get an HTTP request header and write the response to the client.
 * Again here we assume that the socket buffer is large enough without doing
 * any kind of userspace buffering.
 *
 * Returns 1 on error to signal the caller the client connection should
 * be closed.
 */
int handleHTTPRequest (struct client *c)
{
  char  hdr[512];
  int   clen, hdrlen;
  int   httpver, keepalive;
  char *p, *url, *content;
  char *ctype;

  TRACE (DEBUG_NET, "\nHTTP request: %s\n", c->buf);

  /* Minimally parse the request.
   */
  httpver = (strstr(c->buf, "HTTP/1.1") != NULL) ? 11 : 10;
  if (httpver == 10)
  {
    /* HTTP 1.0 defaults to close, unless "keep-alive" specified.
     */
    keepalive = strstr (c->buf, "Connection: keep-alive") != NULL;
  }
  else if (httpver == 11)
  {
    /* HTTP 1.1 defaults to "keep-alive", unless "close" is specified.
     */
    keepalive = strstr (c->buf, "Connection: close") == NULL;
  }

  /* Identify he URL. */
  p = strchr (c->buf,' ');

  if (!p)  /* There should be the method and a space... */
     return (1);

  url = ++p;    /* Now this should point to the requested URL. */
  p = strchr (p, ' ');
  if (!p)       /* There should be a space before HTTP/... */
     return (1);

  *p = '\0';

  TRACE (DEBUG_NET, "\nHTTP keep alive: %d\n", keepalive);
  TRACE (DEBUG_NET, "HTTP requested URL: %s\n\n", url);

  /* Select the content to send, we have just two so far:
   * "/" -> Our google map application.
   * "/data.json" -> Our ajax request to update planes.
   */
  if (strstr(url, "/data.json"))
  {
    content = aircrafts_to_son (&clen);
    ctype = MODES_CONTENT_TYPE_JSON;
  }
  else
  {
    struct stat sbuf;
    int fd = -1;

    if (stat(GMAP_HTML, &sbuf) != -1 && (fd = open(GMAP_HTML, O_RDONLY)) != -1)
    {
      content = malloc (sbuf.st_size);
      if (read(fd, content, sbuf.st_size) == -1)
        snprintf (content, sbuf.st_size, "Error reading from file: %s", strerror(errno));

      clen = sbuf.st_size;
    }
    else
    {
      char buf[128];

      clen = snprintf (buf, sizeof(buf), "Error opening HTML file: %s", strerror(errno));
      content = strdup (buf);
    }
    if (fd != -1)
       close (fd);
    ctype = MODES_CONTENT_TYPE_HTML;
  }

  /* Create the header and send the reply.
   */
  hdrlen = snprintf (hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Server: Dump1090\r\n"
                     "Content-Type: %s\r\n"
                     "Connection: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "\r\n",
                     ctype, keepalive ? "keep-alive" : "close", clen);

  TRACE (DEBUG_NET, "HTTP Reply header:\n%s", hdr);

  /* Send header and content.
   */
  if (send(c->fd, hdr, hdrlen, 0) != hdrlen || send(c->fd, content, clen, 0) != clen)
  {
    free (content);
    return (1);
  }
  free (content);
  Modes.stat.HTTP_get_requests++;
  return (!keepalive);
}
#endif  /* USE_MONGOOSE */

/**
 * This function polls the clients using recv() in order to receive new
 * messages from the net.
 *
 * The message is supposed to be separated by the next message by the
 * separator 'sep', that is a NUL-terminated C string.
 *
 * Every full message received is decoded and passed to the higher layers
 * calling the function `handler`.
 *
 * The hander returns 0 on success, or 1 to signal this function we
 * should close the connection with the client in case of non-recoverable
 * errors.
 */
void read_from_client (struct client *cli, char *sep, int (*handler)(struct client *))
{
#ifdef USE_MONGOOSE   /** \todo */
   const struct mg_iobuf *msg = &cli->conn->recv;

   /* copy over before Mongoose discards this
    */
   memcpy (cli->buf, msg->buf, min(msg->len, sizeof(cli->buf)));

#else
  while (1)
  {
    int left = MODES_CLIENT_BUF_SIZE - cli->buflen;
    int nread = recv (cli->fd, cli->buf + cli->buflen, left, 0);
    int fullmsg = 0;
    int i;
    char *p;

    if (nread <= 0)
    {
      if (nread == 0 || errno != EAGAIN)
      {
        /* Error, or end of file. */
        free_client (cli->fd);
      }
      break; /* Serve next client. */
    }
    cli->buflen += nread;

    /* Always null-term so we are free to use strstr() */
    cli->buf[c->buflen] = '\0';

    /* If there is a complete message there must be the separator 'sep'
     * in the buffer, note that we full-scan the buffer at every read
     * for simplicity.
     */
    while ((p = strstr(cli->buf, sep)) != NULL)
    {
      i = p - cli->buf;    /* Turn it as an index inside the buffer. */
      cli->buf[i] = '\0';  /* Te handler expects null terminated strings. */

      /* Call the function to process the message. It returns 1
       * on error to signal we should close the client connection.
       */
      if ((*handler)(cli))
      {
        free_client (cli->fd);
        return;
      }

      /* Move what's left at the start of the buffer.
       */
      i += strlen (sep);  /* The separator is part of the previous msg. */
      memmove (cli->buf, cli->buf + i, cli->buflen - i);
      cli->buflen -= i;
      cli->buf[c->buflen] = '\0';

      /* Maybe there are more messages inside the buffer.
       * Start looping from the start again.
       */
      fullmsg = 1;
    }

    /* If our buffer is full discard it, this is some badly
     * formatted shit.
     */
    if (cli->buflen == MODES_CLIENT_BUF_SIZE)
    {
      cli->buflen = 0;

      /* If there is garbage, read more to discard it ASAP.
       */
      continue;
    }

    /* If no message was decoded, process the next client.
     * Otherwise read more data from the same client.
     */
    if (!fullmsg)
       break;
  }
#endif
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
  {
    /**
     * \todo Add some version info (for Mongoose, librtsdr, libusb) and feature
     *       string depending on build options.
     */
    printf ("A 1090 MHz receiver, decoder and web-server for\n%s.\n",
            ADS_B_ACRONYM);
  }

  printf ("Usage: %s [options]\n"
          "  --aggressive             More CPU for more messages (two bits fixes, ...).\n"
          "  --debug <flags>          Debug mode; see below for details.\n"
          "  --device-index <index>   Select RTL device (default: 0).\n"
          "  --freq <hz>              Set frequency (default: %u MHz).\n"
          "  --gain <db>              Set gain (default: AUTO)\n"
          "  --infile <filename>      Read data from file (use `-' for stdin).\n"
          "  --interactive            Interactive mode refreshing data on screen.\n"
          "  --interactive-rows <num> Max number of rows in interactive mode (default: 15).\n"
          "  --interactive-ttl <sec>  Remove from list if idle for <sec> (default: %u).\n"
          "  --loop <N>               With --infile, read the file in a loop <N> times (default: 2^63).\n"
          "  --metric                 Use metric units (meters, km/h, ...).\n"
          "  --net                    Enable networking.\n"
          "  --net-only               Enable just networking, no RTL device or file used.\n"
          "  --net-ro-port <port>     TCP listening port for raw output (default: %u).\n"
          "  --net-ri-port <port>     TCP listening port for raw input (default: %u).\n"
          "  --net-sbs-port <port>    TCP listening port for BaseStation format output (default: %u).\n"
          "  --net-http-port <port>   HTTP server port (default: %u).\n"
          "  --web-page <file>        The Web-page to server for HTTP clients (default: \"%s\").\n"
          "  --no-fix                 Disable single-bits error correction using CRC.\n"
          "  --no-crc-check           Disable messages with broken CRC (discouraged).\n"
          "  --onlyaddr               Show only ICAO addresses (testing purposes).\n"
          "  --samplerate <Hz>        Set sample-rate (default: %uMS/s).\n"
          "  --raw                    Show only messages with raw hex values.\n"
          "  --strip <level>          Strip IQ file removing samples below level.\n"
          "  -h, --help               Show this help.\n"
          "  \n"
          "  Debug mode flags: E = Log frames decoded with errors.\n"
          "                    D = Log frames decoded with zero errors.\n"
          "                    c = Log frames with bad CRC.\n"
          "                    C = Log frames with good CRC.\n"
          "                    p = Log frames with bad preamble.\n"
          "                    n = Log network debugging information.\n"
          "                    N = a bit more network information than flag 'n'.\n"
          "                    j = Log frames to frames.js, loadable by `debug.html'.\n"
          "                    G = Log general debugging info.\n",
          Modes.who_am_I,
          (uint32_t)(MODES_DEFAULT_FREQ / 1000000), MODES_ICAO_CACHE_TTL,
          MODES_NET_OUTPUT_RAW_PORT, MODES_NET_INPUT_RAW_PORT,
          MODES_NET_OUTPUT_SBS_PORT, MODES_NET_HTTP_PORT, GMAP_HTML, MODES_DEFAULT_RATE/1000000);

  modeS_exit();  /* free Pthread-W32 data */
  exit (1);
}

/**
 * The background function is called a few times every second by
 * `main_data_loop()` in order to perform tasks we need to do continuously.
 *
 * Like accepting new clients from the net, refreshing the screen in
 * interactive mode, and so forth.
 */
void background_tasks (void)
{
  uint64_t now = MSEC_TIME();
  int    refresh;
  static uint64_t start;  /* program start time */
  const struct aircraft *next_a;

  if (start == 0)
     start = now;

  next_a = remove_stale_aircrafts (now);

  if (Modes.net)
     mg_mgr_poll (&Modes.mgr, MG_NET_POLL_TIME); /* Poll Mongoose for network events */

  if (Modes.exit)
     return;

  refresh = (now - Modes.last_update_ms) > MODES_INTERACTIVE_REFRESH_TIME;
  if (refresh)
  {
    if (Modes.interactive)  /* Refresh screen when in interactive mode */
       interactive_show_data (now/1000, next_a);
    Modes.last_update_ms = now;
  }
}

/**
 * The handler called in a `SIGINT`. <br>
 * I.e. user presses `^C`.
 */
void sigint_handler (int sig)
{
  signal (sig, SIG_DFL);   /* reset signal handler - bit extra safety */
  Modes.exit = 1;          /* Signal to threads that we are done */

  console_exit();

  fputs ("Caught SIGINT, shutting down..\n", stderr);

  if (Modes.dev)
  {
    int rc;

    pthread_mutex_lock (&Modes.data_mutex);
    rc = rtlsdr_cancel_async (Modes.dev);
    TRACE (DEBUG_GENERAL, "rtlsdr_cancel_async(): rc: %d.\n", rc);
    pthread_mutex_unlock (&Modes.data_mutex);
  }
}

void show_statistics (void)
{
  puts ("Decoder statistics:");
  printf (" %8llu valid preambles.\n", Modes.stat.valid_preamble);
  printf (" %8llu demodulated after phase correction.\n", Modes.stat.out_of_phase);
  printf (" %8llu demodulated with zero errors.\n", Modes.stat.demodulated);
  printf (" %8llu with CRC okay.\n", Modes.stat.good_CRC);
  printf (" %8llu with CRC failure.\n", Modes.stat.bad_CRC);
  printf (" %8llu errors corrected.\n", Modes.stat.fixed);
  printf (" %8llu messages with 1 bit errors fixed.\n", Modes.stat.single_bit_fix);
  printf (" %8llu messages with 2 bit errors fixed.\n", Modes.stat.two_bits_fix);
  printf (" %8llu total usable messages.\n", Modes.stat.good_CRC + Modes.stat.fixed);
  printf (" %8llu unique aircrafts.\n", Modes.stat.unique_aircrafts);
  printf (" %8llu unrecognized ME types.\n", Modes.stat.unrecognized_ME);

  if (Modes.net)
  {
    int s;

    puts ("\nNetwork statistics:");
    for (s = MODES_NET_SERVICE_RAW_OUT; s <= MODES_NET_SERVICE_HTTP; s++)
    {
      printf ("  %s (port %u):\n", handler_descr(s), handler_port(s));
      if (s == MODES_NET_SERVICE_HTTP)
      {
        printf ("    %8llu HTTP GET requests received.\n", Modes.stat.HTTP_get_requests);
        printf ("    %8llu HTTP/WebSocket upgrades.\n", Modes.stat.HTTP_websockets);
        printf ("    %8llu server connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_sent);
        printf ("    %8llu client connection \"keep-alive\".\n", Modes.stat.HTTP_keep_alive_recv);
      }
      printf ("    %8llu client connections accepted.\n", Modes.stat.cli_accepted[s]);
      printf ("    %8llu client connections removed.\n", Modes.stat.cli_removed[s]);
      printf ("    %8llu client connections unknown.\n", Modes.stat.cli_unknown[s]);
      printf ("    %8llu bytes sent.\n", Modes.stat.bytes_sent[s]);
      printf ("    %8llu bytes recv.\n", Modes.stat.bytes_recv[s]);
      printf ("    %8u clients now.\n", *handler_num_clients(s));
    }
  }
}

/**
 * Our exit function. Free all resources here.
 */
void modeS_exit (void)
{
  int rc;

  if (Modes.net)
  {
    unsigned num = free_all_clients();
    net_flushall();
    mg_mgr_free (&Modes.mgr);
    Modes.mgr.conns = NULL;
    if (num > 0)
       Sleep (100);
  }

  if (Modes.dev)
  {
    rc = rtlsdr_close (Modes.dev);
    Modes.dev = NULL;
    TRACE (DEBUG_GENERAL, "rtlsdr_close(), rc: %d.\n", rc);
  }

  if (!pthread_equal(Modes.reader_thread, PTHREAD_NULL))
  {
    rc = pthread_detach (Modes.reader_thread);
    TRACE (DEBUG_GENERAL, "pthread_detach(): rc: %d.\n", rc);
  }

  /* This should not hurt if we've not created the 'Modes.reader_thread'
   */
#if defined(__PTW32_STATIC_LIB)
  TRACE (DEBUG_GENERAL, "Cleaning up Pthreads-W32.\n\n");
  Modes.reader_thread = PTHREAD_NULL;
  pthread_win32_thread_detach_np();
  pthread_win32_process_detach_np();
#endif

  if (Modes.fd > STDIN_FILENO)
     close (Modes.fd);

  free_all_aircrafts();

  if (Modes.magnitude_lut)
     free (Modes.magnitude_lut);

  if (Modes.magnitude)
     free (Modes.magnitude);

  if (Modes.data)
     free (Modes.data);

  if (Modes.ICAO_cache)
     free (Modes.ICAO_cache);

   if (Modes.data_mutex)
      pthread_mutex_destroy (&Modes.data_mutex);

  Modes.data = NULL;
  Modes.magnitude = Modes.magnitude_lut = NULL;
  Modes.ICAO_cache = NULL;

#if defined(_MSC_VER) && (defined(_DEBUG) || defined(USE_VLD))
  crtdbug_exit();
#endif
}

/**
 * Our main entry.
 */
int main (int argc, char **argv)
{
  int j, rc;

#if defined(_MSC_VER) && (defined(_DEBUG) || defined(USE_VLD))
  crtdbug_init();
#endif

  /* Set sane defaults. */
  modeS_init_config();

  /* Parse the command line options */
  for (j = 1; j < argc; j++)
  {
    int more = j + 1 < argc; /* There are more arguments. */

    if (!strcmp(argv[j], "--device-index") && more)
       Modes.dev_index = atoi (argv[++j]);

    else if (!strcmp(argv[j], "--gain") && more)
        Modes.gain = (int) (10.0 * atof(argv[++j]));   /* Gain is in tens of DBs */

    else if (!strcmp(argv[j], "--freq") && more)
        Modes.freq = (uint32_t) ato_hertz (argv[++j]);

    else if (!strcmp(argv[j], "--infile") && more)
        Modes.infile = argv[++j];

    else if (!strcmp(argv[j], "--loop"))
        Modes.loops = (more && isdigit(*argv[j+1])) ? _atoi64 (argv[++j]) : LLONG_MAX;

    else if (!strcmp(argv[j], "--no-fix"))
        Modes.fix_errors = 0;

    else if (!strcmp(argv[j], "--no-crc-check"))
        Modes.check_crc = 0;

    else if (!strcmp(argv[j], "--raw"))
        Modes.raw = 1;

    else if (!strcmp(argv[j], "--samplerate") && more)
        Modes.sample_rate = (uint32_t) ato_hertz (argv[++j]);

    else if (!strcmp(argv[j], "--net"))
        Modes.net = 1;

    else if (!strcmp(argv[j], "--net-only"))
        Modes.net = Modes.net_only = 1;

    else if (!strcmp(argv[j], "--net-ro-port") && more)
        modeS_net_services [MODES_NET_SERVICE_RAW_OUT].port = atoi (argv[++j]);

    else if (!strcmp(argv[j], "--net-ri-port") && more)
        modeS_net_services [MODES_NET_SERVICE_RAW_IN].port = atoi (argv[++j]);

    else if (!strcmp(argv[j], "--net-http-port") && more)
        modeS_net_services [MODES_NET_SERVICE_HTTP].port = atoi (argv[++j]);

    else if (!strcmp(argv[j], "--net-sbs-port") && more)
        modeS_net_services [MODES_NET_SERVICE_SBS].port = atoi (argv[++j]);

    else if (!strcmp(argv[j], "--web-page") && more)
    {
      strncpy (Modes.web_root, dirname(argv[++j]), sizeof(Modes.web_root)-1);
      strncpy (Modes.web_page, basename(argv[j]), sizeof(Modes.web_page)-1);
    }

    else if (!strcmp(argv[j], "--onlyaddr"))
        Modes.only_addr = 1;

    else if (!strcmp(argv[j], "--metric"))
        Modes.metric = 1;

    else if (!strcmp(argv[j], "--aggressive"))
        Modes.aggressive++;

    else if (!strcmp(argv[j], "--interactive"))
        Modes.interactive = 1;

    else if (!strcmp(argv[j], "--interactive-rows") && more)
        Modes.interactive_rows = atoi (argv[++j]);

    else if (!strcmp(argv[j], "--interactive-ttl") && more)
        Modes.interactive_ttl = atoi (argv[++j]);

    else if (!strcmp(argv[j], "--strip") && more)
    {
      Modes.strip_level = atoi (argv[++j]);
      if (Modes.strip_level == 0)
         show_help ("Illegal --strip level %d.\n\n", Modes.strip_level);
    }
    else if (!strcmp(argv[j], "-h") || !strcmp(argv[j], "--help"))
    {
      show_help (NULL);
    }
    else if (!strcmp(argv[j], "--debug") && more)
    {
      char *f = argv[++j];

      while (*f)
      {
        switch (*f)
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
          case 'G':
               Modes.debug |= DEBUG_GENERAL;
               break;
          default:
               show_help ("Unknown debugging flag: %c\n", *f);
               /* not reached */
               break;
        }
        f++;
      }
    }
    else
    {
      show_help ("Unknown option \"%s\".\n\n", argv[j]);
      /* not reached */
    }
  }

  rc = modeS_init();        /* Initialization */
  if (rc)
     goto quit;

  if (Modes.net_only)
  {
    fprintf (stderr, "Net-only mode, no RTL device or file open.\n");
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
    else if ((Modes.fd = open(Modes.infile, O_RDONLY)) == -1)
    {
      fprintf (stderr, "Error opening `%s`: %s\n", Modes.infile, strerror(errno));
      goto quit;
    }
  }
  else
  {
    rc = modeS_init_RTLSDR();
    TRACE (DEBUG_GENERAL, "rtlsdr_open(): rc: %d.\n", rc);
    if (rc)
       goto quit;
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
    /* Create the thread that will read the data from the device and the network.
     */
    rc = pthread_create (&Modes.reader_thread, NULL, data_thread_fn, NULL);
    TRACE (DEBUG_GENERAL, "pthread_create(): rc: %d.\n", rc);
    main_data_loop();
  }

quit:
  modeS_exit();
  if (rc == 0)
     show_statistics();
  return (0);
}
