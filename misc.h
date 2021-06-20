/**\file    misc.h
 * \ingroup Misc
 *
 * Various macros and definitions.
 */
#ifndef _MISC_H
#define _MISC_H

/* Avoid pulling in <winsock.h> since we want <winsock2.h>.
 */
#define _WINSOCKAPI_

#include <stdio.h>
#include <stdbool.h>
#include <winsock2.h>
#include <windows.h>
#include <mongoose.h>
#include <rtl-sdr.h>
#include <libusb.h>
#include <sdrplay_api.h>

#include "csv.h"

/**
 * Various helper macros.
 */
#define MODES_NOTUSED(V)   ((void)V)
#define IS_SLASH(c)        ((c) == '\\' || (c) == '/')
#define TWO_PI             (2 * M_PI)
#define DIM(array)         (sizeof(array) / sizeof(array[0]))
#define ONE_MBYTE          (1024*1024)
#define STDIN_FILENO       0

/**
 * \def GMAP_HTML
 * Our default main server page relative to `Mode.who_am_I`.
 */
#define GMAP_HTML         "web_root/gmap.html"

#define ADS_B_ACRONYM     "ADS-B; Automatic Dependent Surveillance - Broadcast"
#define AIRCRAFT_CSV      "aircraftDatabase.csv"

#ifdef _DEBUG
  /*
   * Since it takes too much time to load in '_DEBUG' mode.
   */
  #undef  AIRCRAFT_CSV
  #define AIRCRAFT_CSV ""
#endif

/**
 * Definitions for network services.
 */
#define MODES_NET_PORT_OUTPUT_SBS  30003
#define MODES_NET_PORT_OUTPUT_RAW  30002
#define MODES_NET_PORT_INPUT_RAW   30001
#define MODES_NET_PORT_HTTP         8080

#define MODES_NET_SERVICE_RAW_OUT  0
#define MODES_NET_SERVICE_RAW_IN   1
#define MODES_NET_SERVICE_SBS      2
#define MODES_NET_SERVICE_HTTP     3
#define MODES_NET_SERVICES_NUM     4

/**
 * \def MSEC_TIME()
 * Returns a 64-bit tick-time value with 1 millisec granularity.
 */
#define MSEC_TIME() GetTickCount64()

/**
 * \def SAFE_COND_SIGNAL(cond, mutex)
 * \def SAFE_COND_WAIT(cond, mutex)
 *
 * Signals are not threadsafe by default.
 * Taken from the Osmocom-SDR code and modified to
 * use Win-Vista+ functions.
 */
#define SAFE_COND_SIGNAL(cond, mutex)   \
        do {                            \
          EnterCriticalSection (mutex); \
          WakeConditionVariable (cond); \
          LeaveCriticalSection (mutex); \
        } while (0)

#define SAFE_COND_WAIT(cond, mutex)     \
        do {                            \
          EnterCriticalSection (mutex); \
          WakeConditionVariable (cond); \
          LeaveCriticalSection (mutex); \
        } while (0)

/**
 * Bits for 'Modes.debug':
 */
#define DEBUG_DEMOD      (1 << 0)
#define DEBUG_DEMODERR   (1 << 1)
#define DEBUG_BADCRC     (1 << 2)
#define DEBUG_GOODCRC    (1 << 3)
#define DEBUG_NOPREAMBLE (1 << 4)
#define DEBUG_JS         (1 << 5)
#define DEBUG_NET        (1 << 6)
#define DEBUG_NET2       (1 << 7)
#define DEBUG_GENERAL    (1 << 8)
#define DEBUG_GENERAL2   (1 << 9)
#define DEBUG_LIBUSB     (1 << 10)
#define DEBUG_LIBUSB2    (1 << 11)

/**
 * \def TRACE(bit, fmt, ...)
 * A more compact tracing macro
 */
#define TRACE(bit, fmt, ...)                       \
        do {                                       \
          if (Modes.debug & (bit))                 \
             modeS_flogf (stdout, "%s(%u): " fmt,  \
                 __FILE__, __LINE__, __VA_ARGS__); \
        } while (0)

/**
 * \def LOG_STDOUT(fmt, ...)
 * \def LOG_STDERR(fmt, ...)
 *
 * Print to both `stdout` and optionally to `Modes.log`.
 * Print to both `stderr` and optionally to `Modes.log`.
 */
#define LOG_STDOUT(fmt, ...)  modeS_flogf (stdout, fmt, __VA_ARGS__)
#define LOG_STDERR(fmt, ...)  modeS_flogf (stderr, fmt, __VA_ARGS__)

