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
#include <rtl-sdr.h>
#include <sdrplay_api.h>

#include "mongoose.h"
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
 * \def INDEX_HTML
 * Our default main server page relative to `Modes.where_am_I`.
 */
#define INDEX_HTML     "web_root/index.html"

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
#define DEBUG_BADCRC     0x0001
#define DEBUG_GOODCRC    0x0002
#define DEBUG_DEMOD      0x0004
#define DEBUG_DEMODERR   0x0008
#define DEBUG_GENERAL    0x0010
#define DEBUG_GENERAL2   0x0020
#define DEBUG_MONGOOSE   0x0040
#define DEBUG_MONGOOSE2  0x0080
#define DEBUG_NOPREAMBLE 0x0100
#define DEBUG_JS         0x0200
#define DEBUG_NET        0x0400
#define DEBUG_NET2       0x0800

/**
 * \def DEBUG(bit, fmt, ...)
 * A more compact tracing macro
 */
#define DEBUG(bit, fmt, ...)                       \
        do {                                       \
          if (Modes.debug & (bit))                 \
             modeS_flogf (stdout, "%s(%u): " fmt,  \
                 __FILE__, __LINE__, __VA_ARGS__); \
        } while (0)

/**
 * \def TRACE(fmt, ...)
 * As `DEBUG()` for only the `DEBUG_GENERAL` bit set; <br>
 * command-line option `--debug g`.
 */
#define TRACE(fmt, ...)                                     \
        do {                                                \
          if (Modes.debug & DEBUG_GENERAL)                  \
             modeS_flogf (stdout, "%s(%u): " fmt ".\n",     \
                          __FILE__, __LINE__, __VA_ARGS__); \
        } while (0)


/**
 * \def HEX_DUMP(data, len)
 * Do a hex-dump of network data if option `--debug M` was used.
 */
#define HEX_DUMP(data, len)                  \
        do {                                 \
          if (Modes.debug & DEBUG_MONGOOSE2) \
             mg_hexdump (data, len);         \
        } while (0)


/**
 * \def LOG_STDOUT(fmt, ...)
 *  Print to both `stdout` and optionally to `Modes.log`.
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

typedef struct mg_http_message     mg_http_message;
typedef struct mg_connection       mg_connection;
typedef struct mg_mgr              mg_mgr;
typedef struct mg_addr             mg_addr;
typedef struct mg_str              mg_str;
typedef struct mg_timer            mg_timer;
typedef struct mg_iobuf            mg_iobuf;
typedef struct mg_ws_message       mg_ws_message;
typedef struct mg_http_serve_opts  mg_http_serve_opts;
typedef char                       mg_file_path [MG_PATH_MAX];
typedef bool (*msg_handler) (mg_iobuf *msg, int loop_cnt);

/**
 * Structure used to describe a networking client.
 * And also a server when `--net-connect` is used.
 */
typedef struct connection {
        mg_connection     *conn;              /**< Remember which connection this client/server is in */
        intptr_t           service;           /**< This client's service membership */
        uint32_t           id;                /**< A copy of `conn->id` */
        mg_addr            addr;              /**< A copy of `conn->peer` */
        bool               redirect_sent;     /**< Sent a "301 Moved" to HTTP client */
        bool               keep_alive;        /**< Client request contains "Connection: keep-alive" */
        bool               encoding_gzip;     /**< Gzip compressed client data (not yet) */
        struct connection *next;
      } connection;

/**
 * A function-pointer for either `mg_listen()` or `mg_http_listen()`.
 */
typedef struct mg_connection *(*mg_listen_func) (struct mg_mgr     *mgr,
                                                 const char        *url,
                                                 mg_event_handler_t fn,
                                                 void              *fn_data);

/**
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

typedef enum metric_unit_t {
        MODES_UNIT_FEET   = 1,
        MODES_UNIT_METERS = 2
      } metric_unit_t;

#define UNIT_NAME(unit) (unit == MODES_UNIT_METERS ? "meters" : "feet")

/**
 * Spherical position: <br>
 * Latitude (East-West) and Longitude (North-South) coordinates. <br>
 *
 * A position on a Geoid. (ignoring altitude).
 */
