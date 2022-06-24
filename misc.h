/**\file    misc.h
 * \ingroup Misc
 *
 * Various macros and definitions.
 */
#ifndef _MISC_H
#define _MISC_H

#include <stdio.h>
#include <stdbool.h>
#include <winsock2.h>
#include <windows.h>
#include <mongoose.h>
#include <rtl-sdr.h>
#include <sdrplay_api.h>

#include "csv.h"

/**
 * Various helper macros.
 */
#define MODES_NOTUSED(V)   ((void)V)
#define IS_SLASH(c)        ((c) == '\\' || (c) == '/')
#define TWO_PI             (2 * M_PI)
#define DIM(array)         (sizeof(array) / sizeof(array[0]))
#define ONE_MEGABYTE       (1024*1024)
#define STDIN_FILENO       0

/**
 * \def GMAP_HTML
 * Our default main server page relative to `Modes.where_am_I`.
 */
#define GMAP_HTML         "web_root/gmap.html"

#define ADS_B_ACRONYM     "ADS-B; Automatic Dependent Surveillance - Broadcast"

/**
 * Definitions for network services.
 */
#define MODES_NET_PORT_RAW_IN   30001
#define MODES_NET_PORT_RAW_OUT  30002
#define MODES_NET_PORT_SBS      30003
#define MODES_NET_PORT_HTTP      8080

#define MODES_NET_SERVICE_RAW_OUT   0
#define MODES_NET_SERVICE_RAW_IN    1
#define MODES_NET_SERVICE_SBS_OUT   2
#define MODES_NET_SERVICE_SBS_IN    3
#define MODES_NET_SERVICE_HTTP      4
#define MODES_NET_SERVICES_NUM     (MODES_NET_SERVICE_HTTP + 1)

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
 * Print to both `stdout` and optionally to `Modes.log`.
 *
 * \def LOG_STDERR(fmt, ...)
 *  Print to both `stderr` and optionally to `Modes.log`.
 *
 * \def LOG_FILEONLY(fmt, ...)
 *  Print to `Modes.log` only.
 */
#define LOG_STDOUT(fmt, ...)    modeS_flogf (stdout, fmt, __VA_ARGS__)
#define LOG_STDERR(fmt, ...)    modeS_flogf (stderr, fmt, __VA_ARGS__)
#define LOG_FILEONLY(fmt, ...)  modeS_flogf (Modes.log, fmt, __VA_ARGS__)

typedef struct mg_http_message mg_http_message;
typedef struct mg_connection   mg_connection;
typedef struct mg_mgr          mg_mgr;
typedef struct mg_addr         mg_addr;
typedef struct mg_str          mg_str;
typedef struct mg_timer        mg_timer;
typedef struct mg_iobuf        mg_iobuf;
typedef struct mg_ws_message   mg_ws_message;
typedef void (*msg_handler) (mg_iobuf *msg, int loop_cnt);

/**
 * \typedef struct connection
 * Structure used to describe a networking client.
 * And also a server when `--net-connect` is used.
 */
typedef struct connection {
        mg_connection     *conn;              /**< Remember which connection this client/server is in */
        intptr_t           service;           /**< This client's service membership */
        uint32_t           id;                /**< A copy of `conn->id` */
        mg_addr            addr;              /**< A copy of `conn->peer` */
        int                keep_alive;        /**< Client request contains "Connection: keep-alive" */
        struct connection *next;
      } connection;

/**
 * \typedef struct net_service
 * A structure defining a passive or active network service.
 */
typedef struct net_service {
        mg_connection  **conn;             /**< A pointer to the returned Mongoose connection */
        char            *host;             /**< The host address if `Modes.net_active == true` */
        const char      *descr;            /**< A textual description of this service */
        uint16_t         port;             /**< The listening port number */
        uint16_t         num_connections;  /**< Number of clients/servers connected to this service */
        bool             active_send;      /**< We are the sending side. Never duplex. */
        bool             is_ip6;           /**< The above `host` address is an IPv6 address */
        char            *last_err;         /**< Last error from a `MG_EV_ERROR` event */
        mg_timer         timer;            /**< Timer for a `mg_connect()` */
      } net_service;

/**
 * \typedef struct aircraft_CSV
 * Describes an aircraft from a .CSV-file.
 */
typedef struct aircraft_CSV {
        uint32_t addr;
        char     reg_num [10];
        char     manufact [30];
        char     call_sign [20];
      } aircraft_CSV;

/**
 * \typedef ICAO_range
 * The low and high values used to lookup military ranges.
 */
