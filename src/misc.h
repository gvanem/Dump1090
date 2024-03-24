/**\file    misc.h
 * \ingroup Misc
 *
 * Various macros and definitions.
 */
#ifndef _MISC_H
#define _MISC_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <winsock2.h>
#include <windows.h>
#include <wchar.h>
#include <rtl-sdr.h>
#include <sdrplay_api.h>

#include "mongoose.h"
#include "cfg_file.h"
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
#define FREE(p)            (p ? (void) (free((void*)p), p = NULL) : (void)0)

/**
 * Network services indices; `global_data::connections [N]`:
 */
#define MODES_NET_SERVICE_RAW_OUT   0
#define MODES_NET_SERVICE_RAW_IN    1
#define MODES_NET_SERVICE_SBS_OUT   2
#define MODES_NET_SERVICE_SBS_IN    3
#define MODES_NET_SERVICE_HTTP      4
#define MODES_NET_SERVICE_RTL_TCP   5
#define MODES_NET_SERVICES_NUM     (MODES_NET_SERVICE_RTL_TCP + 1)

#define MODES_NET_SERVICE_FIRST     0
#define MODES_NET_SERVICE_LAST      MODES_NET_SERVICE_RTL_TCP

/**
 * \def SAFE_COND_SIGNAL(cond, mutex)
 * \def SAFE_COND_WAIT(cond, mutex)
 *
 * Signals are not threadsafe by default.
 * Taken from the Osmocom-SDR code and modified to
 * use Win-Vista+ functions.
 *
 * But not used yet.
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
 * Bits for `Modes.debug`:
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
#define DEBUG_ADSB_LOL   0x1000
#define DEBUG_CFG_FILE   0x2000

/**
 * \def DEBUG(bit, fmt, ...)
 * A more compact tracing macro.
 */
#define DEBUG(bit, fmt, ...)                       \
        do {                                       \
          if (Modes.debug & (bit))                 \
             modeS_flogf (stdout, "%s(%u): " fmt,  \
                 __FILE__, __LINE__, __VA_ARGS__); \
        } while (0)

/**
 * \def TRACE(fmt, ...)
 * As `DEBUG()`, but for the `DEBUG_GENERAL` bit only.
 * And with an added new-line for brevity.
 *
 * Ref. command-line option `--debug g`.
 */
#define TRACE(fmt, ...) DEBUG (DEBUG_GENERAL, fmt ".\n", __VA_ARGS__)

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
#define LOG_FILEONLY(fmt, ...)  do {                                           \
                                 if (Modes.log)                                \
                                    modeS_flogf (Modes.log, fmt, __VA_ARGS__); \
                               } while (0)

/**
 * A structure for things to ignore in `modeS_log()`
 */
typedef struct log_ignore {
        char               msg [100];
        struct log_ignore *next;
      } log_ignore;

#define SETMODE(fd, mode)  (void)_setmode (fd, mode)

#define NO_RETURN __declspec(noreturn)

typedef struct mg_http_message    mg_http_message;
typedef struct mg_connection      mg_connection;
typedef struct mg_mgr             mg_mgr;
typedef struct mg_addr            mg_addr;
typedef struct mg_str             mg_str;
typedef struct mg_timer           mg_timer;
typedef struct mg_iobuf           mg_iobuf;
typedef struct mg_ws_message      mg_ws_message;
typedef struct mg_http_serve_opts mg_http_serve_opts;
typedef char                      mg_file_path [MG_PATH_MAX];
typedef char                      mg_host_name [200];
typedef char                      mg_http_uri [256];

/**
 * Structure used to describe a network connection,
 * client or server.
 */
typedef struct connection {
        mg_connection     *c;                 /**< remember which connection this client/server is in */
        intptr_t           service;           /**< this client's service membership */
        mg_addr            rem;               /**< copy of `mg_connection::rem` */
        mg_host_name       rem_buf;           /**< address-string of `mg_connection::rem` */
        ULONG              id;                /**< copy of `mg_connection::id` */
        bool               keep_alive;        /**< client request contains "Connection: keep-alive" */
        bool               encoding_gzip;     /**< gzip compressed client data (not yet) */
        struct connection *next;              /**< next connection in this list for this service */
      } connection;

/**
 * A structure defining a passive or active network service.
 */
