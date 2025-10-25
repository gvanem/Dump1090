/**\file    aircraft.h
 * \ingroup Main
 */
#pragma once

#include "misc.h"
#include "geo.h"

/**
 * \def AIRCRAFT_DATABASE_CSV
 * Our default aircraft-database relative to `Modes.where_am_I`.
 */
#define AIRCRAFT_DATABASE_CSV   "aircraft-database.csv"

/**
 * \def AIRCRAFT_DATABASE_URL
 * The default URL for the `--update` option.
 */
#define AIRCRAFT_DATABASE_URL   "https://s3.opensky-network.org/data-samples/metadata/aircraftDatabase.zip"

/**
 * \def AIRCRAFT_DATABASE_TMP
 * The basename for downloading a new `aircraft-database.csv`.
 *
 * E.g. Use WinInet API to download:<br>
 *  `AIRCRAFT_DATABASE_URL` -> `%TEMP%\\dump1090\\aircraft-database-temp.zip`
 *
 * extract this using: <br>
 *  `zip_extract (\"%TEMP%\\dump1090\\aircraft-database-temp.zip\", \"%TEMP%\\dump1090\\aircraft-database-temp.csv\")`.
 *
 * and finally call: <br>
 *   `CopyFile ("%TEMP%\\dump1090\\aircraft-database-temp.csv", <final_destination>)`.
 */
#define AIRCRAFT_DATABASE_TMP  "aircraft-database-temp"

/**
 * \def AIRCRAFT_JSON_BUF_LEN
 * The initial and increment buffer-size in `aircraft_make_json()`
 */
#define AIRCRAFT_JSON_BUF_LEN  (20*1024)

/**
 * \typedef a_show_t
 * The "show-state" for an aircraft in the interactive TUI-screen.
 */
typedef enum a_show_t {
        A_SHOW_FIRST_TIME = 1,  /**< Print in green colour when shown for the first time */
        A_SHOW_LAST_TIME,       /**< Print in red colour when shown for the last time */
        A_SHOW_NORMAL,          /**< Print in default colour when shown as a normal live aircraft */
        A_SHOW_NONE,
      } a_show_t;

/**
 * \typedef a_sort_t
 * The sort methods for aircrafts in interactive mode.
 */
typedef enum a_sort_t {
        INTERACTIVE_SORT_NONE = 0,    /**< No sorting; show in order of discovery */
        INTERACTIVE_SORT_ICAO,        /**< Sort on ICAO address */
        INTERACTIVE_SORT_CALLSIGN,    /**< Sort on Call-sign / Flight */
        INTERACTIVE_SORT_REGNUM,      /**< Sort on Registration Number \todo */
        INTERACTIVE_SORT_COUNTRY,     /**< Sort on Registration Country \todo */
        INTERACTIVE_SORT_DEP_DEST,    /**< Sort on Departure - Destination (DEP -DEST) \todo */
        INTERACTIVE_SORT_ALTITUDE,    /**< Sort on altitude */
        INTERACTIVE_SORT_SPEED,       /**< Sort on speed */
        INTERACTIVE_SORT_DISTANCE,    /**< Sort on distance to Modes.home_pos */
        INTERACTIVE_SORT_MESSAGES,    /**< Sort on number of messages */
        INTERACTIVE_SORT_SEEN         /**< Sort on seconds since message recv */
      } a_sort_t;

/**
 * \typedef AIRCRAFT_FLAGS
 * Bit-flags for `modeS_message::AC_flags` and `aircraft::AC_flags`
 */