typedef struct ICAO_range {
        uint32_t low;
        uint32_t high;
      } ICAO_range;

/**
 * \enum a_show_t
 * The "show-state" for an aircraft.
 */
typedef enum a_show_t {
        A_SHOW_FIRST_TIME = 1,
        A_SHOW_LAST_TIME,
        A_SHOW_NORMAL,
        A_SHOW_NONE,
      } a_show_t;

/**
 * \typedef struct pos_t
 *
 * Latitude (East-West) and Longitude (North-South) coordinates.
 * (ignoring altitude).
 */
typedef struct pos_t {
        double lat;
        double lon;
      } pos_t;

/**
 * \typedef struct cartesian_t
 *
 * A point in Cartesian coordinates.
 */
typedef struct cartesian_t {
        double c_x;
        double c_y;
        double c_z;
      } cartesian_t;

/**
 * \def SMALL_VAL
 * \def VALID_POS()
 *
 * Simple check for a valid geo-position
 */
#define SMALL_VAL        0.0001
#define VALID_POS(pos)   (fabs(pos.lon) >= SMALL_VAL && fabs(pos.lat) >= SMALL_VAL)
#define ASSERT_POS(pos)  do {                                         \
                           assert (pos.lon >= -180 && pos.lon < 180); \
                           assert (pos.lat >= -180 && pos.lat < 180); \
                         } while (0)

/**
 * \typedef struct aircraft
 * Structure used to describe an aircraft in interactive mode.
 */
typedef struct aircraft {
        uint32_t addr;              /**< ICAO address */
        char     flight [9];        /**< Flight number */
        int      altitude;          /**< Altitude */
        uint32_t speed;             /**< Velocity computed from EW and NS components. In Knots. */
        int      heading;           /**< Horizontal angle of flight. */
        bool     heading_is_valid;  /**< Have a valid heading. */
        uint64_t seen_first;        /**< Tick-time (in milli-sec) at which the first packet was received. */
        uint64_t seen_last;         /**< Tick-time (in milli-sec) at which the last packet was received. */
        uint64_t EST_seen_last;     /**< Tick-time (in milli-sec) at which the last estimated position was done. */
        uint32_t messages;          /**< Number of Mode S messages received. */
        int      identity;          /**< 13 bits identity (Squawk). */
        a_show_t show;              /**< The plane's show-state */
        double   distance;          /**< Distance (in meters) to home position */
        double   EST_distance;      /**< Estimated `distance` based on last `speed` and `heading` */
        double   sig_levels [4];    /**< RSSI signal-levels from the last 4 messages */
        int      sig_idx;

        /* Encoded latitude and longitude as extracted by odd and even
         * CPR encoded messages.
         */
        int      odd_CPR_lat;       /**< Encoded odd CPR latitude */
        int      odd_CPR_lon;       /**< Encoded odd CPR longitude */
        int      even_CPR_lat;      /**< Encoded even CPR latitude */
        int      even_CPR_lon;      /**< Encoded even CPR longitude */
        uint64_t odd_CPR_time;      /**< Tick-time for reception of an odd CPR message */
        uint64_t even_CPR_time;     /**< Tick-time for reception of an even CPR message */
        pos_t    position;          /**< Coordinates obtained from decoded CPR data. */
        pos_t    EST_position;      /**< Estimated position based on last `speed` and `heading`. */

        const aircraft_CSV *CSV;  /**< A pointer to a CSV record (or NULL). */
        struct aircraft    *next; /**< Next aircraft in our linked list. */
      } aircraft;

/**
 * \typedef struct statistics
 * Keep all collected statistics in this structure.
 */
typedef struct statistics {
        uint64_t  valid_preamble;
        uint64_t  demodulated;
        uint64_t  good_CRC;
        uint64_t  bad_CRC;
        uint64_t  fixed;
        uint64_t  single_bit_fix;
        uint64_t  two_bits_fix;
        uint64_t  out_of_phase;
        uint64_t  unique_aircrafts;
        uint64_t  unique_aircrafts_CSV;
        uint64_t  unrecognized_ME;

        /* Network statistics:
         */
        uint64_t  cli_accepted [MODES_NET_SERVICES_NUM];
        uint64_t  cli_removed  [MODES_NET_SERVICES_NUM];
        uint64_t  cli_unknown  [MODES_NET_SERVICES_NUM];
        uint64_t  srv_connected[MODES_NET_SERVICES_NUM];
        uint64_t  srv_removed  [MODES_NET_SERVICES_NUM];
        uint64_t  srv_unknown  [MODES_NET_SERVICES_NUM];
        uint64_t  bytes_sent [MODES_NET_SERVICES_NUM];
        uint64_t  bytes_recv [MODES_NET_SERVICES_NUM];
        uint64_t  HTTP_get_requests;
        uint64_t  HTTP_keep_alive_recv;
        uint64_t  HTTP_keep_alive_sent;
        uint64_t  HTTP_websockets;

        /* Network statistics for receiving raw and SBS messages:
         */
        uint64_t  good_SBS;
        uint64_t  good_raw;
        uint64_t  unrecognized_SBS;
        uint64_t  unrecognized_raw;
        uint64_t  empty_SBS;
        uint64_t  empty_raw;
        uint64_t  empty_unknown;
      } statistics;