typedef struct pos_t {
        double lat;   /* geodetic latitude */
        double lon;
      } pos_t;

/**
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
#define EARTH_RADIUS     6371000.0    /* meters. Assuming a sphere. Approx. 40.000.000 / TWO_PI meters */
#define ASSERT_POS(pos)  do {                                         \
                           assert (pos.lon >= -180 && pos.lon < 180); \
                           assert (pos.lat >= -90  && pos.lat < 90);  \
                         } while (0)

#define MAX_ME_TYPE    37
#define MAX_ME_SUBTYPE  8

/**
 * Statistics on unrecognized ME types and sub-types.
 * The sum of a type [0..36] is the sum of the `sub_type[]` array.
 */
typedef struct unrecognized_ME {
        uint64_t  sub_type [MAX_ME_SUBTYPE];    /**< unrecognized subtypes [0..7] of this type */
      } unrecognized_ME;

/**
 * Keep all collected statistics in this structure.
 */
typedef struct statistics {
        uint64_t        valid_preamble;
        uint64_t        demodulated;
        uint64_t        good_CRC;
        uint64_t        bad_CRC;
        uint64_t        fixed;
        uint64_t        single_bit_fix;
        uint64_t        two_bits_fix;
        uint64_t        out_of_phase;
        uint64_t        unique_aircrafts;
        uint64_t        unique_aircrafts_CSV;
        uint64_t        unique_aircrafts_SQL;
        uint64_t        messages_total;
        unrecognized_ME unrecognized_ME [MAX_ME_TYPE];

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
        uint64_t  HTTP_400_responses;
        uint64_t  HTTP_404_responses;
        uint64_t  HTTP_500_responses;

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
 * The device configuration for a RTLSDR device.
 */
typedef struct rtlsdr_conf {
        char         *name;            /**< The manufacturer name of the RTLSDR device to use. As in e.g. `"--device silver"`. */
        int           index;           /**< The index of the RTLSDR device to use. As in e.g. `"--device 1"`. */
        rtlsdr_dev_t *device;          /**< The RTLSDR handle from `rtlsdr_open()`. */
        int           ppm_error;       /**< Set RTLSDR frequency correction. */
        int           calibrate;       /**< Enable calibration for R820T/R828D type devices */
        int          *gains;           /**< Gain table reported from `rtlsdr_get_tuner_gains()` */
        int           gain_count;      /**< Number of gain values in above array */
      } rtlsdr_conf;

/**
 * The private data for a SDRplay device.
 * Ref \file sdrplay.c for this.
 */
struct sdrplay_priv;

/**
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

/*
 * Forwards:
 * Details in "aircraft.h", "airports".h" and "externals/sqlite3.c"
 */
struct aircraft;
struct aircraft_CSV;
struct airport;
struct sqlite3;

/**
 * All program global state is in this structure.
 */
typedef struct global_data {
        mg_file_path      who_am_I;                 /**< The full name of this program. */
        mg_file_path      where_am_I;               /**< The current directory (no trailing `\\` and not used). */
        mg_file_path      tmp_dir;                  /**< The `%TEMP%` directory. (no trailing `\\`). */
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
        struct aircraft  *aircrafts;                /**< Linked list of active aircrafts. */
        struct airport   *airports;                 /**< Linked list of "dynamic" airports. */
        uint64_t          last_update_ms;           /**< Last screen update in milliseconds. */
        uint64_t          max_messages;             /**< How many messages to process before quitting. */

        /** Common stuff for RTLSDR and SDRplay:
         */
        char             *selected_dev;             /**< Name of selected device. */
        int               dig_agc;                  /**< Enable digital AGC. */
        int               bias_tee;                 /**< Enable bias-T voltage on coax input. */
        int               gain_auto;                /**< Use auto-gain */
        uint32_t          band_width;               /**< The wanted bandwidth. Default is 0. */
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
        mg_mgr         mgr;                    /**< Only one connection manager. */

        /** Aircraft history
         */
        uint64_t json_interval;
        int      json_aircraft_history_next;
        mg_str   json_aircraft_history [120];

        /** Configuration
         */
        const char  *infile;                    /**< Input IQ samples from file with option `--infile file`. */
        const char  *logfile;                   /**< Write debug/info to file with option `--logfile file`. */
        FILE        *log;
        uint64_t     loops;                     /**< Read input file in a loop. */
        uint32_t     debug;                     /**< `DEBUG()` mode bits. */
        int          raw;                       /**< Raw output format. */
        int          net;                       /**< Enable networking. */
        int          net_only;                  /**< Enable just networking. */
        int          net_active;                /**< With `Modes.net`, call `connect()` (not `listen()`). */
        int          silent;                    /**< Silent mode for network testing. */
        int          interactive;               /**< Interactive mode */
        uint16_t     interactive_rows;          /**< Interactive mode: max number of rows. */
        uint32_t     interactive_ttl;           /**< Interactive mode: TTL before deletion. */
        int          win_location;              /**< Use 'Windows Location API' to get the 'Modes.home_pos'. */
        int          only_addr;                 /**< Print only ICAO addresses. */
        int          metric;                    /**< Use metric units. */
        int          aggressive;                /**< Aggressive detection algorithm. */
        int          keep_alive;                /**< Send "Connection: keep-alive" if HTTP client sends it. */
        mg_file_path web_page;                  /**< The base-name of the web-page to server for HTTP clients. */
        mg_file_path web_root;                  /**< And it's directory. */
        int          touch_web_root;            /**< Touch all files in `web_root` first. */
        mg_file_path aircraft_db;               /**< The `aircraftDatabase.csv` file. */
        mg_file_path aircraft_sql;              /**< The `aircraftDatabase.csv.sqlite` file. */
        bool         have_sql_file;             /**< The `aircraftDatabase.csv.sqlite` file exists. */
        char        *aircraft_db_update;        /**< Option `--aircrafts-update<=url>` was used. */
        int          use_sql_db;                /**< Option `--aircrafts-sql` was used. */
        int          strip_level;               /**< For '--strip X' mode. */
        pos_t        home_pos;                  /**< Coordinates of home position. */
        cartesian_t  home_pos_cart;             /**< Coordinates of home position (cartesian). */
        bool         home_pos_ok;               /**< We have a good home position. */
        const char  *wininet_last_error;        /**< Last error from WinInet API. */
        int          tests;                     /**< Perform some tests. */
        int          tests_arg;                 /**< With optional tests-count. */
        int          tui_interface;             /**< Selected `--tui` interface. */

        /** For handling a `Modes.aircraft_db` file:
         */
        CSV_context          csv_ctx;           /**< Structure for the CSV parser */
        struct aircraft_CSV *aircraft_list_CSV; /**< List of aircrafts sorted on address. From CSV-file only. */
        uint32_t             aircraft_num_CSV;  /**< The length of the list */
        struct sqlite3      *sql_db;            /**< For the `Modes.aircraft_sql` file */

        /** For handling a airport-data from .CSV files. It reuses the `csv_ctx` above.
         */
        mg_file_path    airport_db;                /**< The `airports-code.csv` file generated by `tools/gen_airport_codes_csv.py` */
        mg_file_path    airport_freq_db;           /**< The `airports-frequencies.csv` file. Not generated yet */
        mg_file_path    airport_cache;             /**< The `%%TEMP%%/airport-api-cache.csv` */
        struct airport *airport_list_CSV;          /**< List of airports sorted on ICAO address. From CSV-file only. */
        uint32_t        airport_num_CSV;           /**< The length of the above list */
        int             airport_db_update;         /**< Option `--airports-update` was used. */
        int             airport_show;              /**< Show airport / flight-info in interactive mode? */

      } global_data;

extern global_data Modes;

#if defined(USE_READSB_DEMOD)
  typedef struct mag_buf {
          uint16_t *data;            /**< Magnitude data, starting with overlap from the previous block */
          unsigned  length;          /**< Number of valid samples _after_ overlap */
          unsigned  overlap;         /**< Number of leading overlap samples at the start of "data" */
                                     /**< also the number of trailing samples that will be preserved for next time */
          uint64_t sampleTimestamp;  /**< Clock timestamp of the start of this block, 12MHz clock */
          uint64_t sysTimestamp;     /**< Estimated system time at start of block */
          double   mean_level;       /**< Mean of normalized (0..1) signal level */
          double   mean_power;       /**< Mean of normalized (0..1) power level */
          unsigned dropped;          /**< (approx) number of dropped samples */
          struct mag_buf *next;      /**< linked list forward link */
        } mag_buf;

  extern void demodulate2400 (struct mag_buf *mag);
#endif

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

/**
 * When debug is set to DEBUG_NOPREAMBLE, the first sample must be
 * at least greater than a given level for us to dump the signal.
 */
#define DEBUG_NOPREAMBLE_LEVEL           25
#define MODES_CONNECT_TIMEOUT          5000   /* msec timeout for an active connect */

#define MG_NET_POLL_TIME             (MODES_INTERACTIVE_REFRESH_TIME / 2)

#define MODES_CONTENT_TYPE_ICON   "image/x-icon"
#define MODES_CONTENT_TYPE_JSON   "application/json"
#define MODES_CONTENT_TYPE_PNG    "image/png"
#define MODES_RAW_HEART_BEAT      "*0000;\n*0000;\n*0000;\n*0000;\n*0000;\n"

/**
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
        uint8_t  AA [3];                     /**< ICAO Address bytes 1, 2 and 3 (big-endian) */
        bool     phase_corrected;            /**< True if phase correction was applied. */

        /** DF11
         */
        int ca;                              /**< Responder capabilities. */

        /** DF 17
         */
        int  ME_type;                        /**< Extended squitter message type. */
        int  ME_subtype;                     /**< Extended squitter message subtype. */
        int  heading;                        /**< Horizontal angle of flight. */
        bool heading_is_valid;               /**< We got a valid `heading` */
        int  aircraft_type;                  /**< Aircraft identification. "Type A..D" */
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
        int           altitude;
        metric_unit_t unit;
      } modeS_message;