typedef enum AIRCRAFT_FLAGS {
	MODES_ACFLAGS_ALTITUDE_VALID     = 0x000001,
        MODES_ACFLAGS_AOG                = 0x000002,
        MODES_ACFLAGS_AOG_VALID          = 0x000004,
        MODES_ACFLAGS_LLEVEN_VALID       = 0x000008,
        MODES_ACFLAGS_LLODD_VALID        = 0x000010,
        MODES_ACFLAGS_CALLSIGN_VALID     = 0x000020,
        MODES_ACFLAGS_LATLON_VALID       = 0x000040,
        MODES_ACFLAGS_ALTITUDE_HAE_VALID = 0x000080,
        MODES_ACFLAGS_HAE_DELTA_VALID    = 0x000100,
        MODES_ACFLAGS_HEADING_VALID      = 0x000200,
        MODES_ACFLAGS_SQUAWK_VALID       = 0x000400,
        MODES_ACFLAGS_SPEED_VALID        = 0x000800,
        MODES_ACFLAGS_CATEGORY_VALID     = 0x001000,
        MODES_ACFLAGS_FROM_MLAT          = 0x002000,
        MODES_ACFLAGS_FROM_TISB          = 0x004000,
        MODES_ACFLAGS_VERTRATE_VALID     = 0x008000,
        MODES_ACFLAGS_REL_CPR_USED       = 0x010000,   /* Lat/lon derived from relative CPR */
        MODES_ACFLAGS_LATLON_REL_OK      = 0x020000,   /* Indicates it's OK to do a relative CPR */
        MODES_ACFLAGS_NSEWSPD_VALID      = 0x040000,   /* Aircraft EW and NS Speed is known */
        MODES_ACFLAGS_FS_VALID           = 0x080000,   /* Aircraft Flight Status is known */
        MODES_ACFLAGS_EWSPEED_VALID      = 0x100000,   /* Aircraft East West Speed is known */
        MODES_ACFLAGS_NSSPEED_VALID      = 0x200000,   /* Aircraft North South Speed is known */
        MODES_ACFLAGS_LLBOTH_VALID       = (MODES_ACFLAGS_LLEVEN_VALID | MODES_ACFLAGS_LLODD_VALID),
        MODES_ACFLAGS_LLEITHER_VALID     = (MODES_ACFLAGS_LLEVEN_VALID | MODES_ACFLAGS_LLODD_VALID)
      } AIRCRAFT_FLAGS;

/**
 * \typedef MODEAC_FLAGS
 * Bit-flags for `aircraft::mode_AC_flags`
 */
typedef enum MODEAC_FLAGS {
        MODEAC_MSG_FLAG       = 0x01,
        MODEAC_MSG_MODES_HIT  = 0x02,
        MODEAC_MSG_MODEA_HIT  = 0x04,
        MODEAC_MSG_MODEC_HIT  = 0x08,
        MODEAC_MSG_MODEA_ONLY = 0x10,
        MODEAC_MSG_MODEC_OLD  = 0x20
      } MODEAC_FLAGS;

/**
 * \typedef addrtype_t
 * What sort of address is this and who sent it?
 */
typedef enum addrtype_t {
        ADDR_ADSB_ICAO,       /**< Mode S or ADS-B, ICAO address, transponder sourced */
        ADDR_ADSB_ICAO_NT,    /**< ADS-B, ICAO address, non-transponder */
        ADDR_ADSR_ICAO,       /**< ADS-R, ICAO address */
        ADDR_TISB_ICAO,       /**< TIS-B, ICAO address */
        ADDR_ADSB_OTHER,      /**< ADS-B, other address format */
        ADDR_ADSR_OTHER,      /**< ADS-R, other address format */
        ADDR_TISB_TRACKFILE,  /**< TIS-B, Mode A code + track file number */
        ADDR_TISB_OTHER,      /**< TIS-B, other address format */
        ADDR_MODE_A,          /**< Mode A */
        ADDR_UNKNOWN          /**< unknown address format */
      } addrtype_t;

/**
 * \typedef aircraft_info
 * Describes an aircraft from a .CSV/.SQL-file.
 */
typedef struct aircraft_info {
        uint32_t addr;
        char     reg_num  [10];
        char     manufact [30];
        char     type     [10];
        char     call_sign[20];
      } aircraft_info;

/**
 * \typedef aircraft
 * Structure used to describe an aircraft in interactive mode.
 */