/**
 * \typedef struct rtlsdr_conf
 * The device configuration for a RTLSDR device.
 */
typedef struct rtlsdr_conf {
        char         *name;            /**< The manufacturer name of the RTLSDR device to use. As in e.g. `"--device silver"`. */
        int           index;           /**< The index of the RTLSDR device to use. As in e.g. `"--device 1"`. */
        rtlsdr_dev_t *device;          /**< The RTLSDR handle from `rtlsdr_open()`. */
        int           ppm_error;       /**< Set RTLSDR frequency correction. */
        bool          calibrate;       /**< Enable calibration for R820T/R828D type devices */
        int          *gains;           /**< Gain table reported from `rtlsdr_get_tuner_gains()` */
        int           gain_count;      /**< Number of gain values in above array */
      } rtlsdr_conf;

/**
 * \typedef struct sdrplay_priv
 * The private data for a SDRplay device.
 * Ref \file sdrplay.c for this.
 */
struct sdrplay_priv;

/**
 * \typedef struct sdrplay_conf
 * The device configuration for a SDRplay device.
 */
typedef struct sdrplay_conf {
        struct sdrplay_priv             *priv;
        char                            *name;               /**< Name of SDRplay instance to use. */
        int                              index;              /**< The index of the SDRplay device to use. As in e.g. `"--device sdrplay1"`. */
        void                            *device;             /**< Device-handle from `sdrplay_init()`. */
        bool                             if_mode;
        bool                             over_sample;
        bool                             disable_broadcast_notch;
        bool                             disable_DAB_notch;
        int                              gain_reduction;
        int                              ADSB_mode;
        int                              BW_mode;
        int                             *gains;
        int                              gain_count;
        sdrplay_api_Rsp2_AntennaSelectT  antenna_port;
        sdrplay_api_RspDx_AntennaSelectT DX_antenna_port;
        sdrplay_api_TunerSelectT         tuner;
        sdrplay_api_RspDuoModeT          mode;
      } sdrplay_conf;

/**
 * \typedef struct global_data
 * All program global state is in this structure.
 */