/**
 * \struct client
 * Structure used to describe a networking client.
 */
struct client {
    struct mg_connection *conn;              /**< Remember which connection the client is in */
    int                   service;           /**< This client's service membership */
    uint32_t              id;                /**< A copy of `conn->id` */
    struct mg_addr        addr;              /**< A copy of `conn->peer` */
    int                   keep_alive;        /**< Client request contains "Connection: keep-alive" */
    char                  buf [1024];        /**< Read buffer. */
    int                   buflen;            /**< Amount of data on buffer. */
    struct client        *next;
};

/**
 * \struct aircraft_CSV
 * Describes an aircraft from a .CSV-file.
 */
struct aircraft_CSV {
       uint32_t addr;
       char     reg_num [10];
       char     manufact [30];
     };

typedef enum a_show_t {
        A_FIRST_TIME = 1,
        A_LAST_TIME,
        A_SHOW_NORMAL
      } a_show_t;

/**
 * \struct aircraft
 * Structure used to describe an aircraft in interactive mode.
 */
struct aircraft {
       uint32_t addr;              /**< ICAO address */
       char     flight [9];        /**< Flight number */
       int      altitude;          /**< Altitude */
       uint32_t speed;             /**< Velocity computed from EW and NS components. */
       int      heading;           /**< Horizontal angle of flight. */
       bool     heading_is_valid;  /**< Have a valid heading. */
       uint32_t seen;              /**< Tick-time (in sec) at which the last packet was received. */
       long     messages;          /**< Number of Mode S messages received. */
       int      identity;          /**< 13 bits identity (Squawk). */
       a_show_t showing;           /**< Shown normal or for the first or last time? */

       /* Encoded latitude and longitude as extracted by odd and even
        * CPR encoded messages.
        */
       int      odd_CPR_lat;       /**< Encoded odd CPR latitude */
       int      odd_CPR_lon;       /**< Encoded odd CPR longitude */
       int      even_CPR_lat;      /**< Encoded even CPR latitude */
       int      even_CPR_lon;      /**< Encoded even CPR longitude */
       uint64_t odd_CPR_time;      /**< Tick-time for reception of an odd CPR message */
       uint64_t even_CPR_time;     /**< Tick-time for reception of an even CPR message */
       double   lat, lon;          /**< Coordinates obtained from decoded CPR data. */

       const struct aircraft_CSV *CSV;  /**< A pointer to a CSV record (or NULL). */
       struct aircraft           *next; /**< Next aircraft in our linked list. */
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
       uint64_t unique_aircrafts_CSV;
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
 * The per-device configuration is in these structures.
 */
struct rtlsdr_conf {
       char          *name;            /**< The manufacturer name of the RTLSDR device to use. */
       int            index;           /**< The index of the RTLSDR device to use. As in e.g. `"--device 1"`. */
       rtlsdr_dev_t  *device;          /**< The RTLSDR handle from `rtlsdr_open()`. */
       int            ppm_error;       /**< Set RTLSDR frequency correction. */
       bool           dig_agc;         /**< Enable RTLSDR digital AGC. */
       bool           bias_tee;        /**< Enable RTLSDR bias-T voltage on coax input. */
       bool           calibrate;       /**< Enable calibration for R820T/R828D type devices */
     };

struct sdrplay_conf {
       char                            *name;      /**< Name of SDRplay instance to use. */
       int                              index;     /**< The index of the SDRplay device to use. As in e.g. `"--device sdrplay1"`. */
       sdrplay_dev                     *device;    /**< Device-handle from `sdrplay_init()`. */
       bool                             if_mode;
       bool                             over_sample;
       bool                             disable_broadcast_notch;
       bool                             disable_DAB_notch;
       int                              gain_reduction;
       int                              ADSB_mode;
       int                              BW_mode;
       sdrplay_api_Rsp2_AntennaSelectT  antenna_port;
       sdrplay_api_RspDx_AntennaSelectT DX_antenna_port;
       sdrplay_api_TunerSelectT         tuner;
       sdrplay_api_RspDuoModeT          mode;
     };

/**
 * \struct global_data
 * All program global state is in this structure.
 */
struct global_data {
       char              who_am_I [MG_PATH_MAX];   /**< The full name of this program. */
       char              where_am_I [MG_PATH_MAX]; /**< The current directory (no trailing `\\`. not used). */
       uintptr_t         reader_thread;            /**< Device reader thread ID. */
       CRITICAL_SECTION  data_mutex;               /**< Mutex to synchronize buffer access. */
       CRITICAL_SECTION  print_mutex;              /**< Mutex to synchronize printouts. */
       uint8_t          *data;                     /**< Raw IQ samples buffer. */
       uint32_t          data_len;                 /**< Length of raw IQ buffer. */
       uint16_t         *magnitude;                /**< Magnitude vector. */
       uint16_t         *magnitude_lut;            /**< I/Q -> Magnitude lookup table. */
       int               fd;                       /**< `--infile` option file descriptor. */
       volatile int      exit;                     /**< Exit from the main loop when true. */
       volatile int      data_ready;               /**< Data ready to be processed. */
       uint32_t         *ICAO_cache;               /**< Recently seen ICAO addresses. */
       struct statistics stat;                     /**< Decoding and network statistics. */
       struct aircraft  *aircrafts;                /**< Linked list of active aircrafts. */
       uint64_t          last_update_ms;           /**< Last screen update in milliseconds. */
       uint64_t          message_count;            /**< How many messages to process before quitting. */