typedef struct aircraft {
        uint32_t  addr;                   /**< ICAO address */
        uint16_t  addrtype;               /**< Highest priority address type seen for this aircraft. \ref addrtype_t */
        char      call_sign [9];          /**< Call-sign / flight number. Not normalized */
        int       altitude;               /**< Altitude */
        int       altitude_C;             /**< Altitude for Mode-C */
        int       altitude_HAE;           /**< Altitude for HAE */
        uint64_t  seen_altitude;          /**< Time (msec) at which altitude was measured */
        double    speed;                  /**< Velocity computed from EW and NS components. In Knots */
        uint64_t  seen_speed;             /**< Tick-time (in milli-sec) at which speed was measured */
        double    heading;                /**< Horizontal angle of flight; [0 ... 360] */
        bool      is_helicopter;          /**< It is a helicopter */
        bool      done_flight_info;       /**< Have we shown the flight-info? */

        uint64_t  seen_first;             /**< Tick-time (in milli-sec) at which the first packet was received */
        uint64_t  seen_last;              /**< Tick-time (in milli-sec) at which the last packet was received */
        uint64_t  seen_pos;               /**< Time (millis) at which latitude/longitude was measured */
        uint64_t  seen_pos_EST;           /**< Tick-time (in milli-sec) at which the last estimated position was done */

        uint32_t  messages;               /**< Number of Mode S messages received */
        uint32_t  global_dist_ok;         /**< Number of CPR global distance check okay */
        uint32_t  global_dist_checks;     /**< Number of CPR global distance check failures */
        int       identity;               /**< 13 bits identity (Squawk) */
        int       vert_rate;              /**< Vertical rate */
        a_show_t  show;                   /**< The plane's show-state */
        double    distance;               /**< Distance (in meters) to home position */
        bool      distance_ok;            /**< Distance is valid */
        char      distance_buf [20];      /**< Buffer for `get_home_distance()` */
        double    distance_EST;           /**< Estimated `distance` based on last `speed` and `heading`. In meters. */
        char      distance_buf_EST [20];  /**< Buffer for `get_est_home_distance()` */
        double    sig_levels [4];         /**< RSSI signal-levels from the last 4 messages */
        int       sig_idx;

        /* Encoded latitude and longitude as extracted by odd and even CPR encoded messages.
         */
        uint64_t  odd_CPR_time;           /**< Tick-time for reception of an odd CPR message */
        int       odd_CPR_lat;            /**< Encoded odd CPR latitude */
        int       odd_CPR_lon;            /**< Encoded odd CPR longitude */
        unsigned  odd_CPR_nuc;            /**< "Navigation Uncertainty Category" for odd message */

        uint64_t  even_CPR_time;          /**< Tick-time for reception of an even CPR message */
        int       even_CPR_lat;           /**< Encoded even CPR latitude */
        int       even_CPR_lon;           /**< Encoded even CPR longitude */
        unsigned  even_CPR_nuc;           /**< "Navigation Uncertainty Category" for even message */

        pos_t     position;               /**< Coordinates obtained from decoded CPR data */
        pos_t     position_EST;           /**< Estimated position based on last `speed` and `heading` */
        unsigned  pos_nuc;                /**< NUCp of last computed position
                                            * NUCp == "Navigation Uncertainty Category"
                                            */

        uint32_t  AC_flags;               /**< Flags; \ref enum AIRCRAFT_FLAGS */
        uint32_t  mode_AC_flags;          /**< Flags for mode A/C; \ref MODEAC_FLAGS */
        uint32_t  mode_A_count;           /**< Mode A Squawk hit Count */
        uint32_t  mode_C_count;           /**< Mode C Altitude hit Count */
        uint32_t  MLAT_flags;             /**< Data derived from MLAT messages*/
        uint32_t  TISB_flags;             /**< Data derived from TIS-B messages*/
        int       HAE_delta;              /**< Difference between HAE and Baro altitudes */
        uint32_t  category;               /**< A0 - D7 encoded as a single hex byte */

        modeS_message        first_msg;   /**< A copy of the first message we received for this aircraft */
        aircraft_info       *SQL;         /**< A pointer to a SQL record (or NULL) */
        const aircraft_info *CSV;         /**< A pointer to a CSV record in `Modes.aircraft_list_CSV` (or NULL) */
      } aircraft;