/*
 * Defined in MSVC's <sal.h>.
 */
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif

#if defined(__clang__)
  #define ATTR_PRINTF(_1, _2)  __attribute__((format(printf, _1, _2)))
#else
  #define ATTR_PRINTF(_1, _2)
#endif

void        modeS_log (const char *buf);
void        modeS_logc (char c, void *param);
void        modeS_flogf (FILE *f, _Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(2, 3);
void        modeS_set_log (void);
uint32_t    ato_hertz (const char *Hertz);
bool        str_startswith (const char *s1, const char *s2);
bool        str_endswith (const char *s1, const char *s2);
char       *basename (const char *fname);
char       *dirname (const char *fname);
char       *slashify (char *fname);
int        _gettimeofday (struct timeval *tv, void *timezone);
double      get_usec_now (void);
void        test_assert (void);
const char *win_strerror (DWORD err);
char       *_mg_straddr (struct mg_addr *a, char *buf, size_t len);
bool        set_host_port (const char *host_port, net_service *serv, uint16_t def_port);
uint32_t    random_range (uint32_t min, uint32_t max);
int32_t     random_range2 (int32_t min, int32_t max);
int         touch_file (const char *file);
int         touch_dir (const char *dir, bool recurse);
uint32_t    download_file (const char *file, const char *url);
void        show_version_info (bool verbose);
void        spherical_to_cartesian (const pos_t *pos, cartesian_t *cart);
void        cartesian_to_spherical (const cartesian_t *cart, pos_t *pos, double heading);
double      cartesian_distance (const cartesian_t *a, const cartesian_t *b);
double      great_circle_dist (pos_t pos1, pos_t pos2);
double      closest_to (double val, double val1, double val2);
void        decode_CPR (struct aircraft *a);

/*
 * Generic table for loading DLLs and functions from them.
 */
struct dyn_struct {
       const bool  optional;
       HINSTANCE   mod_handle;
       const char *mod_name;
       const char *func_name;
       void      **func_addr;
     };

extern int load_dynamic_table (struct dyn_struct *tab, int tab_size);
extern int unload_dynamic_table (struct dyn_struct *tab, int tab_size);

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
 * A typedef for `getopt()` command-line handling.
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