       /** Common stuff for RTLSDR and SDRplay:
        */
       bool              dig_agc;                  /**< Enable digital AGC. */
       bool              bias_tee;                 /**< Enable bias-T voltage on coax input. */
       uint16_t          gain;                     /**< The gain setting for this device. Default is MODES_AUTO_GAIN. */
       uint32_t          freq;                     /**< The tuned frequency. Default is MODES_DEFAULT_FREQ. */
       uint32_t          sample_rate;              /**< The sample-rate. Default is MODES_DEFAULT_RATE.
                                                     *  \note This cannot be used yet since the code assumes a
                                                     *        pulse-width of 0.5 usec based on a fixed rate of 2 MS/s.
                                                     */
       struct rtlsdr_conf  rtlsdr;                 /**< RTLSDR specific settings. */
       struct sdrplay_conf sdrplay;                /**< SDRplay specific settings. */

       /** Lists of clients for each network service
        */
       struct client        *clients [MODES_NET_SERVICES_NUM];
       struct mg_connection *sbsos;           /**< SBS output listening connection. */
       struct mg_connection *ros;             /**< Raw output listening connection. */
       struct mg_connection *ris;             /**< Raw input listening connection. */
       struct mg_connection *http;            /**< HTTP listening connection. */
       struct mg_mgr         mgr;             /**< Only one connection manager */

       /** Configuration
        */
       const char *infile;                    /**< Input IQ samples from file with option `--infile file`. */
       const char *logfile;                   /**< Write debug/info to file with option `--logfile file`. */
       FILE       *log;
       uint64_t    loops;                     /**< Read input file in a loop. */
       bool        fix_errors;                /**< Single bit error correction if true. */
       bool        check_crc;                 /**< Only display messages with good CRC. */
       int         raw;                       /**< Raw output format. */
       int         debug;                     /**< Debugging mode. */
       int         net;                       /**< Enable networking. */
       int         net_only;                  /**< Enable just networking. */
       int         interactive;               /**< Interactive mode */
       uint16_t    interactive_rows;          /**< Interactive mode: max number of rows. */
       uint32_t    interactive_ttl;           /**< Interactive mode: TTL before deletion. */
       bool        only_addr;                 /**< Print only ICAO addresses. */
       int         metric;                    /**< Use metric units. */
       int         aggressive;                /**< Aggressive detection algorithm. */
       char        web_page [MG_PATH_MAX];    /**< The base-name of the web-page to server for HTTP clients */
       char        web_root [MG_PATH_MAX];    /**< And it's directory */
       char        aircraft_db [MG_PATH_MAX]; /**< The `aircraftDatabase.csv` file */
       int         strip_level;               /**< For '--strip X' mode */

       /** For parsing a `Modes.aircraft_db` file:
        */
       struct CSV_context   csv_ctx;
       struct aircraft_CSV *aircraft_list;
       uint32_t             aircraft_num_CSV;
     };

extern struct global_data Modes;

/*
 * Defined in MSVC's <sal.h>.
 */
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif

#if defined(__clang__)
  #define ATTR_PRINTF(_1, _2)  __attribute__((format(printf, _1, _2)))
  #define ATTR_FALLTHROUGH()   __attribute__((fallthrough))
#else
  #define ATTR_PRINTF(_1, _2)
  #define ATTR_FALLTHROUGH()   ((void)0)
#endif

extern void modeS_flogf (FILE *f, _Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(2, 3);

#endif
