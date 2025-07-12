/**\file    misc.h
 * \ingroup Misc
 * \brief   Various macros, definitions and prototypes for "misc.c".
 */
#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <winsock2.h>
#include <windows.h>
#include <wchar.h>
#include <rtl-sdr/rtl-sdr.h>
#include <SDRplay/sdrplay_api.h>

#include "mongoose.h"
#include "cfg_file.h"
#include "convert.h"
#include "csv.h"
#include "geo.h"
#include "fifo.h"

/**
 * Various helper macros.
 */
#define MODES_NOTUSED(V)   ((void)V)
#define IS_SLASH(c)        ((c) == '\\' || (c) == '/')
#define DIM(array)         (sizeof(array) / sizeof(array[0]))
#define NONE_STR           "<none>"
#define STDIN_FILENO       0

/**
 * Network services indices; `global_data::connections [N]`:
 */
#define MODES_NET_SERVICE_RAW_OUT   0
#define MODES_NET_SERVICE_RAW_IN    1
#define MODES_NET_SERVICE_SBS_OUT   2
#define MODES_NET_SERVICE_SBS_IN    3
#define MODES_NET_SERVICE_HTTP4     4
#define MODES_NET_SERVICE_HTTP6     5
#define MODES_NET_SERVICE_RTL_TCP   6
#define MODES_NET_SERVICE_DNS       7
#define MODES_NET_SERVICES_NUM     (MODES_NET_SERVICE_DNS + 1)

#define MODES_NET_SERVICE_FIRST     0
#define MODES_NET_SERVICE_LAST      MODES_NET_SERVICE_DNS

/**
 * \def DEF_WIN_FUNC
 * Handy macro to both define and declare the function-pointers
 * for WINAPI functions.
 */
#define DEF_WIN_FUNC(ret, name, args)  typedef ret (WINAPI *func_##name) args; \
                                       static func_##name p_##name = NULL

/**
 * \def DEF_C_FUNC
 * As above, but for C-functions.
 */
#define DEF_C_FUNC(ret, name, args)  typedef ret (*func_##name) args; \
                                     static func_##name p_##name = NULL


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
#define DEBUG_PLANE      0x4000

/**
 * \def DEBUG(bit, fmt, ...)
 * A more compact tracing macro.
 */