typedef struct net_service {
        mg_connection  **c;                /**< A pointer to the returned Mongoose connection(s) */
        char             descr [100];      /**< A textual description of this service */
        char             protocol [4];     /**< Equals "tcp" unless `is_udp == 1` */
        uint16_t         port;             /**< The port number */
        mg_host_name     host;             /**< The host name/address if `Modes.net_active == true` */
        uint16_t         num_connections;  /**< Number of clients/servers connected to this service */
        uint64_t         mem_allocated;    /**< Number of bytes allocated total for this service */
        bool             active_send;      /**< We are the sending side. Never duplex */
        bool             is_ip6;           /**< The above `host` address is an IPv6 address */
        bool             is_udp;           /**< The above `host` address was prefixed with `udp://` */
        char            *url;              /**< The allocated url for `mg_listen()` or `mg_connect()` */
        char            *last_err;         /**< Last error from a `MG_EV_ERROR` event */
      } net_service;

typedef enum metric_unit_t {
        MODES_UNIT_FEET   = 1,
        MODES_UNIT_METERS = 2
      } metric_unit_t;

#define UNIT_NAME(unit) (unit == MODES_UNIT_METERS ? "meters" : "feet")

/**
 * Spherical position: <br>
 * Latitude (North-South) and Longitude (East-West) coordinates. <br>
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
 * \def BIG_VAL
 * \def VALID_POS()
 *
 * Simple check for a valid geo-position
 */
#define SMALL_VAL        0.0001
#define BIG_VAL          9999999.0
#define VALID_POS(pos)   (fabs(pos.lon) >= SMALL_VAL && fabs(pos.lon) < 180.0 && \
                          fabs(pos.lat) >= SMALL_VAL && fabs(pos.lat) < 90.0)

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

        /* Hardware device statistics:
         */
        uint64_t        valid_preamble;
        uint64_t        demodulated;
        uint64_t        good_CRC;
        uint64_t        bad_CRC;
        uint64_t        fixed;
        uint64_t        single_bit_fix;
        uint64_t        two_bits_fix;
        uint64_t        out_of_phase;
        uint64_t        messages_total;
        unrecognized_ME unrecognized_ME [MAX_ME_TYPE];

        /* Aircraft statistics: \todo Move to 'aircraft_show_stats()'
         */
        uint64_t        unique_aircrafts;
        uint64_t        unique_aircrafts_CSV;
        uint64_t        unique_aircrafts_SQL;
        uint64_t        unique_helicopters;

        /* Network statistics:
         */
        uint64_t  cli_accepted   [MODES_NET_SERVICES_NUM];
        uint64_t  cli_removed    [MODES_NET_SERVICES_NUM];
        uint64_t  cli_unknown    [MODES_NET_SERVICES_NUM];
        uint64_t  srv_connected  [MODES_NET_SERVICES_NUM];
        uint64_t  srv_removed    [MODES_NET_SERVICES_NUM];
        uint64_t  srv_unknown    [MODES_NET_SERVICES_NUM];
        uint64_t  bytes_sent     [MODES_NET_SERVICES_NUM];
        uint64_t  bytes_recv     [MODES_NET_SERVICES_NUM];
        uint64_t  unique_clients [MODES_NET_SERVICES_NUM];
        uint64_t  HTTP_get_requests;
        uint64_t  HTTP_keep_alive_recv;
        uint64_t  HTTP_keep_alive_sent;
        uint64_t  HTTP_websockets;
        uint64_t  HTTP_400_responses;
        uint64_t  HTTP_404_responses;
        uint64_t  HTTP_500_responses;

        /* Network statistics for receiving RAW and SBS messages:
         */
        uint64_t  SBS_good;
        uint64_t  SBS_unrecognized;
        uint64_t  SBS_MSG_msg;
        uint64_t  SBS_AIR_msg;
        uint64_t  SBS_STA_msg;

        uint64_t  RAW_good;
        uint64_t  RAW_unrecognized;
        uint64_t  RAW_empty;
      } statistics;

/**
 * The device configuration for a local RTLSDR device.
 * NOT used for a RTL_TCP connection.
 */
typedef struct rtlsdr_conf {
        char         *name;              /**< The manufacturer name of the RTLSDR device to use. As in e.g. `"--device silver"` */
        int           index;             /**< The index of the RTLSDR device to use. As in e.g. `"--device 1"` */
        rtlsdr_dev_t *device;            /**< The RTLSDR handle from `rtlsdr_open()` */
        int           ppm_error;         /**< Set RTLSDR frequency correction (negative or positive) */
        bool          calibrate;         /**< Enable calibration for R820T/R828D type devices */
        int          *gains;             /**< Gain table reported from `rtlsdr_get_tuner_gains()` */
        int           gain_count;        /**< Number of gain values in above array */
        bool          power_cycle;       /**< \todo power down and up again before calling any RTLSDR API function */
      } rtlsdr_conf;