typedef struct global_data {
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
        volatile bool     exit;                     /**< Exit from the main loop when true. */
        volatile bool     data_ready;               /**< Data ready to be processed. */
        uint32_t         *ICAO_cache;               /**< Recently seen ICAO addresses. */
        statistics        stat;                     /**< Decoding and network statistics. */
        aircraft         *aircrafts;                /**< Linked list of active aircrafts. */
        uint64_t          last_update_ms;           /**< Last screen update in milliseconds. */
        uint64_t          max_messages;             /**< How many messages to process before quitting. */

        /** Common stuff for RTLSDR and SDRplay:
         */
        char             *selected_dev;             /**< Name of selected device. */
        bool              dig_agc;                  /**< Enable digital AGC. */
        bool              bias_tee;                 /**< Enable bias-T voltage on coax input. */
        bool              gain_auto;                /**< Use auto-gain */
        uint16_t          gain;                     /**< The gain setting for this device. Default is MODES_AUTO_GAIN. */
        uint32_t          freq;                     /**< The tuned frequency. Default is MODES_DEFAULT_FREQ. */
        uint32_t          sample_rate;              /**< The sample-rate. Default is MODES_DEFAULT_RATE.
                                                      *  \note This cannot be used yet since the code assumes a
                                                      *        pulse-width of 0.5 usec based on a fixed rate of 2 MS/s.
                                                      */
        rtlsdr_conf  rtlsdr;                        /**< RTLSDR specific settings. */
        sdrplay_conf sdrplay;                       /**< SDRplay specific settings. */
        bool         emul_loaded;                   /**< RTLSDR-emul.dll loaded. */

        /** Lists of clients for each network service
         */
        connection    *connections [MODES_NET_SERVICES_NUM];
        mg_connection *sbs_out;                /**< SBS output listening connection. */
        mg_connection *sbs_in;                 /**< SBS input active connection. */
        mg_connection *raw_out;                /**< Raw output active/listening connection. */
        mg_connection *raw_in;                 /**< Raw input listening connection. */
        mg_connection *http_out;               /**< HTTP listening connection. */
        mg_mgr         mgr;                    /**< Only one connection manager */

        /** Configuration
         */
        const char *infile;                    /**< Input IQ samples from file with option `--infile file`. */
        const char *logfile;                   /**< Write debug/info to file with option `--logfile file`. */
        FILE       *log;
        uint64_t    loops;                     /**< Read input file in a loop. */
        uint32_t    debug;                     /**< Debugging mode bits. */
        bool        raw;                       /**< Raw output format. */
        bool        net;                       /**< Enable networking. */
        bool        net_only;                  /**< Enable just networking. */
        bool        net_active;                /**< With `Modes.net`, call `connect()` (not `listen()`). */
        bool        silent;                    /**< Silent mode for network testing */
        bool        interactive;               /**< Interactive mode */
        uint16_t    interactive_rows;          /**< Interactive mode: max number of rows. */
        uint32_t    interactive_ttl;           /**< Interactive mode: TTL before deletion. */
        bool        only_addr;                 /**< Print only ICAO addresses. */
        bool        metric;                    /**< Use metric units. */
        bool        aggressive;                /**< Aggressive detection algorithm. */
        char        web_page [MG_PATH_MAX];    /**< The base-name of the web-page to server for HTTP clients */
        char        web_root [MG_PATH_MAX];    /**< And it's directory */
        char        aircraft_db [MG_PATH_MAX]; /**< The `aircraftDatabase.csv` file */
        int         strip_level;               /**< For '--strip X' mode */
        pos_t       home_pos;                  /**< Coordinates of home position */
        cartesian_t home_pos_cart;             /**< Coordinates of home position (cartesian) */
        bool        home_pos_ok;               /**< We have a good home position */

        /** For parsing a `Modes.aircraft_db` file:
         */
        CSV_context   csv_ctx;
        aircraft_CSV *aircraft_list;
        uint32_t      aircraft_num_CSV;
      } global_data;

extern global_data Modes;

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

extern void   modeS_log (const char *buf);
extern void   modeS_flogf (FILE *f, _Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(2, 3);
extern double ato_hertz (const char *Hertz);
extern char  *basename (const char *fname);
extern char  *dirname (const char *fname);
extern int   _gettimeofday (struct timeval *tv, void *timezone);
extern void   set_host_port (const char *host_port, net_service *serv, uint16_t def_port);

/**
 * \def MSEC_TIME()
 * Returns a 64-bit tick-time value with 1 millisec granularity.
 */
#if defined(USE_gettimeofday)
  static __inline uint64_t MSEC_TIME (void)
  {
    struct timeval now;

    _gettimeofday (&now, NULL);
    return (1000 * now.tv_sec) + (now.tv_usec / 1000);
  }
#else
  #define MSEC_TIME() GetTickCount64()
#endif

/**
 * GNU-like getopt_long() / getopt_long_only() with 4.4BSD optreset extension.
 *
 * \def no_argument
 * The option takes no argument
 *
 * \def required_argument
 * The option must have an argument
 *
 * \def optional_argument
 * The option has an optional argument
 */
#define no_argument        0
#define required_argument  1
#define optional_argument  2

/**
 * \typedef struct option
 * For `getopt()` command-line handling.
 */
typedef struct option {
        const char *name; /**< name of long option */

        /**
         * one of `no_argument`, `required_argument` or `optional_argument`:<br>
         * whether option takes an argument.
         */
        int  has_arg;
        int *flag;    /**< if not NULL, set *flag to val when option found */
        int  val;     /**< if flag not NULL, value to set \c *flag to; else return value */
      } option;

int getopt_long (int, char * const *, const char *,
                 const struct option *, int *);

int getopt_long_only (int nargc, char * const *nargv, const char *options,
                      const struct option *long_options, int *idx);

int getopt (int nargc, char * const *nargv, const char *options);

extern char *optarg;  /**< the argument to an option in `optsstring`. */
extern int   optind;  /**< the index of the next element to be processed in `argv`. */
extern int   opterr;  /**< if caller set this to zero, an error-message will never be printed. */
extern int   optopt;  /**< on errors, an unrecognised option character is stored in `optopt`. */

#endif