#define DEBUG(bit, fmt, ...)                      \
        do {                                      \
          if (Modes.debug & (bit))                \
             modeS_flogf (stdout, "%s(%u): " fmt, \
                 __FILE__, __LINE__,              \
                 ## __VA_ARGS__);                 \
        } while (0)

/**
 * \def TRACE(fmt, ...)
 * As `DEBUG()`, but for the `DEBUG_GENERAL` bit only.
 *
 * Ref. command-line option `--debug g`.
 */
#define TRACE(fmt, ...) DEBUG (DEBUG_GENERAL, fmt, ## __VA_ARGS__)

/**
 * \def LOG_STDOUT(fmt, ...)
 *  Print to both `stdout` and optionally to `Modes.log`.
 *
 * \def LOG_STDERR(fmt, ...)
 *  Print to both `stderr` and optionally to `Modes.log`.
 *
 * \def LOG_FILEONLY(fmt, ...)
 *  Print to `Modes.log` only.
 *
 * \def LOG_FILEONLY2(fmt, ...)
 *  Like `LOG_FILEONLY()`, but not when `--debug P` is active.
 */
#define LOG_STDOUT(fmt, ...)    modeS_flogf (stdout, fmt, ## __VA_ARGS__)
#define LOG_STDERR(fmt, ...)    modeS_flogf (stderr, fmt, ## __VA_ARGS__)
#define LOG_FILEONLY(fmt, ...)  do {                               \
                                  if (Modes.log)                   \
                                     modeS_flogf (Modes.log, fmt,  \
                                                  ## __VA_ARGS__); \
                                } while (0)

#define LOG_FILEONLY2(fmt, ...) do {                                     \
                                  if (!(Modes.debug & DEBUG_PLANE))      \
                                     LOG_FILEONLY (fmt, ## __VA_ARGS__); \
                                } while (0)

/**
 * \typedef search_list
 * A structure for things to search in `search_list_name()` and `flags_decode()`
 */
typedef struct search_list {
        DWORD       value;
        const char *name;
      } search_list;

/*
 * Forwards:
 * Details elsewhere.
 */
typedef struct aircraft      aircraft;
typedef struct aircraft_info aircraft_info;
typedef struct airports_priv airports_priv;
typedef struct sqlite3       sqlite3;
typedef struct smartlist_t   smartlist_t;
typedef enum   a_sort_t      a_sort_t;

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
        uint32_t         num_connections;  /**< Number of clients/servers connected to this service */
        mg_timer        *timer;            /**< For handling timeout in the network service. */
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
 * Statistics for HTTP4 / HTTP6 servers
 */
typedef struct HTTP_statistics {
        uint64_t  HTTP_get_requests;
        uint64_t  HTTP_keep_alive_recv;
        uint64_t  HTTP_keep_alive_sent;
        uint64_t  HTTP_websockets;
        uint64_t  HTTP_tls_handshakes;
        uint64_t  HTTP_400_responses;
        uint64_t  HTTP_404_responses;
        uint64_t  HTTP_500_responses;
      } HTTP_statistics;

/**
 * Keep all collected statistics in this structure.
 * \todo Move to 'stats.h'
 */
typedef struct statistics {

        /* Hardware device statistics:
         */
        uint64_t        FIFO_enqueue;
        uint64_t        FIFO_dequeue;
        uint64_t        FIFO_full;
        uint64_t        samples_processed;
        uint64_t        samples_dropped;
        uint64_t        samples_recv_rtltcp;  /**< Samples from RTLTCP. Equals `samples_processed` if nothing dropped by FIFO */
        uint64_t        valid_preamble;
        uint64_t        demod_modeac;
        uint64_t        demod_accepted [3];   /**< MODES_MAX_BITERRORS+1 */
        uint64_t        demodulated;
        uint64_t        demod_rejected_unknown;
        uint64_t        CRC_good;             /**< good message; no CRC fixed */
        uint64_t        CRC_bad;              /**< unfixable message */
        uint64_t        CRC_fixed;            /**< 1 or 2 bit error fixed */
        uint64_t        CRC_single_bit_fix;
        uint64_t        CRC_two_bits_fix;
        uint64_t        out_of_phase;
        uint64_t        messages_total;
        uint64_t        messages_shown;
        uint64_t        addr_filtered;
        unrecognized_ME unrecognized_ME [MAX_ME_TYPE];

        /* Signal/noise statistics:
         */
        double          noise_power_sum;
        uint64_t        noise_power_count;
        double          signal_power_sum;
        uint64_t        signal_power_count;
        double          peak_signal_power;
        uint64_t        strong_signal_count;

        /* Aircraft statistics: \todo Move to 'aircraft_show_stats()'
         */
        uint64_t        unique_aircrafts;
        uint64_t        unique_aircrafts_CSV;
        uint64_t        unique_aircrafts_SQL;
        uint64_t        unique_helicopters;
        uint64_t        cart_errors;
        uint64_t        cpr_errors;

        uint64_t        cpr_global_ok;
        uint64_t        cpr_global_bad;
        uint64_t        cpr_global_skipped;
        uint64_t        cpr_global_speed_checks;
        uint64_t        cpr_global_dist_checks;

        uint64_t        cpr_local_ok;
        uint64_t        cpr_local_bad;
        uint64_t        cpr_local_skipped;
        uint64_t        cpr_local_speed_checks;
        uint64_t        cpr_local_dist_checks;
        uint64_t        cpr_local_aircraft_relative;
        uint64_t        cpr_local_receiver_relative;

        uint64_t        cpr_airborne;
        uint64_t        cpr_surface;
        uint64_t        cpr_filtered;
        uint64_t        suppressed_altitude_messages;

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

        /* `HTTP_stat[0]` is for HTTP IPv4 and `HTTP_stat[1]` is for IPv6
         */
        HTTP_statistics HTTP_stat [2];

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
        char         *remote;            /**< The URL of the RTLTCP host */
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

/**
 * The device configuration for a AirSpy device.
 */
typedef struct airspy_conf {
        mg_file_path     dll_name;           /**< Name and (relative) path of the "airspy.dll" to use */
        char            *name;               /**< Name of the AirSpy device to use */
        int              index;              /**< The index of the AirSpy device to use. As in e.g. `"--device airspy0"` */
        void            *device;             /**< Device-handle from `airspy_init()` */
        int             *gains;
        int              gain_count;
      } airspy_conf;

/**
 * All program global state is in this structure.
 */
typedef struct global_data {
        mg_file_path        who_am_I;                 /**< The full name of this program. */
        mg_file_path        where_am_I;               /**< The directory of this program. */
        mg_file_path        tmp_dir;                  /**< The `%TEMP%\\dump1090` directory. (no trailing `\\`). */
        mg_file_path        results_dir;              /**< The `%TEMP%\\dump1090\\standing-data\\results` directory. */
        mg_file_path        cfg_file;                 /**< The config-file (default: "where_am_I\\dump1090.cfg"). */
        mg_file_path        sys_dir;                  /**< The full name of `%SystemRoot\\system32`. */
        FILETIME            start_FILETIME;           /**< The start-time on `FILETIME` form. */
        SYSTEMTIME          start_SYSTEMTIME;         /**< The start-time on `SYSTEMTIME` form. */
        LONG                timezone;                 /**< Our time-zone in minutes. */
        HANDLE              reader_thread;            /**< Device reader thread handle. */
        HANDLE              reader_event;             /**< Event to signal end of process (not used ATM). */
        CRITICAL_SECTION    data_mutex;               /**< Mutex to synchronize buffer access. */
        CRITICAL_SECTION    print_mutex;              /**< Mutex to synchronize printouts. */
        uint16_t           *mag_lut;                  /**< I/Q -> Magnitude lookup table. */
        uint16_t           *log10_lut;                /**< Magnitude -> log10 lookup table. */
        convert_format      input_format;             /**< Converted input format. */
        uint32_t            FIFO_bufs;                /**< Number of buffers for `fifo_init()` */
        uint32_t            FIFO_acquire_ms;          /**< `fifo_acquire()` timeout in milli-sec (default 100). */
        uint32_t            FIFO_dequeue_ms;          /**< `fifo_dequeue()` timeout in milli-sec (default 100). */
        bool                FIFO_active;              /**< We have (and need) a FIFO for `mag_buf` data. */
        bool                phase_enhance;            /**< Enable phase enhancement in `demod_*()`. */
        int                 infile_fd;                /**< File descriptor for `--infile` option. */
        volatile bool       exit;                     /**< Exit from the main loop when true. */
        uint32_t           *ICAO_cache;               /**< Recently seen ICAO addresses. */
        statistics          stat;                     /**< Decoder, aircraft and network statistics. */
        smartlist_t        *aircrafts;                /**< List of active aircrafts. */
        uint64_t            last_update_ms;           /**< Last screen update in milliseconds. */
        uint64_t            max_messages;             /**< How many messages to process before quitting. */
        uint64_t            max_frames;               /**< How many frames in a sample-buffer to process (for testing SDRPlay). */
        bool                no_stats;                 /**< Set to `true` in case no point showing statistics. */
        bool                mode_AC;                  /**< Enable decoding of SSR Modes A & C. */
        bool                under_appveyor;           /**< true if running on AppVeyor CI testing */
        uint32_t            a_follow;                 /**< Follow and log details of this aircraft. For debugging. */

        /** Common stuff for RTLSDR and SDRplay:
         */
        char               *selected_dev;             /**< Name of selected device. */
        int                 dig_agc;                  /**< Enable digital AGC. */
        int                 bias_tee;                 /**< Enable bias-T voltage on coax input. */
        int                 gain_auto;                /**< Use auto-gain. */
        uint32_t            band_width;               /**< The wanted bandwidth. Default is 0. */
        uint16_t            gain;                     /**< The gain setting for the active device (local or remote). Default is MODES_AUTO_GAIN. */
        uint32_t            freq;                     /**< The tuned frequency. Default is MODES_DEFAULT_FREQ. */
        uint64_t            sample_counter;           /**< Accumulated count of samples */
        uint32_t            sample_rate;              /**< The sample-rate. Default is MODES_DEFAULT_RATE. */
        demod_func          demod_func;               /**< and the associated demodulator-function. */
        unsigned            trailing_samples;         /**< Extra trailing samples in magnitude buffers */
        unsigned            bytes_per_sample;         /**< Bytes per sample; 2 for RTLSDR or 4 for SDRPlay */

        bool                DC_filter;                /**< Should we apply a DC filter? */
        bool                measure_noise;            /**< Should we measure noise power? */
        convert_func        converter_func;           /**< Function for converting IQ-data */
        convert_state      *converter_state;          /**< Function state placeholder */

        rtlsdr_conf         rtlsdr;                   /**< RTLSDR local specific settings. */
        rtltcp_conf         rtltcp;                   /**< RTLSDR remote specific settings. */
        sdrplay_conf        sdrplay;                  /**< SDRplay specific settings. */
        airspy_conf         airspy;                   /**< AirSpy specific settings. */

        /** Lists of connections for each network service:
         */
#ifdef USE_SMARTLIST_NETIO
        smartlist_t   *connections [MODES_NET_SERVICES_NUM];
#else
        connection    *connections [MODES_NET_SERVICES_NUM];
#endif
        mg_connection *sbs_out;                     /**< SBS output listening connection. */
        mg_connection *sbs_in;                      /**< SBS input active connection. */
        mg_connection *raw_out;                     /**< Raw output active/listening connection. */
        mg_connection *raw_in;                      /**< Raw input listening connection. */
        mg_connection *http4_out;                   /**< HTTP listening connection. IPv4 */
        mg_connection *http6_out;                   /**< HTTP listening connection. IPv6 */
        mg_connection *rtl_tcp_in;                  /**< RTL_TCP active connection. IPv4 only */
        mg_connection *dns_in;                      /**< DNS active connection. IPv4 only */
        mg_mgr         mgr;                         /**< Only one Mongoose connection manager */
        char          *dns4;                        /**< Use default Windows DNSv4 server (not 8.8.8.8) */
        char          *dns6;                        /**< Or a IPv6 server */
        bool           show_host_name;              /**< Try to show the hostname too in `net_str_addr()` */
        bool           https_enable;                /**< Enable TLS (MG_TLS_BUILTIN) for HTTP server */
        bool           https_lol_API;               /**< Enable Mongoose's TLS (MG_TLS_BUILTIN) over WinInet. Not yet */
        bool           reverse_resolve;             /**< Call `net_reverse_resolve()` on accepted clients */
        uint32_t       net_poll_ms;                 /**< `mg_mgr_poll()` timeout in milli-sec (default 20). */

        /** Aircraft history
         */
        uint64_t json_interval;
        int      json_aircraft_history_next;
        mg_str   json_aircraft_history [120];

        /** Configuration
         */
        mg_file_path  infile;                     /**< Input IQ samples from file with option `--infile file`. */
        mg_file_path  logfile_current;            /**< Write debug/info to file with option `--logfile file`. */
        mg_file_path  logfile_initial;            /**< The initial `--logfile file` w/o the below pattern. */
        bool          logfile_daily;              /**< Create a new `logfile` at midnight; pattern always `x-<YYYY-MM-DD>.log`. */
        FILE         *log;                        /**< Open it for exclusive write access. */
        uint64_t      loops;                      /**< Read input file in a loop. */
        uint32_t      debug;                      /**< `DEBUG()` mode bits. */
        int           raw;                        /**< Raw output format. */
        int           net;                        /**< Enable networking. */
        int           net_only;                   /**< Enable just networking. */
        int           net_active;                 /**< With `Modes.net`, call `connect()` (not `listen()`). */
        int           silent;                     /**< Silent mode for network testing. */
        int           interactive;                /**< Interactive mode */
        uint16_t      interactive_rows;           /**< Interactive mode: max number of rows. */
        uint32_t      interactive_ttl;            /**< Interactive mode: TTL before deletion. */
        int           win_location;               /**< Use 'Windows Location API' to get the 'Modes.home_pos'. */
        int           only_addr;                  /**< Print only ICAO addresses. */
        int           metric;                     /**< Use metric units. */
        int           prefer_adsb_lol;            /**< Prefer using ADSB-LOL API even with '-DUSE_BIN_FILES'. */
        bool          error_correct_1;            /**< Fix 1 bit errors (default: true). */
        bool          error_correct_2;            /**< Fix 2 bit errors (default: false). */
        int           keep_alive;                 /**< Send "Connection: keep-alive" if HTTP client sends it. */
        int           http_ipv6;                  /**< Enable IPv6 for HTTP server. */
        int           http_ipv6_only;             /**< Allow only IPv6 for HTTP server. */
        int           speech_enable;              /**< Enable speech for planes entering and leaving. */
        int           speech_volume;              /**< Speech volume; 0 - 100 percent */
        mg_file_path  web_page_full;              /**< The fully qualified path of web_page */
        mg_file_path  web_page;                   /**< The base-name of the web-page to server for HTTP clients. */
        mg_file_path  web_root;                   /**< And it's directory. */
        bool          web_root_touch;             /**< Touch all files in `web_root` first. */
        bool          web_send_rssi;              /**< Send the "RSSI" in the JSON-data to the web-server */
        mg_file_path  aircraft_db;                /**< The `aircraft-database.csv` file. */
        char        *aircraft_db_url;             /**< Value of key `aircrafts-update = url` */
        int           strip_level;                /**< For '--strip X' mode. */
        pos_t         home_pos;                   /**< Coordinates of home position. */
        cartesian_t   home_pos_cart;              /**< Coordinates of home position (cartesian). */
        bool          home_pos_ok;                /**< We have a good home position. */
        double        max_dist;                   /**< Absolute maximum decoding distance, in metres */
        double        min_dist;                   /**< Absolute minimum distance for '--only-addr', in metres */
        int           a_sort;                     /**< The column sort method for aircrafts in `--interactive` mode. >= 0 is ascending, < 0 descending */
        bool          wininet_HTTP2;              /**< Enable HTTP/2 for WinInet API. */
        const char   *wininet_last_error;         /**< Last error from WinInet API. */
        char         *tests;                      /**< Perform tests specified by pattern. */
        int           tui_interface;              /**< Selected `--tui` interface. */
        bool          update;                     /**< Option `--update' was used to update missing .csv-files */
        char         *icao_spec;                  /**< A ICAO-filter was specified */
        mg_str        icao_filter;
        bool          icao_invert;
        bool          internal_error;
        bool          cpr_trace;                 /**< Report CPR events to .log-file? default true */

        /** For handling a `Modes.aircraft_db` file:
         */
        CSV_context       csv_ctx;               /**< Structure for the CSV parser. */
        aircraft_info    *aircraft_list_CSV;     /**< List of aircrafts sorted on address. From CSV-file only. */
        uint32_t          aircraft_num_CSV;      /**< The length of the list. */
        sqlite3          *sql_db;                /**< For the `aircraft_sql` file. */

        /** For handling a airport-data from .CSV files.
         */
        mg_file_path    airport_db;              /**< The `airports-codes.csv` file generated by `tools/gen_airport_codes_csv.py`. */
        mg_file_path    airport_freq_db;         /**< The `airports-frequencies.csv` file. Not used yet. */
        mg_file_path    airport_cache;           /**< The `%%TEMP%%\\dump1090\\airport-api-cache.csv`. */
        char           *airport_db_url;          /**< Value of key `airports-update = url`. Not effective yet. */

        airports_priv  *airports_priv;           /**< Private data for `airports.c`. */
        smartlist_t    *logfile_ignore;          /**< Messages to ignore when writing to`logfile`. A list of `log_ignore` */

      } global_data;

extern global_data Modes;

#define MODES_DEFAULT_RATE         2000000
#define MODES_DEFAULT_FREQ         1090000000
#define MODES_ASYNC_BUF_NUMBERS    15
#define MODES_ASYNC_BUF_SIZE       (256*1024)

#define MODES_SHORT_MSG_BYTES      7
#define MODES_LONG_MSG_BYTES      14
#define MODES_SHORT_MSG_BITS      (8 * MODES_SHORT_MSG_BYTES)  /* == 56 */
#define MODES_LONG_MSG_BITS       (8 * MODES_LONG_MSG_BYTES)   /* == 112 */

#define MODES_PREAMBLE_US          8         /* microseconds */
#define MODES_FULL_LEN             (MODES_PREAMBLE_US + MODES_LONG_MSG_BITS)

#define MODES_MAG_BUFFERS          12
#define MODES_MAG_BUF_SAMPLES      (MODES_ASYNC_BUF_SIZE / 2)    /* 256kB. Each sample is 2 bytes */

#define MODES_MAX_SBS_SIZE          256

#define MODES_ICAO_CACHE_LEN       1024   /* Power of two required. */
#define MODES_ICAO_CACHE_TTL         60   /* Time to live of cached addresses (sec). */

/**
 * Flags for the various `demod-*.c' functions:
 */
#define MODEAC_MSG_SQUELCH_LEVEL   0x07FF                 /* Average signal strength limit */
#define MODES_MSG_SQUELCH_DB       4.0                    /* Minimum SNR, in dB */
#define MODEAC_MSG_SAMPLES         (25 * 2)               /* include up to the SPI bit */

#define MODES_PREAMBLE_SAMPLES     (2 * MODES_PREAMBLE_US)
#define MODES_SHORT_MSG_SAMPLES    (2 * MODES_SHORT_MSG_BITS)
#define MODES_LONG_MSG_SAMPLES     (2 * MODES_LONG_MSG_BITS)
#define MODES_MSG_ENCODER_ERRS     3                      /* Maximum number of encoding errors */

#define MAX_AMPLITUDE              65535.0
#define MAX_POWER                  (MAX_AMPLITUDE * MAX_AMPLITUDE)

/**
 * When debug is set to `DEBUG_NOPREAMBLE', the first sample must be
 * at least greater than a given level for us to dump the signal.
 */
#define DEBUG_NOPREAMBLE_LEVEL         25

/**
 * Timeout (milli-sec) for a screen refresh in interactive mode and
 * timeout for removing a stale aircraft.
 */
#define MODES_INTERACTIVE_REFRESH_TIME  250
#define MODES_INTERACTIVE_TTL         60000

/**
 * Set on addresses to indicate they are not ICAO addresses.
 * The 24-bit mask 0xFFFFFF is used to detect such an address.
 */
#define MODES_NON_ICAO_ADDRESS  (1 << 24)
#define MODES_ICAO_ADDRESS_MASK 0xFFFFFF

/**
 * \typedef datasource_t
 * Where did a bit of data arrive from? In order of increasing priority
 */
typedef enum datasource_t {
        SOURCE_INVALID,        /**< data is not valid */
        SOURCE_MODE_AC,        /**< A/C message */
        SOURCE_MLAT,           /**< derived from mlat */
        SOURCE_MODE_S,         /**< data from a Mode S message, no full CRC */
        SOURCE_MODE_S_CHECKED, /**< data from a Mode S message with full CRC */
        SOURCE_TISB,           /**< data from a TIS-B extended squitter message */
        SOURCE_ADSR,           /**< data from a ADS-R extended squitter message */
        SOURCE_ADSB,           /**< data from a ADS-B extended squitter message */
      } datasource_t;

/**
 * \typedef modeS_message
 * The structure we use to store information about a decoded message.
 */
typedef struct modeS_message {
        uint64_t timestamp_msg;              /**< Timestamp of the message (12MHz clock). */
        uint64_t sys_timestamp_msg;          /**< Timestamp of the message (system time). */
        uint8_t  msg [MODES_LONG_MSG_BYTES]; /**< Binary message. */
        int      msg_bits;                   /**< Number of bits in message. */
        int      msg_type;                   /**< Downlink format #. */
        bool     CRC_ok;                     /**< True if CRC was valid. */
        uint32_t CRC;                        /**< Message CRC. */
        double   sig_level;                  /**< RSSI, in the range [0..1], as a fraction of full-scale power. */
        int      error_bits;                 /**< Number of bits corrected. */
        int      score;                      /**< Scoring from score modeS_message, if used. */
        uint32_t addr;                       /**< ICAO Address, little endian. */
        uint16_t addrtype;                   /**< Address format / source. \ref addrtype_t */
        bool     phase_corrected;            /**< True if phase correction was applied. */
        bool     reliable;                   /**< True if ... */
        uint32_t AC_flags;                   /**< Flags. \ref AIRCRAFT_FLAGS. */
        uint32_t category;                   /**< A0 - D7 encoded as a single hex byte. */

        /** DF11, DF17
         */
        int      capa;                       /**< Responder capabilities. */
        int      IID;

        /** DF17, DF18
         */
        int      ME_type;                    /**< Extended squitter message type. */
        int      ME_subtype;                 /**< Extended squitter message subtype. */
        double   heading;                    /**< Horizontal angle of flight. [0 .. 360] */
        int      aircraft_type;              /**< Aircraft identification. "Type A..D". */
        int      odd_flag;                   /**< 1 = Odd, 0 = Even CPR message. */
        int      UTC_flag;                   /**< UTC synchronized? */
        int      raw_latitude;               /**< Non decoded latitude. */
        int      raw_longitude;              /**< Non decoded longitude. */
        char     flight [9];                 /**< 8 chars flight number. */
        int      EW_dir;                     /**< 0 = East, 1 = West. */
        int      EW_velocity;                /**< E/W velocity. */
        int      NS_dir;                     /**< 0 = North, 1 = South. */
        int      NS_velocity;                /**< N/S velocity. */
        int      vert_rate_source;           /**< Vertical rate source. */
        int      vert_rate_sign;             /**< Vertical rate sign. */
        int      vert_rate;                  /**< Vertical rate. */
        double   velocity;                   /**< Computed from EW and NS velocity. In Knots */
        pos_t    position;                   /**< Coordinates obtained from CPR encoded data if/when decoded */
        unsigned nuc_p;                      /**< NUCp value implied by message type
                                              * NUCp == "Navigation Uncertainty Category"
                                              * \sa https://www.icao.int/APAC/Documents/edocs/cns/AIGD%20Edition%2015.0.pdf
                                              * \sa The-1090MHz-Riddle.pdf Chapter 9.2.1
                                              */
        /** DF 18
         */
        int      cf;                         /**< Control Field */

        /** DF4, DF5, DF20, DF21
         */
        int      flight_status;              /**< Flight status for DF4, 5, 20 and 21. */
        int      DR_status;                  /**< Request extraction of downlink request. */
        int      UM_status;                  /**< Request extraction of downlink request. */
        int      identity;                   /**< 13 bits identity (Squawk). */

        /** DF20, DF21
         */
        uint32_t BDS;                        /**< "Comm-B Data Selector" / BDS value implied if overlay control was used
                                               * \ref The-1090MHz-Riddle.pdf Chapter 15.2
                                               */

        /** Fields used by multiple message types.
         */
        int           altitude;
        int           altitude_HAE;
        int           HAE_delta;
        metric_unit_t unit;
        datasource_t  source;

        /** Raw data, just extracted directly from the message
         */
        uint8_t       MB [7];
        uint8_t       MD [10];
        uint8_t       ME [7];
        uint8_t       MV [7];

        /** For messages from a TCP SBS source (basestation input)
         */
        bool SBS_in;          /**< true for a basestation input message */
        int  SBS_msg_type;    /**< "MSG,[1-8],...". \sa http://woodair.net/sbs/article/barebones42_socket_data.htm */
        bool SBS_pos_valid;

      } modeS_message;

/**
 * \typedef dyn_struct
 * Generic table for loading DLLs and functions from them.
 */
typedef struct dyn_struct {
        const bool  optional;
        HINSTANCE   mod_handle;
        const char *mod_name;
        const char *func_name;
        void      **func_addr;
      } dyn_struct;

/**
 * \typedef file_packed
 * For `$(OBJ_DIR)/web-page-*.c` files:
 */
typedef struct file_packed {
        const uint8_t *data;
        size_t         size;
        time_t         mtime;
        const char    *name;
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

void  modeS_log (const char *buf);
void  modeS_logc (char c, void *param);
void  modeS_flog (FILE *f, const char *buf);
void  modeS_flogf (FILE *f, _Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(2, 3);
void  modeS_log_set (void);
bool  modeS_log_init (void);
void  modeS_log_exit (void);
bool  modeS_log_add_ignore (const char *msg);
void  modeS_err_set (bool on);
char *modeS_err_get (void);
char *modeS_SYSTEMTIME_to_str (const SYSTEMTIME *st, bool show_YMD);
char *modeS_FILETIME_to_str (const FILETIME *ft, bool show_YMD);
char *modeS_FILETIME_to_loc_str (const FILETIME *ft, bool show_YMD);
void  modeS_signal_handler (int sig);
int   modeS_vasprintf (char **bufp, _Printf_format_string_ const char *format, va_list args);
int   modeS_asprintf  (char **bufp, _Printf_format_string_ const char *format, ...)  ATTR_PRINTF(2, 3);

/**
 * Decoding functions in `dump1090.c` needed by the various
 * demodulators in `demod-*.c`:
 */
int  decode_mode_S_message (modeS_message *mm, const uint8_t *_msg);
void decode_mode_A_message (modeS_message *mm, int mode_A);
bool decode_RAW_message (mg_iobuf *msg, int loop_cnt);
bool decode_SBS_message (mg_iobuf *msg, int loop_cnt);
void modeS_user_message (modeS_message *mm);
int  modeS_message_len_by_type (int type);
int  modeS_message_score (const uint8_t *msg, int valid_bits);
int  mode_A_to_mode_C (u_int Mode_A);
void background_tasks (void);
void rx_callback (uint8_t *buf, uint32_t len, void *ctx);

/*
 * Functions in `misc.c'
 */
bool        init_misc (void);
const char *get_user_name (void);
char       *copy_path (char *out_path, const char *in_path);
char       *true_path (char *path);
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
DWORD       search_list_value (const char *name, const search_list *sl, int num);
const char *search_list_name (DWORD value, const search_list *sl, int num);
const char *flags_decode (DWORD flags, const search_list *list, int num);
int        _gettimeofday (struct timeval *tv, void *timezone);
int         get_timespec_UTC (struct timespec *ts);
double      get_usec_now (void);
void        get_FILETIME_now (FILETIME *ft);
uint32_t    ato_hertz (const char *Hertz);
int64_t     receiveclock_ms_elapsed (uint64_t t1, uint64_t t2);
void        crtdbug_init (void);
void        crtdbug_exit (void);
const char *win_strerror (DWORD err);
const char *get_rtlsdr_error (int err);
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
int         load_dynamic_table (dyn_struct *tab, int tab_size);
int         unload_dynamic_table (dyn_struct *tab, int tab_size);
bool        test_add (char **pattern, const char *what);
bool        test_contains (const char *pattern, const char *what);
void        puts_long_line (const char *start, size_t indent);
void        fputs_long_line (FILE *file, const char *start, size_t indent);
const char *mz_version (void);                 /* in 'externals/zip.c' */
void        show_version_info (bool verbose);

/*
 * Functions removed from Mongoose ver 7.15
 * re-added to `misc.c':
 */
uint32_t mg_unhex  (const char *str);
uint32_t mg_unhexn (const char *str, size_t len);
char    *mg_hex (const void *buf, size_t len, char *to);

/*
 * in `pconsole.c'. Not used yet.
 */
typedef struct pconsole_t pconsole_t;
bool pconsole_create (pconsole_t *pty, const char *cmd_path, const char **cmd_argv);

#ifndef U8_SIZE
#define U8_SIZE 100
#endif

/*
 * For displaying UTF-8 strings.
 */
const wchar_t *u8_format (const char *s, int min_width);

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
 * GNU-like `getopt_long()' / `getopt_long_only()'.
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
         * One of `no_argument`, `required_argument` or `optional_argument`:<br>
         * whether option takes an argument.
         */
        int  has_arg;
        int *flag;    /**< if not NULL, set *flag to val when option found. */
        int  val;     /**< if flag not NULL, value to set `*flag to'; else return value. */
      } option;

int getopt_long (int, char * const *, const char *, const option *, int *);
int getopt_long_only (int nargc, char * const *nargv, const char *options,
                      const option *long_options, int *idx);

int getopt (int nargc, char * const *nargv, const char *options);

extern char *optarg;  /**< the argument to an option in `optsstring`. */
extern int   optind;  /**< the index of the next element to be processed in `argv`. */
extern int   opterr;  /**< if caller set this to zero, an error-message will never be printed. */
extern int   optopt;  /**< on errors, an unrecognised option character is stored in `optopt`. */