/**
 * \def LOG_FOLLOW(a)
 * \def LOG_UNFOLLOW(a)
 * \def LOG_DISTANCE(a)
 * \def LOG_BEARING(a)
 *
 * To verify the CPR stuff works correctly;
 * follow a single plane at a time and watch the distance changes smoothly.
 * And not jumping around the map as it used to.
 *
 * Effective with cmd-line option `--debug P` only.
 */
#define LOG_FOLLOW(a) do {                             \
        if ((Modes.debug & DEBUG_PLANE) &&             \
            a->addr && Modes.a_follow == 0)            \
        {                                              \
          LOG_FILEONLY ("%06X: Following\n", a->addr); \
          Modes.a_follow = a->addr;                    \
        }                                              \
      } while (0)

#define LOG_UNFOLLOW(a) do {                             \
        if ((Modes.debug & DEBUG_PLANE) &&               \
            a->addr && a->addr == Modes.a_follow)        \
        {                                                \
          LOG_FILEONLY ("%06X: Unfollowing\n", a->addr); \
          Modes.a_follow = 0;                            \
        }                                                \
      } while (0)

#define LOG_DISTANCE(a) do {                             \
        if ((Modes.debug & DEBUG_PLANE) &&               \
            Modes.a_follow && a->addr == Modes.a_follow) \
          LOG_FILEONLY ("%06X: dist: %7.3f km\n",        \
                        a->addr, a->distance / 1000.0);  \
      } while (0)

#define LOG_BEARING(a) do {                                                  \
        if ((Modes.debug & DEBUG_PLANE) &&                                   \
            a->addr == Modes.a_follow && Modes.home_pos_ok)                  \
        {                                                                    \
          double _bearing = geo_get_bearing (&Modes.home_pos, &a->position); \
          LOG_FILEONLY ("%06X: bearing: %.1lf / %s\n",                       \
                        a->addr, _bearing, geo_bearing_name(_bearing));      \
        }                                                                    \
      } while (0)

bool        aircraft_init (void);
void        aircraft_exit (bool free_aircrafts);
bool        aircraft_valid (const aircraft *a);
int         aircraft_numbers (void);
int         aircraft_numbers_valid (void);
aircraft   *aircraft_find (uint32_t addr);
aircraft   *aircraft_update_from_message (modeS_message *mm);
uint32_t    aircraft_get_addr (const uint8_t *a);
const char *aircraft_get_details (const modeS_message *mm);
const char *aircraft_extra_info (const aircraft *a);
const char *aircraft_get_country (uint32_t addr, bool get_short);
const char *aircraft_AC_flags (enum AIRCRAFT_FLAGS flags);
const char *aircraft_mode_AC_flags (enum MODEAC_FLAGS flags);
bool        aircraft_is_military (uint32_t addr, const char **country);
bool        aircraft_is_helicopter (uint32_t addr, const char **code);
bool        aircraft_match_init (const char *arg);
bool        aircraft_match (uint32_t addr);
bool        aircraft_set_est_home_distance (aircraft *a, uint64_t now);
char       *aircraft_make_json (bool extended_client);
void        aircraft_receiver_json (mg_connection *c);
void        aircraft_remove_stale (uint64_t now);
void        aircraft_show_stats (void);
bool        aircraft_set_sort (const char *arg);
a_sort_t    aircraft_sort (int s);
const char *aircraft_sort_name (int s);
void        aircraft_fix_flightaware (void);

#if defined(USE_BIN_FILES)
  const char *aircraft_get_country2 (uint32_t addr, bool get_short);
  bool        aircraft_is_military2 (uint32_t addr, const char **country);
#endif

#define AIRCRAFT_GET_ADDR(a) aircraft_get_addr ((const uint8_t*)(a))