/**
 * The device configuration for a remote RTLSDR device.
 */
typedef struct rtltcp_conf {
        void         *info;              /**< The "RTL0" info message is here if we've got it (\ref RTL_TCP_info) */
        int           ppm_error;         /**< Set RTLSDR frequency correction */
        int           calibrate;         /**< Enable calibration for R820T/R828D type devices */
        int          *gains;             /**< Gain table reported from `rtlsdr_get_tuner_gains()` */
        int           gain_count;        /**< Number of gain values in above array */
      } rtltcp_conf;

/**
 * The device configuration for a SDRplay device.
 */
typedef struct sdrplay_conf {
        mg_file_path                     dll_name;           /**< Name and (relative) path of the "sdrplay_api.dll" to use */
        char                            *name;               /**< Name of the SDRplay device to use */
        int                              index;              /**< The index of the SDRplay device to use. As in e.g. `"--device sdrplay1"` */
        void                            *device;             /**< Device-handle from `sdrplay_init()` */
        bool                             if_mode;
        bool                             over_sample;
        bool                             disable_broadcast_notch;
        bool                             disable_DAB_notch;
        bool                             USB_bulk_mode;
        int                              gain_reduction;
        int                              ADSB_mode;         /**< == sdrplay_api_ControlParamsT::adsbMode */
        int                              BW_mode;
        int                             *gains;
        int                              gain_count;
        float                            min_version;
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
struct aircraft_info;
struct airports_priv;
struct sqlite3;

/**
 * All program global state is in this structure.
 */
typedef struct global_data {
        mg_file_path      who_am_I;                 /**< The full name of this program. */
        mg_file_path      where_am_I;               /**< The directory of this program. */
        mg_file_path      tmp_dir;                  /**< The `%TEMP%\\dump1090` directory. (no trailing `\\`). */
        mg_file_path      cfg_file;                 /**< The config-file (default: "where_am_I\\dump1090.cfg") */
        FILETIME          start_FILETIME;           /**< The start-time on `FILETIME` form */
        SYSTEMTIME        start_SYSTEMTIME;         /**< The start-time on `SYSTEMTIME` form */
        uintptr_t         reader_thread;            /**< Device reader thread ID. */
        CRITICAL_SECTION  data_mutex;               /**< Mutex to synchronize buffer access. */
        CRITICAL_SECTION  print_mutex;              /**< Mutex to synchronize printouts. */
        uint8_t          *data;                     /**< Raw IQ samples buffer. */
        uint32_t          data_len;                 /**< Length of raw IQ buffer. */
        uint16_t         *magnitude;                /**< Magnitude vector. */
        uint16_t         *magnitude_lut;            /**< I/Q -> Magnitude lookup table. */
        int               infile_fd;                /**< File descriptor for `--infile` option. */
        volatile bool     exit;                     /**< Exit from the main loop when true. */
        volatile bool     data_ready;               /**< Data ready to be processed. */
        uint32_t         *ICAO_cache;               /**< Recently seen ICAO addresses. */
        statistics        stat;                     /**< Decoder, aircraft and network statistics. */
        struct aircraft  *aircrafts;                /**< Linked list of active aircrafts. */
        uint64_t          last_update_ms;           /**< Last screen update in milliseconds. */
        uint64_t          max_messages;             /**< How many messages to process before quitting. */
        uint64_t          max_frames;               /**< How many frames in a sample-buffer to process (for testing SDRPlay) */

        /** Common stuff for RTLSDR and SDRplay:
         */
        char             *selected_dev;             /**< Name of selected device. */
        int               dig_agc;                  /**< Enable digital AGC. */
        int               bias_tee;                 /**< Enable bias-T voltage on coax input. */
        int               gain_auto;                /**< Use auto-gain. */
        uint32_t          band_width;               /**< The wanted bandwidth. Default is 0. */
        uint16_t          gain;                     /**< The gain setting for the active device (local or remote). Default is MODES_AUTO_GAIN. */
        uint32_t          freq;                     /**< The tuned frequency. Default is MODES_DEFAULT_FREQ. */
        uint32_t          sample_rate;              /**< The sample-rate. Default is MODES_DEFAULT_RATE.
                                                      *  \note This cannot be used yet since the code assumes a
                                                      *        pulse-width of 0.5 usec based on a fixed rate of 2 MS/s.
                                                      */
        rtlsdr_conf  rtlsdr;                        /**< RTLSDR local specific settings. */
        rtltcp_conf  rtltcp;                        /**< RTLSDR remote specific settings. */
        sdrplay_conf sdrplay;                       /**< SDRplay specific settings. */

        /** Lists of connections for each network service:
         */
        connection    *connections [MODES_NET_SERVICES_NUM];
        mg_connection *sbs_out;                     /**< SBS output listening connection. */
        mg_connection *sbs_in;                      /**< SBS input active connection. */
        mg_connection *raw_out;                     /**< Raw output active/listening connection. */
        mg_connection *raw_in;                      /**< Raw input listening connection. */
        mg_connection *http_out;                    /**< HTTP listening connection. */
        mg_connection *rtl_tcp_in;                  /**< RTL_TCP active connection. */
        mg_mgr         mgr;                         /**< Only one Mongoose connection manager. */
        char          *dns;                         /**< Use default Windows DNS server (not 8.8.8.8) */

        /** Aircraft history
         */
        uint64_t json_interval;
        int      json_aircraft_history_next;
        mg_str   json_aircraft_history [120];

        /** Configuration
         */
        mg_file_path infile;                     /**< Input IQ samples from file with option `--infile file`. */
        mg_file_path logfile_current;            /**< Write debug/info to file with option `--logfile file`. */
        mg_file_path logfile_initial;            /**< The initial `--logfile file` w/o the below pattern. */
        bool         logfile_daily;              /**< Create a new `logfile` at midnight; pattern always `x-<YYYY-MM-DD>.log`. */
        log_ignore  *logfile_ignore;             /**< Messages to ignore when writing to`logfile` */
        FILE        *log;                        /**< Open it for exclusive write access. */
        uint64_t     loops;                      /**< Read input file in a loop. */
        uint32_t     debug;                      /**< `DEBUG()` mode bits. */
        int          raw;                        /**< Raw output format. */
        int          net;                        /**< Enable networking. */
        int          net_only;                   /**< Enable just networking. */
        int          net_active;                 /**< With `Modes.net`, call `connect()` (not `listen()`). */
        int          silent;                     /**< Silent mode for network testing. */
        int          interactive;                /**< Interactive mode */
        uint16_t     interactive_rows;           /**< Interactive mode: max number of rows. */
        uint32_t     interactive_ttl;            /**< Interactive mode: TTL before deletion. */
        int          win_location;               /**< Use 'Windows Location API' to get the 'Modes.home_pos'. */
        int          only_addr;                  /**< Print only ICAO addresses. */
        int          metric;                     /**< Use metric units. */
        int          prefer_adsb_lol;            /**< Prefer using ADSB-LOL API even with '-DUSE_GEN_ROUTES'. */
        bool         error_correct_1;            /**< Fix 1 bit errors (default: true). */
        bool         error_correct_2;            /**< Fix 2 bit errors (default: false). */
        int          keep_alive;                 /**< Send "Connection: keep-alive" if HTTP client sends it. */
        mg_file_path web_page;                   /**< The base-name of the web-page to server for HTTP clients. */
        mg_file_path web_root;                   /**< And it's directory. */
        bool         web_root_touch;             /**< Touch all files in `web_root` first. */
        mg_file_path aircraft_db;                /**< The `aircraft-database.csv` file. */
        char        *aircraft_db_url;            /**< Value of key `aircrafts-update = url` */
        int          strip_level;                /**< For '--strip X' mode. */
        pos_t        home_pos;                   /**< Coordinates of home position. */
        cartesian_t  home_pos_cart;              /**< Coordinates of home position (cartesian). */
        bool         home_pos_ok;                /**< We have a good home position. */
        const char  *wininet_last_error;         /**< Last error from WinInet API. */
        char        *tests;                      /**< Perform tests specified by pattern. */
        int          tui_interface;              /**< Selected `--tui` interface. */
        bool         update;                     /**< Option `--update' was used to update missing .csv-files */

        /** For handling a `Modes.aircraft_db` file:
         */
        CSV_context           csv_ctx;           /**< Structure for the CSV parser. */
        struct aircraft_info *aircraft_list_CSV; /**< List of aircrafts sorted on address. From CSV-file only. */
        uint32_t              aircraft_num_CSV;  /**< The length of the list. */
        struct sqlite3        *sql_db;           /**< For the `aircraft_sql` file. */

        /** For handling a airport-data from .CSV files.
         */
        mg_file_path    airport_db;              /**< The `airports-codes.csv` file generated by `tools/gen_airport_codes_csv.py`. */
        mg_file_path    airport_freq_db;         /**< The `airports-frequencies.csv` file. Not used yet. */
        mg_file_path    airport_cache;           /**< The `%%TEMP%%\\dump1090\\airport-api-cache.csv`. */
        char           *airport_db_url;          /**< Value of key `airports-update = url`. Not effective yet. */

        struct airports_priv *airports_priv;     /**< Private data for `airports.c`. */

      } global_data;

extern global_data Modes;

#if defined(USE_READSB_DEMOD)
  typedef struct mag_buf {
          uint16_t *data;             /**< Magnitude data, starting with overlap from the previous block. */
          unsigned  length;           /**< Number of valid samples _after_ overlap. */
          unsigned  overlap;          /**< Number of leading overlap samples at the start of "data". */
                                      /**< also the number of trailing samples that will be preserved for next time. */
          uint64_t  sampleTimestamp;  /**< Clock timestamp of the start of this block, 12MHz clock. */
          uint64_t  sysTimestamp;     /**< Estimated system time at start of block. */
          double    mean_level;       /**< Mean of normalized (0..1) signal level. */
          double    mean_power;       /**< Mean of normalized (0..1) power level. */
          unsigned  dropped;          /**< (approx) number of dropped samples. */
          struct mag_buf *next;       /**< linked list forward link */
        } mag_buf;

  void demodulate2400 (struct mag_buf *mag);
#endif

#define MODES_DEFAULT_RATE         2000000
#define MODES_DEFAULT_FREQ         1090000000
#define MODES_ASYNC_BUF_NUMBERS    12
#define MODES_ASYNC_BUF_SIZE       (256*1024)

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
#define DEBUG_NOPREAMBLE_LEVEL       25

/**
 * Timeout for a screen refresh in interactive mode and
 * timeout for removing a stale aircraft.
 */
#define MODES_INTERACTIVE_REFRESH_TIME  250
#define MODES_INTERACTIVE_TTL         60000

/**
 * The structure we use to store information about a decoded message.
 */
typedef struct modeS_message {
        uint8_t  msg [MODES_LONG_MSG_BYTES]; /**< Binary message. */
        int      msg_bits;                   /**< Number of bits in message. */
        int      msg_type;                   /**< Downlink format #. */
        bool     CRC_ok;                     /**< True if CRC was valid. */
        uint32_t CRC;                        /**< Message CRC. */
        double   sig_level;                  /**< RSSI, in the range [0..1], as a fraction of full-scale power. */
        int      error_bit;                  /**< Bit corrected. -1 if no bit corrected. */
        uint8_t  AA [3];                     /**< ICAO Address bytes 1, 2 and 3 (big-endian). */
        bool     phase_corrected;            /**< True if phase correction was applied. */

        /** DF11
         */
        int ca;                              /**< Responder capabilities. */

        /** DF 17
         */
        int  ME_type;                        /**< Extended squitter message type. */
        int  ME_subtype;                     /**< Extended squitter message subtype. */
        int  heading;                        /**< Horizontal angle of flight. */
        bool heading_is_valid;               /**< We got a valid `heading`. */
        int  aircraft_type;                  /**< Aircraft identification. "Type A..D". */
        int  odd_flag;                       /**< 1 = Odd, 0 = Even CPR message. */
        int  UTC_flag;                       /**< UTC synchronized? */
        int  raw_latitude;                   /**< Non decoded latitude. */
        int  raw_longitude;                  /**< Non decoded longitude. */
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
        int flight_status;                   /**< Flight status for DF4, 5, 20 and 21. */
        int DR_status;                       /**< Request extraction of downlink request. */
        int UM_status;                       /**< Request extraction of downlink request. */
        int identity;                        /**< 13 bits identity (Squawk). */

        /** Fields used by multiple message types.
         */
        int           altitude;
        metric_unit_t unit;

        /** For messages from a TCP SBS source (basestation input)
         */
        bool SBS_in;          /**< true for a basestation input message */
        int  SBS_msg_type;    /**< "MSG,[1-8],...". \ref http://woodair.net/sbs/article/barebones42_socket_data.htm */
        bool SBS_pos_valid;

      } modeS_message;

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

/*
 * For `$(OBJ_DIR)/web-page-*.c` files:
 */
typedef struct file_packed {
        const unsigned char *data;
        size_t               size;
        time_t               mtime;
        const char          *name;
      } file_packed;

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
void        modeS_log_set (void);
bool        modeS_log_init (void);
void        modeS_log_exit (void);
bool        modeS_log_add_ignore (const char *msg);
void        modeS_err_set (bool on);
char       *modeS_err_get (void);
char       *modeS_SYSTEMTIME_to_str (const SYSTEMTIME *st, bool show_YMD);
char       *modeS_FILETIME_to_str (const FILETIME *ft, bool show_YMD);
char       *modeS_FILETIME_to_loc_str (const FILETIME *ft, bool show_YMD);
void        modeS_signal_handler (int sig);
bool        decode_RAW_message (mg_iobuf *msg, int loop_cnt);  /* in 'dump1090.c' */
bool        decode_SBS_message (mg_iobuf *msg, int loop_cnt);  /* in 'dump1090.c' */
uint32_t    ato_hertz (const char *Hertz);
bool        str_startswith (const char *s1, const char *s2);
bool        str_endswith (const char *s1, const char *s2);
char       *str_ltrim (char *s);
char       *str_rtrim (char *s);
char       *str_trim (char *s);
char       *str_join (char *const *array, const char *sep);
char       *str_tokenize (char *ptr, const char *sep, char **end);
char       *str_sep (char **stringp, const char *delim);
int         hex_digit_val (int c);
const char *unescape_hex (const char *value);
char       *basename (const char *fname);
char       *dirname (const char *fname);
char       *slashify (char *fname);
int        _gettimeofday (struct timeval *tv, void *timezone);
int         get_timespec_UTC (struct timespec *ts);
double      get_usec_now (void);
void        get_FILETIME_now (FILETIME *ft);
void        init_timings (void);
void        crtdbug_init (void);
void        crtdbug_exit (void);
const char *win_strerror (DWORD err);
const char *get_rtlsdr_error (void);
const char *qword_str (uint64_t val);
const char *dword_str (DWORD val);
void       *memdup (const void *from, size_t size);
uint32_t    random_range (uint32_t min, uint32_t max);
int32_t     random_range2 (int32_t min, int32_t max);
int         touch_file (const char *file);
int         touch_dir (const char *dir, bool recurse);
FILE       *fopen_excl (const char *file, const char *mode);
uint32_t    download_to_file (const char *url, const char *file);
char       *download_to_buf  (const char *url);
int         download_status (void);
int         load_dynamic_table (struct dyn_struct *tab, int tab_size);
int         unload_dynamic_table (struct dyn_struct *tab, int tab_size);
bool        test_add (char **pattern, const char *what);
bool        test_contains (const char *pattern, const char *what);
void        puts_long_line (const char *start, size_t indent);
void        spherical_to_cartesian (const pos_t *pos, cartesian_t *cart);
bool        cartesian_to_spherical (const cartesian_t *cart, pos_t *pos, double heading);
double      cartesian_distance (const cartesian_t *a, const cartesian_t *b);
double      great_circle_dist (pos_t pos1, pos_t pos2);
double      closest_to (double val, double val1, double val2);
void        decode_CPR (struct aircraft *a);
const char *mz_version (void);                 /* in 'externals/zip.c' */
void        rx_callback (uint8_t *buf, uint32_t len, void *ctx);

void show_version_info (bool verbose);

#if defined(USE_MIMALLOC)
  void mimalloc_init (void);
  void mimalloc_exit (void);
  void mimalloc_stats (void);
#endif

/*
 * in 'pconsole.c'. Not used yet.
 */
struct pconsole_t;
bool pconsole_create (struct pconsole_t *pty, const char *cmd_path, const char **cmd_argv);

#if 0
  /*
   * in 'externals/wcwidth.c'
   */
  int wcwidth (wchar_t wc);
  int wcswidth (const wchar_t *wcs, size_t n);
#endif

/**
 * \def MSEC_TIME()
 * Returns a 64-bit tick-time value with 1 millisec granularity.
 */
#if defined(USE_gettimeofday)
  static __inline uint64_t MSEC_TIME (void)
  {
    struct timeval now;

    _gettimeofday (&now, NULL);
    return (1000 * (uint64_t)now.tv_sec) + (now.tv_usec / 1000);
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
        int *flag;    /**< if not NULL, set *flag to val when option found. */
        int  val;     /**< if flag not NULL, value to set \c *flag to; else return value. */
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
